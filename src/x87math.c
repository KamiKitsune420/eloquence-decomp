/* x87math - see x87math.h. The addresses are msvcrt.dll 7.0 (Windows 11, SysWOW64). */
#include "x87math.h"

#include <math.h>
#include <string.h>

static fx80 from_double(double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    return fx_from_f64(b);
}

static double to_double(fx80 v)
{
    fx_env e = { FX_PC53, FX_RN };
    uint64_t b = fx_to_f64(v, e);
    double d;
    memcpy(&d, &b, 8);
    return d;
}

#define BIAS_PLUS_15 (16383 + 15)      /* 2^15: past the extended exponent range as a power of two */

static int finite_nonzero(double v) { return isfinite(v) && v != 0; }

/* a nan argument comes back as fld leaves it: quieted, payload kept */
static fx80 quiet(double v)
{
    fx80 r = from_double(v);
    r.sig |= 0x4000000000000000ull;
    return r;
}

/* The result check every routine ends with (_ctrandisp, 0x100a56a9): the value is stored as a double
 * and an exponent of 0 or 0x7ff (underflow, overflow) goes through the error handler, which returns the
 * double. Otherwise st0 comes back as it is. */
static fx80 result(fx80 v)
{
    double d = to_double(v);
    if (d == 0 || !isfinite(d) || fabs(d) < 2.2250738585072014e-308) return from_double(d);
    return v;
}

/* exp: _trandisp1 sets the control word to 0x133f (64-bit precision, nearest), then 0x100a6930:
 * t = x log2 e; n = rndint t; f = t - n; 2^|f| = 1 + f2xm1 |f|, inverted when f < 0; scaled by 2^n. */
fx80 x87m_exp(double x)
{
    if (isnan(x)) return quiet(x);
    if (!finite_nonzero(x)) return from_double(exp(x));        /* 0, inf: exact results */
    fx_env e = { FX_PC64, FX_RN };
    fx80 one = fx_const(0);
    fx80 t = fx_mul(from_double(x), fx_const(2), e);
    fx80 limit = { 0xffff000000000000ull, 0x400d, 0 };            /* 0x100bcc8e: 32767 */
    if (fx_cmp(fx_abs(t), limit) >= 0) return from_double(exp(x));   /* far out of range: inf or 0 */
    fx80 n = fx_rndint(t, e);
    fx80 f = fx_sub(t, n, e);
    int neg = f.sign && !fx_is_zero(f);
    fx80 v = fx_add(fx_f2xm1(fx_abs(f)), one, e);
    if (neg) v = fx_div(one, v, e);
    if (!fx_is_zero(n)) v = fx_scale(v, n);
    return result(v);
}

/* log: 0x1009913f. Positive finite x: fldln2, fyl2x. */
fx80 x87m_log(double x)
{
    if (isnan(x)) return quiet(x);
    if (!(x > 0) || !isfinite(x)) return from_double(log(x));    /* 0, negative, inf */
    return fx_yl2x(fx_const(5), from_double(x));
}

/* 2^t at 0x100a55c0 in the caller's precision: n = rndint t; f = -(n - t); (1 + f2xm1 f) 2^n */
static fx80 exp2_x87(fx80 t, fx_env e)
{
    fx80 n = fx_rndint(t, e);
    fx80 f = fx_neg(fx_sub(n, t, e));
    fx80 v = fx_add(fx_f2xm1(f), fx_const(0), e);
    return fx_scale(v, n);
}

/* pow: 0x10099404. The control word keeps the caller's precision with rounding to nearest. x < 0 needs
 * an integer y (odd: negate). Zeros, infinities and nans have exact answers the host gives too. */
fx80 x87m_pow(double x, double y, uint16_t caller_cw)
{
    if (!isfinite(x) || !isfinite(y) || x == 0) return from_double(pow(x, y));
    fx_env e = { (caller_cw >> 8) & 3, FX_RN };
    int odd = 0;
    if (x < 0) {
        fx80 fy = from_double(y);
        if (fx_cmp(fx_rndint(fy, e), fy) != 0) return from_double(pow(x, y));   /* nan: domain error */
        fx80 half = fx_mul(fy, from_double(0.5), e);
        odd = fx_cmp(fx_rndint(half, e), half) != 0;
        x = -x;
    }
    fx80 t = fx_yl2x(from_double(y), from_double(x));
    fx80 big = { 0x8000000000000000ull, BIAS_PLUS_15, 0 };
    if (fx_cmp(fx_abs(t), big) >= 0) return from_double(pow(odd ? -x : x, y));   /* inf or 0 */
    fx80 v = exp2_x87(t, e);
    if (odd) v = fx_neg(v);
    return result(v);
}
