/* fx80 - the x87 80-bit extended format in software, bit-exact with the hardware for what compiled code
 * uses: arithmetic rounded to the precision-control setting (24/53/64 bits) with the rounding-control
 * mode, loads and stores of float/double/extended/integers, comparisons, frndint, fscale, and the
 * transcendentals f2xm1/fsin/fcos computed to high precision and rounded to 64 bits (with the x87's
 * 66-bit pi for argument reduction). The exponent range is the extended one throughout, as on the chip.
 *
 * Representation: sign, 15-bit biased exponent (bias 16383), 64-bit significand with explicit integer
 * bit - the memory layout of `fstp tbyte`.
 */
#ifndef FX80_H
#define FX80_H

#include <stdint.h>

typedef struct {
    uint64_t sig;
    uint16_t exp;      /* biased; 0 with sig 0 = zero; 0x7fff = inf/nan */
    uint8_t  sign;
} fx80;

/* control word fields */
enum { FX_PC24 = 0, FX_PC53 = 2, FX_PC64 = 3 };
enum { FX_RN = 0, FX_RD = 1, FX_RU = 2, FX_RZ = 3 };

typedef struct {
    int pc;            /* precision control */
    int rc;            /* rounding control */
} fx_env;

static inline fx_env fx_env_from_cw(uint16_t cw) { fx_env e = { (cw >> 8) & 3, (cw >> 10) & 3 }; return e; }

fx80 fx_zero(int sign);
int  fx_is_zero(fx80 a);
int  fx_is_nan(fx80 a);
int  fx_is_inf(fx80 a);

/* conversions: loads are exact; stores round with env.rc (not pc) */
fx80 fx_from_i64(int64_t v);
fx80 fx_from_f32(uint32_t bits);
fx80 fx_from_f64(uint64_t bits);
uint32_t fx_to_f32(fx80 a, fx_env e);
uint64_t fx_to_f64(fx80 a, fx_env e);
int64_t  fx_to_int(fx80 a, fx_env e, int bits);     /* fist/fistp: 16/32/64; out of range = indefinite */
fx80 fx_from_tbyte(const uint8_t *p);
void fx_to_tbyte(fx80 a, uint8_t *p);

/* arithmetic, rounded to env.pc bits with env.rc */
fx80 fx_add(fx80 a, fx80 b, fx_env e);
fx80 fx_sub(fx80 a, fx80 b, fx_env e);
fx80 fx_mul(fx80 a, fx80 b, fx_env e);
fx80 fx_div(fx80 a, fx80 b, fx_env e);
fx80 fx_sqrt(fx80 a, fx_env e);
fx80 fx_neg(fx80 a);
fx80 fx_abs(fx80 a);

/* comparison: -1 less, 0 equal, 1 greater, 2 unordered */
int fx_cmp(fx80 a, fx80 b);

fx80 fx_rndint(fx80 a, fx_env e);        /* frndint: to an integer with env.rc */
fx80 fx_scale(fx80 a, fx80 b);           /* fscale: a * 2^trunc(b) */
fx80 fx_f2xm1(fx80 a);                   /* 2^a - 1, |a| <= 1, rounded to 64 bits */
fx80 fx_sin(fx80 a);                     /* fsin, rounded to 64 bits */
fx80 fx_cos(fx80 a);                     /* fcos */
fx80 fx_yl2x(fx80 y, fx80 x);            /* fyl2x: y * log2(x) */

/* constants as the chip loads them (rounded to nearest 64 bits) */
fx80 fx_const(int which);                /* 0 1, 1 l2t, 2 l2e, 3 pi, 4 lg2, 5 ln2, 6 zero */

double fx_to_double_debug(fx80 a);
void fx_set_transcendental_rounding(int rc);    /* experiments: RN by default */

#endif
