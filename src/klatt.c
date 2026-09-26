/* klatt - Eloquence 6.1's formant synthesizer, hand-ported from ENU.SYN (FUN_1013caf0 and helpers).
 *
 * ---------------------------------------------------------------------------------------------------
 * What it is
 *
 * A Klatt-style cascade/parallel formant synthesizer working in blocks (up to 200 samples at a time)
 * rather than sample by sample:
 *
 *   glottal source   A polynomial (KLGLOTT88-like) pulse, u = g - b*g^2 with g rising by a each sample,
 *                    during the open part of each pitch period, silence during the closed part.
 *                    The pitch period, open quotient and amplitude come from the frame; F0 can be
 *                    modulated by a three-sine "flutter", and alternate periods can be delayed and
 *                    weakened ("diplophonia"). The pulse train is low-passed by a spectral-tilt
 *                    resonator (block 0).
 *   aspiration       16-bit LCG noise, halved during the glottal open phases when voicing is on,
 *                    added to the source.
 *   cascade branch   nasal pole 1, nasal zero 1, nasal pole 2, nasal zero 2, then the formant
 *                    resonators from the highest down to F1.
 *   parallel branch  frication noise (its own LCG stream) through the parallel formant resonators,
 *                    alternating in sign, plus a bypass path; after frication stops each resonator
 *                    keeps ringing (a two-pole oscillator without input) until its hold time runs out.
 *   output           truncated to int32, peak tracked, gain applied, handed to the output callback.
 *
 * Resonator coefficients are the classic ones: C = -exp(-2 pi B / fs), B = 2 exp(-pi B / fs)
 * cos(2 pi F / fs), A = 1 - B - C; when a cascade resonator's frequency or bandwidth changes, B and C
 * are ramped linearly over the first 3 samples of the block (quarter steps from the old values).
 * Anti-resonators use the inverted coefficients (1/A, -B/A, -C/A). cos and exp are cached per block
 * and recomputed only when the frequency or bandwidth changes.
 *
 * The "hold" timers (voicing at +0x149b, frication at +0x149f) are set to 20 ms whenever the frame has
 * voicing/aspiration or frication and count down by the block duration when they stop, so the filters
 * ring out; while a hold timer is zero its branch is not computed at all.
 *
 * ---------------------------------------------------------------------------------------------------
 * Arithmetic
 *
 * The original is x87 code run with the control word 0x027f: 53-bit precision, round to nearest. Every
 * x87 add/sub/mul/div of float or double operands is then exactly the IEEE double operation, so the port
 * computes in double and rounds to float exactly where the original stores to a float (fst/fstp dword).
 * The ORDER of operations is the original's everywhere (x + y is commutative in IEEE arithmetic, but
 * (x + y) + z is not x + (y + z)); where an intermediate stays on the x87 stack unrounded across a store
 * the port keeps the double too (see e.g. the resonator coefficients and the glottal phase).
 * Products of two floats are exact in double, which is why their operand order never matters.
 * The transcendentals (fcos, fsin, f2xm1) return 64-bit-precision results; they are computed with fx80
 * on the exact 80-bit operand and their results are rounded exactly as the x87 does (fx80 PC53 for
 * arithmetic on them, fx_to_f32 for stores). _ftol truncates toward zero to int64 and returns the low
 * 32 bits (ftol() below).
 * NaNs are not expected: x87 comparisons count "unordered" as equal/less, C comparisons do not.
 *
 * ---------------------------------------------------------------------------------------------------
 * The state block (byte offsets; f = float, i = int32, s = int16, b = byte, p = 32-bit guest pointer)
 *
 * All access goes through getf/setf/geti/seti (memcpy), since the fields are unaligned.
 */
#include "klatt.h"

#include "fx80.h"

#include <stdio.h>
#include <string.h>

enum {
    S_ID           = 0x0000, /* p  -> "\r\nKlattID version..." in the image: marks a valid handle */
    S_COOKIE       = 0x0004, /* i  passed to the output callback */
    S_NFRAMES      = 0x0008, /* i  frames synthesized */
    S_GAIN         = 0x004a, /* f  output gain (1.0 = leave the samples alone) */
    S_BLOCKS       = 0x0056, /* 21 filter blocks of 0x50 bytes: see BLK_* and RES_* below */
    S_ZERO_COEF    = 0x06f2, /* f[2][3] inverted (anti-resonator) A, B, C of the two nasal zeros */
    S_OUT          = 0x070a, /* i[200] output samples of the current block */
    S_TIME_SCALE   = 0x0a2a, /* f  frame duration multiplier */
    S_SAMPLE_RATE  = 0x0a2e, /* f  fs */
    S_SOURCE_ONLY  = 0x0a46, /* i  nonzero: output the filtered glottal source, no filters/noise */
    S_OUT_MODE     = 0x0a63, /* i  2: call the output callback */
    S_BUF_PTR      = 0x0d93, /* p  -> main sample buffer (state + 0xa73), 2 history floats before it */
    S_BYPASS_GAIN  = 0x0d97, /* f  gain of frication noise straight to the output */
    S_FRIC         = 0x0d9b, /* f[200] frication noise of the current block */
    S_PBUF_PTR     = 0x13e3, /* p  -> parallel-branch scratch buffer (state + 0x10c3), 2 history floats */
    S_COS          = 0x13e7, /* f[21] cos(2 pi F / fs) per block */
    S_EXP          = 0x143b, /* f[21] exp(-pi B / fs) per block */
    S_TIME_MS      = 0x1497, /* f  running time in ms, the flutter's phase */
    S_VOICE_HOLD   = 0x149b, /* f  ms the cascade branch keeps running (20 while voiced/aspirated) */
    S_FRIC_HOLD    = 0x149f, /* f  ms the parallel branch keeps running (20 while frication) */
    S_BLOCK_MS     = 0x14a3, /* f  duration of the current block in ms */
    S_DIPLO_RATIO  = 0x14a7, /* f  amplitude ratio of the delayed (alternate) pulses */
    S_DIPLO        = 0x14ab, /* f  frame[6]: diplophonia amount (0..100) */
    S_F0           = 0x14af, /* f  F0 * 10 (frame[1]), after flutter */
    S_T0_MS        = 0x14b3, /* f  pitch period in ms */
    S_T0_SAMPLES   = 0x14b7, /* f  pitch period in samples */
    S_OQ           = 0x14bb, /* f  open quotient (frame[3] / 100) */
    S_PHASE_MS     = 0x14bf, /* f  where the next period starts relative to the block, in ms */
    S_PI_FS        = 0x14c7, /* f  pi / fs */
    S_2PI_FS       = 0x14cb, /* f  2 pi / fs */
    S_SPMS         = 0x14cf, /* f  samples per ms */
    S_MSPS         = 0x14d3, /* f  ms per sample */
    S_GLOT_G       = 0x14d7, /* f  pulse generator g, kept when an open phase crosses a block */
    S_GLOT_A       = 0x14db, /* f  slope a of the current pulse */
    S_GLOT_B       = 0x14df, /* f  curvature b of the current pulse */
    S_ALT_A        = 0x14e3, /* f  a, b of the alternate (diplophonic) pulses */
    S_ALT_B        = 0x14e7,
    S_NORM_A       = 0x14eb, /* f  a, b of normal pulses */
    S_NORM_B       = 0x14ef,
    S_NBLOCK       = 0x14f3, /* i  samples in the current block */
    S_PERIOD       = 0x14f7, /* i  samples in the current period */
    S_DIPLO_DELAY  = 0x14fb, /* i  samples the alternate pulses are delayed by */
    S_OPEN         = 0x14ff, /* i  open-phase samples of the period */
    S_OPEN_NOW     = 0x1503, /* i  open-phase samples produced in this block */
    S_CLOSED       = 0x1507, /* i  closed-phase samples of the period */
    S_CLOSED_NOW   = 0x150b, /* i  closed-phase samples of this period (less the delay) */
    S_PEND_DELAY   = 0x150f, /* i  delay samples still due at the start of the next block */
    S_PEND_OPEN    = 0x1513, /* i  open-phase samples still due */
    S_OPEN_LEN     = 0x1517, /* i  copy of the open-phase length (not read here) */
    S_SLOPE_INT    = 0x151b, /* i  ftol(a) (not read here) */
    S_PEND_CLOSED  = 0x151f, /* i  closed-phase samples still due */
    S_TOTAL        = 0x1523, /* i  samples synthesized */
    S_PEAK         = 0x1527, /* i  largest |sample| */
    S_SEG          = 0x152b, /* i[] lengths of alternating open / not-open stretches of the block */
    S_NRES         = 0x184b, /* i  number of cascade (and parallel) formants, 5..8 */
    S_TILT         = 0x184f, /* i  spectral-tilt index (frame[4], 0..35) */
    S_AV           = 0x1853, /* i  voicing amplitude, dB (frame[2]) */
    S_AH           = 0x1857, /* i  aspiration amplitude, dB (frame[7]) */
    S_AF           = 0x185b, /* i  frication amplitude, dB (frame[8]) */
    S_GAIN_VOICE   = 0x185f, /* i  dB offsets from KlattSetConstParms */
    S_GAIN_MASTER  = 0x1863,
    S_GAIN_ASP     = 0x1867,
    S_GAIN_FRIC    = 0x186b,
    S_NSEG         = 0x186f, /* i  index of the last S_SEG entry */
    S_JITTER       = 0x1877, /* i  flutter's F0 change */
    S_NOISE        = 0x187b, /* s[200] LCG noise of the current block */
    S_ASP_SEED     = 0x1a0b, /* s  LCG state, aspiration */
    S_FRIC_SEED    = 0x1a0d, /* s  LCG state, frication */
    S_DIPLO_ON     = 0x1a0f, /* b  diplophonia active */
    S_DIPLO_ALT    = 0x1a10, /* b  next pulse is an alternate one */
    S_OUT_RESULT   = 0x1a11, /* b  the output callback's result */
    S_F1_MOD       = 0x1a12, /* b  open-phase F1 modulation (see the note in synth_block) */
    S_NOISE_FLAG   = 0x1adc, /* b  cleared by the noise generator */
    S_OUT_ON       = 0x1add, /* b  output enabled */
    S_RATE_MODE    = 0x1ade  /* i  1: fs = 11025, 0: fs = 8000 (tilt filter from tables), 2: other */
};

/* The filter blocks, 0x50 bytes each at S_BLOCKS + 0x50 * k. */
enum {
    BLK_TILT  = 0,          /* spectral tilt (glottal low-pass) */
    BLK_ZERO1 = 1,          /* nasal zero 1 (anti-resonator), frame[29], frame[30] */
    BLK_ZERO2 = 2,          /* nasal zero 2, frame[33], frame[34] */
    BLK_POLE1 = 3,          /* nasal pole 1, frame[27], frame[28] */
    BLK_POLE2 = 4,          /* nasal pole 2, frame[31], frame[32] */
    BLK_CASC  = 5,          /* cascade F1..F8: blocks 5..12 */
    BLK_PAR   = 13,         /* parallel F1..F8: blocks 13..20 */
    NBLOCKS   = 21
};

/* Fields of a filter block. Resonator: y = A x + B y1 + C y2. Anti-resonator: y = A x + B x1 + C x2
 * (history of inputs; its coefficients live at S_ZERO_COEF). */
enum {
    RES_A      = 0x00,      /* f */
    RES_B      = 0x04,      /* f */
    RES_C      = 0x08,      /* f */
    RES_RAMP_A = 0x0c,      /* f[3] A, B, C for the first samples of a block after a change */
    RES_RAMP_B = 0x18,      /* f[3] */
    RES_RAMP_C = 0x24,      /* f[3] */
    RES_Y1     = 0x30,      /* f  previous output (anti-resonator: input) */
    RES_Y2     = 0x34,      /* f  the one before */
    RES_FREQ   = 0x38,      /* f  frequency / bandwidth the coefficients were last made from */
    RES_BW     = 0x3c,      /* f */
    RES_PREV_B = 0x40,      /* f  B and C at the end of the previous frame: the ramps start there */
    RES_PREV_C = 0x44,      /* f */
    RES_ON     = 0x48,      /* f  nonzero = active (the hold time it was switched on with, or ms left) */
    RES_RAMP_N = 0x4c       /* i  ramp samples left (3 after a change) */
};

/* Image data. */
#define IMG_ID_STRING   0x101448c0u   /* "\r\nKlattID version..." */
#define IMG_TILT_FREQ   0x10144910u   /* f[36] spectral-tilt bandwidths by S_TILT (5000 .. 469 Hz) */
#define IMG_DB_HI       0x101449b8u   /* f[21] 10^(20k/20)-style steps: lin(dB) = HI[dB/20] * LO[dB%20] */
#define IMG_DB_LO       0x10144a0cu   /* f[20] */
#define IMG_TILT_11K    0x10144a60u   /* f[36][3] tilt resonator A, B, C at 11025 Hz */
#define IMG_TILT_8K     0x10144c58u   /* f[36][3] at 8000 Hz */

/* Numeric constants, written as literals; klatt_check_constants() compares each with the image. */
#define K_ZERO          0.0f            /* 0x10144828 = 0x00000000 */
#define K_ONE           1.0f            /* 0x10144ea8 = 0x3f800000 */
#define K_QUARTER       0.25f           /* 0x10144ea4 = 0x3e800000: ramp step */
#define K_BYPASS        -3.35987593e-9f /* 0x10144ea0 = 0xb166e390: bypass gain scale */
#define K_PAR_SIGN      3.35987593e-9f  /* 0x3166e390, an immediate: parallel gain scale, sign alternates */
#define K_HOLD_ON       9.99999975e-5f  /* 0x10144e9c = 0x38d1b717: hold timer "running" threshold */
#define K_HOLD_FLOOR    9.99999975e-6f  /* 0x10144eac = 0x3727c5ac */
#define K_HOLD_ON_D     1e-05           /* 0x10144eb0 = 0x3ee4f8b588e368f1 (double) */
#define K_PERCENT       0.00999999978f  /* 0x10144e98 = 0x3c23d70a: 0.01 */
#define K_ASP           1.19212928e-5f  /* 0x10144e94 = 0x37480190: aspiration gain scale */
#define K_FLUTTER1      0.0797964558f   /* 0x10144e90 = 0x3da36c53: flutter angular rates, rad/ms */
#define K_FLUTTER2      0.0446106158f   /* 0x10144e8c = 0x3d36b99f */
#define K_FLUTTER3      0.0295309704f   /* 0x10144e88 = 0x3cf1eaef */
#define K_FLUTTER_DEPTH 0.000199999995f /* 0x10144e84 = 0x3951b717 */
#define K_T0_SCALE      10000.0f        /* 0x10144e80 = 0x461c4000: period ms = 10000 / (F0*10) */
#define K_GLOT_B        1.5f            /* 0x10144e7c = 0x3fc00000 */
#define K_GLOT_A        18.4687519f     /* 0x10144e78 = 0x4193c001 */
#define K_TILT_MAX      35.0f           /* 0x10144e74 = 0x420c0000 */
#define K_TILT_FREQ     0.375f          /* 0x10144e70 = 0x3ec00000: tilt resonator F = 0.375 B */
#define K_ROUND_PERIOD  0.9999          /* 0x10144e68 = 0x3fefff2e48e8a71e (double) */
#define K_ROUND_OPEN    0.4999          /* 0x10144e60 = 0x3fdffe5c91d14e3c (double) */
#define K_PER_MS        0.00100000005f  /* 0x10144e54 = 0x3a83126f: 1/1000 */
#define K_ZERO_D        0.0             /* 0x101448b0 = 0 (double) */
#define K_ONE_D         1.0             /* 0x101442b0 = 0x3ff0000000000000 (double) */

#define HOLD_MS         20.0f           /* 0x41a00000, an immediate */
#define MAX_BLOCK       200
#define NOISE_MUL       0x4e6d          /* LCG: x = x * 20077 + 12345 (16 bits) */
#define NOISE_ADD       0x3039

/* ------------------------------------------------------------------------------------------ access */

static float getf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static void setf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static int32_t geti(const uint8_t *p) { int32_t v; memcpy(&v, p, 4); return v; }
static void seti(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }
static int16_t get16(const uint8_t *p) { int16_t v; memcpy(&v, p, 2); return v; }
static void set16(uint8_t *p, int16_t v) { memcpy(p, &v, 2); }

#define F(off)          getf(s + (off))
#define SETF(off, v)    setf(s + (off), (v))
#define I(off)          geti(s + (off))
#define SETI(off, v)    seti(s + (off), (v))

#define BLK(k)          (s + S_BLOCKS + 0x50 * (k))
#define RF(blk, f)      getf((blk) + (f))
#define SETRF(blk, f, v) setf((blk) + (f), (v))
#define COSK(k)         (S_COS + 4 * (k))
#define EXPK(k)         (S_EXP + 4 * (k))

/* sample buffers: arrays of floats inside the state, indexed from the buffer start (-1, -2 = history) */
static float bget(const uint8_t *b, int i) { return getf(b + 4 * (ptrdiff_t)i); }
static void bset(uint8_t *b, int i, float v) { setf(b + 4 * (ptrdiff_t)i, v); }
static void bclear(uint8_t *b, int from, int count)
{
    if (count > 0) memset(b + 4 * (ptrdiff_t)from, 0, 4 * (size_t)count);
}

static float imgf(const klatt_ctx *ctx, uint32_t addr)
{
    const void *p = ctx->img(ctx->user, addr, 4);
    float v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/* ------------------------------------------------------------------------------- x87 equivalents */

static const fx_env PC53 = { FX_PC53, FX_RN };

static uint64_t dbits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static double bitsd(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static float bitsf(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

/* msvcrt _ftol: truncate to int64 (fistp qword with RC = chop), return the low 32 bits */
static int32_t ftol(double v)
{
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) return 0;   /* integer indefinite */
    return (int32_t)(uint32_t)(uint64_t)(int64_t)v;
}

/* fld w; fmul f; fcos; fstp dword -> cos(2 pi F / fs) */
static float x87_cos(float w, float f)
{
    return bitsf(fx_to_f32(fx_cos(fx_from_f64(dbits((double)w * f))), PC53));
}

/* fld w; fmul b; fchs; fldl2e; fmulp; 2^x by frndint/f2xm1/fld1/faddp/fscale; fstp dword
 * -> exp(-pi B / fs) */
static float x87_exp_neg(float w, float b)
{
    fx80 y = fx_mul(fx_from_f64(dbits(-((double)w * b))), fx_const(2), PC53);   /* * log2(e) */
    fx80 n = fx_rndint(y, PC53);
    fx80 m = fx_add(fx_f2xm1(fx_sub(y, n, PC53)), fx_const(0), PC53);           /* 2^frac */
    return bitsf(fx_to_f32(fx_scale(m, n), PC53));
}

/* dB -> linear amplitude from the two image tables: 0 for dB <= 0, capped at 400; exact in double */
static double db_to_lin(const klatt_ctx *ctx, int db)
{
    if (db <= 0) return K_ZERO;
    if (db >= 400) db = 400;
    return (double)imgf(ctx, IMG_DB_HI + 4 * (uint32_t)(db / 20)) * imgf(ctx, IMG_DB_LO + 4 * (uint32_t)(db % 20));
}

/* ---------------------------------------------------------------------------------- filter blocks */

/* The block's history goes into x[-2], x[-1] before a run and is taken from the end of the run after it,
 * as the original does (the buffer's history slots are part of the state). */
static void save_history(uint8_t *blk, const uint8_t *x, int n, int end)
{
    if (n > 1) {
        SETRF(blk, RES_Y2, bget(x, end - 2));
        SETRF(blk, RES_Y1, bget(x, end - 1));
    } else {
        SETRF(blk, RES_Y2, RF(blk, RES_Y1));
        SETRF(blk, RES_Y1, bget(x, end - 1));
    }
}

/* Two-pole resonator, in place: y[i] = A x[i] + B y[i-1] + C y[i-2]. The first RES_RAMP_N samples use
 * the ramp coefficients (FUN_1013e9e0). */
static void resonate(uint8_t *blk, uint8_t *x, int n)
{
    if (RF(blk, RES_ON) == K_ZERO) return;
    bset(x, -2, RF(blk, RES_Y2));
    bset(x, -1, RF(blk, RES_Y1));
    int i = 0, ramp = geti(blk + RES_RAMP_N);
    if (ramp != 0) {
        int m = ramp < n ? ramp : n;
        for (int k = 0; k < m; k++) {
            int j = 3 - ramp + k;              /* ramp step: 0, 1, 2 */
            double y = (double)bget(x, k - 1) * RF(blk, RES_RAMP_B + 4 * j);
            y += (double)RF(blk, RES_RAMP_A + 4 * j) * bget(x, k);
            y += (double)RF(blk, RES_RAMP_C + 4 * j) * bget(x, k - 2);
            bset(x, k, (float)y);
        }
        if (m > 0) i = m;
        seti(blk + RES_RAMP_N, ramp - m);
    }
    const float a = RF(blk, RES_A), b = RF(blk, RES_B), c = RF(blk, RES_C);
    int end = i;
    for (; i < n; i++) {
        double y = (double)bget(x, i - 2) * c;
        y += (double)b * bget(x, i - 1);
        y += (double)a * bget(x, i);
        bset(x, i, (float)y);
        end = n;
    }
    save_history(blk, x, n, end);
}

/* Two-zero anti-resonator, in place: y[i] = A x[i] + B x[i-1] + C x[i-2] with the inverted coefficients
 * at coef (the ramp coefficients of blk are inverted in place) (FUN_1013eac0). */
static void antiresonate(uint8_t *blk, const uint8_t *coef, uint8_t *x, int n)
{
    if (RF(blk, RES_ON) == K_ZERO) return;
    float x1 = RF(blk, RES_Y1), x2 = RF(blk, RES_Y2);
    int i = 0, ramp = geti(blk + RES_RAMP_N);
    if (ramp != 0) {
        int m = ramp < n ? ramp : n;
        for (int k = 0; k < m; k++) {
            int j = 3 - ramp + k;
            float in = bget(x, k);
            double y = (double)x2 * RF(blk, RES_RAMP_C + 4 * j);
            y += (double)RF(blk, RES_RAMP_A + 4 * j) * in;
            y += (double)x1 * RF(blk, RES_RAMP_B + 4 * j);
            bset(x, k, (float)y);
            x2 = x1;
            x1 = in;
        }
        if (m > 0) i = m;
        seti(blk + RES_RAMP_N, ramp - m);
    }
    const float a = getf(coef), b = getf(coef + 4), c = getf(coef + 8);
    for (; i < n; i++) {
        float in = bget(x, i);
        double y = (double)x2 * c;
        y += (double)in * a;
        y += (double)x1 * b;
        bset(x, i, (float)y);
        x2 = x1;
        x1 = in;
    }
    if (n > 1) {
        SETRF(blk, RES_Y1, x1);
        SETRF(blk, RES_Y2, x2);
    } else {
        SETRF(blk, RES_Y2, RF(blk, RES_Y1));
        SETRF(blk, RES_Y1, x1);
    }
}

/* A resonator without input, ringing from its history: y[i] = B y[i-1] + C y[i-2] (FUN_1013c650). */
static void ring(uint8_t *blk, uint8_t *x, int n)
{
    bset(x, -2, RF(blk, RES_Y2));
    bset(x, -1, RF(blk, RES_Y1));
    const float b = RF(blk, RES_B), c = RF(blk, RES_C);
    for (int i = 0; i < n; i++) {
        double y = (double)bget(x, i - 2) * c;
        y += (double)b * bget(x, i - 1);
        bset(x, i, (float)y);
    }
    save_history(blk, x, n, n > 0 ? n : 0);
}

/* Resonator coefficients from the cached exp(-pi B/fs) and cos(2 pi F/fs) of block k. */
static void set_resonator(uint8_t *s, int k)
{
    uint8_t *blk = BLK(k);
    float e = F(EXPK(k));
    SETRF(blk, RES_C, (float)-((double)e * e));
    double b = (double)e * F(COSK(k));
    b = b + b;
    SETRF(blk, RES_B, (float)b);
    SETRF(blk, RES_A, (float)(((double)K_ONE - b) - RF(blk, RES_C)));
}

/* 16-bit LCG noise for the block into S_NOISE; with voicing on it is halved in the open-phase stretches
 * of S_SEG (pairs: open length, closed length). Returns the new seed: the last value before halving
 * (FUN_1013e8f0). */
static uint16_t noise(uint8_t *s, uint16_t seed)
{
    s[S_NOISE_FLAG] = 0;
    uint16_t x = (uint16_t)(seed * NOISE_MUL + NOISE_ADD);
    set16(s + S_NOISE, (int16_t)x);
    int i = 1;
    for (; i < I(S_NBLOCK); i++) {
        x = (uint16_t)(x * NOISE_MUL + NOISE_ADD);
        set16(s + S_NOISE + 2 * i, (int16_t)x);
    }
    uint16_t last = (uint16_t)get16(s + S_NOISE + 2 * (i - 1));
    if (I(S_AV) != 0) {
        int pos = 0, end = I(S_SEG);
        for (int k = 0; k < I(S_NSEG) / 2; k++) {
            for (; pos < end; pos++) {
                int16_t v = get16(s + S_NOISE + 2 * pos);
                set16(s + S_NOISE + 2 * pos, (int16_t)(v < 0 ? ~(~v >> 1) : v >> 1));   /* sar 1 */
            }
            pos += I(S_SEG + 4 * (2 * k + 1));
            end = I(S_SEG + 4 * (2 * k + 2)) + pos;
        }
    }
    return last;
}

/* Apply the output gain and hand the block to the host (FUN_1013c6c0). */
static void emit(const klatt_ctx *ctx, uint8_t *s, int n)
{
    if (!s[S_OUT_ON]) return;
    float gain = F(S_GAIN);
    if ((double)gain != K_ONE_D)
        for (int i = 0; i < n; i++) SETI(S_OUT + 4 * i, ftol((double)I(S_OUT + 4 * i) * gain));
    if (I(S_OUT_MODE) == 2)
        s[S_OUT_RESULT] = ctx->output(ctx->user, (uint32_t)I(S_COOKIE), s + S_OUT, n);
}

/* ----------------------------------------------------------------------------------- the frame */

typedef struct {
    const klatt_ctx *ctx;
    uint8_t *s;
    const float *frame;
    uint8_t *buf;          /* main buffer: source, then everything summed into it */
    uint8_t *pbuf;         /* parallel-branch scratch */
    int nres;
    float freq[NBLOCKS];   /* this frame's frequency and bandwidth per filter block */
    float bw[NBLOCKS];
    int par_db[8];         /* parallel formant amplitudes, dB */
    int fric_gain;         /* S_GAIN_FRIC + S_GAIN_MASTER + AF */
    float asp_amp;         /* aspiration noise gain */
    int first_block;       /* per-frame setup of the source not done yet */
    int remember_from;     /* first block whose frequency/bandwidth is remembered at the end */
    klatt_trace *tr;       /* the original's stack frame, or NULL */
    uint32_t ebp;          /* what the original's ebp holds after the glottal source and the branches */
} synth;

/* ------------------------------------------------------------------ the original's stack frame */

/* Where the original keeps things in its frame (offsets from its esp after the prologue). Its locals are
 * the synth fields above, a few loop variables and x87 scratch; only the values they end up with are
 * recorded (with a trace, see klatt.h). */
enum {
    L_SCRATCH    = 0x10,    /* x87 scratch: n * 1000, the pitch period in samples, the period, ... */
    L_SIGN       = 0x14,    /* the parallel gain's sign */
    L_FILTERED   = 0x18,    /* samples through the tilt filter; also noise and a pointer scratch */
    L_FIRST      = 0x1f,    /* b  first_block */
    L_PAR_K      = 0x20,    /* the parallel formant loop's block number */
    L_ASP_AMP    = 0x24,
    L_REMEMBER   = 0x28,    /* remember_from */
    L_NSAMP      = 0x2c,
    L_LEFT       = 0x30,
    L_BYPASS     = 0x34,
    L_BW         = 0x38,    /* f[21] bw[] */
    L_FREQ       = 0x8c,    /* f[21] freq[] */
    L_PAR_DB     = 0xe0     /* i[8] par_db[] */
};

static void tr_bytes(synth *sy, int off, const void *v, size_t n)
{
    if (!sy->tr) return;
    memcpy(sy->tr->frame + off, v, n);
    memset(sy->tr->written + off, 1, n);
}
static void tr32(synth *sy, int off, uint32_t v) { tr_bytes(sy, off, &v, 4); }
static void trf(synth *sy, int off, float v) { tr_bytes(sy, off, &v, 4); }
static void tr8(synth *sy, int off, uint8_t v) { tr_bytes(sy, off, &v, 1); }
/* a guest address in the original's frame */
static uint32_t tr_addr(const synth *sy, int off) { return sy->tr ? sy->tr->stack + (uint32_t)off : 0; }

/* Seg list: S_SEG[S_NSEG] is the stretch being counted. */
static void seg_next(uint8_t *s, int32_t v)
{
    SETI(S_NSEG, I(S_NSEG) + 1);
    SETI(S_SEG + 4 * I(S_NSEG), v);
}
static void seg_add(uint8_t *s, int32_t v) { SETI(S_SEG + 4 * I(S_NSEG), I(S_SEG + 4 * I(S_NSEG)) + v); }

static int handle_ok(const klatt_ctx *ctx, const uint8_t *s)
{
    uint32_t p = (uint32_t)I(S_ID), q = IMG_ID_STRING;
    for (;; p++, q++) {
        const uint8_t *a = (const uint8_t *)ctx->img(ctx->user, p, 1);
        const uint8_t *b = (const uint8_t *)ctx->img(ctx->user, q, 1);
        if (!a || !b || *a != *b) return 0;
        if (!*a) return 1;
    }
}

/* Frame slots of the cascade formants F1..F8 (the bandwidth follows each) */
static const int FORMANT_SLOT[8] = { 9, 13, 15, 17, 19, 21, 23, 25 };

static void read_frame(synth *sy)
{
    const float *fr = sy->frame;
    sy->freq[BLK_ZERO1] = fr[29]; sy->bw[BLK_ZERO1] = fr[30];
    sy->freq[BLK_ZERO2] = fr[33]; sy->bw[BLK_ZERO2] = fr[34];
    sy->freq[BLK_POLE1] = fr[27]; sy->bw[BLK_POLE1] = fr[28];
    sy->freq[BLK_POLE2] = fr[31]; sy->bw[BLK_POLE2] = fr[32];
    /* F1..F5 always, F6..F8 only for more than 5 formants (the others are not used then) */
    int n = sy->nres > 5 ? 8 : 5;
    for (int i = 0; i < n; i++) {
        sy->freq[BLK_CASC + i] = sy->freq[BLK_PAR + i] = fr[FORMANT_SLOT[i]];
        sy->bw[BLK_CASC + i] = fr[FORMANT_SLOT[i] + 1];
        sy->bw[BLK_PAR + i] = fr[44 + i];
        sy->par_db[i] = ftol(fr[35 + i]);
    }
    if (sy->tr) {                       /* the original's copies */
        for (int k = BLK_ZERO1; k <= BLK_POLE2; k++) {
            trf(sy, L_FREQ + 4 * k, sy->freq[k]);
            trf(sy, L_BW + 4 * k, sy->bw[k]);
        }
        for (int i = 0; i < n; i++) {
            trf(sy, L_FREQ + 4 * (BLK_CASC + i), sy->freq[BLK_CASC + i]);
            trf(sy, L_FREQ + 4 * (BLK_PAR + i), sy->freq[BLK_PAR + i]);
            trf(sy, L_BW + 4 * (BLK_CASC + i), sy->bw[BLK_CASC + i]);
            trf(sy, L_BW + 4 * (BLK_PAR + i), sy->bw[BLK_PAR + i]);
            tr32(sy, L_PAR_DB + 4 * i, (uint32_t)sy->par_db[i]);
        }
        tr32(sy, L_REMEMBER, (uint32_t)sy->remember_from);
    }
}

/* Nasal pole/zero pair: identical pole and zero cancel, and both are switched off. */
static void nasal_pair(synth *sy, int pole, int zero)
{
    uint8_t *s = sy->s;
    uint8_t *bp = BLK(pole), *bz = BLK(zero);
    if (sy->freq[pole] == sy->freq[zero] && sy->bw[pole] == sy->bw[zero]) {
        SETRF(bz, RES_ON, 0);
        SETRF(bp, RES_ON, 0);
        return;
    }
    SETRF(bz, RES_ON, F(S_VOICE_HOLD));
    SETRF(bp, RES_ON, F(S_VOICE_HOLD));
    if (sy->freq[pole] != RF(bp, RES_FREQ)) SETF(COSK(pole), x87_cos(F(S_2PI_FS), sy->freq[pole]));
    if (sy->freq[zero] != RF(bz, RES_FREQ)) SETF(COSK(zero), x87_cos(F(S_2PI_FS), sy->freq[zero]));
    if (sy->bw[pole] != RF(bp, RES_BW)) SETF(EXPK(pole), x87_exp_neg(F(S_PI_FS), sy->bw[pole]));
    if (sy->bw[zero] != RF(bz, RES_BW)) SETF(EXPK(zero), x87_exp_neg(F(S_PI_FS), sy->bw[zero]));
}

/* Switch filters on/off for this frame and refresh the cos/exp caches of those that changed. */
static void update_filters(synth *sy)
{
    uint8_t *s = sy->s;
    if ((double)F(S_VOICE_HOLD) > K_HOLD_ON_D) {
        nasal_pair(sy, BLK_POLE1, BLK_ZERO1);
        nasal_pair(sy, BLK_POLE2, BLK_ZERO2);
    }
    for (int i = 0; i < sy->nres; i++) {
        int k = BLK_CASC + i, p = BLK_PAR + i;
        uint8_t *bc = BLK(k), *bp = BLK(p);
        SETRF(bc, RES_ON, F(S_VOICE_HOLD));
        if (sy->freq[k] != RF(bc, RES_FREQ)) SETF(COSK(k), x87_cos(F(S_2PI_FS), sy->freq[k]));
        if (sy->bw[k] != RF(bc, RES_BW) && F(S_VOICE_HOLD) != K_ZERO)
            SETF(EXPK(k), x87_exp_neg(F(S_PI_FS), sy->bw[k]));
        if (F(S_FRIC_HOLD) > K_HOLD_FLOOR) {
            if (sy->par_db[i] != 0) SETRF(bp, RES_ON, F(S_FRIC_HOLD));
            SETF(COSK(p), F(COSK(k)));          /* the parallel formant shares the cascade's frequency */
            if (sy->bw[p] != RF(bp, RES_BW)) SETF(EXPK(p), x87_exp_neg(F(S_PI_FS), sy->bw[p]));
        } else {
            SETRF(bp, RES_ON, 0);
        }
    }
}

/* Cascade coefficients (nasal pairs and formants), with a 3-sample ramp of B and C from their values at
 * the end of the previous frame when the frequency or bandwidth changed. */
static void cascade_coefficients(synth *sy)
{
    uint8_t *s = sy->s;
    for (int k = 1; k < sy->nres + BLK_CASC; k++) {
        uint8_t *blk = BLK(k);
        if (RF(blk, RES_ON) == K_ZERO) continue;
        set_resonator(s, k);
        if (RF(blk, RES_FREQ) == K_ZERO || (sy->freq[k] == RF(blk, RES_FREQ) && sy->bw[k] == RF(blk, RES_BW))) {
            seti(blk + RES_RAMP_N, 0);
            continue;
        }
        seti(blk + RES_RAMP_N, 3);
        double db = ((double)RF(blk, RES_B) - RF(blk, RES_PREV_B)) * K_QUARTER;
        double dc = ((double)RF(blk, RES_C) - RF(blk, RES_PREV_C)) * K_QUARTER;
        for (int j = 1; j <= 3; j++) {
            SETRF(blk, RES_RAMP_C + 4 * (j - 1), (float)(j * dc + RF(blk, RES_PREV_C)));
            double b = j * db + RF(blk, RES_PREV_B);
            SETRF(blk, RES_RAMP_B + 4 * (j - 1), (float)b);
            SETRF(blk, RES_RAMP_A + 4 * (j - 1), (float)(((double)K_ONE - b) - RF(blk, RES_RAMP_C + 4 * (j - 1))));
        }
    }
}

/* Parallel formants: B, C as usual, A = gain (sign alternating by formant) * (1 - B - C); and the
 * bypass gain. */
static void parallel_coefficients(synth *sy)
{
    uint8_t *s = sy->s;
    sy->fric_gain = I(S_GAIN_FRIC) + I(S_GAIN_MASTER) + I(S_AF);
    if (sy->frame[43] != K_ZERO)
        SETF(S_BYPASS_GAIN, (float)(db_to_lin(sy->ctx, ftol(sy->frame[43]) + sy->fric_gain) * K_BYPASS));
    float sign = K_PAR_SIGN;
    for (int i = 0; i < sy->nres; i++) {
        int k = BLK_PAR + i;
        uint8_t *blk = BLK(k);
        if (RF(blk, RES_ON) != K_ZERO) {
            float e = F(EXPK(k));
            SETRF(blk, RES_C, (float)-((double)e * e));
            double b = (double)F(COSK(k)) * e;
            SETRF(blk, RES_B, (float)(b + b));
            if (sy->par_db[i] == 0) {
                SETRF(blk, RES_A, 0);
            } else {
                double lin = db_to_lin(sy->ctx, sy->par_db[i] + sy->fric_gain);
                double norm = ((double)K_ONE - RF(blk, RES_B)) - RF(blk, RES_C);
                SETRF(blk, RES_A, (float)(lin * sign * norm));
            }
        }
        sign = -sign;
    }
    trf(sy, L_SIGN, sign);
    tr32(sy, L_PAR_K, (uint32_t)(BLK_PAR + (sy->nres > 0 ? sy->nres : 0)));
}

/* The nasal zeros run as anti-resonators: invert their coefficients (and ramps). */
static void invert_zeros(synth *sy)
{
    uint8_t *s = sy->s;
    if (!(F(S_VOICE_HOLD) > K_HOLD_ON)) return;
    for (int z = 0; z < 2; z++) {
        uint8_t *blk = BLK(BLK_ZERO1 + z), *coef = s + S_ZERO_COEF + 12 * z;
        if (RF(blk, RES_ON) == K_ZERO) continue;
        float a = RF(blk, RES_A);
        setf(coef, (float)((double)K_ONE / a));
        setf(coef + 4, (float)-(RF(blk, RES_B) / (double)a));
        setf(coef + 8, (float)-(RF(blk, RES_C) / (double)a));
        if (geti(blk + RES_RAMP_N) == 0) continue;
        for (int j = 0; j < 3; j++) {
            float ra = RF(blk, RES_RAMP_A + 4 * j);
            SETRF(blk, RES_RAMP_A + 4 * j, (float)((double)K_ONE / ra));
            SETRF(blk, RES_RAMP_B + 4 * j, (float)-(RF(blk, RES_RAMP_B + 4 * j) / (double)ra));
            SETRF(blk, RES_RAMP_C + 4 * j, (float)-(RF(blk, RES_RAMP_C + 4 * j) / (double)ra));
        }
    }
}

/* ------------------------------------------------------------------------------- glottal source */

/* Open phase of a pulse: u = g - b g^2, g += a (g stays in double across the loop). */
static double pulse(uint8_t *buf, int from, int to, double g, float a, float b)
{
    for (int i = from; i < to; i++) {
        bset(buf, i, (float)(g - (double)b * g * g));
        g += a;
    }
    return g;
}

/* Length of the period starting at `phase` ms (relative to the block) and of its open phase. The period
 * uses the phase as given (the loop passes it unrounded), the open phase the stored float. */
static void period_lengths(uint8_t *s, double phase)
{
    SETI(S_PERIOD, ftol(((double)F(S_T0_MS) - phase) * F(S_SPMS) + K_ROUND_PERIOD));
    int open = ftol(((double)F(S_OQ) * F(S_T0_MS) - F(S_PHASE_MS)) * F(S_SPMS) + K_ROUND_OPEN);
    SETI(S_OPEN, open);
    SETI(S_OPEN_NOW, open);
    SETI(S_CLOSED, I(S_PERIOD) - open);
    SETI(S_CLOSED_NOW, I(S_PERIOD) - open);
}

/* Once per frame, at the first voiced block: flutter, pitch period, pulse shape, diplophonia and the
 * spectral-tilt filter. */
static void start_voicing(synth *sy)
{
    uint8_t *s = sy->s;
    const float *fr = sy->frame;

    if (fr[5] != K_ZERO) {             /* flutter: F0 += F0 * depth * (sum of three slow sines) */
        float t = F(S_TIME_MS);
        fx80 s1 = fx_sin(fx_from_f64(dbits((double)t * K_FLUTTER1)));
        fx80 s2 = fx_sin(fx_from_f64(dbits((double)t * K_FLUTTER2)));
        fx80 s3 = fx_sin(fx_from_f64(dbits((double)t * K_FLUTTER3)));
        double sum = bitsd(fx_to_f64(fx_add(fx_add(s1, s2, PC53), s3, PC53), PC53));
        int jitter = ftol(sum * F(S_F0) * fr[5] * K_FLUTTER_DEPTH);
        SETI(S_JITTER, jitter);
        double f0 = (double)jitter + F(S_F0);
        SETF(S_F0, (float)f0);
        if (f0 <= K_ZERO_D) SETF(S_F0, 1.0f);
    }

    double t0 = K_T0_SCALE / (double)F(S_F0);
    SETF(S_T0_MS, (float)t0);
    float t0_samples = (float)((double)F(S_SAMPLE_RATE) * t0 * K_PER_MS);
    SETF(S_T0_SAMPLES, t0_samples);
    trf(sy, L_SCRATCH, t0_samples);

    /* pulse shape: the open phase lasts OQ * T0; b sets its length, a its amplitude */
    if ((double)F(S_OQ) > K_ZERO_D) {
        int db = I(S_GAIN_MASTER) + I(S_GAIN_VOICE) + I(S_AV);
        if (db <= 0) {
            SETI(S_NORM_B, 0);
            SETI(S_NORM_A, 0);
        } else {
            SETF(S_NORM_B, (float)(K_GLOT_B / ((double)F(S_OQ) * t0_samples)));
            double a = db_to_lin(sy->ctx, db) * F(S_NORM_B) * K_GLOT_A;
            SETF(S_NORM_A, (float)a);
            SETF(S_NORM_B, (float)(F(S_NORM_B) / a));
        }
    }

    /* diplophonia: every other pulse delayed into the closed phase and scaled by (1 - amount/100) */
    float dp = F(S_DIPLO);
    if (!s[S_DIPLO_ON]) {
        if (dp > K_ZERO) { s[S_DIPLO_ON] = 1; s[S_DIPLO_ALT] = 0; }
    } else if (dp == K_ZERO) {
        s[S_DIPLO_ALT] = 0;
        s[S_DIPLO_ON] = 0;
    }
    if (s[S_DIPLO_ON]) {
        double ratio = (double)K_ONE - (double)dp * K_PERCENT;
        SETF(S_DIPLO_RATIO, (float)ratio);
        SETI(S_DIPLO_DELAY, ftol(((double)K_ONE - F(S_OQ)) * dp * F(S_T0_SAMPLES) * K_PERCENT));
        if (ratio > K_ZERO) {
            SETF(S_ALT_A, (float)(F(S_NORM_A) * ratio));
            SETF(S_ALT_B, (float)(F(S_NORM_B) / ratio));
        } else {
            SETI(S_ALT_B, 0);
            SETI(S_ALT_A, 0);
        }
    } else {
        SETF(S_GLOT_A, F(S_NORM_A));
        SETF(S_GLOT_B, F(S_NORM_B));
    }

    /* spectral tilt: a low-pass resonator with bandwidth from the table and F = 0.375 B */
    int tilt = K_TILT_MAX < fr[4] ? 35 : ftol(fr[4]);
    SETI(S_TILT, tilt);
    uint8_t *blk = BLK(BLK_TILT);
    if (tilt == 0 || !(F(S_VOICE_HOLD) > K_HOLD_ON)) {
        SETI(S_BLOCKS + RES_ON, 0);
        return;
    }
    seti(blk + RES_RAMP_N, 0);
    SETRF(blk, RES_ON, F(S_VOICE_HOLD));
    sy->bw[BLK_TILT] = imgf(sy->ctx, IMG_TILT_FREQ + 4 * (uint32_t)tilt);
    sy->remember_from = BLK_TILT;
    sy->freq[BLK_TILT] = (float)((double)sy->bw[BLK_TILT] * K_TILT_FREQ);
    tr32(sy, L_REMEMBER, BLK_TILT);
    trf(sy, L_BW, sy->bw[BLK_TILT]);
    trf(sy, L_FREQ, sy->freq[BLK_TILT]);
    int mode = I(S_RATE_MODE);
    if (mode == 1 || mode == 0) {       /* precomputed A, B, C for 11025 / 8000 Hz */
        uint32_t tab = (mode == 1 ? IMG_TILT_11K : IMG_TILT_8K) + 12 * (uint32_t)tilt;
        SETRF(blk, RES_A, imgf(sy->ctx, tab));
        SETRF(blk, RES_B, imgf(sy->ctx, tab + 4));
        SETRF(blk, RES_C, imgf(sy->ctx, tab + 8));
        return;
    }
    if (sy->freq[BLK_TILT] != RF(blk, RES_FREQ)) SETF(COSK(BLK_TILT), x87_cos(F(S_2PI_FS), sy->freq[BLK_TILT]));
    if (sy->bw[BLK_TILT] != RF(blk, RES_BW)) SETF(EXPK(BLK_TILT), x87_exp_neg(F(S_PI_FS), sy->bw[BLK_TILT]));
    if (F(S_VOICE_HOLD) != K_ZERO) {
        seti(blk + RES_RAMP_N, 0);
        set_resonator(s, BLK_TILT);
        trf(sy, L_SCRATCH, RF(blk, RES_C));
    }
}

/* Choose the pulse for the next period: with diplophonia, alternate pulses are delayed and weakened. */
static int alternate_pulse(uint8_t *s)
{
    if (!s[S_DIPLO_ON]) return -1;
    int alt = s[S_DIPLO_ALT] != 0;
    SETF(S_GLOT_A, alt ? F(S_ALT_A) : F(S_NORM_A));
    SETF(S_GLOT_B, alt ? F(S_ALT_B) : F(S_NORM_B));
    return alt;
}

/* Phase of the next period after one of S_PERIOD samples; the double result is what the original keeps
 * on the stack. */
static double advance_phase(uint8_t *s)
{
    double t = ((double)I(S_PERIOD) * F(S_MSPS) + F(S_PHASE_MS)) - F(S_T0_MS);
    SETF(S_PHASE_MS, (float)t);
    return t;
}

/* The source for `left` samples from `pos`: whole periods, then the start of the last one, whose rest is
 * left pending for the next block. */
static void voiced_source(synth *sy, int pos, int left)
{
    uint8_t *s = sy->s, *buf = sy->buf;

    if (sy->first_block) {
        sy->first_block = 0;
        tr8(sy, L_FIRST, 0);
        start_voicing(sy);
    }
    period_lengths(s, F(S_PHASE_MS));

    while (left >= I(S_PERIOD)) {
        if (left <= 0) goto nothing_pending;
        int alt = alternate_pulse(s);
        if (alt == 1) {                 /* the delayed pulse: silence first, taken from the closed phase */
            int d = I(S_DIPLO_DELAY);
            SETI(S_CLOSED_NOW, I(S_CLOSED) - d);
            bclear(buf, pos, d);
            pos += d;
            seg_add(s, d);
        } else if (alt == 0) {
            SETI(S_CLOSED_NOW, I(S_CLOSED));
        }
        if (alt >= 0) s[S_DIPLO_ALT] = !s[S_DIPLO_ALT];

        seg_next(s, 0);                 /* open phase */
        float a = F(S_GLOT_A);
        double g = (double)a * F(S_SPMS) * F(S_PHASE_MS);
        if (a == K_ZERO) bclear(buf, pos, I(S_OPEN));
        else pulse(buf, pos, pos + I(S_OPEN), g, a, F(S_GLOT_B));
        pos += I(S_OPEN);
        seg_add(s, I(S_OPEN));

        seg_next(s, 0);                 /* closed phase */
        bclear(buf, pos, I(S_CLOSED_NOW));
        pos += I(S_CLOSED_NOW);
        seg_add(s, I(S_CLOSED_NOW));

        left -= I(S_PERIOD);
        tr32(sy, L_SCRATCH, (uint32_t)I(S_PERIOD));
        period_lengths(s, advance_phase(s));
    }
    if (left <= 0) goto nothing_pending;

    /* the period that does not fit: as much of it as there is room for */
    int alt = alternate_pulse(s);
    if (alt == 1) {
        int d = I(S_DIPLO_DELAY);
        SETI(S_CLOSED_NOW, I(S_CLOSED_NOW) - d);
        int m = d < left ? d : left;
        bclear(buf, pos, m);
        pos += m;
        SETI(S_PEND_DELAY, d - m);
        left -= m;
        seg_add(s, m);
    } else if (alt == 0) {
        SETI(S_CLOSED_NOW, I(S_CLOSED));
        SETI(S_PEND_DELAY, 0);
    }
    if (alt >= 0) s[S_DIPLO_ALT] = !s[S_DIPLO_ALT];

    seg_next(s, 0);
    int m = I(S_OPEN) < left ? I(S_OPEN) : left;
    SETI(S_OPEN_NOW, m);
    float a = F(S_GLOT_A);
    double g = (double)F(S_SPMS) * a * F(S_PHASE_MS);
    if (a != K_ZERO) g = pulse(buf, pos, pos + I(S_OPEN_NOW), g, a, F(S_GLOT_B));
    else bclear(buf, pos, m);
    SETF(S_GLOT_G, (float)g);
    SETI(S_PEND_OPEN, I(S_OPEN) - I(S_OPEN_NOW));
    SETI(S_OPEN_LEN, I(S_OPEN));
    SETI(S_SLOPE_INT, ftol(F(S_GLOT_A)));
    seg_add(s, m);
    pos += m;
    left -= m;
    sy->ebp = (uint32_t)I(S_BUF_PTR) + 4u * (uint32_t)pos;  /* the original's pointer to the closed part */

    seg_next(s, 0);
    m = I(S_CLOSED_NOW) < left ? I(S_CLOSED_NOW) : left;
    bclear(buf, pos, m);
    SETI(S_PEND_CLOSED, I(S_CLOSED_NOW) - m);
    seg_add(s, m);
    advance_phase(s);
    return;

nothing_pending:
    sy->ebp = (uint32_t)pos;
    SETI(S_PEND_CLOSED, 0);
    SETI(S_PEND_OPEN, 0);
    SETI(S_PEND_DELAY, 0);
}

/* The glottal source of a block into buf, low-passed by the tilt filter. */
static void glottal_source(synth *sy, int n)
{
    uint8_t *s = sy->s, *buf = sy->buf;
    int pos = 0, left = n, filtered = 0;

    SETI(S_NSEG, 0);
    SETI(S_SEG, 0);

    /* what the previous block left: delay, rest of the open phase, rest of the closed phase */
    if (I(S_PEND_DELAY) != 0) {
        int m = I(S_PEND_DELAY) < left ? I(S_PEND_DELAY) : left;
        bclear(buf, 0, m);
        left -= m;
        SETI(S_PEND_DELAY, I(S_PEND_DELAY) - m);
        seg_add(s, m);
        pos = m;
    }
    seg_next(s, 0);
    if (I(S_PEND_OPEN) != 0) {
        SETI(S_OPEN_NOW, I(S_PEND_OPEN) < left ? I(S_PEND_OPEN) : left);
        double g = pulse(buf, pos, pos + I(S_OPEN_NOW), F(S_GLOT_G), F(S_GLOT_A), F(S_GLOT_B));
        SETF(S_GLOT_G, (float)g);
        SETI(S_PEND_OPEN, I(S_PEND_OPEN) - I(S_OPEN_NOW));
        left -= I(S_OPEN_NOW);
        pos += I(S_OPEN_NOW);
        seg_add(s, I(S_OPEN_NOW));
    }
    seg_next(s, 0);
    if (I(S_PEND_CLOSED) != 0 && left != 0) {
        SETI(S_CLOSED_NOW, I(S_PEND_CLOSED) < left ? I(S_PEND_CLOSED) : left);
        bclear(buf, pos, I(S_CLOSED_NOW));
        SETI(S_PEND_CLOSED, I(S_PEND_CLOSED) - I(S_CLOSED_NOW));
        left -= I(S_CLOSED_NOW);
        pos += I(S_CLOSED_NOW);
        seg_add(s, I(S_CLOSED_NOW));
    }
    /* that part still goes through the previous frame's tilt filter */
    if (pos > 0) {
        tr32(sy, L_FILTERED, (uint32_t)pos);
        resonate(BLK(BLK_TILT), buf, pos);
        filtered = pos;
    }

    sy->ebp = (uint32_t)pos;
    if (left > 0) {
        if (F(S_F0) != K_ZERO && I(S_AV) != 0) {
            voiced_source(sy, pos, left);
        } else {                        /* no voicing: silence (a single "open" stretch if aspirated) */
            if (I(S_AH) != 0) {
                SETI(S_PERIOD, left);
                SETI(S_OPEN, left);
                seg_next(s, left);
                SETI(S_CLOSED, 0);
                seg_next(s, 0);
            } else {
                SETI(S_OPEN, 0);
                seg_next(s, 0);
                SETI(S_PERIOD, left);
                SETI(S_CLOSED, left);
                seg_next(s, left);
            }
            bclear(buf, pos, left);
            SETI(S_PHASE_MS, 0);
            SETI(S_PEND_CLOSED, 0);
            SETI(S_PEND_OPEN, 0);
            SETI(S_PEND_DELAY, 0);
        }
    }
    seg_next(s, 0);
    seg_next(s, 0);

    /* Note: here the original also has an open-phase F1 modulation (F1 resonator switching to fixed
     * coefficient sets while the glottis is open, driven by the S_SEG stretches), enabled by S_F1_MOD.
     * The frame function clears S_F1_MOD on entry and nothing sets it, so that code never runs and is
     * not ported. */
    resonate(BLK(BLK_TILT), buf + 4 * filtered, n - filtered);
}

/* ------------------------------------------------------------------------------------- the block */

static void synth_block(synth *sy, int n)
{
    uint8_t *s = sy->s, *buf = sy->buf;

    glottal_source(sy, n);

    if (I(S_SOURCE_ONLY) == 0) {
        /* cascade branch: source + aspiration through the nasal pairs and the formants */
        if (F(S_VOICE_HOLD) > K_HOLD_ON) {
            if (I(S_AH) != 0) {
                set16(s + S_ASP_SEED, (int16_t)noise(s, (uint16_t)get16(s + S_ASP_SEED)));
                for (int i = 0; i < n; i++)
                    bset(buf, i, (float)((double)get16(s + S_NOISE + 2 * i) * sy->asp_amp + bget(buf, i)));
                if (n > 0) tr32(sy, L_FILTERED, (uint32_t)(int32_t)get16(s + S_NOISE + 2 * (n - 1)));
            }
            resonate(BLK(BLK_POLE1), buf, I(S_NBLOCK));
            antiresonate(BLK(BLK_ZERO1), s + S_ZERO_COEF, buf, I(S_NBLOCK));
            resonate(BLK(BLK_POLE2), buf, I(S_NBLOCK));
            antiresonate(BLK(BLK_ZERO2), s + S_ZERO_COEF + 12, buf, I(S_NBLOCK));
            for (int k = BLK_CASC + sy->nres - 1; k > BLK_CASC; k--) resonate(BLK(k), buf, I(S_NBLOCK));
            resonate(BLK(BLK_CASC), buf, I(S_NBLOCK));

            if (I(S_AH) == 0 && I(S_AV) == 0) {         /* count the hold time down */
                double hold = (double)F(S_VOICE_HOLD) - F(S_BLOCK_MS);
                SETF(S_VOICE_HOLD, (float)hold);
                if (hold < K_HOLD_FLOOR) SETI(S_VOICE_HOLD, 0);
            }
        }

        /* parallel branch: frication noise through the parallel formants, added to the output */
        if (F(S_FRIC_HOLD) > K_HOLD_ON) {
            if (I(S_AF) != 0) {
                set16(s + S_FRIC_SEED, (int16_t)noise(s, (uint16_t)get16(s + S_FRIC_SEED)));
                for (int i = 0; i < I(S_NBLOCK); i++) SETF(S_FRIC + 4 * i, (float)get16(s + S_NOISE + 2 * i));
                if (I(S_NBLOCK) > 0)
                    tr32(sy, L_FILTERED, (uint32_t)(int32_t)get16(s + S_NOISE + 2 * (I(S_NBLOCK) - 1)));
            }
            if (sy->frame[43] != K_ZERO && I(S_AF) != 0) {
                float gain = F(S_BYPASS_GAIN);
                trf(sy, L_BYPASS, gain);
                for (int i = 0; i < n; i++)
                    bset(buf, i, (float)((double)gain * F(S_FRIC + 4 * i) + bget(buf, i)));
            }
            if (sy->nres > 0) {                         /* the original walks par_db[] with a pointer */
                sy->ebp = tr_addr(sy, L_PAR_DB + 4 * sy->nres);
                tr32(sy, L_FILTERED, sy->ebp);
            }
            for (int i = 0; i < sy->nres; i++) {
                uint8_t *blk = BLK(BLK_PAR + i);
                if (RF(blk, RES_ON) == K_ZERO) continue;
                if (I(S_AF) != 0 && sy->par_db[i] != 0) {
                    for (int j = 0; j < I(S_NBLOCK); j++) bset(sy->pbuf, j, F(S_FRIC + 4 * j));
                    resonate(blk, sy->pbuf, I(S_NBLOCK));
                } else {
                    ring(blk, sy->pbuf, I(S_NBLOCK));
                }
                if ((double)sy->par_db[i] == K_ZERO_D) {  /* silent formant: count its hold down */
                    double hold = (double)RF(blk, RES_ON) - F(S_BLOCK_MS);
                    SETRF(blk, RES_ON, (float)hold);
                    if (hold < K_ZERO) SETRF(blk, RES_ON, 0);
                }
                for (int j = 0; j < I(S_NBLOCK); j++)
                    bset(buf, j, (float)((double)bget(sy->pbuf, j) + bget(buf, j)));
            }
            if (I(S_AF) == 0) {
                double hold = (double)F(S_FRIC_HOLD) - F(S_BLOCK_MS);
                SETF(S_FRIC_HOLD, (float)hold);
                if (hold < K_HOLD_ON) SETI(S_FRIC_HOLD, 0);
            }
        }
    }

    /* to integers, tracking the peak (the original's ebx walks the output, edi counts) */
    if (sy->tr && I(S_NBLOCK) > 0) {
        sy->tr->ebx = sy->ctx->state_addr + S_OUT + 4u * (uint32_t)I(S_NBLOCK);
        sy->tr->edi = (uint32_t)I(S_NBLOCK);
    }
    for (int i = 0; i < I(S_NBLOCK); i++) {
        int32_t v = ftol(bget(buf, i));
        SETI(S_OUT + 4 * i, v);
        int32_t mag = v < 0 ? (int32_t)(0u - (uint32_t)v) : v;
        if (mag > I(S_PEAK)) SETI(S_PEAK, mag);
    }
}

/* --------------------------------------------------------------------------------- entry point */

static uint8_t *self_pointer(const klatt_ctx *ctx, uint8_t *s, int field, uint32_t expect)
{
    uint32_t p = (uint32_t)I(field);
    if (ctx->state_addr && p != ctx->state_addr + expect) return NULL;
    return s + expect;
}

int klatt_synth(const klatt_ctx *ctx, uint8_t *s, const float frame[KLATT_FRAME_SIZE])
{
    if (!handle_ok(ctx, s)) return 0;

    synth sy;
    memset(&sy, 0, sizeof sy);
    sy.ctx = ctx;
    sy.s = s;
    sy.frame = frame;
    sy.buf = self_pointer(ctx, s, S_BUF_PTR, 0x0a73);
    sy.pbuf = self_pointer(ctx, s, S_PBUF_PTR, 0x10c3);
    if (!sy.buf || !sy.pbuf) return -1;
    sy.nres = I(S_NRES);
    sy.first_block = 1;
    sy.remember_from = 1;
    sy.tr = ctx->trace;
    if (sy.tr) {
        memset(sy.tr->written, 0, sizeof sy.tr->written);
        sy.tr->blocks = 0;
    }

    SETI(S_NFRAMES, I(S_NFRAMES) + 1);
    int nsamp = ftol((double)F(S_SPMS) * F(S_TIME_SCALE) * frame[0]);
    tr32(&sy, L_NSAMP, (uint32_t)nsamp);
    SETI(S_AV, ftol(frame[2]));
    SETI(S_AH, ftol(frame[7]));
    SETI(S_AF, ftol(frame[8]));
    if (I(S_AV) != 0 || I(S_AH) != 0) SETF(S_VOICE_HOLD, HOLD_MS);
    if (I(S_AF) != 0) SETF(S_FRIC_HOLD, HOLD_MS);
    s[S_F1_MOD] = 0;

    read_frame(&sy);
    update_filters(&sy);
    cascade_coefficients(&sy);
    parallel_coefficients(&sy);
    invert_zeros(&sy);

    SETF(S_F0, frame[1]);
    SETF(S_DIPLO, frame[6]);
    SETF(S_OQ, (float)((double)frame[3] * K_PERCENT));
    SETI(S_NBLOCK, nsamp > MAX_BLOCK ? MAX_BLOCK : nsamp);
    if (I(S_AH) != 0)
        sy.asp_amp = (float)(db_to_lin(ctx, I(S_GAIN_ASP) + I(S_GAIN_MASTER) + I(S_AH)) * K_ASP);
    tr8(&sy, L_FIRST, 1);
    trf(&sy, L_ASP_AMP, sy.asp_amp);

    for (int left = nsamp; left > 0;) {
        int n = I(S_NBLOCK) < left ? I(S_NBLOCK) : left;
        SETI(S_NBLOCK, n);
        SETF(S_BLOCK_MS, (float)((double)(n * 1000) / F(S_SAMPLE_RATE)));
        left -= n;
        tr32(&sy, L_FILTERED, 0);
        tr32(&sy, L_SCRATCH, (uint32_t)(n * 1000));
        tr32(&sy, L_LEFT, (uint32_t)left);
        if (F(S_VOICE_HOLD) < K_HOLD_ON && F(S_FRIC_HOLD) < K_HOLD_ON && I(S_PEND_DELAY) == 0 &&
            I(S_PEND_CLOSED) == 0 && I(S_PEND_OPEN) == 0) {
            for (int i = 0; i < I(S_NBLOCK); i++) SETI(S_OUT + 4 * i, 0);   /* silence */
            if (sy.tr) {
                sy.tr->ebx = (uint32_t)n;
                sy.tr->ebp = 0;
                sy.tr->edi = 0;
            }
        } else {
            synth_block(&sy, n);
            if (sy.tr) sy.tr->ebp = sy.ebp;
        }
        if (sy.tr) sy.tr->blocks++;
        emit(ctx, s, I(S_NBLOCK));
    }

    SETI(S_TOTAL, I(S_TOTAL) + nsamp);
    SETF(S_TIME_MS, (float)((double)F(S_TIME_SCALE) * frame[0] + F(S_TIME_MS)));

    /* remember what each active filter was made from; reset the inactive ones */
    for (int k = sy.remember_from; k < NBLOCKS; k++) {
        uint8_t *blk = BLK(k);
        if (RF(blk, RES_ON) != K_ZERO) {
            SETRF(blk, RES_FREQ, sy.freq[k]);
            SETRF(blk, RES_BW, sy.bw[k]);
            SETRF(blk, RES_PREV_B, RF(blk, RES_B));
            SETRF(blk, RES_PREV_C, RF(blk, RES_C));
        } else {
            seti(blk + RES_FREQ, 0);
            seti(blk + RES_BW, 0);
            seti(blk + RES_Y1, 0);
            seti(blk + RES_Y2, 0);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------ constant check */

typedef struct { uint32_t addr; int is_double; double value; const char *name; } konst;

static const konst CONSTANTS[] = {
    { 0x10144828, 0, K_ZERO, "K_ZERO" },
    { 0x10144ea8, 0, K_ONE, "K_ONE" },
    { 0x10144ea4, 0, K_QUARTER, "K_QUARTER" },
    { 0x10144ea0, 0, K_BYPASS, "K_BYPASS" },
    { 0x10144e9c, 0, K_HOLD_ON, "K_HOLD_ON" },
    { 0x10144eac, 0, K_HOLD_FLOOR, "K_HOLD_FLOOR" },
    { 0x10144eb0, 1, K_HOLD_ON_D, "K_HOLD_ON_D" },
    { 0x10144e98, 0, K_PERCENT, "K_PERCENT" },
    { 0x10144e94, 0, K_ASP, "K_ASP" },
    { 0x10144e90, 0, K_FLUTTER1, "K_FLUTTER1" },
    { 0x10144e8c, 0, K_FLUTTER2, "K_FLUTTER2" },
    { 0x10144e88, 0, K_FLUTTER3, "K_FLUTTER3" },
    { 0x10144e84, 0, K_FLUTTER_DEPTH, "K_FLUTTER_DEPTH" },
    { 0x10144e80, 0, K_T0_SCALE, "K_T0_SCALE" },
    { 0x10144e7c, 0, K_GLOT_B, "K_GLOT_B" },
    { 0x10144e78, 0, K_GLOT_A, "K_GLOT_A" },
    { 0x10144e74, 0, K_TILT_MAX, "K_TILT_MAX" },
    { 0x10144e70, 0, K_TILT_FREQ, "K_TILT_FREQ" },
    { 0x10144e68, 1, K_ROUND_PERIOD, "K_ROUND_PERIOD" },
    { 0x10144e60, 1, K_ROUND_OPEN, "K_ROUND_OPEN" },
    { 0x10144e54, 0, K_PER_MS, "K_PER_MS" },
    { 0x101448b0, 1, K_ZERO_D, "K_ZERO_D" },
    { 0x101442b0, 1, K_ONE_D, "K_ONE_D" },
};

int klatt_check_constants(const klatt_ctx *ctx, char *msg, size_t msgsize)
{
    int bad = 0;
    if (msg && msgsize) msg[0] = 0;
    for (size_t i = 0; i < sizeof CONSTANTS / sizeof CONSTANTS[0]; i++) {
        const konst *k = &CONSTANTS[i];
        const void *p = ctx->img(ctx->user, k->addr, k->is_double ? 8 : 4);
        int ok = 0;
        if (p && k->is_double) { double d; memcpy(&d, p, 8); ok = dbits(d) == dbits(k->value); }
        if (p && !k->is_double) {
            float f = (float)k->value;
            ok = memcmp(p, &f, 4) == 0;
        }
        if (!ok && !bad++ && msg) snprintf(msg, msgsize, "%s at 0x%08x differs from the image", k->name, k->addr);
    }
    /* the two immediates */
    float sign = K_PAR_SIGN, hold = HOLD_MS;
    uint32_t b1, b2;
    memcpy(&b1, &sign, 4);
    memcpy(&b2, &hold, 4);
    if (b1 != 0x3166e390u || b2 != 0x41a00000u) {
        if (!bad++ && msg) snprintf(msg, msgsize, "immediate K_PAR_SIGN/HOLD_MS wrong");
    }
    return bad;
}
