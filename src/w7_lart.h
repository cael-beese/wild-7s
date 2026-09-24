/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 - see LICENSE.md */
/* w7_lart.h - shared vector motifs for the lounge look, ported from Beese's Poker
 * Lounge (render/art.h): the four suit pips, the bee mascot, brushed brass and
 * honeycomb fills, and the house palette. All original designs. */
#ifndef W7_LART_H
#define W7_LART_H

#include "w7_raster.h"

/* Suits, in the poker engine's order (clubs, diamonds, hearts, spades). */
enum { LSUIT_C, LSUIT_D, LSUIT_H, LSUIT_S };

/* House palette ("neon honey lounge"), straight alpha. */
#define LART_CHARCOAL  0x0E0C10
#define LART_FELT      0x1A1320
#define LART_PLUM      0x2A0F2E
#define LART_HONEY     0xE8AA28
#define LART_GOLD      0xFFD25A
#define LART_AMBER     0xFF8010
#define LART_MAGENTA   0xFF28C8
#define LART_CYAN      0x28E6FF
#define LART_IVORY     0xF8F1E2
#define LART_RUBY      0xD01A48
#define LART_INK       0x1E1A24
#define LART_BROWN     0x2B1D14

/* Suit pip, point-symmetric unit design (half height 1), painted centred at
 * (cx, cy) with half height `size` px, rotated by rot. top/bottom = gradient. */
void lart_pip(LCanvas *cv, int suit, float cx, float cy, float size, float rot, LCol top, LCol bottom,
             const LFillOpt *opt);
/* The suit's ink colours (card red / card black), top and bottom of the gradient. */
void lart_suit_colors(int suit, LCol *top, LCol *bottom);

/* The bee mascot, facing left, about 2 units wide; scale = px per unit.
 * alpha fades it; dark_outline adds the sticker-style outline. */
void lart_bee(LCanvas *cv, float cx, float cy, float scale, float rot, float alpha, int dark_outline);

/* Paints for lcv_fill. */
typedef struct { float angle; uint32_t seed; float light; } LBrassParams;   /* brushed brass */
LCol lart_brass(float lx, float ly, float d_px, const void *user);

typedef struct { float r; LCol base, line, cell; float line_w; uint32_t seed; float cell_prob; } LHoneyParams;
LCol lart_honeycomb(float lx, float ly, float d_px, const void *user);

#endif
