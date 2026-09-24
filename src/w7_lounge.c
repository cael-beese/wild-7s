/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md */
/* w7_lounge.c - the Beese's lounge look, shared with Beese's Poker Lounge:
 * its fonts (Barlow Condensed, Bungee, Tilt Neon, Neonderthaw), neon and
 * gold type, glass panels, neon buttons, the honeycomb room, chasing bulbs,
 * the bee and the panel icon. #included by wild7_libretro.c after the
 * framebuffer primitives, which it draws through.
 *
 * What costs is done once in lz_init(): the fonts are embedded in the core
 * (.incbin), their glyphs rasterised with stb_truetype and their glows
 * blurred; sprites are painted with the poker game's vector rasteriser
 * (w7_raster.c, w7_lart.c). Two kinds of function follow:
 *
 *   lz_paint_* / lz_glass / lz_button   build time only: they allocate,
 *                                       and paint the whole frame (bg)
 *   everything else (lz_text*, lz_neon, lz_gold, lz_icon, lz_add_tint,
 *   lz_marquee, lz_bulbs)               draw code: no state, every row
 *                                       loop clipped to the band
 *
 * so the draw half keeps the render contract (DEVELOPING.md). */

#define LZ_API static __attribute__((unused))

/* The rasteriser and the motifs, compiled into this unit but not exported
 * from the core. */
#pragma GCC visibility push(hidden)
#include "w7_raster.c"
#include "w7_lart.c"
#pragma GCC visibility pop

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "third_party/stb_truetype.h"
#pragma GCC diagnostic pop

/* ── the house palette (the poker game's "neon honey lounge") ─────── */
#define LZ_CHARCOAL 0x0E0C10u
#define LZ_PLUM     0x2A0F2Eu
#define LZ_HONEY    0xE8AA28u
#define LZ_GOLD     0xFFD25Au
#define LZ_AMBER    0xFF8010u
#define LZ_MAGENTA  0xFF28C8u
#define LZ_CYAN     0x28E6FFu
#define LZ_GREEN    0x78FF8Cu
#define LZ_IVORY    0xF8F1E2u
#define LZ_RUBY     0xD01A48u
#define LZ_INK      0x0E0C10u
#define LZ_DIM      0x9A8CA6u

/* ── the fonts, embedded ───────────────────────────────────────────
 *  W7_ASSETS is the absolute path of assets/ (the Makefile passes it),
 *  so the assembler finds the files wherever the compiler runs.        */
#ifndef W7_ASSETS
#  define W7_ASSETS "assets"
#endif
#define LZ_EMBED(sym, file) \
  __asm__(".pushsection .rodata\n.balign 16\n.globl " #sym "\n.hidden " #sym "\n" #sym ":\n" \
          ".incbin \"" W7_ASSETS "/fonts/" file "\"\n.globl " #sym "_end\n.hidden " #sym "_end\n" \
          #sym "_end:\n.byte 0\n.popsection\n"); \
  extern const unsigned char sym[] __attribute__((visibility("hidden"))), \
                             sym##_end[] __attribute__((visibility("hidden")))
LZ_EMBED(lz_ttf_barlow, "BarlowCondensed-SemiBold.ttf");
LZ_EMBED(lz_ttf_bungee, "Bungee-Regular.ttf");
LZ_EMBED(lz_ttf_tilt, "TiltNeon.ttf");
LZ_EMBED(lz_ttf_script, "Neonderthaw-Regular.ttf");

enum { LZF_UI_S, LZF_UI_M, LZF_UI_L, LZF_DISP_S, LZF_DISP_M, LZF_DISP_L,
       LZF_NEON_M, LZF_NEON_L, LZF_SCRIPT, LZF_COUNT };
enum { LZ_LEFT, LZ_CENTER, LZ_RIGHT };

/* file, base size, capitals only, has a glow - as in the poker game */
static const struct { int file, size, caps, glow; } LZ_SPEC[LZF_COUNT] = {
  { 0, 20, 0, 0 }, { 0, 28, 0, 0 }, { 0, 44, 0, 0 },          /* Barlow Condensed */
  { 1, 26, 1, 0 }, { 1, 52, 1, 1 }, { 1, 104, 1, 1 },         /* Bungee           */
  { 2, 36, 0, 1 }, { 2, 64, 0, 1 },                            /* Tilt Neon        */
  { 3, 84, 0, 1 },                                             /* Neonderthaw      */
};

typedef struct {
  uint8_t *m; int w, h;          /* coverage                                  */
  float ox, oy, adv;             /* from the pen and the line top, base px     */
  uint8_t *gm; int gw, gh;       /* glow: half resolution, padded              */
  float gox, goy;                /* the glow's corner, base px                 */
} lz_glyph;
typedef struct { lz_glyph g[95]; float base, capTop, capH; int caps, ok; } lz_font;
static lz_font lzf[LZF_COUNT];

static void lz_bake_font(int f){
  static const unsigned char *const DATA[4] = { lz_ttf_barlow, lz_ttf_bungee, lz_ttf_tilt, lz_ttf_script };
  const unsigned char *data = DATA[LZ_SPEC[f].file];
  stbtt_fontinfo fi;
  if(!stbtt_InitFont(&fi, data, stbtt_GetFontOffsetForIndex(data, 0))) return;
  float size = (float)LZ_SPEC[f].size, sc = stbtt_ScaleForPixelHeight(&fi, size);
  int asc, desc, gap;
  stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
  float ascent = (float)(int)(asc * sc);
  lz_font *F = &lzf[f];
  F->base = size;
  F->caps = LZ_SPEC[f].caps;
  int pad = LZ_SPEC[f].glow ? (int)ceilf(size * 0.09f) : 0;
  for(int c = 32; c < 127; c++){
    if(F->caps && c >= 'a' && c <= 'z') continue;
    lz_glyph *g = &F->g[c - 32];
    int adv, lsb, w = 0, h = 0, xo = 0, yo = 0;
    stbtt_GetCodepointHMetrics(&fi, c, &adv, &lsb);
    g->adv = adv * sc;
    unsigned char *bm = stbtt_GetCodepointBitmap(&fi, 0, sc, c, &w, &h, &xo, &yo);
    g->ox = (float)xo;
    g->oy = ascent + (float)yo;
    if(bm && w > 0 && h > 0){
      g->m = (uint8_t *)malloc((size_t)w * h);
      if(g->m){ memcpy(g->m, bm, (size_t)w * h); g->w = w; g->h = h; }
    }
    if(bm) stbtt_FreeBitmap(bm, NULL);
    if(!pad || !g->m) continue;
    /* The glow: the glyph at half size, padded, blurred, then lifted so
     * thin strokes keep their light (the poker game's recipe). */
    int hw = (w + 1) / 2, hh = (h + 1) / 2, gw = hw + 2 * pad, gh = hh + 2 * pad;
    LCanvas cv = { (uint8_t *)calloc((size_t)gw * gh, 4), gw, gh, gw };
    if(!cv.px) continue;
    for(int y = 0; y < hh; y++) for(int x = 0; x < hw; x++){
      int s = 0;
      for(int k = 0; k < 4; k++){
        int xx = 2 * x + (k & 1), yy = 2 * y + (k >> 1);
        if(xx < w && yy < h) s += g->m[yy * w + xx];
      }
      uint8_t *d = cv.px + ((size_t)(y + pad) * gw + x + pad) * 4;
      d[0] = d[1] = d[2] = d[3] = (uint8_t)(s / 4);
    }
    lcv_blur(&cv, pad / 3 > 1 ? pad / 3 : 1);
    g->gm = (uint8_t *)malloc((size_t)gw * gh);
    if(g->gm){
      for(int i = 0; i < gw * gh; i++){ int v = cv.px[i * 4 + 3] * 5 / 2; g->gm[i] = (uint8_t)(v > 255 ? 255 : v); }
      g->gw = gw; g->gh = gh;
      g->gox = g->ox - 2.0f * pad;
      g->goy = g->oy - 2.0f * pad;
    }
    free(cv.px);
  }
  /* Where the capitals sit, from the H: the old type is placed by its caps. */
  const lz_glyph *H = &F->g['H' - 32];
  F->capTop = H->oy;
  F->capH = H->h > 0 ? (float)H->h : size * 0.7f;
  F->ok = 1;
}

static inline const lz_glyph *lz_glyph_of(const lz_font *F, unsigned char ch){
  if(F->caps && ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 32);
  if(ch < 32 || ch > 126) ch = '?';
  return &F->g[ch - 32];
}

/* ── coverage masks into the frame ─────────────────────────────────── */

/* Bilinear coverage at mask point (u, v), 0..255; outside is 0. */
static inline int lz_samp(const uint8_t *m, int w, int h, float u, float v){
  int iu = (int)floorf(u), iv = (int)floorf(v);
  float fu = u - iu, fv = v - iv;
  int s = 0;
  float acc = 0;
  for(int j = 0; j < 2; j++){
    int y = iv + j;
    if((unsigned)y >= (unsigned)h) continue;
    float wy = j ? fv : 1 - fv;
    for(int i = 0; i < 2; i++){
      int x = iu + i;
      if((unsigned)x >= (unsigned)w) continue;
      acc += m[y * w + x] * wy * (i ? fu : 1 - fu);
    }
  }
  s = (int)(acc + 0.5f);
  return s > 255 ? 255 : s;
}

/* A mask of mw x mh drawn with its corner at (x, y), sc screen px per mask
 * px, in col (to col2 from row g0 over gh rows when gh > 0), alpha 0..255 - blended, or
 * added (add). Rows are clipped to the band. */
static void lz_mask(const uint8_t *m, int mw, int mh, float x, float y, float sc,
                    uint32_t col, uint32_t col2, float g0, float gh, int alpha, int add){
  if(!m || alpha <= 0 || sc <= 0.001f) return;
  /* at the base size, snap to whole pixels and copy: sharpest, cheapest */
  int exact = sc == 1.0f;
  if(exact){ x = floorf(x + 0.5f); y = floorf(y + 0.5f); }
  float dw = mw * sc, dh = mh * sc;
  int x0 = (int)floorf(x), x1 = (int)ceilf(x + dw), y0 = (int)floorf(y), y1 = (int)ceilf(y + dh);
  if(x0 < 0) x0 = 0;
  if(x1 > FBW) x1 = FBW;
  if(y0 < 0) y0 = 0;
  if(y1 > FBH) y1 = FBH;
  if(x0 >= x1 || !clip_rows(&y0, &y1)) return;
  float inv = 1.0f / sc;
  for(int py = y0; py < y1; py++){
    uint32_t c = col;
    if(gh > 0) c = mixc(col, col2, clampf((py + 0.5f - g0) / gh, 0, 1));
    int cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
    uint32_t *row = fb + (size_t)py * FBW;
    float v = (py + 0.5f - y) * inv - 0.5f;
    for(int px = x0; px < x1; px++){
      int s;
      if(exact){
        int mx = px - (int)x, my = py - (int)y;
        s = ((unsigned)mx < (unsigned)mw && (unsigned)my < (unsigned)mh) ? m[my * mw + mx] : 0;
      } else {
        s = lz_samp(m, mw, mh, (px + 0.5f - x) * inv - 0.5f, v);
      }
      int a = s * alpha / 255;
      if(a <= 0) continue;
      if(add){
        uint32_t d = row[px];
        int r = ((d >> 16) & 255) + (cr * a >> 8), g = ((d >> 8) & 255) + (cg * a >> 8), b = (d & 255) + (cb * a >> 8);
        row[px] = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
      } else {
        int a256 = a + (a >> 7);
        row[px] = a256 >= 256 ? c : blend_px(row[px], c, a256);
      }
    }
  }
}

/* ── text ───────────────────────────────────────────────────────────── */

typedef struct {
  uint32_t color, color2;  int grad;   /* face; top-to-bottom gradient when grad  */
  uint32_t glow;     float glow_k;     /* additive halo (glow fonts), 0 = none    */
  uint32_t shadow;   float shadow_k;   /* soft dark halo / offset copy, 0 = none  */
  uint32_t outline;  float outline_px; /* a dark rim, 0 = none                    */
  float spacing;                       /* extra px between glyphs                 */
  int   align;                         /* LZ_LEFT / LZ_CENTER / LZ_RIGHT          */
  float opacity;                       /* 0 is treated as 1                       */
  const float *gpow;                   /* per glyph 0..1 (flicker), NULL = all 1  */
} lz_style;

LZ_API float lz_width(int f, const char *s, float size, float spacing){
  const lz_font *F = &lzf[f];
  if(!F->ok || !s) return 0;
  float k = size / F->base, w = 0;
  int n = 0;
  for(const unsigned char *p = (const unsigned char *)s; *p; p++, n++) w += lz_glyph_of(F, *p)->adv * k;
  return n ? w + spacing * (n - 1) : 0;
}

/* ── the string cache ────────────────────────────────────────────────
 *  Resampling every glyph every frame is most of what text costs, so a
 *  string is rasterised once - its face, its outline and its glow, as
 *  whole-pixel coverage masks - and after that only copied (lz_mask's
 *  exact path), in whatever colours the frame wants. The least recently
 *  used entry goes. Under the band renderer the lookup, and a miss's
 *  rasterising, happen under bp_lock and the entry is pinned while it is
 *  drawn, as textb()'s cache does. Per-glyph flicker (gpow) is uncached. */
#define LZC_N 96
typedef struct {
  char s[64]; int f; float size, spacing, outline;
  int used, pins;
  int w, h, ox, oy;            /* the masks, and their corner from (pen 0, line top 0) */
  float gradTop, gradH;        /* the letters' rows within the masks, for the gradient */
  uint8_t *face, *out, *glow;  /* out / glow NULL when not wanted / no glow            */
} lz_centry;
static lz_centry lzc[LZC_N];
static int lzc_clock;

/* A mask into an 8-bit buffer at (x, y), scaled, combined by max. */
static void lz_rast(uint8_t *d, int dw, int dh, const uint8_t *m, int mw, int mh, float x, float y, float sc){
  if(!m || sc <= 0.001f) return;
  int x0 = (int)floorf(x), x1 = (int)ceilf(x + mw * sc), y0 = (int)floorf(y), y1 = (int)ceilf(y + mh * sc);
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(x1 > dw) x1 = dw;
  if(y1 > dh) y1 = dh;
  float inv = 1.0f / sc;
  for(int py = y0; py < y1; py++){
    float v = (py + 0.5f - y) * inv - 0.5f;
    uint8_t *row = d + (size_t)py * dw;
    for(int px = x0; px < x1; px++){
      int s = lz_samp(m, mw, mh, (px + 0.5f - x) * inv - 0.5f, v);
      if(s > row[px]) row[px] = (uint8_t)s;
    }
  }
}

static void lz_centry_free(lz_centry *e){
  free(e->face); free(e->out); free(e->glow);
  e->face = e->out = e->glow = NULL;
  e->used = 0;
}

/* Caller holds bp_lock when bands run in parallel. */
static lz_centry *lz_cache_get(int f, const char *s, float size, float spacing, float outline){
  for(int i = 0; i < LZC_N; i++){
    lz_centry *e = &lzc[i];
    if(e->used && e->f == f && e->size == size && e->spacing == spacing && e->outline == outline && !strcmp(e->s, s)){
      e->used = ++lzc_clock;
      return e;
    }
  }
  int slot = -1;
  for(int i = 0; i < LZC_N; i++)
    if(!lzc[i].pins && (slot < 0 || lzc[i].used < lzc[slot].used)) slot = i;
  if(slot < 0) return NULL;
  lz_centry *e = &lzc[slot];
  lz_centry_free(e);
  const lz_font *F = &lzf[f];
  float k = size / F->base, pen = 0, o = outline;
  float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f, ftop = 1e9f, fbot = -1e9f;
  #define LZ_EXT(ax, ay, aw, ah) do { \
      minx = fminf(minx, (ax)); miny = fminf(miny, (ay)); \
      maxx = fmaxf(maxx, (ax) + (aw)); maxy = fmaxf(maxy, (ay) + (ah)); } while(0)
  int glow = 0;
  for(const unsigned char *p = (const unsigned char *)s; *p; p++){
    const lz_glyph *g = lz_glyph_of(F, *p);
    if(g->m){
      float gx = pen + g->ox * k, gy = g->oy * k, gw = g->w * k, gh = g->h * k;
      LZ_EXT(gx, gy, gw, gh);
      if(gy < ftop) ftop = gy;
      if(gy + gh > fbot) fbot = gy + gh;
      if(o > 0) LZ_EXT(gx - o, gy - o * 0.8f, gw + 2 * o, gh + 1.6f * o);
      if(g->gm){ glow = 1; LZ_EXT(pen + g->gox * k, g->goy * k, g->gw * 2 * k, g->gh * 2 * k); }
    }
    pen += g->adv * k + spacing;
  }
  #undef LZ_EXT
  snprintf(e->s, sizeof e->s, "%s", s);
  e->f = f; e->size = size; e->spacing = spacing; e->outline = outline;
  e->used = ++lzc_clock;
  e->w = e->h = 0;
  if(minx > maxx) return e;                                 /* only spaces */
  e->ox = (int)floorf(minx); e->oy = (int)floorf(miny);
  e->w = (int)ceilf(maxx) - e->ox + 1; e->h = (int)ceilf(maxy) - e->oy + 1;
  e->gradTop = ftop - e->oy; e->gradH = fbot - ftop;
  size_t n = (size_t)e->w * e->h;
  e->face = (uint8_t *)calloc(n, 1);
  if(o > 0) e->out = (uint8_t *)calloc(n, 1);
  if(glow) e->glow = (uint8_t *)calloc(n, 1);
  if(!e->face || (o > 0 && !e->out) || (glow && !e->glow)){ lz_centry_free(e); return NULL; }
  pen = 0;
  for(const unsigned char *p = (const unsigned char *)s; *p; p++){
    const lz_glyph *g = lz_glyph_of(F, *p);
    if(g->m){
      float gx = pen + g->ox * k - e->ox, gy = g->oy * k - e->oy;
      lz_rast(e->face, e->w, e->h, g->m, g->w, g->h, gx, gy, k);
      if(o > 0) lz_rast(e->out, e->w, e->h, g->m, g->w, g->h, gx - o, gy - o * 0.8f, (g->w * k + 2 * o) / g->w);
      if(g->gm) lz_rast(e->glow, e->w, e->h, g->gm, g->gw, g->gh, pen + g->gox * k - e->ox, g->goy * k - e->oy, 2 * k);
    }
    pen += g->adv * k + spacing;
  }
  return e;
}

/* lz_text_ex through the cache; 0 if it could not (then draw uncached). */
static int lz_text_cached(int f, const char *s, float x, float y, float size, const lz_style *st, float op){
  if(bp_active) bp_lock();
  lz_centry *e = lz_cache_get(f, s, size, st->spacing, st->outline_px);
  if(e) e->pins++;
  if(bp_active) bp_unlock();
  if(!e) return 0;
  if(e->w > 0){
    float X = floorf(x + 0.5f) + e->ox, Y = floorf(y + 0.5f) + e->oy;
    if(st->shadow_k > 0){
      int a = (int)(255 * st->shadow_k * op);
      if(e->glow) lz_mask(e->glow, e->w, e->h, X, Y + floorf(size * 0.04f + 0.5f), 1.0f, st->shadow, 0, 0, 0, a, 0);
      else lz_mask(e->face, e->w, e->h, X + floorf(size * 0.04f + 0.5f), Y + floorf(size * 0.05f + 0.5f), 1.0f,
                   st->shadow, 0, 0, 0, a, 0);
    }
    if(st->glow_k > 0 && e->glow)
      lz_mask(e->glow, e->w, e->h, X, Y, 1.0f, st->glow, 0, 0, 0, (int)(255 * clampf(st->glow_k * op, 0, 1)), 1);
    if(st->outline_px > 0 && e->out) lz_mask(e->out, e->w, e->h, X, Y, 1.0f, st->outline, 0, 0, 0, (int)(255 * op), 0);
    lz_mask(e->face, e->w, e->h, X, Y, 1.0f, st->color, st->color2, Y + e->gradTop, st->grad ? e->gradH : 0.0f,
            (int)(255 * op), 0);
  }
  if(bp_active) bp_lock();
  e->pins--;
  if(bp_active) bp_unlock();
  return 1;
}

/* The line's top at y; x is its left edge, centre or right edge by align. */
LZ_API void lz_text_ex(int f, const char *s, float x, float y, float size, const lz_style *st){
  const lz_font *F = &lzf[f];
  if(!F->ok || !s || !*s || size < 1) return;
  if(!rows_visible((int)y - (int)(size * 0.4f), (int)(y + size * 1.4f))) return;
  float k = size / F->base, op = st->opacity > 0 ? st->opacity : 1.0f;
  float w = lz_width(f, s, size, st->spacing);
  if(st->align == LZ_CENTER) x -= w * 0.5f;
  else if(st->align == LZ_RIGHT) x -= w;
  if(!st->gpow && strlen(s) < sizeof lzc[0].s && lz_text_cached(f, s, x, y, size, st, op)) return;
  for(int pass = 0; pass < 4; pass++){
    if(pass == 0 && st->shadow_k <= 0) continue;
    if(pass == 1 && st->glow_k <= 0) continue;
    if(pass == 2 && st->outline_px <= 0) continue;
    float pen = x;
    int i = 0;
    for(const unsigned char *p = (const unsigned char *)s; *p; p++, i++){
      const lz_glyph *g = lz_glyph_of(F, *p);
      float al = op * (st->gpow ? st->gpow[i] : 1.0f);
      float gx = pen + g->ox * k, gy = y + g->oy * k;
      if(al > 0.004f){
        switch(pass){
        case 0:
          if(g->gm) lz_mask(g->gm, g->gw, g->gh, pen + g->gox * k, y + g->goy * k + size * 0.04f, 2 * k,
                            st->shadow, 0, 0, 0, (int)(255 * st->shadow_k * al), 0);
          else lz_mask(g->m, g->w, g->h, gx + size * 0.04f, gy + size * 0.05f, k, st->shadow, 0, 0, 0,
                       (int)(255 * st->shadow_k * al), 0);
          break;
        case 1:
          if(g->gm) lz_mask(g->gm, g->gw, g->gh, pen + g->gox * k, y + g->goy * k, 2 * k,
                            st->glow, 0, 0, 0, (int)(255 * clampf(st->glow_k * al, 0, 1)), 1);
          break;
        case 2: {
          /* the glyph a little larger behind the face (as in the poker game:
             one pass instead of eight offset copies) */
          float o = st->outline_px, gw = g->w * k;
          if(gw > 0) lz_mask(g->m, g->w, g->h, gx - o, gy - o * 0.8f, (gw + 2 * o) / g->w, st->outline, 0, 0, 0,
                             (int)(255 * al), 0);
          break;
        }
        default:
          lz_mask(g->m, g->w, g->h, gx, gy, k, st->color, st->color2, gy, st->grad ? g->h * k : 0.0f, (int)(255 * al), 0);
          break;
        }
      }
      pen += g->adv * k + st->spacing;
    }
  }
}

/* Plain text, top at y. */
LZ_API void lz_text(int f, const char *s, float x, float y, float size, uint32_t col, int align){
  lz_style st;
  memset(&st, 0, sizeof st);
  st.color = col;
  st.align = align;
  lz_text_ex(f, s, x, y, size, &st);
}

/* Plain text with a soft dark shadow, for labels over busy art. */
LZ_API void lz_text_sh(int f, const char *s, float x, float y, float size, uint32_t col, int align){
  lz_style st;
  memset(&st, 0, sizeof st);
  st.color = col;
  st.align = align;
  st.shadow = 0x000000;
  st.shadow_k = 0.8f;
  lz_text_ex(f, s, x, y, size, &st);
}

/* A neon sign centred on (cx, cy): the tube colour run pastel-hot at full
 * power, down to unlit glass as power drops (the poker game's text_neon). */
LZ_API void lz_neon_ex(int f, const char *s, float cx, float cy, float size, uint32_t tube, float power,
                       float glow_k, const float *gpow){
  power = clampf(power, 0, 1);
  int tr = (tube >> 16) & 255, tg = (tube >> 8) & 255, tb = tube & 255;
  uint32_t hot = RGB(tr + (int)((255 - tr) * 0.62f), tg + (int)((255 - tg) * 0.62f), tb + (int)((255 - tb) * 0.62f));
  uint32_t off = RGB((int)(tr * 0.28f) + 20, (int)(tg * 0.28f) + 16, (int)(tb * 0.28f) + 22);
  lz_style st;
  memset(&st, 0, sizeof st);
  st.align = LZ_CENTER;
  st.color = mixc(off, hot, power);
  st.glow = tube;
  st.glow_k = glow_k * power;
  st.gpow = gpow;
  lz_text_ex(f, s, cx, cy - size * 0.5f, size, &st);
}
LZ_API void lz_neon(int f, const char *s, float cx, float cy, float size, uint32_t tube, float power, float glow_k){
  lz_neon_ex(f, s, cx, cy, size, tube, power, glow_k, NULL);
}

/* Gold display type centred on (cx, cy): cream to amber, a dark rim, a warm
 * glow (the poker game's text_gold). */
LZ_API void lz_gold(int f, const char *s, float cx, float cy, float size, float opacity, float glow_k){
  lz_style st;
  memset(&st, 0, sizeof st);
  st.align = LZ_CENTER;
  st.opacity = opacity;
  st.glow = 0xFFAA28;
  st.glow_k = glow_k;
  st.outline = 0x5A2808;
  st.outline_px = size * 0.045f;
  st.color = 0xFFF6C4;
  st.color2 = 0xD68016;
  st.grad = 1;
  lz_text_ex(f, s, cx, cy - size * 0.5f, size, &st);
}

/* ── canvases (build time) ─────────────────────────────────────────── */

static int lz_cv_new(LCanvas *cv, int w, int h){
  cv->px = (uint8_t *)calloc((size_t)w * h, 4);
  cv->w = w; cv->h = h; cv->stride = w;
  return cv->px != NULL;
}

/* A premultiplied canvas into a straight-alpha sprite; frees the canvas. */
static void lz_cv_to_spr(LCanvas *cv, spr_t *s){
  memset(s, 0, sizeof *s);
  if(!cv->px) return;
  s->w = cv->w; s->h = cv->h; s->px = cv->px;
  for(int i = 0; i < cv->w * cv->h; i++){
    uint8_t *p = s->px + (size_t)i * 4;
    int a = p[3];
    if(a > 0 && a < 255){
      p[0] = (uint8_t)(p[0] * 255 / a > 255 ? 255 : p[0] * 255 / a);
      p[1] = (uint8_t)(p[1] * 255 / a > 255 ? 255 : p[1] * 255 / a);
      p[2] = (uint8_t)(p[2] * 255 / a > 255 ? 255 : p[2] * 255 / a);
    }
  }
  cv->px = NULL;
  spr_bounds(s);
}

/* A premultiplied canvas composited over fb at (x, y) ("over"). Build time. */
static void lz_cv_to_fb(const LCanvas *cv, int x, int y){
  if(!cv->px) return;
  for(int j = 0; j < cv->h; j++){
    int fy = y + j;
    if(!in_band(fy)) continue;
    for(int i = 0; i < cv->w; i++){
      int fx = x + i;
      if((unsigned)fx >= FBW) continue;
      const uint8_t *p = cv->px + ((size_t)j * cv->stride + i) * 4;
      if(!p[3] && !p[0] && !p[1] && !p[2]) continue;
      uint32_t d = fb[fy * FBW + fx];
      int ia = 255 - p[3];
      int r = p[0] + (((d >> 16) & 255) * ia + 127) / 255, g = p[1] + (((d >> 8) & 255) * ia + 127) / 255,
          b = p[2] + ((d & 255) * ia + 127) / 255;
      fb[fy * FBW + fx] = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    }
  }
}

static inline LCol lz_col(uint32_t c, float a){ return lrc_hex(c, a); }

/* ── the room: the honeycomb backdrop (build time) ─────────────────── */
/* theme 0: the lounge's warm plum; 1: the free-spins night, in blue.    */
LZ_API void lz_paint_room(uint32_t *dst, int theme){
  LCol top = lz_col(theme ? 0x0C1430 : 0x1D1026, 1), bot = lz_col(theme ? 0x04060E : 0x09070B, 1);
  LCol warm = lz_col(theme ? 0x16244A : 0x3A1C2A, 1);
  LCol line = lz_col(theme ? 0x3C8CFF : LZ_HONEY, 1), a1 = lz_col(theme ? LZ_CYAN : LZ_AMBER, 1),
       a2 = lz_col(LZ_MAGENTA, 1);
  for(int y = 0; y < FBH; y++) for(int x = 0; x < FBW; x++){
    float fx = x + 0.5f, fy = y + 0.5f;
    float t = fy / FBH;
    t = t * (1.6f - 0.6f * t);
    LCol c = lrc_mix(top, bot, t);
    float wx = (fx - 640) / 620, wy = (fy - 440) / 420, w = 1 - (wx * wx + wy * wy);
    if(w > 0) c = lrc_mix(c, warm, 0.55f * w * w);
    int q, r;
    float e = lhex_grid_dist(fx, fy, 36.0f, &q, &r);
    float h = lnoise_hash(q, r, 77);
    if(h < 0.08f) c = lrc_mix(c, a1, 0.05f);
    else if(h < 0.10f) c = lrc_mix(c, a2, 0.04f);
    float cov = 1 - e / 1.3f;
    float vx = (fx - 640) / 760, vy = (fy - 360) / 520, vig = 1 - 0.6f * (vx * vx + vy * vy);
    if(cov > 0) c = lrc_mix(c, line, 0.11f * cov * (0.5f + 0.5f * vig));
    if(e > 1.3f && e < 3.0f) c = lrc_scale(c, 0.93f);
    c = lrc_scale(c, vig < 0.25f ? 0.25f : vig);
    float g = (lnoise_hash(x, y, 5) - 0.5f) * 0.012f;
    dst[y * FBW + x] = RGB((int)(clampf(c.r + g, 0, 1) * 255), (int)(clampf(c.g + g, 0, 1) * 255),
                           (int)(clampf(c.b + g, 0, 1) * 255));
  }
}

/* ── glass panels and neon buttons (build time) ───────────────────── */

static uint32_t lz_hot(uint32_t c, float k){
  int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
  return RGB(r + (int)((255 - r) * k), g + (int)((255 - g) * k), b + (int)((255 - b) * k));
}

/* Dark glass with a neon edge and its glow (the poker game's ui_panel).
 * Paints into fb; x, y, w, h is the glass itself, the glow spills 24 px. */
LZ_API void lz_glass(int x, int y, int w, int h, uint32_t neon, float glow_k, float alpha){
  const int m = 26;
  LCanvas cv;
  if(!lz_cv_new(&cv, w + 2 * m, h + 2 * m)) return;
  float hw = w * 0.5f, hh = h * 0.5f, cx = m + hw, cy = m + hh, r = h < 40 ? h * 0.3f : 14;
  LShape box[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw, hh, r }, NULL, 0 } };
  if(glow_k > 0){
    LFillOpt g = { 0 };
    g.outline = 2;
    g.glow = 9;
    g.opacity = clampf(0.8f * glow_k * alpha, 0, 1);
    g.blend = LBL_ADD;
    LPaint p = lpaint_solid(lz_col(neon, 1));
    lcv_fill(&cv, NULL, box, 1, &p, &g);
  }
  LPaint fill = lpaint_linear(lz_col(0x1E1624, 0.88f * alpha), 0, cy - hh, lz_col(0x0C080F, 0.88f * alpha), 0, cy + hh);
  lcv_fill(&cv, NULL, box, 1, &fill, NULL);
  LFillOpt ol = { 0 };
  ol.outline = 3;
  LPaint tube = lpaint_solid(lz_col(lz_hot(neon, 0.3f), alpha));
  lcv_fill(&cv, NULL, box, 1, &tube, &ol);
  lz_cv_to_fb(&cv, x - m, y - m);
  free(cv.px);
}

/* A plain dark glass well (a readout window), no neon. */
LZ_API void lz_well(int x, int y, int w, int h, float r){
  LCanvas cv;
  if(!lz_cv_new(&cv, w, h)) return;
  LShape box[1] = { { LSH_RBOX, LOP_UNION, { w * 0.5f, h * 0.5f, w * 0.5f, h * 0.5f, r }, NULL, 0 } };
  LPaint fill = lpaint_linear(lz_col(0x07050A, 0.92f), 0, 0, lz_col(0x16101C, 0.92f), 0, (float)h);
  lcv_fill(&cv, NULL, box, 1, &fill, NULL);
  LFillOpt ol = { 0 };
  ol.outline = 1.2f;
  LPaint rim = lpaint_solid(lz_col(0x000000, 0.7f));
  lcv_fill(&cv, NULL, box, 1, &rim, &ol);
  lz_cv_to_fb(&cv, x, y);
  free(cv.px);
}

/* ── the panel icon (draw code) ─────────────────────────────────────
 *  The cabinet's panel in miniature - per side two rows of three buttons,
 *  then SELECT and START - h px high with its top left at (x, y), every
 *  position in `lit` (bit per slot, 0..15 = P1 T1..START, P2 T1..START)
 *  lit in col. Returns its width.                                     */
static inline float lz_icon_w(float h){ return h * 2.5f; }
static void lz_dot(float cx, float cy, float r, uint32_t c, int a){
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1), y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
  if(x0 < 0) x0 = 0;
  if(x1 > FBW) x1 = FBW;
  if(y0 < 0) y0 = 0;
  if(y1 > FBH) y1 = FBH;
  if(x0 >= x1 || !clip_rows(&y0, &y1)) return;
  for(int y = y0; y < y1; y++) for(int x = x0; x < x1; x++){
    float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy) - r;
    int k = (int)(clampf(0.5f - d, 0, 1) * a);
    if(k > 0) fb[y * FBW + x] = k >= 255 ? c : blend_px(fb[y * FBW + x], c, k + (k >> 7));
  }
}
static void lz_pill(float cx, float cy, float w, float h, uint32_t c, int a){
  float r = h * 0.5f;
  for(float o = -w * 0.5f + r; o <= w * 0.5f - r + 0.01f; o += r * 0.5f) lz_dot(cx + o, cy, r, c, a);
}
LZ_API float lz_icon(float x, float y, float h, uint32_t lit, uint32_t col, int alpha){
  const float u = h / 3.0f, dr = u * 0.42f, pitch = u * 1.02f, side_w = pitch * 3, gap = h * 2.5f - 2 * side_w;
  if(!rows_visible((int)y - 4, (int)(y + h) + 4)) return h * 2.5f;
  for(int s = 0; s < 16; s++){
    int side = s / 8, k = s % 8, on = (lit >> s) & 1;
    float ox = x + side * (side_w + gap);
    uint32_t c = on ? col : 0x786C7C;
    int a = on ? alpha : alpha * 140 / 255;
    if(k < 6){
      float cx = ox + pitch * ((float)(k % 3) + 0.5f), cy = y + u * ((float)(k / 3) + 0.5f);
      lz_dot(cx, cy, dr * (on ? 1.2f : 1.0f), c, a);
    } else {
      float cx = ox + side_w * (k == 6 ? 0.3f : 0.7f), cy = y + u * 2.55f;
      lz_pill(cx, cy, u * 0.95f * (on ? 1.2f : 1.0f), u * 0.5f * (on ? 1.2f : 1.0f), c, a);
    }
  }
  return h * 2.5f;
}

/* ── sprites baked at init ─────────────────────────────────────────── */
static spr_t lzGlow;          /* soft round light, white (tint it)        */
static spr_t lzHexGlow;       /* soft hexagon light, white                */
static spr_t lzBulb[2];       /* 0 unlit glass, 1 lit                      */
static spr_t lzBee;           /* the mascot, ~72 px wide                   */
static spr_t lzBeeSmall;      /* ~40 px, for the marquee                   */

/* A sprite's alpha, added in one colour at strength k (0..256). */
LZ_API void lz_add_tint(const spr_t *s, int dx, int dy, uint32_t col, int k){
  if(!s->px || k <= 0) return;
  int y0 = dy, y1 = dy + s->h;
  if(y0 < 0) y0 = 0;
  if(y1 > FBH) y1 = FBH;
  if(!clip_rows(&y0, &y1)) return;
  int cr = ((col >> 16) & 255) * k >> 8, cg = ((col >> 8) & 255) * k >> 8, cb = (col & 255) * k >> 8;
  for(int y = y0; y < y1; y++){
    int sy = y - dy;
    int xa = s->rx0 ? s->rx0[sy] : 0, xb = s->rx1 ? s->rx1[sy] : s->w - 1;
    if(dx + xa < 0) xa = -dx;
    if(dx + xb >= FBW) xb = FBW - 1 - dx;
    const uint8_t *row = s->px + (size_t)sy * s->w * 4;
    uint32_t *d = fb + (size_t)y * FBW + dx;
    for(int x = xa; x <= xb; x++){
      int a = row[x * 4 + 3];
      if(!a) continue;
      uint32_t o = d[x];
      int r = ((o >> 16) & 255) + (cr * a >> 8), g = ((o >> 8) & 255) + (cg * a >> 8), b = (o & 255) + (cb * a >> 8);
      d[x] = RGB(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b);
    }
  }
}

static void lz_paint_glow(LCanvas *cv, float sigma){
  for(int y = 0; y < cv->h; y++) for(int x = 0; x < cv->w; x++){
    float dx = x + 0.5f - cv->w * 0.5f, dy = y + 0.5f - cv->h * 0.5f, r = sqrtf(dx * dx + dy * dy);
    float e = 1 - r / (cv->w * 0.5f);
    float a = expf(-r * r / (2 * sigma * sigma)) * (e <= 0 ? 0 : e < 0.2f ? e / 0.2f : 1);
    uint8_t *p = cv->px + ((size_t)y * cv->stride + x) * 4;
    p[0] = p[1] = p[2] = p[3] = (uint8_t)(clampf(a, 0, 1) * 255);
  }
}

static void lz_bake_sprites(void){
  LCanvas cv;
  if(lz_cv_new(&cv, 64, 64)){ lz_paint_glow(&cv, 11); lz_cv_to_spr(&cv, &lzGlow); }
  if(lz_cv_new(&cv, 96, 96)){
    LShape h[1] = { { LSH_HEX, LOP_UNION, { 48, 48, 30, 1 }, NULL, 0 } };
    LFillOpt g = { 0 };
    g.glow = 7;
    g.opacity = 0.9f;
    LPaint p = lpaint_radial(lrc(1, 1, 1, 0.5f), 48, 48, lrc(1, 1, 1, 0.9f), 30);
    lcv_fill(&cv, NULL, h, 1, &p, &g);
    lz_cv_to_spr(&cv, &lzHexGlow);
  }
  if(lz_cv_new(&cv, 28, 28)){
    LShape c[1] = { { LSH_CIRCLE, LOP_UNION, { 14, 14, 7.5f }, NULL, 0 } };
    LPaint p = lpaint_radial(lz_col(0x6A4A2A, 1), 12, 11, lz_col(0x241408, 1), 9);
    lcv_fill(&cv, NULL, c, 1, &p, NULL);
    LFillOpt o = { 0 };
    o.outline = 1.2f;
    LPaint rim = lpaint_solid(lz_col(0xB08A40, 0.8f));
    lcv_fill(&cv, NULL, c, 1, &rim, &o);
    LShape hl[1] = { { LSH_CIRCLE, LOP_UNION, { 11.5f, 11, 1.8f }, NULL, 0 } };
    LPaint hp = lpaint_solid(lrc(1, 1, 1, 0.45f));
    lcv_fill(&cv, NULL, hl, 1, &hp, NULL);
    lz_cv_to_spr(&cv, &lzBulb[0]);
  }
  if(lz_cv_new(&cv, 28, 28)){
    LShape c[1] = { { LSH_CIRCLE, LOP_UNION, { 14, 14, 6.5f }, NULL, 0 } };
    LFillOpt g = { 0 };
    g.glow = 3.2f;
    LPaint p = lpaint_radial(lrc(1, 1, 0.95f, 1), 13, 13, lz_col(0xFFD070, 1), 7);
    lcv_fill(&cv, NULL, c, 1, &p, &g);
    lz_cv_to_spr(&cv, &lzBulb[1]);
  }
  if(lz_cv_new(&cv, 132, 124)){ lart_bee(&cv, 66, 62, 48, -0.08f, 1.0f, 1); lz_cv_to_spr(&cv, &lzBee); }
  if(lz_cv_new(&cv, 74, 70)){ lart_bee(&cv, 37, 35, 27, -0.08f, 1.0f, 1); lz_cv_to_spr(&cv, &lzBeeSmall); }
}

/* ── bulbs (draw code) ──────────────────────────────────────────────
 *  A row of marquee bulbs from x0 to x1 at y, every `step` px; the chase
 *  pattern comes from t (seconds) and never from state.                */
LZ_API void lz_bulbs_row(int x0, int x1, int y, int step, float t, int phase){
  if(!rows_visible(y - 14, y + 14)) return;
  int n = (x1 - x0) / step + 1, mode = ((int)(t / 5.0f)) % 3;
  float mt = fmodf(t, 5.0f);
  for(int i = 0; i < n; i++){
    int j = i + phase;
    float v;
    if(mode == 0) v = 0.5f + 0.5f * sinf(t * 6.0f - j * 0.6f);
    else if(mode == 1) v = ((((int)(t * 4.0f)) + j) & 1) ? 1.0f : 0.15f;
    else v = (i <= (int)(mt / 4.0f * (n + 1))) ? 1.0f : 0.2f;
    int x = x0 + i * step;
    blit(&lzBulb[0], x - 14, y - 14, 0, FBH, 255, 0, 0.0f);
    int k = (int)(clampf(v, 0, 1) * 255);
    if(k > 20) blit(&lzBulb[1], x - 14, y - 14, 0, FBH, k, 0, 0.0f);
    if(k > 60) lz_add_tint(&lzGlow, x - 32, y - 32, 0xFFB040, k / 5);
  }
}

/* ── init / free ──────────────────────────────────────────────────── */
/* lzReady is declared with text(), which needs it first. */
static void lz_init(void){
  if(lzReady) return;
  for(int f = 0; f < LZF_COUNT; f++) lz_bake_font(f);
  lz_bake_sprites();
  lzReady = 1;
}
static void lz_free(void){
  for(int i = 0; i < LZC_N; i++){ lz_centry_free(&lzc[i]); lzc[i].pins = 0; }
  for(int f = 0; f < LZF_COUNT; f++){
    for(int c = 0; c < 95; c++){ free(lzf[f].g[c].m); free(lzf[f].g[c].gm); }
    memset(&lzf[f], 0, sizeof lzf[f]);
  }
  spr_t *all[] = { &lzGlow, &lzHexGlow, &lzBulb[0], &lzBulb[1], &lzBee, &lzBeeSmall };
  for(size_t i = 0; i < sizeof all / sizeof all[0]; i++){
    free(all[i]->px); free(all[i]->rx0); free(all[i]->rx1);
    memset(all[i], 0, sizeof *all[i]);
  }
  lzReady = 0;
}

/* ── the old type, re-set in the lounge fonts ──────────────────────
 *  The game's own type calls keep their signatures and metrics - px is
 *  the size of a block of the old 5x7 font, so capitals stand 7*px tall
 *  with their tops at y - and now set the poker game's faces:
 *    text(), text_run()      Barlow Condensed (the UI face)
 *    textb(), bake_title()   Bungee (the display face), through the
 *                            existing gradient / outline / bevel shading
 *  Everything stays in capitals, as the old font was.                 */

static void lz_upper(char *d, size_t n, const char *s){
  size_t i = 0;
  for(; s[i] && i + 1 < n; i++) d[i] = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
  d[i] = 0;
}

/* The UI face for caps of height capH: the baked size nearest above. */
static int lz_ui_font(float capH){
  if(capH <= lzf[LZF_UI_S].capH * 1.05f) return LZF_UI_S;
  if(capH <= lzf[LZF_UI_M].capH * 1.1f) return LZF_UI_M;
  return LZF_UI_L;
}
static int lz_disp_font(float capH){
  return capH <= lzf[LZF_DISP_M].capH * 1.1f ? LZF_DISP_M : LZF_DISP_L;
}

/* text() in Barlow: caps 7*px tall (8 at px 1, where 7 is hard to read),
 * fitted to the screen when fit is set, as the old text() was. */
static void lz_compat_text(const char *s, int x, int y, int px, uint32_t col, int align, int shadow, int fit){
  if(!lzReady || !s || !*s) return;
  char u[512];
  lz_upper(u, sizeof u, s);
  float capH = px <= 1 ? 8.0f : 7.0f * px;
  int f = lz_ui_font(capH);
  const lz_font *F = &lzf[f];
  float size = capH / F->capH * F->base;
  if(fit) while(size > 8 && lz_width(f, u, size, 0) > FBW - 10) size *= 0.92f;
  float k = size / F->base;
  if(!rows_visible(y - 4, y + (int)(size * 1.2f))) return;
  lz_style st;
  memset(&st, 0, sizeof st);
  st.color = col;
  st.align = align == 1 ? LZ_CENTER : align == 2 ? LZ_RIGHT : LZ_LEFT;
  if(shadow){ st.shadow = 0x000000; st.shadow_k = 0.85f; }
  lz_text_ex(f, u, (float)x, y - F->capTop * k, size, &st);
}

/* The width of a caption in the display face at cap height capH. */
static float lz_caption_width(const char *s, float capH){
  char u[256];
  lz_upper(u, sizeof u, s);
  int f = lz_disp_font(capH);
  return lz_width(f, u, capH / lzf[f].capH * lzf[f].base, 0);
}

/* A caption's coverage in the display face into mask m (w x h, cleared by
 * the caller): caps capH tall, their top at row `top`, the pen starting at
 * column `left`. Glyphs combine by max, so overlaps do not saturate. */
static void lz_caption_mask(const char *s, float capH, uint8_t *m, int w, int h, float left, float top){
  char u[256];
  lz_upper(u, sizeof u, s);
  int f = lz_disp_font(capH);
  const lz_font *F = &lzf[f];
  if(!F->ok) return;
  float k = capH / F->capH, inv = 1.0f / k, pen = left;
  float ytop = top - F->capTop * k;
  for(const unsigned char *p = (const unsigned char *)u; *p; p++){
    const lz_glyph *g = lz_glyph_of(F, *p);
    if(g->m){
      float gx = pen + g->ox * k, gy = ytop + g->oy * k;
      int x0 = (int)floorf(gx), x1 = (int)ceilf(gx + g->w * k), y0 = (int)floorf(gy), y1 = (int)ceilf(gy + g->h * k);
      if(x0 < 0) x0 = 0;
      if(y0 < 0) y0 = 0;
      if(x1 > w) x1 = w;
      if(y1 > h) y1 = h;
      for(int y = y0; y < y1; y++){
        float v = (y + 0.5f - gy) * inv - 0.5f;
        for(int x = x0; x < x1; x++){
          int s2 = lz_samp(g->m, g->w, g->h, (x + 0.5f - gx) * inv - 0.5f, v);
          uint8_t *d = m + (size_t)y * w + x;
          if(s2 > *d) *d = (uint8_t)s2;
        }
      }
    }
    pen += g->adv * k;
  }
}

/* dst = src grown by r px (a round max filter), for outlines. */
static void lz_dilate(const uint8_t *src, uint8_t *dst, int w, int h, float r){
  int R = (int)ceilf(r);
  for(int y = 0; y < h; y++) for(int x = 0; x < w; x++){
    int best = 0;
    for(int j = -R; j <= R && best < 255; j++){
      int yy = y + j;
      if((unsigned)yy >= (unsigned)h) continue;
      for(int i = -R; i <= R; i++){
        int xx = x + i;
        if((unsigned)xx >= (unsigned)w) continue;
        float d = sqrtf((float)(i * i + j * j)) - r;
        if(d >= 1.0f) continue;
        int v = src[(size_t)yy * w + xx];
        if(d > 0) v = (int)(v * (1.0f - d));
        if(v > best) best = v;
      }
    }
    dst[(size_t)y * w + x] = (uint8_t)best;
  }
}

/* ── readouts (draw code) ─────────────────────────────────────────── */
/* A number in the display face, right-aligned at rx with its caps' top at
 * y and h tall, in col: pale at the top running to col, a dark rim and a
 * little of its own light (the poker game's meters). Returns its width.
 * seg_num() draws this once the fonts are baked. */
static int lz_readout(long long v, int rx, int y, float h, uint32_t col, int commas){
  char raw[24], s[32];
  snprintf(raw, sizeof raw, "%lld", v < 0 ? 0 : v);
  int n = (int)strlen(raw), o = 0;
  for(int i = 0; i < n && o < (int)sizeof s - 2; i++){
    s[o++] = raw[i];
    int left = n - 1 - i;
    if(commas && left > 0 && left % 3 == 0) s[o++] = ',';
  }
  s[o] = 0;
  int f = lz_disp_font(h);
  const lz_font *F = &lzf[f];
  float size = h / F->capH * F->base, k = size / F->base;
  lz_style st;
  memset(&st, 0, sizeof st);
  st.align = LZ_RIGHT;
  st.color = mixc(col, 0xFFFFFF, 0.55f);
  st.color2 = col;
  st.grad = 1;
  st.glow = col;
  st.glow_k = 0.35f;
  st.outline = 0x000000;
  st.outline_px = size * 0.035f;
  lz_text_ex(f, s, (float)rx, y - F->capTop * k, size, &st);
  return (int)lz_width(f, s, size, 0);
}

/* ── the neon deck button and the SPIN button (build time) ─────────── */

/* A neon button (the poker game's ui_button_draw) painted into fb: a dark
 * body lit from below in its colour, a sheen, the tube round the edge and
 * its glow - brighter, and seated 2 px lower, when lit. */
LZ_API void lz_button(int x, int y, int w, int h, uint32_t neon, int lit){
  const int m = 30;
  LCanvas cv;
  if(!lz_cv_new(&cv, w + 2 * m, h + 2 * m)) return;
  float dy = lit ? 2.0f : 0.0f, hw = w * 0.5f, hh = h * 0.5f, cx = m + hw, cy = m + hh + dy, r = 14;
  LShape box[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw, hh, r }, NULL, 0 } };
  LShape sh[1] = { { LSH_RBOX, LOP_UNION, { m + hw, m + hh + 6, hw, hh, r }, NULL, 0 } };
  LFillOpt so = { 0 };
  so.feather = 12;
  LPaint black = lpaint_solid(lrc(0, 0, 0, 0.6f));
  lcv_fill(&cv, NULL, sh, 1, &black, &so);
  LFillOpt g = { 0 };
  g.outline = 2;
  g.glow = lit ? 13 : 8;
  g.opacity = lit ? 1.0f : 0.5f;
  g.blend = LBL_ADD;
  LPaint np = lpaint_solid(lz_col(neon, 1));
  lcv_fill(&cv, NULL, box, 1, &np, &g);
  LCol top = lz_col(0x26182C, 1), bot = lrc_mix(lz_col(0x0C080E, 1), lz_col(neon, 1), lit ? 0.72f : 0.32f);
  LPaint face = lpaint_linear(top, 0, cy - hh, bot, 0, cy + hh);
  lcv_fill(&cv, NULL, box, 1, &face, NULL);
  LShape shn[1] = { { LSH_RBOX, LOP_UNION, { cx, cy - hh * 0.52f, hw - 5, hh * 0.44f, r - 5 }, NULL, 0 } };
  LPaint sp = lpaint_linear(lrc(1, 1, 1, lit ? 0.22f : 0.13f), 0, cy - hh, lrc(1, 1, 1, 0.02f), 0, cy);
  lcv_fill(&cv, NULL, shn, 1, &sp, NULL);
  LFillOpt ol = { 0 };
  ol.outline = 3;
  LPaint tp = lpaint_solid(lz_col(lz_hot(neon, lit ? 0.6f : 0.25f), 1));
  lcv_fill(&cv, NULL, box, 1, &tp, &ol);
  lz_cv_to_fb(&cv, x - m, y - m);
  free(cv.px);
}

/* The round SPIN button as a sprite, r px radius, centred in it: a gold
 * tube on dark glass, lit from inside when pressed, with its own glow. */
LZ_API void lz_bake_dome(spr_t *out, int r, uint32_t neon, int pressed){
  const int m = 30, sz = 2 * (r + m);
  LCanvas cv;
  memset(out, 0, sizeof *out);
  if(!lz_cv_new(&cv, sz, sz)) return;
  float c = sz * 0.5f, dy = pressed ? 2.0f : 0.0f;
  LShape disc[1] = { { LSH_CIRCLE, LOP_UNION, { c, c + dy, (float)r }, NULL, 0 } };
  LShape sh[1] = { { LSH_CIRCLE, LOP_UNION, { c, c + 6, (float)r }, NULL, 0 } };
  LFillOpt so = { 0 };
  so.feather = 12;
  LPaint black = lpaint_solid(lrc(0, 0, 0, 0.65f));
  lcv_fill(&cv, NULL, sh, 1, &black, &so);
  LFillOpt g = { 0 };
  g.outline = 3;
  g.glow = pressed ? 16 : 11;
  g.opacity = pressed ? 1.0f : 0.8f;
  g.blend = LBL_ADD;
  LPaint np = lpaint_solid(lz_col(neon, 1));
  lcv_fill(&cv, NULL, disc, 1, &np, &g);
  LPaint face = lpaint_radial(lrc_mix(lz_col(0x2A1A10, 1), lz_col(neon, 1), pressed ? 0.85f : 0.45f), c, c + dy + r * 0.35f,
                              lz_col(0x0C080E, 1), (float)r * 1.1f);
  lcv_fill(&cv, NULL, disc, 1, &face, NULL);
  LShape shn[1] = { { LSH_ELLIPSE, LOP_UNION, { c, c + dy - r * 0.45f, r * 0.62f, r * 0.34f }, NULL, 0 } };
  LPaint sp = lpaint_linear(lrc(1, 1, 1, 0.20f), 0, c - r, lrc(1, 1, 1, 0.0f), 0, c - r * 0.1f);
  lcv_fill(&cv, NULL, shn, 1, &sp, NULL);
  LFillOpt ol = { 0 };
  ol.outline = 4;
  LPaint tp = lpaint_solid(lz_col(lz_hot(neon, pressed ? 0.6f : 0.3f), 1));
  lcv_fill(&cv, NULL, disc, 1, &tp, &ol);
  lz_cv_to_spr(&cv, out);
}

/* ── the reel window's frame (build time) ─────────────────────────── */
/* Brushed brass round the window, a dark lip inside it, a thin magenta
 * tube along the lip and honey light spilling outside - the poker table's
 * rail, in the slot's shape. x, y, w, h is the window itself.          */
LZ_API void lz_reel_frame(int x, int y, int w, int h){
  const int m = 44;
  LCanvas cv;
  if(!lz_cv_new(&cv, w + 2 * m, h + 2 * m)) return;
  float cx = m + w * 0.5f, cy = m + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
  LShape outer[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw + 20, hh + 20, 26 }, NULL, 0 } };
  LFillOpt glow = { 0 };
  glow.outline = 3;
  glow.glow = 16;
  glow.opacity = 0.75f;
  glow.blend = LBL_ADD;
  LPaint honey = lpaint_solid(lz_col(LZ_HONEY, 1));
  lcv_fill(&cv, NULL, outer, 1, &honey, &glow);
  LShape ring[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw + 13, hh + 13, 20 }, NULL, 0 } };
  LFillOpt band = { 0 };
  band.outline = 14;
  LBrassParams bp = { 0.35f, 57, 0.7f };
  LPaint brass = lpaint_fn(lart_brass, &bp);
  lcv_fill(&cv, NULL, ring, 1, &brass, &band);
  LShape lip[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw + 4, hh + 4, 12 }, NULL, 0 } };
  LFillOpt lo = { 0 };
  lo.outline = 6;
  LPaint dark = lpaint_solid(lz_col(0x0A0608, 1));
  lcv_fill(&cv, NULL, lip, 1, &dark, &lo);
  LShape tube[1] = { { LSH_RBOX, LOP_UNION, { cx, cy, hw + 2, hh + 2, 10 }, NULL, 0 } };
  LFillOpt to = { 0 };
  to.outline = 1.6f;
  to.glow = 4;
  LPaint mag = lpaint_solid(lz_col(lz_hot(LZ_MAGENTA, 0.35f), 1));
  lcv_fill(&cv, NULL, tube, 1, &mag, &to);
  lz_cv_to_fb(&cv, x - m, y - m);
  free(cv.px);
}
