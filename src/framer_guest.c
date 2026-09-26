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
#include "port.h"

/* the frame builder's stack frame: 0x128 bytes of locals and four saved registers below the return
 * address, the frame buffer (62 floats) at the top of the locals */
#define FRAME_BYTES 0x138u
#define FRAME_BUF   0xf8u        /* below the return address */

#define ret port_ret

PORT_FN(101306b0)
{
    uint32_t entry = c->esp;
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    /* the frame lives where the original has it, so that the 64 floats the synthesizer is given end with
     * the same two words (this function's return address and first argument) */
    c->esp = entry - FRAME_BYTES;
    uint32_t r = framer_build(c, entry + 4, entry - FRAME_BUF);
    c->esp = entry;
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    ret(c, r, 0);
}

/* bool FUN_1012f8a0(eng, short track, uint *time, int *value) */
PORT_FN(1012f8a0)
{
    uint32_t eng = rd32(c, c->esp + 4), time_p = rd32(c, c->esp + 12), value_p = rd32(c, c->esp + 16);
    int16_t track = (int16_t)rd16(c, c->esp + 8);
    uint32_t time;
    int ok = framer_next_point(c, eng, track, time_p, value_p, &time);
    ret(c, ok ? (time & 0xffffff00u) | 1u : 0, 0);
}

/* int FUN_1012f960(eng, short track): time of the track's last queued breakpoint */
PORT_FN(1012f960)
{
    ret(c, framer_track_end(c, rd32(c, c->esp + 4), (int16_t)rd16(c, c->esp + 8)), 0);
}

/* int FUN_1012f980(eng): number of tracks */
PORT_FN(1012f980)
{
    ret(c, (uint32_t)framer_track_count(c, rd32(c, c->esp + 4)), 0);
}

/* bool __fastcall FUN_10130bd0(queue) */
PORT_FN(10130bd0)
{
    ret(c, (uint32_t)framer_queue_empty(c, c->ecx), 0);
}

/* bool __thiscall FUN_10130c60(queue, uint *entry) */
PORT_FN(10130c60)
{
    uint32_t r = framer_queue_pop(c, c->ecx, rd32(c, c->esp + 4));
    ret(c, r, 4);
}

/* bool __fastcall FUN_10130d40(queue) */
PORT_FN(10130d40)
{
    ret(c, framer_queue_shrink(c, c->ecx), 0);
}

/* bool FUN_10142350(eng): stop requested */
PORT_FN(10142350)
{
    uint32_t eng = rd32(c, c->esp + 4);
    c->ecx = rd32(c, eng + 0x68);
    ret(c, (eng & 0xffffff00u) | (uint32_t)framer_stop_requested(c, eng), 0);
}
