/* klatt - Eloquence 6.1's formant synthesizer (ENU.SYN FUN_1013caf0 and the five functions it calls),
 * hand-ported to C99 and bit-exact with the original.
 *
 * The engine keeps one synthesizer state block (KLATT_STATE_SIZE bytes, byte-packed, most fields at odd
 * offsets; the layout is documented in klatt.c) and calls the synthesizer once per parameter frame:
 * 64 floats, normally 5 ms. The synthesizer turns the frame into samples in blocks of at most 200,
 * leaves them as int32 in the state (offset 0x70a, count at 0x14f3) and hands each block to the output
 * callback.
 *
 * What the port needs from its host (klatt_ctx):
 *   img        read-only access to ENU.SYN's data at its preferred base (0x10000000): the dB and
 *              spectral-tilt tables and the handle's ID string. `n` is the number of bytes wanted;
 *              return NULL if they are not in the image.
 *   output     the callback that the state's +0xa67 pointer stands for in the original: called with
 *              the state's cookie (+0x4), the block's samples (little-endian int32, possibly unaligned:
 *              they live inside the state) and their count; its result is stored at +0x1a11.
 *   state_addr the guest address of the state block, or 0. Two fields of the state (+0xd93, +0x13e3)
 *              point to sample buffers inside the state itself, which KlattOpen (FUN_1013c980) always
 *              sets to state+0xa73 and state+0x10c3; the port uses those buffers, and if state_addr is
 *              nonzero it checks that the stored pointers agree.
 *   trace      NULL, or where to record what the original leaves on its stack (klatt_trace): for a host
 *              that runs the port in the original's place and wants its stack scratch exact too.
 */
#ifndef KLATT_H
#define KLATT_H

#include <stddef.h>
#include <stdint.h>

#define KLATT_STATE_SIZE 0x1c00
#define KLATT_FRAME_SIZE 64

/* The original's stack frame, for a host that keeps the stack scratch exact (see klatt_guest.c). The
 * frame is 0x100 bytes from the original's esp after its prologue (`stack`, a guest address the host
 * sets): the saved registers at 0..0xf, its locals at 0x10..0xff. The port fills in the bytes the
 * original leaves there (written[i] nonzero for those it wrote in this call) and, for each block it
 * outputs, the registers the original has when it calls the output function (FUN_1013c6c0), which that
 * function and the engine's callback save on the stack. */
#define KLATT_FRAME_BYTES 0x100

typedef struct klatt_trace {
    uint32_t stack;                         /* in: guest address of the frame */
    uint8_t frame[KLATT_FRAME_BYTES];       /* out */
    uint8_t written[KLATT_FRAME_BYTES];
    uint32_t ebx, ebp, edi;                 /* at the last output call */
    int32_t blocks;                         /* output calls made */
} klatt_trace;

typedef struct klatt_ctx {
    const void *(*img)(void *user, uint32_t addr, size_t n);
    uint8_t (*output)(void *user, uint32_t cookie, void *samples, int32_t count);
    void *user;
    uint32_t state_addr;
    klatt_trace *trace;
} klatt_ctx;

/* Synthesize one frame. Returns what FUN_1013caf0 returns in al: 1, or 0 if the state is not a
 * synthesizer handle. Returns -1 (the original has no such case) if the state's buffer pointers do not
 * point into the state. */
int klatt_synth(const klatt_ctx *ctx, uint8_t *state, const float frame[KLATT_FRAME_SIZE]);

/* Compare every numeric constant the port writes as a literal with the bytes at its address in the
 * image. Returns the number of mismatches; describes the first one in msg. */
int klatt_check_constants(const klatt_ctx *ctx, char *msg, size_t msgsize);

#endif
