/* =====================================================================
 *  w7_thread.c - the band renderer's worker pool.
 *
 *  The frame is cut into horizontal bands.  Every band runs the SAME
 *  draw list with its own thread-local clip rows (clip_y0/clip_y1), so
 *  draw code does not know it is threaded: the primitives simply write
 *  only their band's rows.  Bands are handed out from an atomic counter,
 *  so a thread that finishes a cheap band (the marquee, the button deck)
 *  takes the next one instead of waiting on the busy reel window.
 *
 *  The pool is created once (bp_config, from the core option) and torn
 *  down in retro_deinit; nothing is created per frame.  Workers SLEEP on
 *  a condition variable between frames - an idle attract screen must not
 *  hold three cores busy - and run at the default SCHED_OTHER priority,
 *  so RetroArch's own video and audio threads are never starved.  One
 *  thread (bp_nthreads == 1) calls the draw list directly: no locks, no
 *  atomics, nothing but the call.
 *
 *  Everything here is generic: it knows rows, not the game.
 * ===================================================================== */
#ifndef W7_NOTHREADS
#  include <pthread.h>
#  include <signal.h>
#  include <unistd.h>
#  ifdef __linux__
#    include <sys/prctl.h>
#  endif
#endif

#define BP_MAXT     4           /* the option offers 1..4                */
#define BP_MAXBANDS 64

static int bp_nthreads = 1;     /* threads drawing, the caller included   */
static int bp_nbands   = 1;     /* bands per frame                        */
static int bp_band_y[BP_MAXBANDS+1] = { 0, FBH };
static int bp_active   = 0;     /* 1 only while bands run in parallel:
                                   shared caches take bp_lock() then     */

/*  Band edges.  Equal heights: with the bands pulled dynamically the
 *  uneven cost of the rows (the reel window is dearer than the deck)
 *  evens out on its own, and more bands balance better but repeat the
 *  per-band overhead of every draw call, so the count is a trade-off
 *  measured in DEVELOPING.md.                                          */
static void bp_layout(int nb){
  if(nb<1) nb=1;
  if(nb>BP_MAXBANDS) nb=BP_MAXBANDS;
  bp_nbands=nb;
  for(int b=0;b<=nb;b++) bp_band_y[b]=b*FBH/nb;
}

#ifdef W7_NOTHREADS
static void bp_lock(void){}
static void bp_unlock(void){}
static int  bp_config(int want,int bands){ (void)want; (void)bands; return 1; }
static void bp_shutdown(void){}
static void bp_run(void(*fn)(int,int)){ fn(0,FBH); }
#else

/*  Guards the caches the draw code shares (textb masks, cached frames).
 *  Taken only when bp_active: never on the single-threaded path.       */
static pthread_mutex_t bp_data_mx = PTHREAD_MUTEX_INITIALIZER;
static void bp_lock(void)  { pthread_mutex_lock(&bp_data_mx); }
static void bp_unlock(void){ pthread_mutex_unlock(&bp_data_mx); }

static pthread_t       bp_tid[BP_MAXT];
static int             bp_nworkers = 0;
static int             bp_want_bands = 0;
static pthread_mutex_t bp_mx   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  bp_go   = PTHREAD_COND_INITIALIZER;   /* a frame   */
static pthread_cond_t  bp_done = PTHREAD_COND_INITIALIZER;   /* all bands */
static unsigned        bp_gen  = 0;       /* frame number, under bp_mx   */
static int             bp_quit = 0;       /* under bp_mx                 */
static int             bp_next = 0;       /* atomic: next band to take   */
static int             bp_left = 0;       /* atomic: bands not finished  */
static void          (*bp_fn)(int,int);

static inline void bp_relax(void){
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

/*  Take bands until there are none left.  Called by the main thread and
 *  every worker.  A worker that wakes late for a frame whose bands are
 *  all taken simply finds none; one that wakes very late takes bands of
 *  the NEXT frame, which is correct too, because the counter is only
 *  reset once that frame's state is complete.                         */
static void bp_take(void){
  for(;;){
    int b=__atomic_fetch_add(&bp_next,1,__ATOMIC_ACQ_REL);
    if(b>=bp_nbands) return;
    bp_fn(bp_band_y[b],bp_band_y[b+1]);
    if(__atomic_sub_fetch(&bp_left,1,__ATOMIC_ACQ_REL)==0){
      pthread_mutex_lock(&bp_mx);
      pthread_cond_signal(&bp_done);
      pthread_mutex_unlock(&bp_mx);
    }
  }
}

static void* bp_worker(void*arg){
#ifdef __linux__
  { char nm[16]="w7band0";               /* shows in top -H on the Pi */
    nm[6]=(char)('1'+(int)(intptr_t)arg);
    prctl(PR_SET_NAME,nm,0,0,0); }
#else
  (void)arg;
#endif
  pthread_mutex_lock(&bp_mx);
  unsigned seen=bp_gen;
  for(;;){
    while(bp_gen==seen && !bp_quit) pthread_cond_wait(&bp_go,&bp_mx);
    if(bp_quit) break;
    seen=bp_gen;
    pthread_mutex_unlock(&bp_mx);
    bp_take();
    pthread_mutex_lock(&bp_mx);
  }
  pthread_mutex_unlock(&bp_mx);
  return NULL;
}

static void bp_run(void(*fn)(int,int)){
  if(bp_nworkers==0){ fn(0,FBH); return; }        /* the plain path */
  pthread_mutex_lock(&bp_mx);
  bp_fn=fn;
  __atomic_store_n(&bp_left,bp_nbands,__ATOMIC_RELAXED);
  __atomic_store_n(&bp_next,0,__ATOMIC_RELEASE);
  bp_active=1;
  bp_gen++;
  pthread_cond_broadcast(&bp_go);
  pthread_mutex_unlock(&bp_mx);

  bp_take();

  /* The last band is usually a moment from done: spin briefly on it,
     then sleep rather than burn the core. */
  for(int i=0;i<4000 && __atomic_load_n(&bp_left,__ATOMIC_ACQUIRE)>0;i++) bp_relax();
  if(__atomic_load_n(&bp_left,__ATOMIC_ACQUIRE)>0){
    pthread_mutex_lock(&bp_mx);
    while(__atomic_load_n(&bp_left,__ATOMIC_ACQUIRE)>0)
      pthread_cond_wait(&bp_done,&bp_mx);
    pthread_mutex_unlock(&bp_mx);
  }
  bp_active=0;
}

static void bp_shutdown(void){
  if(bp_nworkers){
    pthread_mutex_lock(&bp_mx);
    bp_quit=1;
    pthread_cond_broadcast(&bp_go);
    pthread_mutex_unlock(&bp_mx);
    for(int i=0;i<bp_nworkers;i++) pthread_join(bp_tid[i],NULL);
    bp_nworkers=0;
    bp_quit=0;
  }
  bp_nthreads=1;
  bp_want_bands=0;
  bp_layout(1);
}

/*  (Re)build the pool for `want` drawing threads, the caller included,
 *  cutting the frame into `bands` bands (0 = the default for that many
 *  threads).  Only called between frames.  If a worker cannot be
 *  created the pool keeps the ones it has, down to none at all, which
 *  is the single-threaded renderer.  Returns the threads in use.      */
static int bp_config(int want,int bands){
  if(want<1) want=1;
  if(want>BP_MAXT) want=BP_MAXT;
  if(want==bp_nthreads && bands==bp_want_bands) return bp_nthreads;
  bp_shutdown();
  if(want>1){
    /* Workers block every signal: the frontend's handlers belong on its
       own threads.  They inherit the mask, so set it around creation. */
    sigset_t all,old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK,&all,&old);
    pthread_attr_t at;
    int attr_ok = pthread_attr_init(&at)==0;
    if(attr_ok) pthread_attr_setstacksize(&at,512*1024);
    for(int i=0;i<want-1;i++){
      if(pthread_create(&bp_tid[i],attr_ok?&at:NULL,bp_worker,(void*)(intptr_t)i)!=0) break;
      bp_nworkers++;
    }
    if(attr_ok) pthread_attr_destroy(&at);
    pthread_sigmask(SIG_SETMASK,&old,NULL);
  }
  bp_nthreads=bp_nworkers+1;
  bp_want_bands=bands;
  bp_layout(bp_nthreads==1 ? 1 : (bands>0 ? bands : bp_nthreads*4));
  return bp_nthreads;
}
#endif

/*  auto = one core per band thread, leaving one for RetroArch's video,
 *  audio and input and some thermal headroom: min(3, cpus-1), >= 1.   */
static int bp_auto_threads(void){
#ifdef W7_NOTHREADS
  return 1;
#else
  long n=sysconf(_SC_NPROCESSORS_ONLN);
  int t=(n>1)?(int)n-1:1;
  return t>3?3:t;
#endif
}
