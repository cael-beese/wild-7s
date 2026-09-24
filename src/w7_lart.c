/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md */
/* w7_lart.c - see w7_lart.h. */
#include "w7_lart.h"

#include <math.h>
#include <string.h>



static const float lk_diamond[] = { 0, -1, 0.40f, -0.42f, 0.76f, 0, 0.40f, 0.42f, 0, 1, -0.40f, 0.42f, -0.76f, 0, -0.40f, -0.42f };
static const float lk_stem[] = { -0.07f, 0.18f, 0.07f, 0.18f, 0.20f, 0.78f, 0.36f, 0.98f, -0.36f, 0.98f, -0.20f, 0.78f };

static int lsuit_shapes(int suit, LShape *s)
{
    memset(s, 0, sizeof(LShape) * 6);
    switch (suit) {
    case LSUIT_H:
        s[0] = (LShape){ LSH_HEART, LOP_UNION, { 0, 0.02f, 1.0f, 0 }, NULL, 0 };
        return 1;
    case LSUIT_D:
        s[0] = (LShape){ LSH_POLY, LOP_UNION, { 0 }, lk_diamond, 8 };
        return 1;
    case LSUIT_S:
        s[0] = (LShape){ LSH_HEART, LOP_UNION, { 0, -0.16f, 0.84f, 1 }, NULL, 0 };
        s[1] = (LShape){ LSH_POLY, LOP_SMOOTH, { 0, 0, 0, 0, 0, 0, 0, 0.12f }, lk_stem, 6 };
        return 2;
    default: /* clubs */
        s[0] = (LShape){ LSH_CIRCLE, LOP_UNION, { 0, -0.50f, 0.38f }, NULL, 0 };
        s[1] = (LShape){ LSH_CIRCLE, LOP_SMOOTH, { -0.46f, 0.12f, 0.38f, 0, 0, 0, 0, 0.08f }, NULL, 0 };
        s[2] = (LShape){ LSH_CIRCLE, LOP_SMOOTH, { 0.46f, 0.12f, 0.38f, 0, 0, 0, 0, 0.08f }, NULL, 0 };
        s[3] = (LShape){ LSH_CIRCLE, LOP_SMOOTH, { 0, 0.0f, 0.22f, 0, 0, 0, 0, 0.1f }, NULL, 0 };
        s[4] = (LShape){ LSH_POLY, LOP_SMOOTH, { 0, 0, 0, 0, 0, 0, 0, 0.10f }, lk_stem, 6 };
        return 5;
    }
}

void lart_suit_colors(int suit, LCol *top, LCol *bottom)
{
    if (suit == LSUIT_H || suit == LSUIT_D) {
        *top = lrc_hex(0xE8264F, 1);
        *bottom = lrc_hex(0xA80E36, 1);
    } else {
        *top = lrc_hex(0x3A3346, 1);
        *bottom = lrc_hex(0x141118, 1);
    }
}

void lart_pip(LCanvas *cv, int suit, float cx, float cy, float size, float rot, LCol top, LCol bottom,
             const LFillOpt *opt)
{
    LShape s[6];
    int n = lsuit_shapes(suit, s);
    LXf xf = lxf_make(cx, cy, size, rot);
    LPaint p = lpaint_linear(top, 0, -1, bottom, 0, 1);
    lcv_fill(cv, &xf, s, n, &p, opt);
}

/* ---- paints ------------------------------------------------------------- */

LCol lart_brass(float lx, float ly, float d, const void *user)
{
    const LBrassParams *bp = user;
    float c = cosf(bp->angle), s = sinf(bp->angle);
    float u = lx * c + ly * s, v = -lx * s + ly * c;
    /* Brushing: fine streaks along u, from 1-D noise across v. */
    float streak = 0.55f * lnoise_value(v * 0.9f, u * 0.02f, bp->seed) + 0.45f * lnoise_value(v * 3.1f, u * 0.05f, bp->seed + 7);
    float band = 0.5f + 0.5f * sinf(u * 0.035f + v * 0.05f);
    LCol dark = lrc_hex(0x8A5E1C, 1), mid = lrc_hex(0xC9982F, 1), light = lrc_hex(0xF6DC8A, 1);
    float t = 0.35f * streak + 0.45f * band + 0.2f * bp->light;
    LCol col = t < 0.5f ? lrc_mix(dark, mid, t * 2) : lrc_mix(mid, light, (t - 0.5f) * 2);
    /* Bevel: brighter right at the outer edge, darker a little inside. */
    float e = d < 0 ? -d : d;
    if (e < 1.5f) col = lrc_mix(col, light, 0.35f * (1.5f - e) / 1.5f);
    return col;
}

LCol lart_honeycomb(float lx, float ly, float d, const void *user)
{
    (void)d;
    const LHoneyParams *hp = user;
    int q, r;
    float e = lhex_grid_dist(lx, ly, hp->r, &q, &r);
    LCol c = hp->base;
    if (hp->cell_prob > 0 && lnoise_hash(q, r, hp->seed) < hp->cell_prob) {
        float k = 0.5f + 0.5f * lnoise_hash(q, r, hp->seed + 1);
        c = lrc_mix(c, hp->cell, k);
    }
    float cov = 1.0f - e / hp->line_w;
    if (cov > 0) c = lrc_mix(c, hp->line, cov > 1 ? 1 : cov);
    return c;
}

/* ---- the bee ------------------------------------------------------------ */

typedef struct { float alpha; } LBeeBody;

static LCol lbee_body_paint(float lx, float ly, float d, const void *user)
{
    const LBeeBody *b = user;
    (void)d;
    LCol top = lrc_hex(0xFFD85C, b->alpha), bot = lrc_hex(0xE0850E, b->alpha);
    LCol c = lrc_mix(top, bot, (ly + 0.3f) / 0.8f < 0 ? 0 : (ly + 0.3f) / 0.8f > 1 ? 1 : (ly + 0.3f) / 0.8f);
    /* Three dark bands across the abdomen, softened at their edges. */
    float u = lx - 0.02f;
    float ph = fmodf(u + 3.4f, 0.34f);
    float band = 0;
    if (u > -0.22f) band = ph < 0.13f ? 1.0f : 0.0f;
    if (band > 0) c = lrc_mix(c, lrc_hex(LART_BROWN, b->alpha), 0.92f);
    /* A soft highlight on the upper body. */
    float hx = lx - 0.05f, hy = ly + 0.18f;
    float h = 1.0f - (hx * hx / 0.09f + hy * hy / 0.012f);
    if (h > 0) c = lrc_mix(c, lrc_hex(0xFFF6C8, b->alpha), 0.55f * h);
    return c;
}

void lart_bee(LCanvas *cv, float cx, float cy, float scale, float rot, float alpha, int outline)
{
    LXf xf = lxf_make(cx, cy, scale, rot);
    LCol ink = lrc_hex(LART_BROWN, alpha);
    LFillOpt ol = { 0 };
    ol.offset = fmaxf(1.2f, scale * 0.06f);

    LShape wing1[1] = { { LSH_ELLIPSE, LOP_UNION, { 0.10f, -0.52f, 0.26f, 0.44f }, NULL, 0 } };
    LShape wing2[1] = { { LSH_ELLIPSE, LOP_UNION, { 0.44f, -0.44f, 0.22f, 0.38f }, NULL, 0 } };
    static const float sting[] = { 0.70f, 0.02f, 1.0f, 0.18f, 0.70f, 0.30f };
    LShape body[2] = {
        { LSH_ELLIPSE, LOP_UNION, { 0.18f, 0.14f, 0.58f, 0.42f }, NULL, 0 },
        { LSH_POLY, LOP_SMOOTH, { 0, 0, 0, 0, 0, 0, 0, 0.06f }, sting, 3 },
    };
    LShape head[1] = { { LSH_CIRCLE, LOP_UNION, { -0.50f, 0.04f, 0.31f }, NULL, 0 } };
    LShape ant[4] = {
        { LSH_SEG, LOP_UNION, { -0.60f, -0.22f, -0.80f, -0.62f, 0.035f }, NULL, 0 },
        { LSH_CIRCLE, LOP_UNION, { -0.82f, -0.66f, 0.075f }, NULL, 0 },
        { LSH_SEG, LOP_UNION, { -0.42f, -0.25f, -0.40f, -0.68f, 0.035f }, NULL, 0 },
        { LSH_CIRCLE, LOP_UNION, { -0.40f, -0.72f, 0.075f }, NULL, 0 },
    };

    /* Wings sit behind the body; each gets its own tilt. */
    LXf w1 = lxf_mul(xf, lxf_make(0.10f, -0.52f, 1, -0.45f)), w2 = lxf_mul(xf, lxf_make(0.44f, -0.44f, 1, -0.9f));
    LShape wl1[1] = { { LSH_ELLIPSE, LOP_UNION, { 0, 0, 0.26f, 0.44f }, NULL, 0 } };
    LShape wl2[1] = { { LSH_ELLIPSE, LOP_UNION, { 0, 0, 0.22f, 0.38f }, NULL, 0 } };
    (void)wing1; (void)wing2;
    if (outline) {
        LPaint pi = lpaint_solid(ink);
        lcv_fill(cv, &w1, wl1, 1, &pi, &ol);
        lcv_fill(cv, &w2, wl2, 1, &pi, &ol);
        lcv_fill(cv, &xf, body, 2, &pi, &ol);
        lcv_fill(cv, &xf, head, 1, &pi, &ol);
        lcv_fill(cv, &xf, ant, 4, &pi, &ol);
    }
    LPaint wp = lpaint_linear(lrc(0.88f, 0.98f, 1.0f, 0.92f * alpha), 0, -0.4f, lrc(0.55f, 0.86f, 1.0f, 0.7f * alpha), 0, 0.4f);
    lcv_fill(cv, &w1, wl1, 1, &wp, NULL);
    lcv_fill(cv, &w2, wl2, 1, &wp, NULL);
    LFillOpt vein = { 0 };
    vein.outline = fmaxf(0.8f, scale * 0.02f);
    LPaint vp = lpaint_solid(lrc(0.3f, 0.6f, 0.8f, 0.5f * alpha));
    lcv_fill(cv, &w1, wl1, 1, &vp, &vein);
    lcv_fill(cv, &w2, wl2, 1, &vp, &vein);

    LBeeBody bb = { alpha };
    LPaint bp = lpaint_fn(lbee_body_paint, &bb);
    lcv_fill(cv, &xf, body, 2, &bp, NULL);

    LPaint ip = lpaint_solid(ink);
    lcv_fill(cv, &xf, ant, 4, &ip, NULL);
    LPaint hp = lpaint_radial(lrc_hex(0x4A3526, alpha), -0.58f, -0.06f, lrc_hex(LART_BROWN, alpha), 0.34f);
    lcv_fill(cv, &xf, head, 1, &hp, NULL);

    LShape eye[1] = { { LSH_CIRCLE, LOP_UNION, { -0.58f, -0.02f, 0.105f }, NULL, 0 } };
    LShape pupil[1] = { { LSH_CIRCLE, LOP_UNION, { -0.615f, -0.01f, 0.06f }, NULL, 0 } };
    LShape glint[1] = { { LSH_CIRCLE, LOP_UNION, { -0.63f, -0.04f, 0.022f }, NULL, 0 } };
    LShape cheek[1] = { { LSH_CIRCLE, LOP_UNION, { -0.43f, 0.15f, 0.06f }, NULL, 0 } };
    LShape smile[1] = { { LSH_ARC, LOP_UNION, { -0.56f, 0.10f, 0.09f, 0.018f, 1.9f, 0.7f }, NULL, 0 } };
    LPaint white = lpaint_solid(lrc(1, 1, 1, alpha)), dark = lpaint_solid(lrc_hex(0x0C0806, alpha));
    LPaint blush = lpaint_solid(lrc_hex(0xFF6FA8, 0.6f * alpha)), smilep = lpaint_solid(lrc_hex(0xFFD85C, 0.9f * alpha));
    lcv_fill(cv, &xf, eye, 1, &white, NULL);
    lcv_fill(cv, &xf, pupil, 1, &dark, NULL);
    lcv_fill(cv, &xf, glint, 1, &white, NULL);
    lcv_fill(cv, &xf, cheek, 1, &blush, NULL);
    lcv_fill(cv, &xf, smile, 1, &smilep, NULL);
}
