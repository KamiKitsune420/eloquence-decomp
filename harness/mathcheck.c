/* mathcheck - the 32-bit msvcrt.dll's log/exp/pow against src/x87math.c, all 80 bits of st0.
 *
 *   mathcheck math.log        the arguments eloq_run logged (ELOQ_MATHLOG=file)
 *   mathcheck --random N      N random arguments of each kind
 *
 * math.log has "fn x y result" per call, doubles in hex (fn 0 log, 1 exp, 2 pow). Every call runs with
 * ENU.SYN's control word 0x027f. A difference that survives rounding to double is counted separately:
 * that is the kind that could change what the engine computes.
 */
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/x87math.h"

typedef struct { uint8_t b[10]; } tb;

/* the hardware control word as is (_control87 takes an abstract layout) */
static void set_cw(unsigned short cw) { __asm fldcw cw }

static void *lg, *ex, *pw;

static tb real1(void *fn, double x)
{
    tb r;
    __asm {
        sub esp, 8
        fld x
        fstp qword ptr [esp]
        call fn
        add esp, 8
        fstp tbyte ptr r
    }
    return r;
}

static tb real2(void *fn, double x, double y)
{
    tb r;
    __asm {
        sub esp, 16
        fld y
        fstp qword ptr [esp + 8]
        fld x
        fstp qword ptr [esp]
        call fn
        add esp, 16
        fstp tbyte ptr r
    }
    return r;
}

static const char *names[3] = { "log", "exp", "pow" };
static long n[3], diff80[3], diff64[3];

static void check(int fn, double x, double y)
{
    fx80 got = fn == 0 ? x87m_log(x) : fn == 1 ? x87m_exp(x) : x87m_pow(x, y, 0x027f);
    tb want = fn == 0 ? real1(lg, x) : fn == 1 ? real1(ex, x) : real2(pw, x, y);
    {
        unsigned mx = 0x1f80;
        __asm fninit         /* msvcrt's error paths can leave the register stack uneven, and MXCSR changed */
        __asm ldmxcsr mx
    }
    set_cw(0x027f);
    tb g;
    fx_to_tbyte(got, g.b);
    n[fn]++;
    if (!memcmp(want.b, g.b, 10)) return;
    if (diff80[fn]++ < 3)
        printf("  st0 %s x=%.17g y=%.17g: msvcrt %02x%02x:%016llx ours %02x%02x:%016llx\n", names[fn], x, y, want.b[9],
               want.b[8], *(unsigned long long *)want.b, g.b[9], g.b[8], *(unsigned long long *)g.b);
    fx_env e = { FX_PC53, FX_RN };
    uint64_t wd = fx_to_f64(fx_from_tbyte(want.b), e), gd = fx_to_f64(got, e);
    if (wd != gd) {
        if (diff64[fn]++ < 10)
            printf("%s x=%.17g y=%.17g: msvcrt %02x%02x:%016llx ours %02x%02x:%016llx\n", names[fn], x, y, want.b[9],
                   want.b[8], *(unsigned long long *)want.b, g.b[9], g.b[8], *(unsigned long long *)g.b);
    }
}

static double d(unsigned long long b) { double v; memcpy(&v, &b, 8); return v; }

static unsigned long long rng = 88172645463325252ull;
static unsigned long long next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double uniform(double lo, double hi) { return lo + (hi - lo) * (double)(next() >> 11) / 9007199254740992.0; }

static double arg(int fn, int which)
{
    switch (next() % 3) {
    case 0: return d(next() & 0x7fffffffffffffffull) * ((fn == 1 || which) && (next() & 1) ? -1 : 1);
    case 1: return fn == 0 ? uniform(0.5, 2) : fn == 1 ? uniform(-5, 5) : which ? uniform(-8, 8) : uniform(0, 4);
    default: return fn == 0 ? uniform(0, 4) * pow(2, (int)(next() % 80) - 40)
                  : fn == 1 ? uniform(-700, 700) : which ? (double)(int)(next() % 21) - 10 : uniform(-4, 4);
    }
}

int main(int argc, char **argv)
{
    HMODULE m = LoadLibraryA("msvcrt.dll");
    lg = GetProcAddress(m, "log"); ex = GetProcAddress(m, "exp"); pw = GetProcAddress(m, "pow");
    if (!lg || !ex || !pw) { fprintf(stderr, "no msvcrt\n"); return 2; }
    set_cw(0x027f);      /* the engine's x87 control word */
    if (argc > 2 && !strcmp(argv[1], "--random")) {
        long k = atol(argv[2]);
        for (int fn = 0; fn < 3; fn++)
            for (long i = 0; i < k; i++) {
                double x = arg(fn, 0), y = arg(fn, 1);
                if (fn != 1 && isfinite(x) && !(x > 0) && (next() & 1)) x = -x;
                check(fn, x, y);
            }
    } else if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
        int fn;
        unsigned long long x, y, r;
        while (fscanf(f, "%d %llx %llx %llx", &fn, &x, &y, &r) == 4) check(fn, d(x), d(y));
    } else {
        fprintf(stderr, "usage: mathcheck math.log | --random N\n");
        return 2;
    }
    int bad = 0;
    for (int fn = 0; fn < 3; fn++) {
        printf("%s: %ld calls, %ld differ in st0, %ld as a double\n", names[fn], n[fn], diff80[fn], diff64[fn]);
        bad |= diff64[fn] != 0;
    }
    return bad;
}
