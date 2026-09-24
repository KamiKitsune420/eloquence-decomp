/* fx80 - see fx80.h */
#include "fx80.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIAS 16383
#define NOLIMIT INT_MIN

/* ---------------------------------------------------------------- 128-bit helpers */
typedef struct { uint64_t hi, lo; } u128;

static u128 mul64(uint64_t a, uint64_t b)
{
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    u128 r;
    r.lo = (mid << 32) | (uint32_t)p00;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

/* shift right by n, OR-ing everything shifted out into the lowest bit (sticky) */
static u128 shr_sticky(u128 v, int n)
{
    uint64_t sticky;
    if (n <= 0) return v;
    if (n >= 128) {
        sticky = (v.hi | v.lo) != 0;
        v.hi = 0;
        v.lo = sticky;
        return v;
    }
    if (n >= 64) {
        sticky = v.lo != 0 || (n > 64 && (v.hi << (128 - n)) != 0);
        v.lo = n == 64 ? v.hi : v.hi >> (n - 64);
        v.hi = 0;
        v.lo |= sticky;
        return v;
    }
    sticky = (v.lo << (64 - n)) != 0;
    v.lo = (v.lo >> n) | (v.hi << (64 - n));
    v.hi >>= n;
    v.lo |= sticky;
    return v;
}

static u128 shl1(u128 v)
{
    v.hi = (v.hi << 1) | (v.lo >> 63);
    v.lo <<= 1;
    return v;
}

static int clz64(uint64_t x)
{
    int n = 0;
    if (!x) return 64;
    while (!(x & 0x8000000000000000ull)) { x <<= 1; n++; }
    return n;
}

/* ---------------------------------------------------------------- rounding
 *
 * The value is (m.hi:m.lo) * 2^(e - BIAS - 127), m nonzero. Round it to at most p significant bits and,
 * if lsb != NOLIMIT, to a multiple of 2^lsb (for narrower formats' subnormals and for frndint), using rc.
 * The result is an fx80 in the extended exponent range. */
static fx80 round_to(int sign, int e, u128 m, int p, int lsb, int rc)
{
    fx80 r;
    r.sign = (uint8_t)sign;
    if (!m.hi && !m.lo) { r.sig = 0; r.exp = 0; return r; }
    if (!m.hi) { m.hi = m.lo; m.lo = 0; e -= 64; }
    int z = clz64(m.hi);
    if (z) { m.hi = (m.hi << z) | (m.lo >> (64 - z)); m.lo <<= z; e -= z; }
    /* now value = m * 2^(e-BIAS-127) with m's top bit set: value in [2^(e-BIAS), 2^(e-BIAS+1)) */
    int eu = e - BIAS;
    if (lsb != NOLIMIT && eu - lsb + 1 < p) p = eu - lsb + 1;
    if (p <= 0) {
        /* the value is below the last representable bit 2^lsb: 0 or 2^lsb */
        int up = 0;
        if (rc == FX_RN) up = p == 0 && !(m.hi == 0x8000000000000000ull && m.lo == 0);    /* > half */
        else if (rc == FX_RU) up = !sign;
        else if (rc == FX_RD) up = sign;
        if (!up) { r.sig = 0; r.exp = 0; return r; }
        r.sig = 0x8000000000000000ull;
        r.exp = (uint16_t)(lsb + BIAS);
        return r;
    }
    int shift = 64 - p;
    uint64_t keep;
    int up = 0, inexact;
    if (shift == 0) {
        keep = m.hi;
        inexact = m.lo != 0;
        if (rc == FX_RN) up = (m.lo >> 63) && ((m.lo << 1) || (keep & 1));
    } else {
        uint64_t mask = (1ull << shift) - 1, dropped = m.hi & mask, half = 1ull << (shift - 1);
        keep = m.hi & ~mask;
        inexact = dropped || m.lo;
        if (rc == FX_RN) up = dropped > half || (dropped == half && (m.lo || (keep & (1ull << shift))));
    }
    if (rc == FX_RU) up = inexact && !sign;
    if (rc == FX_RD) up = inexact && sign;
    if (up) {
        uint64_t inc = 1ull << shift;
        keep += inc;
        if (keep < inc) { keep = 0x8000000000000000ull; e++; }
    }
    if (e >= 0x7fff) { r.sig = 0x8000000000000000ull; r.exp = 0x7fff; return r; }
    if (e <= 0) { fprintf(stderr, "fx80: extended underflow\n"); abort(); }
    r.sig = keep;
    r.exp = (uint16_t)e;
    return r;
}

static int pc_bits(fx_env e) { return e.pc == FX_PC24 ? 24 : e.pc == FX_PC53 ? 53 : 64; }

fx80 fx_zero(int sign) { fx80 r = { 0, 0, (uint8_t)sign }; return r; }
int fx_is_zero(fx80 a) { return a.exp == 0 && a.sig == 0; }
int fx_is_inf(fx80 a) { return a.exp == 0x7fff && (a.sig << 1) == 0; }
int fx_is_nan(fx80 a) { return a.exp == 0x7fff && (a.sig << 1) != 0; }

static void check(fx80 a, const char *op)
{
    if (a.exp == 0x7fff || (a.exp == 0 && a.sig)) {
        fprintf(stderr, "fx80: %s on a special value (exp %04x sig %016llx)\n", op, a.exp,
                (unsigned long long)a.sig);
        abort();
    }
}

/* an fx80's value as round_to input: m = sig in the high half, e = exp */
static u128 sig128(fx80 a) { u128 m = { a.sig, 0 }; return m; }

/* ---------------------------------------------------------------- conversions */
fx80 fx_from_i64(int64_t v)
{
    if (!v) return fx_zero(0);
    int sign = v < 0;
    uint64_t mag = sign ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    u128 m = { mag, 0 };                            /* 128-bit number mag * 2^64 */
    return round_to(sign, BIAS + 63, m, 64, NOLIMIT, FX_RN);
}

fx80 fx_from_f32(uint32_t b)
{
    int sign = (int)(b >> 31), e = (int)((b >> 23) & 0xff);
    uint32_t f = b & 0x7fffff;
    if (e == 0xff) { fx80 r = { 0x8000000000000000ull | ((uint64_t)f << 40), 0x7fff, (uint8_t)sign }; return r; }
    if (e == 0) {
        if (!f) return fx_zero(sign);
        u128 m = { (uint64_t)f, 0 };               /* f * 2^-149 = (f * 2^64) * 2^(e-BIAS-127) */
        return round_to(sign, BIAS - 149 + 127 - 64, m, 64, NOLIMIT, FX_RN);
    }
    fx80 r = { ((uint64_t)(f | 0x800000)) << 40, (uint16_t)(e - 127 + BIAS), (uint8_t)sign };
    return r;
}

fx80 fx_from_f64(uint64_t b)
{
    int sign = (int)(b >> 63), e = (int)((b >> 52) & 0x7ff);
    uint64_t f = b & 0xfffffffffffffull;
    if (e == 0x7ff) { fx80 r = { 0x8000000000000000ull | (f << 11), 0x7fff, (uint8_t)sign }; return r; }
    if (e == 0) {
        if (!f) return fx_zero(sign);
        u128 m = { f, 0 };
        return round_to(sign, BIAS - 1074 + 127 - 64, m, 64, NOLIMIT, FX_RN);
    }
    fx80 r = { (f | 0x10000000000000ull) << 11, (uint16_t)(e - 1023 + BIAS), (uint8_t)sign };
    return r;
}

uint32_t fx_to_f32(fx80 a, fx_env env)
{
    uint32_t s = (uint32_t)a.sign << 31;
    if (fx_is_zero(a)) return s;
    if (a.exp == 0x7fff) return s | 0x7f800000u | (uint32_t)((a.sig << 1) >> 41);
    fx80 r = round_to(a.sign, a.exp, sig128(a), 24, -149, env.rc);
    if (fx_is_zero(r)) return s;
    int e = (int)r.exp - BIAS;
    if (e > 127) return s | 0x7f800000u;
    if (e >= -126) return s | ((uint32_t)(e + 127) << 23) | (uint32_t)((r.sig >> 40) & 0x7fffff);
    return s | (uint32_t)(r.sig >> (63 - (e + 149)));      /* subnormal: r.sig * 2^(e-63) / 2^-149 */
}

uint64_t fx_to_f64(fx80 a, fx_env env)
{
    uint64_t s = (uint64_t)a.sign << 63;
    if (fx_is_zero(a)) return s;
    if (a.exp == 0x7fff) return s | 0x7ff0000000000000ull | ((a.sig << 1) >> 12);
    fx80 r = round_to(a.sign, a.exp, sig128(a), 53, -1074, env.rc);
    if (fx_is_zero(r)) return s;
    int e = (int)r.exp - BIAS;
    if (e > 1023) return s | 0x7ff0000000000000ull;
    if (e >= -1022) return s | ((uint64_t)(e + 1023) << 52) | ((r.sig >> 11) & 0xfffffffffffffull);
    return s | (r.sig >> (63 - (e + 1074)));
}

fx80 fx_rndint(fx80 a, fx_env env)
{
    if (fx_is_zero(a) || a.exp == 0x7fff) return a;
    if ((int)a.exp - BIAS >= 63) return a;
    fx80 r = round_to(a.sign, a.exp, sig128(a), 64, 0, env.rc);
    if (fx_is_zero(r)) r.sign = a.sign;
    return r;
}

int64_t fx_to_int(fx80 a, fx_env env, int bits)
{
    int64_t indefinite = bits == 16 ? -32768 : bits == 32 ? (int64_t)INT32_MIN : INT64_MIN;
    if (fx_is_zero(a)) return 0;
    if (a.exp == 0x7fff) return indefinite;
    fx80 r = fx_rndint(a, env);
    if (fx_is_zero(r)) return 0;
    int e = (int)r.exp - BIAS;
    if (e > 63) return indefinite;
    uint64_t mag = e == 63 ? r.sig : r.sig >> (63 - e);
    uint64_t lim = (uint64_t)1 << (bits - 1);
    if (r.sign ? mag > lim : mag >= lim) return indefinite;
    return r.sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

fx80 fx_from_tbyte(const uint8_t *p)
{
    fx80 r;
    memcpy(&r.sig, p, 8);
    uint16_t se = (uint16_t)(p[8] | (p[9] << 8));
    r.exp = se & 0x7fff;
    r.sign = (uint8_t)(se >> 15);
    return r;
}

void fx_to_tbyte(fx80 a, uint8_t *p)
{
    memcpy(p, &a.sig, 8);
    uint16_t se = (uint16_t)(a.exp | (a.sign << 15));
    p[8] = (uint8_t)se;
    p[9] = (uint8_t)(se >> 8);
}

/* ---------------------------------------------------------------- arithmetic */
fx80 fx_add(fx80 a, fx80 b, fx_env env)
{
    check(a, "add");
    check(b, "add");
    int p = pc_bits(env);
    if (fx_is_zero(a) && fx_is_zero(b)) {
        if (a.sign == b.sign) return fx_zero(a.sign);
        return fx_zero(env.rc == FX_RD);
    }
    if (fx_is_zero(a)) return round_to(b.sign, b.exp, sig128(b), p, NOLIMIT, env.rc);
    if (fx_is_zero(b)) return round_to(a.sign, a.exp, sig128(a), p, NOLIMIT, env.rc);
    if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) { fx80 t = a; a = b; b = t; }
    u128 ma = sig128(a), mb = shr_sticky(sig128(b), a.exp - b.exp);
    int e = a.exp;
    u128 r;
    if (a.sign == b.sign) {
        r.lo = ma.lo + mb.lo;
        uint64_t c = r.lo < ma.lo;
        r.hi = ma.hi + mb.hi + c;
        if (r.hi < ma.hi || (c && r.hi == ma.hi)) {      /* carry out of bit 127 */
            r = shr_sticky(r, 1);
            r.hi |= 0x8000000000000000ull;
            e++;
        }
        return round_to(a.sign, e, r, p, NOLIMIT, env.rc);
    }
    r.lo = ma.lo - mb.lo;
    r.hi = ma.hi - mb.hi - (ma.lo < mb.lo);
    if (!r.hi && !r.lo) return fx_zero(env.rc == FX_RD);
    return round_to(a.sign, e, r, p, NOLIMIT, env.rc);
}

fx80 fx_neg(fx80 a) { a.sign ^= 1; return a; }
fx80 fx_abs(fx80 a) { a.sign = 0; return a; }
fx80 fx_sub(fx80 a, fx80 b, fx_env env) { return fx_add(a, fx_neg(b), env); }

fx80 fx_mul(fx80 a, fx80 b, fx_env env)
{
    check(a, "mul");
    check(b, "mul");
    int sign = a.sign ^ b.sign;
    if (fx_is_zero(a) || fx_is_zero(b)) return fx_zero(sign);
    /* sa*sb * 2^(ea+eb-2*BIAS-126) = m * 2^(e-BIAS-127) with e = ea+eb-BIAS+1 */
    return round_to(sign, (int)a.exp + (int)b.exp - BIAS + 1, mul64(a.sig, b.sig), pc_bits(env), NOLIMIT, env.rc);
}

fx80 fx_div(fx80 a, fx80 b, fx_env env)
{
    check(a, "div");
    check(b, "div");
    int sign = a.sign ^ b.sign;
    if (fx_is_zero(b)) { fx80 r = { 0x8000000000000000ull, 0x7fff, (uint8_t)sign }; return r; }
    if (fx_is_zero(a)) return fx_zero(sign);
    /* q = floor(sa/sb * 2^127) with a sticky bit; value = q * 2^(ea-eb-127) */
    uint64_t rem = a.sig, d = b.sig;
    int carry = 0;
    u128 q = { 0, 0 };
    for (int i = 0; i < 128; i++) {
        int bit = 0;
        if (carry || rem >= d) { rem -= d; bit = 1; }
        q = shl1(q);
        q.lo |= (uint64_t)bit;
        carry = (int)(rem >> 63);
        rem <<= 1;
    }
    if (rem || carry) q.lo |= 1;
    return round_to(sign, (int)a.exp - (int)b.exp + BIAS, q, pc_bits(env), NOLIMIT, env.rc);
}

fx80 fx_sqrt(fx80 a, fx_env env)
{
    (void)a; (void)env;
    fprintf(stderr, "fx80: fsqrt not implemented yet\n");
    abort();
}

int fx_cmp(fx80 a, fx80 b)
{
    if (fx_is_nan(a) || fx_is_nan(b)) return 2;
    if (fx_is_zero(a) && fx_is_zero(b)) return 0;
    if (a.sign != b.sign) return a.sign ? -1 : 1;
    int mag;
    if (fx_is_zero(a)) mag = -1;
    else if (fx_is_zero(b)) mag = 1;
    else if (a.exp != b.exp) mag = a.exp < b.exp ? -1 : 1;
    else mag = a.sig == b.sig ? 0 : a.sig < b.sig ? -1 : 1;
    return a.sign ? -mag : mag;
}

fx80 fx_scale(fx80 a, fx80 b)
{
    if (fx_is_zero(a)) return a;
    fx_env tz = { FX_PC64, FX_RZ };
    int64_t n = fx_to_int(b, tz, 32);
    int e = (int)a.exp + (int)n;
    if (e >= 0x7fff) { fx80 r = { 0x8000000000000000ull, 0x7fff, a.sign }; return r; }    /* overflow: inf (masked) */
    if (e <= 0) return fx_zero(a.sign);     /* below 2^-16382: extended denormals are not modelled; 0 as a double */
    a.exp = (uint16_t)e;
    return a;
}

/* ---------------------------------------------------------------- multi-precision for transcendentals
 * Fixed point: value = w / 2^FRAC, w an unsigned 320-bit integer in 5 little-endian 64-bit limbs,
 * sign kept separately. 256 fraction bits: far more than rounding 64 bits correctly needs. */
#define NL 5
#define FRAC 256
typedef struct { uint64_t w[NL]; int neg; } mp;

static void mp_zero(mp *a) { memset(a, 0, sizeof *a); }
static void mp_one(mp *a) { mp_zero(a); a->w[FRAC / 64] = 1ull << (FRAC % 64); }
static int mp_is_zero(const mp *a) { for (int i = 0; i < NL; i++) if (a->w[i]) return 0; return 1; }

static int mp_cmp_mag(const mp *a, const mp *b)
{
    for (int i = NL - 1; i >= 0; i--)
        if (a->w[i] != b->w[i]) return a->w[i] < b->w[i] ? -1 : 1;
    return 0;
}

static void mag_add(uint64_t *r, const uint64_t *a, const uint64_t *b)
{
    uint64_t c = 0;
    for (int i = 0; i < NL; i++) {
        uint64_t s = a[i] + c;
        c = s < c;
        s += b[i];
        c += s < b[i];
        r[i] = s;
    }
}

static void mag_sub(uint64_t *r, const uint64_t *a, const uint64_t *b)     /* |a| >= |b| */
{
    uint64_t br = 0;
    for (int i = 0; i < NL; i++) {
        uint64_t t = a[i] - br;
        uint64_t b1 = a[i] < br;
        uint64_t b2 = t < b[i];
        r[i] = t - b[i];
        br = b1 + b2;
    }
}

static void mp_add(mp *r, const mp *a, const mp *b)
{
    if (a->neg == b->neg) { mag_add(r->w, a->w, b->w); r->neg = a->neg; return; }
    if (mp_cmp_mag(a, b) >= 0) { int s = a->neg; mag_sub(r->w, a->w, b->w); r->neg = s; }
    else { int s = b->neg; mag_sub(r->w, b->w, a->w); r->neg = s; }
}

static void mp_sub(mp *r, const mp *a, const mp *b)
{
    mp nb = *b;
    nb.neg ^= 1;
    mp_add(r, a, &nb);
}

static void mp_mul(mp *r, const mp *a, const mp *b)
{
    uint64_t t[2 * NL + 1];
    memset(t, 0, sizeof t);
    for (int i = 0; i < NL; i++) {
        if (!a->w[i]) continue;
        uint64_t carry = 0;
        for (int j = 0; j < NL; j++) {
            u128 p = mul64(a->w[i], b->w[j]);
            uint64_t lo = p.lo + carry;
            uint64_t hi = p.hi + (lo < carry);
            uint64_t s = t[i + j] + lo;
            hi += s < lo;
            t[i + j] = s;
            carry = hi;
        }
        for (int k = i + NL; carry; k++) { uint64_t s = t[k] + carry; carry = s < carry; t[k] = s; }
    }
    int neg = a->neg ^ b->neg;
    for (int i = 0; i < NL; i++) r->w[i] = t[i + FRAC / 64];
    r->neg = neg;
}

static void mp_mul_small(mp *r, const mp *a, uint32_t k)
{
    uint64_t carry = 0;
    for (int i = 0; i < NL; i++) {
        u128 p = mul64(a->w[i], k);
        uint64_t lo = p.lo + carry;
        carry = p.hi + (lo < carry);
        r->w[i] = lo;
    }
    r->neg = a->neg;
}

static void mp_div_small(mp *r, const mp *a, uint32_t k)
{
    uint64_t rem = 0;
    for (int i = NL - 1; i >= 0; i--) {
        uint64_t hi = (rem << 32) | (a->w[i] >> 32);
        uint64_t qh = hi / k;
        rem = hi % k;
        uint64_t lo = (rem << 32) | (a->w[i] & 0xffffffffu);
        uint64_t ql = lo / k;
        rem = lo % k;
        r->w[i] = (qh << 32) | ql;
    }
    r->neg = a->neg;
}

static void mp_setbit(mp *a, int b)
{
    if (b < 0) return;
    if (b >= 64 * NL) { fprintf(stderr, "fx80: mp overflow\n"); abort(); }
    a->w[b / 64] |= 1ull << (b % 64);
}

static void mp_from_fx(mp *r, fx80 a)
{
    mp_zero(r);
    r->neg = a.sign;
    if (fx_is_zero(a)) return;
    int pos = FRAC + ((int)a.exp - BIAS) - 63;       /* position of the significand's bit 0 */
    for (int i = 0; i < 64; i++)
        if ((a.sig >> i) & 1) mp_setbit(r, pos + i);
}

static fx80 mp_to_fx_rc(const mp *a, int rc)
{
    if (mp_is_zero(a)) return fx_zero(a->neg);
    int top = NL * 64 - 1;
    while (!((a->w[top / 64] >> (top % 64)) & 1)) top--;
    u128 m = { 0, 0 };
    int sticky = 0;
    for (int b = top, k = 127; b >= 0; b--, k--) {
        int bit = (int)((a->w[b / 64] >> (b % 64)) & 1);
        if (k >= 64) m.hi |= (uint64_t)bit << (k - 64);
        else if (k >= 0) m.lo |= (uint64_t)bit << k;
        else if (bit) { sticky = 1; break; }
    }
    if (sticky) m.lo |= 1;
    /* value = m * 2^(top - 127 - FRAC) = m * 2^(e - BIAS - 127) */
    return round_to(a->neg, top - FRAC + BIAS, m, 64, NOLIMIT, rc);
}

static int g_trc = FX_RN;          /* rounding of the transcendentals' 64-bit result (experiments) */
void fx_set_transcendental_rounding(int rc) { g_trc = rc; }
static fx80 mp_to_fx(const mp *a) { return mp_to_fx_rc(a, g_trc); }

static mp g_ln2, g_halfpi66;
static int g_init;

static void mp_init(void)
{
    if (g_init) return;
    /* ln 2 = sum_{k>=1} 1 / (k * 2^k) */
    mp_zero(&g_ln2);
    for (uint32_t k = 1; (int)k < FRAC; k++) {
        mp t;
        mp_zero(&t);
        mp_setbit(&t, FRAC - (int)k);
        mp_div_small(&t, &t, k);
        mag_add(g_ln2.w, g_ln2.w, t.w);
    }
    /* the x87's 66-bit pi = 0xC90FDAA22168C234 * 2^-62 + 0b11 * 2^-64; pi/2 halves every weight.
     * A bit of weight 2^w sits at position FRAC + w. */
    mp_zero(&g_halfpi66);
    uint64_t hi = 0xC90FDAA22168C234ull;
    for (int i = 0; i < 64; i++)
        if ((hi >> i) & 1) mp_setbit(&g_halfpi66, FRAC + i - 63);
    mp_setbit(&g_halfpi66, FRAC - 64);           /* extra bits: 2^-63, 2^-64 halved */
    mp_setbit(&g_halfpi66, FRAC - 65);
    g_init = 1;
}

static void mp_exp(mp *r, const mp *y)          /* |y| < 1 */
{
    mp term, acc;
    mp_one(&acc);
    mp_one(&term);
    for (uint32_t k = 1; k < 150; k++) {
        mp_mul(&term, &term, y);
        mp_div_small(&term, &term, k);
        if (mp_is_zero(&term)) break;
        mp_add(&acc, &acc, &term);
    }
    *r = acc;
}

static void mp_sincos(mp *s, mp *c, const mp *x)  /* |x| <= pi/4 or so */
{
    mp x2, t;
    mp_mul(&x2, x, x);
    x2.neg = 0;
    *s = *x;
    t = *x;
    for (uint32_t k = 1; k < 100; k++) {
        mp_mul(&t, &t, &x2);
        mp_div_small(&t, &t, (2 * k) * (2 * k + 1));
        if (mp_is_zero(&t)) break;
        t.neg ^= 1;
        mp_add(s, s, &t);
    }
    mp_one(c);
    mp_one(&t);
    for (uint32_t k = 1; k < 100; k++) {
        mp_mul(&t, &t, &x2);
        mp_div_small(&t, &t, (2 * k - 1) * (2 * k));
        if (mp_is_zero(&t)) break;
        t.neg ^= 1;
        mp_add(c, c, &t);
    }
}

fx80 fx_f2xm1(fx80 a)
{
    check(a, "f2xm1");
    mp_init();
    if (fx_is_zero(a)) return a;
    mp x, y, e, one;
    mp_from_fx(&x, a);
    mp_mul(&y, &x, &g_ln2);
    mp_exp(&e, &y);
    mp_one(&one);
    mp_sub(&e, &e, &one);
    return mp_to_fx(&e);
}

static void reduce(const mp *x, mp *r, int *quadrant)
{
    /* k = nearest integer to x / (pi/2); r = x - k * (pi/2)_66, exactly */
    double xd = 0;
    for (int i = NL - 1; i >= 0; i--) xd = xd * 18446744073709551616.0 + (double)x->w[i];
    xd = ldexp(xd, -FRAC) * (x->neg ? -1 : 1);
    long k = lround(xd / 1.5707963267948966);
    mp kp;
    mp_mul_small(&kp, &g_halfpi66, (uint32_t)labs(k));
    kp.neg = k < 0;
    mp_sub(r, x, &kp);
    *quadrant = (int)(((k % 4) + 4) % 4);
}

fx80 fx_sin(fx80 a)
{
    check(a, "fsin");
    mp_init();
    if (fx_is_zero(a)) return a;
    mp x, r, s, c;
    int q;
    mp_from_fx(&x, a);
    reduce(&x, &r, &q);
    mp_sincos(&s, &c, &r);
    mp *v = (q & 1) ? &c : &s;          /* sin(r + q pi/2) = sin r, cos r, -sin r, -cos r */
    if (q >= 2) v->neg ^= 1;
    return mp_to_fx(v);
}

fx80 fx_cos(fx80 a)
{
    check(a, "fcos");
    mp_init();
    mp x, r, s, c;
    int q;
    mp_from_fx(&x, a);
    reduce(&x, &r, &q);
    mp_sincos(&s, &c, &r);
    mp *v = (q & 1) ? &s : &c;          /* cos(r + q pi/2) = cos r, -sin r, -cos r, sin r */
    if (q == 1 || q == 2) v->neg ^= 1;
    return mp_to_fx(v);
}

/* 1/d for d around 1..2: Newton's r = r (2 - d r) from the double estimate, doubling the bits each time */
static void mp_recip(mp *r, const mp *d)
{
    double dd = 0;
    for (int i = NL - 1; i >= 0; i--) dd = dd * 18446744073709551616.0 + (double)d->w[i];
    dd = ldexp(dd, -FRAC);
    uint64_t b;
    double est = 1.0 / dd;
    memcpy(&b, &est, 8);
    mp_from_fx(r, fx_from_f64(b));
    mp two, t;
    mp_one(&two);
    mag_add(two.w, two.w, two.w);
    for (int i = 0; i < 5; i++) {
        mp_mul(&t, d, r);
        mp_sub(&t, &two, &t);
        mp_mul(r, r, &t);
    }
}

static fx80 fx_indefinite(void) { fx80 r = { 0xC000000000000000ull, 0x7fff, 1 }; return r; }

/* fyl2x: y * log2(x), rounded to 64 bits. log2 x = k + 2 atanh(s) / ln 2 with x = m 2^k, m in
 * [sqrt(1/2), sqrt(2)), s = (m - 1) / (m + 1). */
fx80 fx_yl2x(fx80 y, fx80 x)
{
    mp_init();
    if (fx_is_nan(x) || fx_is_nan(y)) return fx_is_nan(x) ? x : y;
    if (x.sign && !fx_is_zero(x)) return fx_indefinite();
    if (fx_is_zero(x) || fx_is_inf(x) || fx_is_inf(y)) {
        /* the edge cases: +-inf, or indefinite for 0 * inf; a program that gets here would want a look */
        fprintf(stderr, "fx80: fyl2x edge case\n");
        if (fx_is_zero(x) && !fx_is_zero(y)) { fx80 r = { 0x8000000000000000ull, 0x7fff, (uint8_t)(y.sign ^ 1) }; return r; }
        if (fx_is_inf(x) && !fx_is_zero(y)) { fx80 r = { 0x8000000000000000ull, 0x7fff, y.sign }; return r; }
        return fx_indefinite();
    }
    /* normalize a denormal x */
    int k = (int)x.exp - BIAS;
    uint64_t sig = x.sig;
    if (x.exp == 0) { k = 1 - BIAS; }
    while (!(sig >> 63)) { sig <<= 1; k--; }
    fx80 m = { sig, BIAS, 0 };
    if (sig > 0xB504F333F9DE6484ull) { m.exp = BIAS - 1; k++; }     /* m >= sqrt 2: halve it */
    if (k == 0 && m.exp == BIAS && sig == 0x8000000000000000ull) {  /* log2 1 = +0 */
        fx80 r = fx_zero(y.sign);
        return r;
    }
    mp mm, one, num, den, s, s2, t, sum, inv_ln2, l;
    mp_from_fx(&mm, m);
    mp_one(&one);
    mp_sub(&num, &mm, &one);
    mp_add(&den, &mm, &one);
    mp_recip(&t, &den);
    mp_mul(&s, &num, &t);
    mp_mul(&s2, &s, &s);
    s2.neg = 0;
    sum = s;
    t = s;
    for (uint32_t j = 1; j < 200; j++) {        /* atanh s = s + s^3/3 + s^5/5 + ... */
        mp term;
        mp_mul(&t, &t, &s2);
        mp_div_small(&term, &t, 2 * j + 1);
        if (mp_is_zero(&term)) break;
        mp_add(&sum, &sum, &term);
    }
    mag_add(sum.w, sum.w, sum.w);               /* ln m */
    mp_recip(&inv_ln2, &g_ln2);
    mp_mul(&l, &sum, &inv_ln2);
    mp kk;
    mp_zero(&kk);
    kk.w[FRAC / 64] = (uint64_t)(k < 0 ? -k : k);
    kk.neg = k < 0;
    mp_add(&l, &l, &kk);                        /* log2 x */
    if (fx_is_zero(y)) return fx_zero(y.sign ^ l.neg);
    /* y's significand only, in [1, 2); its exponent goes on after rounding, which it does not change */
    int ye = (int)y.exp - BIAS;
    uint64_t ys = y.sig;
    if (y.exp == 0) { ye = 1 - BIAS; while (!(ys >> 63)) { ys <<= 1; ye--; } }
    fx80 y1 = { ys, BIAS, y.sign };
    mp yy;
    mp_from_fx(&yy, y1);
    mp_mul(&t, &l, &yy);
    fx80 r = mp_to_fx(&t);
    int e = (int)r.exp + ye;
    if (e >= 0x7fff) { fx80 inf = { 0x8000000000000000ull, 0x7fff, r.sign }; return inf; }
    if (e <= 0) return fx_zero(r.sign);
    r.exp = (uint16_t)e;
    return r;
}

fx80 fx_const(int which)
{
    fx80 r;
    r.sign = 0;
    switch (which) {
    case 0: r.sig = 0x8000000000000000ull; r.exp = BIAS; return r;                 /* 1 */
    case 1: r.sig = 0xD49A784BCD1B8AFEull; r.exp = BIAS + 1; return r;             /* log2(10) */
    case 2: r.sig = 0xB8AA3B295C17F0BCull; r.exp = BIAS; return r;                 /* log2(e) */
    case 3: r.sig = 0xC90FDAA22168C235ull; r.exp = BIAS + 1; return r;             /* pi */
    case 4: r.sig = 0x9A209A84FBCFF799ull; r.exp = BIAS - 2; return r;             /* log10(2) */
    case 5: r.sig = 0xB17217F7D1CF79ACull; r.exp = BIAS - 1; return r;             /* ln(2) */
    default: return fx_zero(0);
    }
}

double fx_to_double_debug(fx80 a)
{
    if (fx_is_zero(a)) return a.sign ? -0.0 : 0.0;
    return ldexp((double)a.sig, (int)a.exp - BIAS - 63) * (a.sign ? -1 : 1);
}
