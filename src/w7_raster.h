/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md */
/* w7_raster.h - the CPU vector rasteriser for the lounge art, ported from
 * Beese's Poker Lounge (render/raster.h), where it draws every generated
 * image at start-up.
 *
 * Shapes are signed distance functions (SDFs) evaluated per pixel over the
 * shape's bounding box. Coverage comes from the distance itself
 * (alpha = clamp(0.5 - d, 0, 1) with d in pixels), which gives the same
 * edge quality as 4x4 supersampling at a sixteenth of the work, and stays
 * exact under rotation and scale. Everything is painted with the
 * premultiplied "over" operator into RGBA8 canvases, which may be views into
 * a larger atlas page (stride), so many threads can paint disjoint parts of
 * one page at once. Nothing here touches raylib or the GPU.
 *
 * Coordinates: canvas pixels, y down, pixel centres at +0.5. A shape is
 * described in its own local units and placed with an affine transform. */
#ifndef W7_RASTER_H
#define W7_RASTER_H

#include <stdint.h>

typedef struct { float r, g, b, a; } LCol;          /* straight alpha, 0..1 */

typedef struct {
    uint8_t *px;         /* RGBA8, premultiplied alpha */
    int      w, h;
    int      stride;     /* in pixels */
} LCanvas;

/* local -> canvas: X = ax*x + bx*y + tx, Y = ay*x + by*y + ty */
typedef struct { float ax, bx, tx, ay, by, ty; } LXf;

LXf   lxf_identity(void);
LXf   lxf_make(float x, float y, float scale, float rot_rad);   /* scale, rotate, then translate */
LXf   lxf_scale2(float x, float y, float sx, float sy, float rot_rad);
LXf   lxf_mul(LXf outer, LXf inner);                               /* outer(inner(p)) */
LXf   lxf_rot180_about(float cx, float cy);                      /* point reflection */

typedef enum {
    LSH_CIRCLE,      /* p0,p1 centre, p2 radius                                  */
    LSH_ELLIPSE,     /* p0,p1 centre, p2,p3 radii                                */
    LSH_RBOX,        /* p0,p1 centre, p2,p3 half size, p4 corner radius          */
    LSH_POLY,        /* pts[npts]                                                */
    LSH_SEG,         /* p0,p1 -> p2,p3, p4 half width (a capsule)                */
    LSH_RING,        /* p0,p1 centre, p2 radius, p3 half width                   */
    LSH_HEART,       /* p0,p1 centre, p2 size (height ~2*size), point down; p3 != 0: point up */
    LSH_HEX,         /* p0,p1 centre, p2 radius (flat top when p3 = 0, pointy when 1) */
    LSH_ARC,         /* p0,p1 centre, p2 radius, p3 half width, p4 mid angle, p5 half aperture */
    LSH_TRI          /* p0..p5 the three corners                                 */
} LShapeType;

typedef enum {
    LOP_UNION,       /* min(d, s)                          */
    LOP_SUB,         /* max(d, -s): cut this shape away   */
    LOP_INTER,       /* max(d, s)                          */
    LOP_SMOOTH       /* smooth union, p7 = blend radius    */
} LShapeOp;

typedef struct {
    uint8_t type, op;
    float   p[8];
    const float *pts;    /* LSH_POLY: x0,y0,x1,y1,... */
    int     npts;
} LShape;

/* Distance in local units to the union/cut chain of n shapes (first op ignored). */
float lshape_sdf(const LShape *s, int n, float x, float y);

typedef enum { LPAINT_SOLID, LPAINT_LINEAR, LPAINT_RADIAL, LPAINT_FN } LPaintType;

typedef LCol (*LPaintFn)(float lx, float ly, float d_px, const void *user);

typedef struct {
    uint8_t type;
    LCol    c0, c1;
    float   x0, y0, x1, y1;   /* LINEAR: c0 at (x0,y0) .. c1 at (x1,y1); RADIAL: centre x0,y0, radius x1 */
    LPaintFn fn;               /* FN: colour at a local point (d_px = pixel distance to the edge) */
    const void *user;
} LPaint;

LPaint lpaint_solid(LCol c);
LPaint lpaint_linear(LCol c0, float x0, float y0, LCol c1, float x1, float y1);
LPaint lpaint_radial(LCol c0, float cx, float cy, LCol c1, float r);
LPaint lpaint_fn(LPaintFn fn, const void *user);

typedef enum { LBL_OVER, LBL_ADD, LBL_ERASE } LBlendOp;

typedef struct {
    float   outline;     /* > 0: stroke of this width (pixels) centred on the edge instead of a fill */
    float   offset;      /* grow (+) or shrink (-) the shape by this many pixels                   */
    float   feather;     /* extra edge softness in pixels (0 = crisp)                               */
    float   glow;        /* > 0: an exponential halo of this falloff (pixels) outside the shape      */
    float   opacity;     /* multiplies everything; 0 is treated as 1                                */
    uint8_t blend;       /* LBlendOp */
} LFillOpt;

/* Paints the shape chain. opt may be NULL (crisp fill, over). */
void lcv_fill(LCanvas *cv, const LXf *xf, const LShape *s, int n, const LPaint *paint, const LFillOpt *opt);

/* A rectangle of solid paint with no shape (fast path for backgrounds). */
void lcv_clear(LCanvas *cv, LCol c);
void lcv_fill_rect(LCanvas *cv, int x, int y, int w, int h, const LPaint *paint);

/* Paints an 8-bit coverage bitmap (a font glyph) through xf (which maps the
 * bitmap's pixel space to the canvas), bilinear, tinted by paint. */
void lcv_blit_alpha(LCanvas *cv, const LXf *xf, const uint8_t *alpha, int aw, int ah, const LPaint *paint,
                   float opacity);

/* Separable box blur, 3 passes (close to a gaussian of sigma ~ radius),
 * on all four channels, zero outside the canvas. Allocates a float copy of
 * the canvas for the duration (start-up only). */
void lcv_blur(LCanvas *cv, int radius);

/* 2x2 box downsample of src into dst (dst is src/2). */
void lcv_downsample2(const LCanvas *src, LCanvas *dst);

/* Colour helpers. */
LCol lrc(float r, float g, float b, float a);
LCol lrc_hex(uint32_t rgb, float a);            /* 0xRRGGBB */
LCol lrc_mix(LCol a, LCol b, float t);
LCol lrc_scale(LCol a, float k);                /* rgb * k */

/* Deterministic hash noise for procedural texture (brushed metal, grain). */
float lnoise_hash(int x, int y, uint32_t seed);          /* 0..1 */
float lnoise_value(float x, float y, uint32_t seed);     /* smooth 0..1 */

/* Honeycomb helper: distance (local units) from p to the nearest hex cell
 * edge, for a hex grid of cell radius r; also returns the cell id. */
float lhex_grid_dist(float x, float y, float r, int *cell_q, int *cell_r);

#endif
