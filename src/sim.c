/* =====================================================================
 *  RTP simulator for WILD 7's.
 *
 *  Includes the core's own source so the maths under test is literally
 *  the code that ships — same strips, same cluster evaluator, same wild,
 *  scatter and jackpot handling.  A reimplementation here would be
 *  worthless.
 *
 *    gcc -O2 -Isrc src/sim.c -o w7sim -lm
 *    ./w7sim 5000000 [betIdx]
 *
 *  The symbol-triggered progressives are also counted EXACTLY: JACKPOT
 *  only lives on reels 2-4, so every combination of those three reels
 *  can be enumerated (96^3), and ULTIMATE is one symbol per reel, so its
 *  chains can be enumerated too.  Monte-Carlo cannot see a 1-in-10M
 *  event; the enumeration can.
 * ===================================================================== */
#include "wild7_libretro.c"

static long long spins, cost, won;
static long long jpTrig[NJP];
static double    jpWonTot[NJP];
static long long hits, fsTrig, pkTrig, fsWonTot, pkWonTot, baseWonTot, scatWonTot;
static long long hoTrig, whTrig, hoWonTot, whWonTot, stormN;
/*  SEEDS=1 (the default) keeps every pot at its bet-multiple seed: the
    pots are emptied before each spin instead of growing, so everything
    won is a seed or a credit prize, and the long-run return is exactly
    that plus the tenth of every bet that feeds the pots.  Pots growing
    are only a transfer from the contributions, which a Monte-Carlo run
    would otherwise count twice (hold and wheel pay pots too) and whose
    ULTIMATE hits swing any finite sample by whole percent. */
static int SEEDS=1;
static long long ultWon;
static long long bandhit[NSYM][6], bandwon[NSYM][6], bandways[NSYM][6], bandwt[NSYM][6];
static long long multhist[64], fsSpins;
static long long sizehist[26];

static void spin_reels(void){
  for(int r=0;r<NREEL;r++) G.rpos[r]=(float)irnd(STRIPLEN);
  snapshot_grid();
}

/* Plays the pick round the way a blind player would: reveal panels in a
   random order until the third stop.  Uses the core's own
   bonus_fill_panels(), so the board under test is the shipped board.  */
static long long sim_pick(void){
  int kind[NPICK], val[NPICK];
  bonus_fill_panels(kind,val);
  int order[NPICK];
  for(int i=0;i<NPICK;i++) order[i]=i;
  for(int i=NPICK-1;i>0;i--){ int j=irnd(i+1); int t=order[i];order[i]=order[j];order[j]=t; }
  long long tot=0; int mult=1, stops=0;
  for(int i=0;i<NPICK;i++){
    int k=kind[order[i]], v=val[order[i]];
    if(k==PICK_STOP){ if(++stops>=3) break; }
    else if(k==PICK_MULT) mult*=v;
    else tot+=v;
  }
  return tot*mult;
}

static void take_jackpot(void){
  if(G.jpWon<0) return;
  jpTrig[G.jpWon]++; jpWonTot[G.jpWon]+=(double)G.jpAmt; won+=G.jpAmt;
  if(G.jpWon==JP_ULT) ultWon+=G.jpAmt;
  G.jpWon=-1; G.jpAmt=0;
}

/* ── exact odds for the symbol progressives ───────────────────────── */
static double exact_jp[NJP];       /* probability per spin of each tier */

static void enumerate_jackpots(void){
  /* JACKPOT: reels 1,2,3 (0-based) carry it; enumerate their stops */
  long long cnt[4]={0,0,0,0}, tot=0;
  int saveIn=G.inFree; G.inFree=0;
  for(int a=0;a<STRIPLEN;a++) for(int b=0;b<STRIPLEN;b++) for(int c=0;c<STRIPLEN;c++){
    uint32_t m=0;
    int st[3]={a,b,c};
    for(int k=0;k<3;k++) for(int row=0;row<NROW;row++)
      if(stripAt(1+k,st[k]-row)==SY_JACKPOT) m|=1u<<((1+k)*NROW+row);
    tot++;
    if(!m) continue;
    int best=0; uint32_t done=0, mm;
    for(int i=0;i<NCELL;i++){
      if(!((m>>i)&1u)||((done>>i)&1u)) continue;
      int n=flood(m,i,&mm); done|=mm; if(n>best) best=n;
    }
    if(best>=JP_NEED[JP_MEGA]) cnt[JP_MEGA]++;
    else if(best>=JP_NEED[JP_MAJOR]) cnt[JP_MAJOR]++;
    else if(best>=JP_NEED[JP_MINOR]) cnt[JP_MINOR]++;
  }
  for(int i=1;i<NJP;i++) exact_jp[i]=(double)cnt[i]/(double)tot;

  /* ULTIMATE: for each reel list the stops that show it and on which row */
  int nst[NREEL], rowOf[NREEL][STRIPLEN];
  for(int r=0;r<NREEL;r++){
    nst[r]=0;
    for(int s=0;s<STRIPLEN;s++) for(int row=0;row<NROW;row++)
      if(stripAt(r,s-row)==SY_ULT){ rowOf[r][nst[r]++]=row; break; }
  }
  /* chains: consecutive reels within one row of each other; five reels
     needed since a reel shows at most one */
  double p=0;
  for(int i0=0;i0<nst[0];i0++) for(int i1=0;i1<nst[1];i1++){
    if(abs(rowOf[0][i0]-rowOf[1][i1])>1) continue;
    for(int i2=0;i2<nst[2];i2++){
      if(abs(rowOf[1][i1]-rowOf[2][i2])>1) continue;
      for(int i3=0;i3<nst[3];i3++){
        if(abs(rowOf[2][i2]-rowOf[3][i3])>1) continue;
        for(int i4=0;i4<nst[4];i4++){
          if(abs(rowOf[3][i3]-rowOf[4][i4])>1) continue;
          p+=1.0;
        }
      }
    }
  }
  exact_jp[JP_ULT]=p/pow((double)STRIPLEN,5.0);
  G.inFree=saveIn;
}

int main(int argc,char**argv){
  long long N = (argc>1)? atoll(argv[1]) : 2000000;
  int betIdx  = (argc>2)? atoi(argv[2]) : 0;
  if(argc>3) FS_AWARD   = atoi(argv[3]);      /* sweep the feature dials */
  if(argc>4) FS_MAXMULT = atoi(argv[4]);
  { const char*e=getenv("W7SIM_POTS"); if(e && !strcmp(e,"grow")) SEEDS=0; }  /* pots as they fell */
  rngs = 0xC0FFEEu;
  build_strips();
  memset(&G,0,sizeof G);
  G.betIdx=betIdx;
  jp_seed();
  if(argc>1 && !strcmp(argv[1],"jp") && argc>4){   /* w7sim jp MINOR MAJOR MEGA [cnt24 cnt3 stk24 stk3] */
    JP_NEED[JP_MINOR]=atoi(argv[2]); JP_NEED[JP_MAJOR]=atoi(argv[3]); JP_NEED[JP_MEGA]=atoi(argv[4]);
    if(argc>=9){                                    /* ... cnt24 cnt3 stk24 stk3 */
      CNT[1][SY_JACKPOT]=atoi(argv[5]); CNT[2][SY_JACKPOT]=atoi(argv[6]);
      STK[1][SY_JACKPOT]=atoi(argv[7]); STK[2][SY_JACKPOT]=atoi(argv[8]);
      build_strips();
    }
  }
  enumerate_jackpots();
  if(argc>1 && !strcmp(argv[1],"jp")){        /* exact progressive odds only */
    for(int i=0;i<NJP;i++)
      printf("%-9s need %d   1 in %.0f\n",JP_NAME[i],JP_NEED[i],exact_jp[i]>0?1.0/exact_jp[i]:0.0);
    return 0;
  }

  for(long long i=0;i<N;i++){
    spins++; cost += TOTBET;
    if(SEEDS) memset(G.jpAcc,0,sizeof G.jpAcc); else jp_contribute(TOTBET);

    G.inFree=0; G.fsMult=1;
    extra_on_spin_start(); if(G.extra.stormArmed) stormN++;
    spin_reels(); evaluate();
    take_jackpot();
    long long w = G.winTotal;
    baseWonTot += w;
    if(w>0) hits++;
    for(int k=0;k<G.nWin;k++){
      int s=G.winSym[k], n=G.winCnt[k];
      bandhit[s][n]++; bandwon[s][n]+=G.winAmt[k]; bandways[s][n]+=G.winWays[k];
      bandwt[s][n]+=G.winWt[k];
      int wy=G.winWays[k]; sizehist[wy>25?25:wy]++;
    }
    if(G.scatCount>=3) scatWonTot += SCATPAY[G.scatCount>5?5:G.scatCount]*TOTBET;

    /* the new features, played out by their own modules' logic */
    if(hold_triggered()) { long long p=hold_sim_play();  hoTrig++; hoWonTot+=p; w+=p; }
    if(wheel_triggered()){ long long p=wheel_sim_play(); whTrig++; whWonTot+=p; w+=p; }
    int scat=G.scatCount, crown=G.bonusCount;

    if(crown>=3){
      pkTrig++;
      long long p=sim_pick();
      w += p; pkWonTot += p;
    }
    if(scat>=3){
      fsTrig++;
      int left=FS_AWARD; long long fw=0;
      G.inFree=1; G.fsMult=1;
      while(left>0){
        left--; fsSpins++;
        extra_on_spin_start();
        spin_reels(); evaluate();
        if(hold_triggered()) { long long p=hold_sim_play();  hoTrig++; hoWonTot+=p; fw+=p; }
        if(wheel_triggered()){ long long p=wheel_sim_play(); whTrig++; whWonTot+=p; fw+=p; }
        { int mm=G.fsMult<1?1:(G.fsMult>63?63:G.fsMult); multhist[mm]++; }
        take_jackpot();
        fw += G.winTotal;
        if(G.scatCount>=3) left += FS_RETRIG;  /* retrigger */
        if(left>400) break;
      }
      G.inFree=0; G.fsMult=1;
      w += fw; fsWonTot += fw;
    }
    won += w;
  }

  printf("spins            %lld   at bet %d\n", spins, TOTBET);
  printf("total bet        %lld\n", cost);
  printf("total won        %lld\n", won);
  printf("-----------------------------------------\n");
  double base=100.0*baseWonTot/cost, fs=100.0*fsWonTot/cost, pk=100.0*pkWonTot/cost;
  if(SEEDS){
    double rates=0; for(int i=0;i<NJP;i++) rates+=100.0*JP_RATE[i];
    double ultx=100.0*JP_MULT[JP_ULT]*exact_jp[JP_ULT];
    printf("RTP              %.2f%%   LONG RUN: %.2f%% prizes and seeds (ULTIMATE aside)\n"
           "                            + %.2f%% contributions + %.3f%% ULTIMATE seed, exact odds\n",
           100.0*(won-ultWon)/cost+rates+ultx, 100.0*(won-ultWon)/cost, rates, ultx);
  } else
  printf("RTP              %.2f%%   (monte-carlo, progressives as they fell)\n", 100.0*won/cost);
  printf("  base game      %.2f%%   of which scatter pays %.2f%%\n", base, 100.0*scatWonTot/cost);
  printf("  free spins     %.2f%%\n", fs);
  printf("  pick bonus     %.2f%%\n", pk);
  printf("  hold & spin    %.2f%%   1 in %.0f spins\n", 100.0*hoWonTot/cost, (double)spins/(hoTrig?hoTrig:1));
  printf("  wheel bonus    %.2f%%   1 in %.0f spins\n", 100.0*whWonTot/cost, (double)spins/(whTrig?whTrig:1));
  hold_sim_report();                          /* w7_hold.c: how the prizes spread */
  printf("  7 strike       1 in %.0f spins\n", (double)spins/(stormN?stormN:1));
  { double jt=0;
    for(int i=0;i<NJP;i++) jt+=jpWonTot[i];
    printf("  jackpots       %.2f%%   (as they fell in the sample)\n", 100.0*jt/cost); }
  for(int i=0;i<NJP;i++)
    printf("    %-9s    sampled 1 in %-12.0f exact 1 in %-14.0f mean pot %.0f\n",
           JP_NAME[i], (double)spins/(jpTrig[i]?jpTrig[i]:1),
           exact_jp[i]>0?1.0/exact_jp[i]:0.0,
           jpTrig[i]? jpWonTot[i]/jpTrig[i] : 0.0);
  printf("hit frequency    %.2f%%  (1 in %.1f spins)\n",
         100.0*hits/spins, (double)spins/(hits?hits:1));
  printf("free spins       1 in %.0f spins\n", (double)spins/(fsTrig?fsTrig:1));
  printf("pick bonus       1 in %.0f spins\n", (double)spins/(pkTrig?pkTrig:1));

  /* The progressive return has a bet-independent part (the contribution
     rates) and a seed part that shrinks as the bet rises.  Free-spin
     jackpots are ignored here; the base-game figure is the design one. */
  printf("-----------------------------------------\n");
  double rates=0; for(int i=0;i<NJP;i++) rates+=100.0*JP_RATE[i];
  double game = base+fs+pk+100.0*(hoWonTot+whWonTot)/cost;
  printf("RTP              game %.2f%% + contributions %.2f%% + bet multiples:\n", game, rates);
  printf("  %9s %9s %9s %9s %11s   %8s\n","ULTIMATE","MEGA","MAJOR","MINOR","multiples","RTP");
  { double sd[NJP], st=0;
    for(int i=0;i<NJP;i++){ sd[i]=100.0*JP_MULT[i]*exact_jp[i]; st+=sd[i]; }
    printf("  %8.3f%% %8.3f%% %8.3f%% %8.3f%% %10.3f%%   %7.2f%%\n",
           sd[0],sd[1],sd[2],sd[3],st,game+rates+st);
    printf("  identical at every rung of the ladder, which is the point of the change\n"); }

  printf("-----------------------------------------\n");
  printf("free-spin mult   ");
  for(int i=1;i<=FS_MAXMULT&&i<64;i++) if(multhist[i])
    printf("x%d:%.1f%% ",i,100.0*multhist[i]/(fsSpins?fsSpins:1));
  printf("\n");
  printf("ways per win     ");
  for(int n=1;n<=25;n++) if(sizehist[n]) printf("%d:%.4f%% ",n,100.0*sizehist[n]/spins);
  printf("\n");

  /* Per-outcome frequency, and what each pay entry contributes to RTP.
     "per 1" is the RTP added by raising that pay by one unit, so retuning
     the table is arithmetic, not guesswork. */
  printf("-----------------------------------------\n");
  printf("%-9s %-7s %11s %7s %9s %10s %8s\n","symbol","reels","freq/spin","pay","RTP%","per 1","ways");
  double tot=0;
  for(int i=0;i<NPAYSYM;i++){
    for(int k=3;k<=5;k++){
      if(!bandhit[i][k]) continue;
      double f=(double)bandhit[i][k]/spins;
      double rtp=100.0*bandwon[i][k]/cost;
      double mw=(double)bandwt[i][k]/bandhit[i][k];
      tot+=rtp;
      printf("%-9s %-7s %11.6f %7d %8.3f%% %9.4f%% %8.2f\n",
             SYMNAME[i],BANDNAME[k-3],f,PAY[i][k],rtp,100.0*f*mw/10.0,mw);
    }
  }
  printf("way pays total   %.2f%%  (base game, before scatter pays)\n", tot);
  return 0;
}
