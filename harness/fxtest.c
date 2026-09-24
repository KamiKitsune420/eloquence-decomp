/* fxtest - src/fx80.c against the real x87, bit for bit. 32-bit MSVC only (inline asm).
 *
 *   fxtest [iterations]
 *
 * Random operands: doubles, floats, integers and full 64-bit-significand extended values; every
 * operation at precision control 24/53/64 with round-to-nearest (and truncation for the integer store).
 * The transcendentals are checked on random arguments and on the ones a formant synthesizer feeds them
 * (2*pi*F/fs, -pi*B/fs for integer F, B).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../src/fx80.h"

static uint64_t rng = 88172645463325252ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

typedef struct { uint8_t b[10]; } tb;

static tb to_tb(fx80 a) { tb t; fx_to_tbyte(a, t.b); return t; }
static fx80 from_tb(tb t) { return fx_from_tbyte(t.b); }

/* a random finite extended value with exponent within +-e of 1 */
static fx80 rnd_ext(int erange)
{
    fx80 a;
    a.sig = rnd() | 0x8000000000000000ull;
    a.exp = (uint16_t)(16383 + (int)(rnd() % (2 * erange + 1)) - erange);
    a.sign = (uint8_t)(rnd() & 1);
    return a;
}

static fx80 rnd_dbl(int erange)
{
    fx80 a = rnd_ext(erange);
    a.sig &= ~0x7ffull;            /* 53 significant bits */
    return a;
}

static long fails, checks;

static void cmp(const char *what, tb hw, fx80 sw, fx80 a, fx80 b)
{
    checks++;
    tb s = to_tb(sw);
    if (memcmp(hw.b, s.b, 10)) {
        if (fails++ < 30) {
            printf("FAIL %s: a=%04x:%016llx b=%04x:%016llx hw=%02x%02x:%016llx sw=%04x%s:%016llx\n", what,
                   a.exp | (a.sign << 15), (unsigned long long)a.sig, b.exp | (b.sign << 15),
                   (unsigned long long)b.sig, hw.b[9], hw.b[8], (unsigned long long)*(uint64_t *)hw.b,
                   sw.exp, sw.sign ? "-" : "+", (unsigned long long)sw.sig);
        }
    }
}

static unsigned short cw_for(int pc, int rc) { return (unsigned short)(0x7f | (pc << 8) | (rc << 10)); }

static tb hw_add(tb a, tb b, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        fld tbyte ptr b
        faddp st(1), st
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_mul(tb a, tb b, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        fld tbyte ptr b
        fmulp st(1), st
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_sub(tb a, tb b, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm fnstcw old
    __asm fldcw cw
    __asm fld tbyte ptr a
    __asm fld tbyte ptr b
    __asm fsubp st(1), st          /* st1 = st1 - st0 = a - b (MASM's fsubp encoding) */
    __asm fstp tbyte ptr r
    __asm fldcw old
    return r;
}

static tb hw_div(tb a, tb b, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm fnstcw old
    __asm fldcw cw
    __asm fld tbyte ptr a
    __asm fld tbyte ptr b
    __asm fdivp st(1), st
    __asm fstp tbyte ptr r
    __asm fldcw old
    return r;
}

static uint32_t hw_f32(tb a, unsigned short cw)
{
    uint32_t r;
    unsigned short old;
    __asm fnstcw old
    __asm fldcw cw
    __asm fld tbyte ptr a
    __asm fstp dword ptr r
    __asm fldcw old
    return r;
}

static uint64_t hw_f64(tb a, unsigned short cw)
{
    uint64_t r;
    unsigned short old;
    __asm fnstcw old
    __asm fldcw cw
    __asm fld tbyte ptr a
    __asm fstp qword ptr r
    __asm fldcw old
    return r;
}

static int32_t hw_i32(tb a, unsigned short cw)
{
    int32_t r;
    unsigned short old;
    __asm fnstcw old
    __asm fldcw cw
    __asm fld tbyte ptr a
    __asm fistp dword ptr r
    __asm fldcw old
    return r;
}

static tb hw_rndint(tb a, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        frndint
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_f2xm1(tb a, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        f2xm1
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_sin(tb a, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        fsin
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_cos(tb a, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr a
        fcos
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_yl2x(tb y, tb x, unsigned short cw)
{
    tb r;
    unsigned short old;
    __asm {
        fnstcw old
        fldcw cw
        fld tbyte ptr y
        fld tbyte ptr x
        fyl2x
        fstp tbyte ptr r
        fldcw old
    }
    return r;
}

static tb hw_scale(tb a, tb b)
{
    tb r;
    __asm fld tbyte ptr b
    __asm fld tbyte ptr a
    __asm fscale
    __asm fstp tbyte ptr r
    __asm fstp st(0)
    return r;
}

static tb hw_const(int which)
{
    tb r;
    if (which == 0) __asm {
        fld1
        fstp tbyte ptr r
    }
    if (which == 1) __asm {
        fldl2t
        fstp tbyte ptr r
    }
    if (which == 2) __asm {
        fldl2e
        fstp tbyte ptr r
    }
    if (which == 3) __asm {
        fldpi
        fstp tbyte ptr r
    }
    if (which == 4) __asm {
        fldlg2
        fstp tbyte ptr r
    }
    if (which == 5) __asm {
        fldln2
        fstp tbyte ptr r
    }
    return r;
}

static tb hw_ld_f32(uint32_t v)
{
    tb r;
    __asm {
        fld dword ptr v
        fstp tbyte ptr r
    }
    return r;
}
static tb hw_ld_i32(int32_t v)
{
    tb r;
    __asm {
        fild dword ptr v
        fstp tbyte ptr r
    }
    return r;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 200000;
    static const int pcs[3] = { FX_PC24, FX_PC53, FX_PC64 };
    for (int c = 0; c < 6; c++) cmp("const", hw_const(c), fx_const(c), fx_zero(0), fx_zero(0));
    for (long i = 0; i < n; i++) {
        int pc = pcs[i % 3];
        fx_env env = { pc, FX_RN };
        unsigned short cw = cw_for(pc, FX_RN);
        fx80 a = (i & 4) ? rnd_ext(40) : rnd_dbl(40), b = (i & 8) ? rnd_ext(40) : rnd_dbl(40);
        if (i % 7 == 0) b.exp = a.exp - (uint16_t)(rnd() % 70);     /* close exponents: cancellation */
        if (i % 11 == 0) { b = a; b.sign ^= 1; b.sig ^= rnd() & 0xffff; }
        tb ta = to_tb(a), tbb = to_tb(b);
        cmp("add", hw_add(ta, tbb, cw), fx_add(a, b, env), a, b);
        cmp("sub", hw_sub(ta, tbb, cw), fx_sub(a, b, env), a, b);
        cmp("mul", hw_mul(ta, tbb, cw), fx_mul(a, b, env), a, b);
        cmp("div", hw_div(ta, tbb, cw), fx_div(a, b, env), a, b);
        /* stores and integer conversion, both rounding modes */
        fx_env rn = { FX_PC53, FX_RN }, rz = { FX_PC53, FX_RZ };
        uint32_t f = hw_f32(ta, cw_for(FX_PC53, FX_RN));
        checks++;
        if (f != fx_to_f32(a, rn) && fails++ < 30) printf("FAIL f32 %04x:%016llx\n", a.exp, (unsigned long long)a.sig);
        uint64_t d = hw_f64(ta, cw_for(FX_PC53, FX_RN));
        checks++;
        if (d != fx_to_f64(a, rn) && fails++ < 30) printf("FAIL f64 %04x:%016llx\n", a.exp, (unsigned long long)a.sig);
        fx80 small = a;
        small.exp = (uint16_t)(16383 + rnd() % 30);
        tb ts = to_tb(small);
        checks += 2;
        if (hw_i32(ts, cw_for(FX_PC53, FX_RZ)) != (int32_t)fx_to_int(small, rz, 32) && fails++ < 30) printf("FAIL i32 rz\n");
        if (hw_i32(ts, cw_for(FX_PC53, FX_RN)) != (int32_t)fx_to_int(small, rn, 32) && fails++ < 30) printf("FAIL i32 rn\n");
        fx80 frac = a;
        frac.exp = (uint16_t)(16383 - 3 + rnd() % 70);
        cmp("rndint", hw_rndint(to_tb(frac), cw), fx_rndint(frac, env), frac, frac);
        /* subnormal float stores */
        fx80 tiny = a;
        tiny.exp = (uint16_t)(16383 - 126 - rnd() % 30);
        tb tt = to_tb(tiny);
        checks++;
        if (hw_f32(tt, cw_for(FX_PC53, FX_RN)) != fx_to_f32(tiny, rn) && fails++ < 30)
            printf("FAIL f32 subnormal %04x:%016llx\n", tiny.exp, (unsigned long long)tiny.sig);
        uint32_t fb = (uint32_t)rnd();
        if (((fb >> 23) & 0xff) != 0xff) cmp("ld f32", hw_ld_f32(fb), fx_from_f32(fb), a, b);
        int32_t iv = (int32_t)rnd();
        cmp("fild", hw_ld_i32(iv), fx_from_i64(iv), a, b);
        /* fscale by a small integer */
        fx80 sc = fx_from_i64((int64_t)(rnd() % 41) - 20);
        cmp("fscale", hw_scale(ta, to_tb(sc)), fx_scale(a, sc), a, sc);
        /* transcendentals */
        fx80 u = a;
        u.exp = (uint16_t)(16383 - 1 - rnd() % 20);      /* |u| < 1 */
        cmp("f2xm1", hw_f2xm1(to_tb(u), cw), fx_f2xm1(u), u, u);
        fx80 t = a;
        t.exp = (uint16_t)(16383 - 3 + rnd() % 5);       /* up to 16 */
        cmp("fsin", hw_sin(to_tb(t), cw), fx_sin(t), t, t);
        cmp("fcos", hw_cos(to_tb(t), cw), fx_cos(t), t, t);
    }
    /* the synthesizer's own arguments: 2 pi F / fs and pi B / fs for integer F, B, fs 11025 */
    for (int F = 0; F <= 11025; F++) {
        double x = 2 * 3.14159265358979323846 * F / 11025.0;
        fx80 a = fx_from_f64(*(uint64_t *)&x);
        tb ta = to_tb(a);
        fx_env env = { FX_PC53, FX_RN };
        cmp("fcos(2piF/fs)", hw_cos(ta, cw_for(FX_PC53, FX_RN)), fx_cos(a), a, a);
        cmp("fsin(2piF/fs)", hw_sin(ta, cw_for(FX_PC53, FX_RN)), fx_sin(a), a, a);
        (void)env;
    }
    printf("%ld checks, %ld failures\n", checks, fails);

    /* how does the hardware round its transcendentals? For each, the share of random arguments whose
     * hardware result equals the exact value rounded each way. */
    {
        static const char *names[4] = { "fsin", "fcos", "f2xm1", "fyl2x" };
        static const char *modes[4] = { "nearest", "down", "up", "zero" };
        for (int op = 0; op < 4; op++) {
            long match[4] = { 0 }, total = 0, any = 0;
            for (int k = 0; k < 20000; k++) {
                fx80 t = rnd_ext(0);
                t.exp = (uint16_t)(op == 2 ? 16383 - 1 - rnd() % 20 : op == 3 ? 16383 - 30 + rnd() % 60 : 16383 - 3 + rnd() % 5);
                fx80 y = (k & 1) ? fx_const(5) : rnd_dbl(10);    /* ln 2 as log() uses it, or a pow() exponent */
                if (op == 3) t.sign = 0;
                if (op == 3 && k % 4 == 2) { t.exp = 16383 - (k & 8 ? 1 : 0); t.sig = (k & 8 ? ~0ull : 0x8000000000000000ull) ^ (rnd() & 0xfffffff); }  /* near 1 */
                unsigned short cw = cw_for(FX_PC53, FX_RN);
                tb hw = op == 0 ? hw_sin(to_tb(t), cw) : op == 1 ? hw_cos(to_tb(t), cw) : op == 2 ? hw_f2xm1(to_tb(t), cw)
                      : hw_yl2x(to_tb(y), to_tb(t), cw);
                int ok = 0;
                for (int rc = 0; rc < 4; rc++) {
                    fx_set_transcendental_rounding(rc);
                    fx80 sw = op == 0 ? fx_sin(t) : op == 1 ? fx_cos(t) : op == 2 ? fx_f2xm1(t) : fx_yl2x(y, t);
                    tb s = to_tb(sw);
                    if (!memcmp(hw.b, s.b, 10)) { match[rc]++; ok = 1; }
                }
                any += ok;
                total++;
            }
            fx_set_transcendental_rounding(FX_RN);
            printf("%-6s", names[op]);
            for (int rc = 0; rc < 4; rc++) printf("  %s %.3f%%", modes[rc], 100.0 * match[rc] / total);
            printf("  within one ulp of exact %.3f%%\n", 100.0 * any / total);
        }
    }
    return fails != 0;
}
