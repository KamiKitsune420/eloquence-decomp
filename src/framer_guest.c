/* framer_guest - the hand-written frame builder (framer.c) in the place of the engine's FUN_101306b0 and
 * the functions it calls.
 *
 * x2c --replace keeps the recompiled ones as f_XXXXXXXX_recomp; the engine's calls land here. Each adapter
 * takes the arguments where the original's calling convention puts them (cdecl on the stack; the queue
 * functions are MSVC __fastcall/__thiscall with the queue in ecx) and returns as the original does: the
 * result in eax (al for the bool-returning ones; the upper bits are the original's where the callers could
 * see a difference, which they never read), the return address popped, and for __thiscall its stack
 * argument too.
 */
#include "framer.h"

/* the frame builder's stack frame: 0x128 bytes of locals and four saved registers below the return
 * address, the frame buffer (62 floats) at the top of the locals */
#define FRAME_BYTES 0x138u
#define FRAME_BUF   0xf8u        /* below the return address */

static void ret(cpu *c, uint32_t eax, uint32_t pop)
{
    c->eax = eax;
    c->esp += 4 + pop;
}

void f_101306b0(cpu *c)
{
    uint32_t entry = c->esp;
    /* the frame lives where the original has it, so that the 64 floats the synthesizer is given end with
     * the same two words (this function's return address and first argument) */
    c->esp = entry - FRAME_BYTES;
    uint32_t r = framer_build(c, entry + 4, entry - FRAME_BUF);
    c->esp = entry;
    ret(c, r, 0);
}

/* bool FUN_1012f8a0(eng, short track, uint *time, int *value) */
void f_1012f8a0(cpu *c)
{
    uint32_t eng = rd32(c, c->esp + 4), time_p = rd32(c, c->esp + 12), value_p = rd32(c, c->esp + 16);
    int16_t track = (int16_t)rd16(c, c->esp + 8);
    uint32_t time = 0;
    int32_t value = 0;
    /* the original writes *value as soon as the first entry is read, *time only on success */
    uint32_t q = rd32(c, rd32(c, rd32(c, eng + 0x64) + 0x21)) + (uint32_t)track * 0x18;
    int had_entry = !framer_queue_empty(c, q);
    int ok = framer_next_point(c, eng, track, &time, &value);
    if (had_entry) wr32(c, value_p, (uint32_t)value);
    if (ok) wr32(c, time_p, time);
    ret(c, ok ? (time & 0xffffff00u) | 1u : 0, 0);
}

/* int FUN_1012f960(eng, short track): time of the track's last queued breakpoint */
void f_1012f960(cpu *c)
{
    ret(c, framer_track_end(c, rd32(c, c->esp + 4), (int16_t)rd16(c, c->esp + 8)), 0);
}

/* int FUN_1012f980(eng): number of tracks */
void f_1012f980(cpu *c)
{
    ret(c, (uint32_t)framer_track_count(c, rd32(c, c->esp + 4)), 0);
}

/* bool __fastcall FUN_10130bd0(queue) */
void f_10130bd0(cpu *c)
{
    ret(c, (uint32_t)framer_queue_empty(c, c->ecx), 0);
}

/* bool __thiscall FUN_10130c60(queue, uint *entry) */
void f_10130c60(cpu *c)
{
    uint32_t out = rd32(c, c->esp + 4), q = c->ecx, e;
    uint32_t buf = rd32(c, q), head = rd16(c, q + 6);
    int nonempty = buf != 0 && head != rd16(c, q + 8);
    if (nonempty) wr32(c, out, rd32(c, buf + head * 4));    /* before the queue changes, as the original */
    int r = framer_queue_pop(c, q, &e);
    ret(c, (uint32_t)r, 4);
}

/* bool __fastcall FUN_10130d40(queue) */
void f_10130d40(cpu *c)
{
    ret(c, (uint32_t)framer_queue_shrink(c, c->ecx), 0);
}

/* bool FUN_10142350(eng): stop requested */
void f_10142350(cpu *c)
{
    uint32_t eng = rd32(c, c->esp + 4);
    c->ecx = rd32(c, eng + 0x68);
    ret(c, (eng & 0xffffff00u) | (uint32_t)framer_stop_requested(c, eng), 0);
}
