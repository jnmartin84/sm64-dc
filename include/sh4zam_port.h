#ifndef SH4ZAM_PORT_H
#define SH4ZAM_PORT_H

// sh4zam from the KOS addons, plus the few SM64-side helpers the port built on it (they used to
// live in a vendored, outdated copy of sh4zam.h).
#if defined(__FAST_MATH__)
// SH4ZAM 0.10 turns its sincos into two __builtin_sinf/cosf calls on float radians under
// __FAST_MATH__ (our -ffast-math), which differs from one FSCA on the 16-bit angle by one unit on
// ~17% of angles and moves the game's sins/coss. Its routines are inline: read without the macro,
// they keep the FSCA path, and nothing else is compiled differently.
#pragma push_macro("__FAST_MATH__")
#undef __FAST_MATH__
#include <sh4zam/shz_sh4zam.h>
#pragma pop_macro("__FAST_MATH__")
#else
#include <sh4zam/shz_sh4zam.h>
#endif
#include <PR/ultratypes.h>

#if DC_EXACT_MATH
// The game logic (src/game, src/engine) is built with this (Makefile): sh4zam's fast division,
// reciprocal and square root (FSRRA approximations) become the exact IEEE operations the N64 does
// (FDIV and FSQRT round correctly on the SH4 too), so the game computes what the N64 does.
#include <math.h>
#define shz_divf(num, denom) ((num) / (denom))
#define shz_divf_fsrra(num, denom) ((num) / (denom))
#define shz_invf(x) (1.0f / (x))
#define shz_invf_fsrra(x) (1.0f / (x))
#define shz_sqrtf(x) sqrtf(x)
#define shz_sqrtf_fsrra(x) sqrtf(x)
#define shz_inv_sqrtf(x) (1.0f / sqrtf(x))
// Dot products and squared lengths summed left to right, as the original code writes them (FIPR
// on the SH4 sums with its own internal precision).
static inline f32 exact_dot6f(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2) {
    return x1 * x2 + y1 * y2 + z1 * z2;
}
static inline f32 exact_dot8f(f32 x1, f32 y1, f32 z1, f32 w1, f32 x2, f32 y2, f32 z2, f32 w2) {
    return x1 * x2 + y1 * y2 + z1 * z2 + w1 * w2;
}
static inline f32 exact_mag_sqr3f(f32 x, f32 y, f32 z) {
    return x * x + y * y + z * z;
}
// Three dot products of l with r1..r3, each summed left to right (the port writes the original's
// matrix-vector sums this way; FIPR on the SH4).
static inline shz_vec3_t exact_vec3_dot3(shz_vec3_t l, shz_vec3_t r1, shz_vec3_t r2, shz_vec3_t r3) {
    return (shz_vec3_t) { .x = l.x * r1.x + l.y * r1.y + l.z * r1.z,
                          .y = l.x * r2.x + l.y * r2.y + l.z * r2.z,
                          .z = l.x * r3.x + l.y * r3.y + l.z * r3.z };
}
// A vector over its length, as the original's normalizations (mtxf_lookat's three axes) write it:
// one correctly rounded 1 / sqrtf, then three products (sh4zam scales by FSRRA's approximation).
// The camera matrix comes from mtxf_lookat, and the renderer computes Mario's HOLP from it, which
// the game logic then reads (interaction.c).
static inline shz_vec3_t exact_vec3_normalize(shz_vec3_t v) {
    f32 inv = 1.0f / sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return (shz_vec3_t) { .x = v.x * inv, .y = v.y * inv, .z = v.z * inv };
}
#define shz_vec3_dot3 exact_vec3_dot3
#define shz_vec3_normalize exact_vec3_normalize
#define shz_dot6f exact_dot6f
#define shz_dot8f exact_dot8f
#define shz_mag_sqr3f exact_mag_sqr3f
#endif

// One SM64 binary angle unit (65536 per turn) in radians.
#define TRIG_ARG_SCALE 0.00009587f

// Sine and cosine of an SM64 s16 angle from the game's own tables, as sins()/coss() and the N64
// compute them: game logic that uses these stays frame-exact with the original (FSCA's values
// differ slightly).
// The cosine table is the sine table a quarter turn on (math_util.h, AVOID_UB).
extern f32 gSineTable[];
static inline void sincoss(s16 angle, f32 *s, f32 *c) {
    *s = gSineTable[(u16) angle >> 4];
    *c = gSineTable[((u16) angle >> 4) + 0x400];
}

static inline void scaled_sincoss(s16 angle, f32 *s, f32 *c, f32 scale) {
    *s = gSineTable[(u16) angle >> 4] * scale;
    *c = gSineTable[((u16) angle >> 4) + 0x400] * scale;
}

#endif
