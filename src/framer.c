/* framer - Eloquence 6.1's frame builder, hand-ported from ENU.SYN (FUN_101306b0 and helpers).
 *
 * ---------------------------------------------------------------------------------------------------
 * What it does
 *
 * The rules of the engine produce, for every synthesis parameter, a track of breakpoints: "at time t
 * (ms) the parameter has value v". The frame builder turns the tracks into the synthesizer's frames:
 *
 *   for t = start, start + step, ... while start <= t < end:
 *       for every frame slot mapped to a track:
 *           advance the track's segment (the two breakpoints around t), reading breakpoints from the
 *           track's queue as needed, and put the linearly interpolated value, truncated to an integer,
 *           into the slot
 *       the frame crossing `end` gets a shorter length in slot 0
 *       synthesize the frame (FUN_1013caf0); stop if it fails
 *
 * Slots not mapped to a track keep the value of the default frame the caller passes (the voice's
 * neutral values); slot 0 is the frame length in ms (the step, normally 5).
 *
 * Incremental mode: when the engine synthesizes while it is still producing the parameter tracks, each
 * call continues where the previous one stopped (unless it starts a new utterance) and, unless it is the
 * final call, only goes as far as every track has breakpoints, rounded down to whole frames.
 *
 * ---------------------------------------------------------------------------------------------------
 * The tracks' queues (one 24-byte record per track; FUN_1012f620/FUN_1012f6c0 fill them)
 *
 * A queue is a ring buffer of 32-bit entries that doubles when it fills up (FUN_10130bf0, not here) and
 * is halved after a read that leaves fewer entries than half its capacity (never below its initial
 * capacity). An entry is a breakpoint: low 16 bits the time since the previous breakpoint, high 16 bits
 * the (signed) value. A time step that does not fit is escaped: low 16 bits 0xffff, and the next entry
 * holds the absolute time with its halves swapped.
 *
 * ---------------------------------------------------------------------------------------------------
 * Arithmetic
 *
 * The only floating point is the interpolation, fild/fimul/fidiv/fiadd on 32-bit integers with the
 * control word 0x027f (53-bit precision, round to nearest): each step is the IEEE double operation, the
 * result truncated by _ftol (to int64, the low 32 bits kept); and the int -> float stores of the frame
 * slots, rounded to nearest. Integer arithmetic wraps at 32 bits as the original's does (hence the
 * unsigned casts), and the queue indices are 16-bit.
 *
 * ---------------------------------------------------------------------------------------------------
 * Engine memory
 *
 * All of it is guest memory, read and written in place (rd32/wr32), and re-read wherever the original
 * re-reads it, since the synthesizer's output callback runs engine code in between.
 */
#include "framer.h"

/* The engine instance (the first argument of every function here). */
enum {
    ENG_SYNTH      = 0x64,   /* p  -> the synthesis context below */
    ENG_CONTROL    = 0x68    /* p  -> control block; its byte +0x1a asks to stop (FUN_10142350) */
};
#define CONTROL_STOP 0x1a

/* The synthesis context (instance + 0x64). */
enum {
    SX_CURSOR      = 0x00,   /* p  -> the cursor below (malloc'd here on first use) */
    SX_RESUME      = 0x04,   /* i  incremental mode: where the next call continues */
    SX_SLOT_MAP    = 0x14,   /* p  -> slot map: a byte, then int[62] track per frame slot (-1: none) */
    SX_KLATT       = 0x1d,   /* p  -> the synthesizer's state (KLATT_STATE_SIZE bytes) */
    SX_QUEUES      = 0x21,   /* p  -> { p queue records, s16 number of tracks } */
    SX_END         = 0x3e,   /* i  end of the current call, after the incremental-mode limits */
    SX_AT_START    = 0x42,   /* i  cleared by a call that starts at time 0 */
    SX_REQ_END     = 0x46    /* i  end of the current call as requested */
};

/* The cursor: where the frame builder is in each track. */
enum {
    CUR_SEGMENTS   = 0x00,   /* p  -> one segment per track (calloc'd) */
    CUR_START      = 0x04,   /* i  time span the tracks' segments are valid for */
    CUR_END        = 0x08    /* i  */
};

/* A segment: the breakpoints (t0, v0) .. (t1, v1) the track is interpolated between. t1 = -1: no
 * breakpoint read yet. */
enum {
    SEG_T0 = 0x00, SEG_V0 = 0x04, SEG_T1 = 0x08, SEG_V1 = 0x0c,
    SEG_DT = 0x10, SEG_DV = 0x14,        /* t1 - t0, v1 - v0 (written, not read here) */
    SEG_SIZE = 0x18
};

/* A track's queue. */
enum {
    Q_BUF          = 0x00,   /* p  -> entries (malloc'd) */
    Q_CAP          = 0x04,   /* u16 capacity in entries */
    Q_HEAD         = 0x06,   /* u16 next entry to read */
    Q_TAIL         = 0x08,   /* u16 next entry to write */
    Q_MIN_CAP      = 0x0a,   /* u16 the initial capacity: never shrunk below it */
    Q_LAST_QUEUED  = 0x10,   /* i  time of the last breakpoint queued */
    Q_LAST_READ    = 0x14,   /* i  time of the last breakpoint read */
    Q_SIZE         = 0x18
};
#define ESCAPED_TIME 0xffffu

/* The frame builder's arguments (cdecl, at `args`). */
enum {
    ARG_ENG        = 0x00,   /* p  engine instance */
    ARG_START      = 0x04,   /* i  first frame's time, ms */
    ARG_END        = 0x08,   /* i  end time, ms */
    ARG_INCREMENTAL= 0x0c,   /* b  incremental mode */
    ARG_NEW_UTT    = 0x10,   /* b  incremental mode: a new utterance starts (else continue at SX_RESUME) */
    ARG_FINAL      = 0x14,   /* b  incremental mode: the last call (else stop where the tracks' data ends) */
                             /* +0x18: unused */
    ARG_STEP       = 0x1c,   /* u  frame length, ms */
    ARG_SLOT_MAP   = 0x20,   /* p  the slot map (SX_SLOT_MAP) */
    ARG_DEFAULTS   = 0x24    /* p  float[62] the default frame */
};
#define SLOT_MAP_TRACKS 1    /* the map's track numbers start after its first byte (so are unaligned) */
#define NO_TRACK        (-1)
#define SLOT_LENGTH     0    /* frame slot 0: frame length in ms */

/* ENU.SYN's import table: the C runtime (src/crt.c implements it on the machine) */
#define IAT_CALLOC   0x10144094u
#define IAT_REALLOC  0x1014409cu
#define IAT_FREE     0x1014410cu
#define IAT_MALLOC   0x10144110u
#define IAT_MEMMOVE  0x10144134u
#define SYNTH_ADDR   0x1013caf0u     /* the synthesizer (klatt_guest.c) */
#define STOP_ADDR    0x10142350u     /* stop requested (framer_guest.c) */
#define TRACK_END_ADDR   0x1012f960u /* the track accessors (framer_guest.c) */
#define TRACK_COUNT_ADDR 0x1012f980u
#define NEXT_POINT_ADDR  0x1012f8a0u
#define QUEUE_EMPTY_ADDR  0x10130bd0u /* the queue functions (framer_guest.c) */
#define QUEUE_POP_ADDR    0x10130c60u
#define QUEUE_SHRINK_ADDR 0x10130d40u

/* ------------------------------------------------------------------------------------------ access */

/* the original's locals the interpolation leaves (esp-relative in its frame; later code can read them
 * uninitialized, so the port writes them too) */
enum {
    LOC_FAILED = 0x13,  /* b */
    LOC_X      = 0x14,  /* t - t0, then the value */
    LOC_T      = 0x18,
    LOC_DV     = 0x1c,
    LOC_OUT    = 0x20,  /* the frame slot being written */
    LOC_MAP    = 0x24,  /* its track */
    LOC_LEFT   = 0x28,  /* slots left */
    LOC_V0     = 0x2c,  /* first the step, as a qword for fild */
    LOC_DT     = 0x34,
    LOC_LAST   = 0x38   /* the last frame's length, a qword for fild */
};

static uint32_t synth_ctx(cpu *c, uint32_t eng) { return rd32(c, eng + ENG_SYNTH); }
static uint32_t cursor(cpu *c, uint32_t eng) { return rd32(c, synth_ctx(c, eng) + SX_CURSOR); }
static uint32_t queues(cpu *c, uint32_t eng) { return rd32(c, synth_ctx(c, eng) + SX_QUEUES); }
static uint32_t queue(cpu *c, uint32_t eng, int16_t track)
{
    return rd32(c, queues(c, eng)) + (uint32_t)track * Q_SIZE;
}

static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

/* A call into engine code or the C runtime as the original makes it: arguments pushed last first, then
 * its return address, which is what the callee finds on the stack. cdecl: the caller pops the arguments. */
static uint32_t call_guest(cpu *c, uint32_t target, uint32_t ret, int nargs, const uint32_t *args)
{
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, ret);
    x86_icall(c, target);
    c->esp += 4u * (uint32_t)nargs;
    return c->eax;
}
static uint32_t call_import(cpu *c, uint32_t slot, uint32_t ret, int nargs, const uint32_t *args)
{
    return call_guest(c, rd32(c, slot), ret, nargs, args);
}

/* The engine's track accessors, called as the original calls them (their return addresses and arguments
 * are what its stack keeps). */
static int32_t track_count_at(cpu *c, uint32_t eng, uint32_t ret)
{
    return (int16_t)call_guest(c, TRACK_COUNT_ADDR, ret, 1, &eng);
}

/* the next breakpoint of the track (FUN_1012f8a0), with the original's registers (ebx the instance, esi the
 * segment, edi the track, ebp the cursor, which the callee saves on the stack) and its outputs in the
 * original's locals: time at LOC_DV, value at LOC_X */
static int next_point_at(cpu *c, uint32_t eng, int32_t track, uint32_t seg, uint32_t cur, uint32_t locals,
                         uint32_t ret, uint32_t *time, int32_t *value)
{
    wr32(c, locals + 0x14, 0);
    wr32(c, locals + 0x1c, 0);
    c->ebx = eng;
    c->esi = seg;
    c->edi = (uint32_t)track;
    c->ebp = cur;
    uint32_t a[4] = { eng, (uint32_t)track, locals + 0x1c, locals + 0x14 };
    int ok = (uint8_t)call_guest(c, NEXT_POINT_ADDR, ret, 4, a) != 0;
    *time = rd32(c, locals + 0x1c);
    *value = (int32_t)rd32(c, locals + 0x14);
    return ok;
}

/* msvcrt _ftol: truncate to int64 (fistp qword with RC = chop), keep the low 32 bits */
static int32_t ftol(double v)
{
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) return 0;   /* integer indefinite */
    return (int32_t)(uint32_t)(uint64_t)(int64_t)v;
}

/* -------------------------------------------------------------------------------- small accessors */

int framer_stop_requested(cpu *c, uint32_t eng)
{
    return rd8(c, rd32(c, eng + ENG_CONTROL) + CONTROL_STOP);
}

int32_t framer_track_count(cpu *c, uint32_t eng)
{
    return (int16_t)rd16(c, queues(c, eng) + 4);
}

uint32_t framer_track_end(cpu *c, uint32_t eng, int16_t track)
{
    return rd32(c, queue(c, eng, track) + Q_LAST_QUEUED);
}

/* ---------------------------------------------------------------------------------------- queues */

int framer_queue_empty(cpu *c, uint32_t q)
{
    return rd32(c, q + Q_BUF) == 0 || rd16(c, q + Q_HEAD) == rd16(c, q + Q_TAIL);
}

/* Give back half the buffer when fewer entries than half the capacity are queued (not below the initial
 * capacity): the entries are moved to the start of the buffer, unwrapping the ring, and the buffer is
 * realloc'd. Returns what the original returns in eax: al 0 if realloc fails (the queue is left with the
 * moved entries then, as in the original), else 1; the upper bits are the capacity's, or the new
 * buffer's. */
uint32_t framer_queue_shrink(cpu *c, uint32_t q)
{
    /* the original's stack: ebx, esi and edi saved (written by PORT_FN's prologue), and ebp too while it
     * moves a wrapped ring; the C runtime calls are made below them */
    const uint32_t entry = c->esp;
    c->esp = entry - 12;
    uint32_t cap = rd16(c, q + Q_CAP), head = rd16(c, q + Q_HEAD), tail = rd16(c, q + Q_TAIL);
    uint32_t r = (cap & 0xff00u) | 1u;
    uint32_t buf = rd32(c, q + Q_BUF);
    int32_t n;
    if (tail > head) {                                  /* one piece: [head, tail) */
        n = (int32_t)(tail - head);
        if (cap <= rd16(c, q + Q_MIN_CAP) || n >= (int32_t)(cap >> 1)) goto out;
        uint32_t a[3] = { buf, buf + head * 4, (uint32_t)n * 4 };
        call_import(c, IAT_MEMMOVE, 0x10130d94u, 3, a);
    } else {                                            /* wrapped: [head, cap) then [0, tail) */
        n = (int32_t)(cap - head + tail);
        if (cap <= rd16(c, q + Q_MIN_CAP) || n >= (int32_t)(cap >> 1)) goto out;
        push32(c, c->ebp);
        uint32_t a1[3] = { buf + (cap - head) * 4, buf, tail * 4 };
        call_import(c, IAT_MEMMOVE, 0x10130ddcu, 3, a1);
        buf = rd32(c, q + Q_BUF);
        head = rd16(c, q + Q_HEAD);
        cap = rd16(c, q + Q_CAP);
        uint32_t a2[3] = { buf, buf + head * 4, (cap - head) * 4 };
        call_import(c, IAT_MEMMOVE, 0x10130dfeu, 3, a2);
        c->esp += 4;
    }
    uint16_t half = (uint16_t)(rd16(c, q + Q_CAP) >> 1);
    uint32_t a[2] = { rd32(c, q + Q_BUF), (uint32_t)half * 4 };
    uint32_t p = call_import(c, IAT_REALLOC, 0x10130e1eu, 2, a);
    r = p ? (p & 0xffffff00u) | 1u : 0;
    if (!p) goto out;
    wr32(c, q + Q_BUF, p);
    wr16(c, q + Q_HEAD, 0);
    wr16(c, q + Q_TAIL, (uint16_t)n);
    wr16(c, q + Q_CAP, half);
out:
    c->esp = entry;
    return r;
}

/* The queue functions called as the original calls them: __fastcall / __thiscall with the queue in ecx
 * (FUN_10130c60 takes one stack argument and pops it). Returns eax (the result is al). */
static uint32_t queue_call(cpu *c, uint32_t fn, uint32_t ret, uint32_t q, int nargs, uint32_t arg)
{
    if (nargs) push32(c, arg);
    push32(c, ret);
    c->ecx = q;
    x86_icall(c, fn);
    return c->eax;
}

/* Take the oldest entry into *out (a guest address). Returns what the original returns in eax: al 0 if
 * the queue was empty (nothing taken), else the result of shrinking the queue afterwards (FUN_10130d40,
 * which it calls last, from its entry esp). */
uint32_t framer_queue_pop(cpu *c, uint32_t q, uint32_t out)
{
    uint32_t buf = rd32(c, q + Q_BUF);
    uint16_t head = rd16(c, q + Q_HEAD);
    if (buf == 0 || head == rd16(c, q + Q_TAIL)) return c->eax & 0xffffff00u;
    wr32(c, out, rd32(c, buf + (uint32_t)head * 4));
    head++;
    if (head == rd16(c, q + Q_CAP)) head = 0;
    wr16(c, q + Q_HEAD, head);
    return queue_call(c, QUEUE_SHRINK_ADDR, 0x10130ca0u, q, 0, 0);
}

/* The track's next breakpoint (FUN_1012f8a0, run at its entry: c->esp holds its return address): its
 * time (absolute: the queued steps accumulate in Q_LAST_READ) into *time_p and value into *value_p (guest
 * addresses). Returns 0 if the queue has none (the value may have been set then, as in the original).
 * The original keeps the entry in a local and calls the queue functions with ebx and esi saved. */
int framer_next_point(cpu *c, uint32_t eng, int16_t track, uint32_t time_p, uint32_t value_p, uint32_t *time)
{
    const uint32_t entry = c->esp, slot = entry - 8;
    const uint32_t ebx = c->ebx, esi = c->esi;
    uint32_t q = queue(c, eng, track), t = 0;
    int ok = 0;
    c->esp = entry - 16;                                /* sub esp, 8; push ebx; push esi */
    c->ebx = (ebx & 0xffffff00u) | 1;                   /* mov bl, 1 */
    c->esi = q;
    if (!(uint8_t)queue_call(c, QUEUE_EMPTY_ADDR, 0x1012f8c5u, q, 0, 0)) {
        queue_call(c, QUEUE_POP_ADDR, 0x1012f8ddu, q, 1, slot);
        uint32_t e = rd32(c, slot);
        wr32(c, value_p, (uint32_t)(int32_t)(int16_t)(e >> 16));
        if ((e & 0xffffu) == ESCAPED_TIME) {            /* the absolute time follows, halves swapped */
            if (!(uint8_t)queue_call(c, QUEUE_EMPTY_ADDR, 0x1012f8f9u, q, 0, 0)) {
                queue_call(c, QUEUE_POP_ADDR, 0x1012f911u, q, 1, slot);
                e = rd32(c, slot);
                t = (e & 0xffffu) << 16 | e >> 16;
                ok = 1;
            }
        } else {
            t = (e & 0xffffu) + rd32(c, q + Q_LAST_READ);
            ok = 1;
        }
    }
    if (ok) {
        wr32(c, time_p, t);
        wr32(c, q + Q_LAST_READ, t);
    }
    c->esp = entry;
    c->ebx = ebx;
    c->esi = esi;
    *time = t;
    return ok;
}

/* ------------------------------------------------------------------------------------- the frames */

/* Reset every track's segment: no breakpoint read yet. */
static void reset_segments(cpu *c, uint32_t eng, uint32_t segs, uint32_t ret_first, uint32_t ret_loop)
{
    for (int32_t k = 0; k < track_count_at(c, eng, k ? ret_loop : ret_first); k++)
        wr32(c, segs + (uint32_t)k * SEG_SIZE + SEG_T1, (uint32_t)-1);
}

/* The first call: the cursor and its segments. Returns 0 if out of memory, with *fail_eax what the
 * original returns in eax then. */
static int new_cursor(cpu *c, uint32_t eng, int32_t start, int32_t end, uint32_t *fail_eax)
{
    uint32_t a[2] = { 12, 0 };
    uint32_t cur = call_import(c, IAT_MALLOC, 0x101307beu, 1, a);
    if (cur) {
        /* calloc(count, SEG_SIZE), the count's call made with the size already pushed */
        push32(c, SEG_SIZE);
        a[0] = (uint32_t)track_count_at(c, eng, 0x101307cfu);
        c->esp += 4;
        a[1] = SEG_SIZE;
        uint32_t segs = call_import(c, IAT_CALLOC, 0x101307d9u, 2, a);
        wr32(c, cur + CUR_SEGMENTS, segs);
        if (segs) {
            wr32(c, cur + CUR_START, (uint32_t)start);
            wr32(c, cur + CUR_END, (uint32_t)end);
            reset_segments(c, eng, segs, 0x101307f7u, 0x10130814u);
        } else {
            a[0] = cur;
            call_import(c, IAT_FREE, 0x1013082bu, 1, a);
            cur = 0;
        }
    }
    *fail_eax = c->eax & 0xffffff00u;                   /* malloc's 0, or what free left */
    wr32(c, synth_ctx(c, eng) + SX_CURSOR, cur);
    return rd32(c, synth_ctx(c, eng) + SX_CURSOR) != 0;
}

/* Move the track's segment forward until it reaches past t: the old end becomes the start, the next
 * breakpoint the end. A track that runs out of breakpoints is held at its last value until the cursor's
 * end (or 0 if it never had one). */
static void advance_segment(cpu *c, uint32_t eng, int32_t track, uint32_t seg, uint32_t cur, int32_t t,
                            uint32_t locals)
{
    int32_t t1 = (int32_t)rd32(c, seg + SEG_T1);
    while (t1 < t) {
        uint32_t time = 0;
        int32_t value = 0;
        if (t1 == -1) {                                 /* the first breakpoint: flat until it */
            if (next_point_at(c, eng, track, seg, cur, locals, 0x10130950u, &time, &value)) {
                wr32(c, seg + SEG_T0, 0);
                wr32(c, seg + SEG_V0, (uint32_t)value);
                wr32(c, seg + SEG_T1, time);
                wr32(c, seg + SEG_V1, (uint32_t)value);
            } else {
                wr32(c, seg + SEG_T0, 0);
                wr32(c, seg + SEG_V0, 0);
                wr32(c, seg + SEG_V1, 0);
                wr32(c, seg + SEG_T1, rd32(c, cur + CUR_END));
            }
        } else {
            wr32(c, seg + SEG_T0, (uint32_t)t1);
            wr32(c, seg + SEG_V0, rd32(c, seg + SEG_V1));
            if (next_point_at(c, eng, track, seg, cur, locals, 0x1013099bu, &time, &value)) {
                wr32(c, seg + SEG_T1, time);
                wr32(c, seg + SEG_V1, (uint32_t)value);
            } else {
                wr32(c, seg + SEG_T1, rd32(c, cur + CUR_END));
            }
        }
        t1 = (int32_t)rd32(c, seg + SEG_T1);
    }
}

/* The track's value at t: v0 + (t - t0) (v1 - v0) / (t1 - t0), truncated (fild, fimul, fidiv, fiadd,
 * _ftol: each step rounded to double). */
static int32_t interpolate(cpu *c, uint32_t seg, int32_t t, uint32_t locals)
{
    uint32_t t0 = rd32(c, seg + SEG_T0), v0 = rd32(c, seg + SEG_V0);
    int32_t dt = (int32_t)(rd32(c, seg + SEG_T1) - t0);
    int32_t dv = (int32_t)(rd32(c, seg + SEG_V1) - v0);
    wr32(c, locals + LOC_DT, (uint32_t)dt);
    wr32(c, seg + SEG_DT, (uint32_t)dt);
    wr32(c, locals + LOC_V0, v0);
    wr32(c, locals + LOC_DV, (uint32_t)dv);
    wr32(c, seg + SEG_DV, (uint32_t)dv);
    if (dv == 0 || dt == 0) {
        wr32(c, locals + LOC_X, v0);
        return (int32_t)v0;
    }
    wr32(c, locals + LOC_X, (uint32_t)t - t0);
    double x = (double)(int32_t)((uint32_t)t - t0) * dv;
    x = x / dt;
    x = x + (int32_t)v0;
    int32_t r = ftol(x);
    wr32(c, locals + LOC_X, (uint32_t)r);
    return r;
}

uint32_t framer_build(cpu *c, uint32_t args, uint32_t frame)
{
    uint32_t eng = rd32(c, args + ARG_ENG);
    int32_t start = (int32_t)rd32(c, args + ARG_START);
    int32_t end = (int32_t)rd32(c, args + ARG_END);
    int incremental = rd8(c, args + ARG_INCREMENTAL) != 0;
    int new_utt = rd8(c, args + ARG_NEW_UTT) != 0;
    int final = rd8(c, args + ARG_FINAL) != 0;
    uint32_t step = rd32(c, args + ARG_STEP);
    uint32_t map = rd32(c, args + ARG_SLOT_MAP) + SLOT_MAP_TRACKS;
    uint32_t defaults = rd32(c, args + ARG_DEFAULTS);
    int failed = 0;
    const uint32_t locals = c->esp;
    wr8(c, locals + LOC_FAILED, 0);
    wr32(c, locals + LOC_V0, step);
    wr32(c, locals + LOC_V0 + 4, 0);

    if (start == 0) wr32(c, synth_ctx(c, eng) + SX_AT_START, 0);
    wr32(c, synth_ctx(c, eng) + SX_REQ_END, (uint32_t)end);

    /* the frame starts as the default frame, with the step as its length */
    for (uint32_t i = 0; i < FRAMER_SLOTS; i++) wr32(c, frame + 4 * i, rd32(c, defaults + 4 * i));
    wr32(c, frame + 4 * SLOT_LENGTH, fbits((float)step));

    /* incremental mode: continue where the last call stopped, and stop where the data does. (The
     * original keeps start and end in its argument slots, so the caller's stack sees the changes.) */
    if (incremental) {
        if (!new_utt) {
            start = (int32_t)rd32(c, synth_ctx(c, eng) + SX_RESUME);
            wr32(c, args + ARG_START, (uint32_t)start);
        }
        if (!final) {
            int32_t avail = end;                        /* the earliest end of the tracks' breakpoints */
            for (uint32_t i = 0; i < FRAMER_SLOTS; i++) {
                int32_t track = (int32_t)rd32(c, map + 4 * i);
                if (track == NO_TRACK) continue;
                uint32_t a[2] = { eng, (uint32_t)track };
                int32_t e = (int32_t)call_guest(c, TRACK_END_ADDR, 0x10130760u, 2, a);
                if (e < avail) avail = e;
            }
            if (avail < start) avail = start;
            if (step == 0) x86_fail(c, 0x1013078bu, "divide error");
            end = (int32_t)((uint32_t)avail - ((uint32_t)avail - (uint32_t)start) % step);   /* whole frames */
            wr32(c, args + ARG_END, (uint32_t)end);
        }
        wr32(c, synth_ctx(c, eng) + SX_RESUME, (uint32_t)end);
    }
    wr32(c, synth_ctx(c, eng) + SX_END, (uint32_t)end);

    /* where each track is: a new cursor, the old one reset, or (incremental, same utterance) kept */
    uint32_t cur = cursor(c, eng);
    if (cur == 0) {
        uint32_t fail_eax;
        if (!new_cursor(c, eng, start, end, &fail_eax)) return fail_eax;
    } else if (!new_utt && incremental) {
        wr32(c, cur + CUR_END, (uint32_t)end);
    } else {
        wr32(c, cur + CUR_START, (uint32_t)start);
        wr32(c, cur + CUR_END, (uint32_t)end);
        reset_segments(c, eng, rd32(c, cur + CUR_SEGMENTS), 0x10130885u, 0x101308a2u);
    }

    wr32(c, locals + LOC_T, (uint32_t)start);
    for (int32_t t = start; !failed; ) {
        /* the stop check is a call (FUN_10142350), whose argument and return address the stack keeps */
        if ((uint8_t)call_guest(c, STOP_ADDR, 0x101308c7u, 1, &eng)) break;
        cur = cursor(c, eng);
        if (t < (int32_t)rd32(c, cur + CUR_START) || t >= (int32_t)rd32(c, cur + CUR_END)) break;

        /* the tracks' values at t */
        for (uint32_t i = 0; i < FRAMER_SLOTS; i++) {
            int32_t track = (int32_t)rd32(c, map + 4 * i);
            if (track == NO_TRACK) continue;
            cur = cursor(c, eng);
            uint32_t seg = rd32(c, cur + CUR_SEGMENTS) + (uint32_t)track * SEG_SIZE;
            advance_segment(c, eng, track, seg, cur, t, locals);
            wr32(c, frame + 4 * i, fbits((float)interpolate(c, seg, t, locals)));
        }
        wr32(c, locals + LOC_OUT, frame + 4 * FRAMER_SLOTS);
        wr32(c, locals + LOC_MAP, map + 4 * FRAMER_SLOTS);
        wr32(c, locals + LOC_LEFT, 0);

        /* the frame that reaches past the end: slot 0 = t + step - end (unsigned) */
        uint32_t next = (uint32_t)t + step;
        if ((int32_t)next > end) {
            wr32(c, locals + LOC_LAST, (uint32_t)t - (uint32_t)end + step);
            wr32(c, locals + LOC_LAST + 4, 0);
            wr32(c, frame + 4 * SLOT_LENGTH, fbits((float)((uint32_t)t - (uint32_t)end + step)));
        }

        uint32_t a[2] = { rd32(c, synth_ctx(c, eng) + SX_KLATT), frame };
        c->ebx = eng;                                   /* the original's registers, which the synthesizer saves */
        c->esi = (int32_t)next > end ? (uint32_t)t - (uint32_t)end + step : (uint32_t)t;
        c->edi = next;
        c->ebp = (uint32_t)end;
        if ((uint8_t)call_guest(c, SYNTH_ADDR, 0x10130a8bu, 2, a) == 0) {
            failed = 1;
            wr8(c, locals + LOC_FAILED, 1);
        }
        t = (int32_t)next;
        wr32(c, locals + LOC_T, next);
    }
    return !failed;
}
