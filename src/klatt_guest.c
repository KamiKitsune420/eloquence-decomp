/* klatt_guest - the hand-written synthesizer (klatt.c) in the place of the engine's FUN_1013caf0.
 *
 * The rest of the engine is still recompiled code (x2c --replace 0x1013caf0 keeps the recompiled one as
 * f_1013caf0_recomp); it calls f_1013caf0(state, frame) cdecl, which lands here. The state block is copied
 * out of the machine, synthesized on, and copied back. The output callback at state+0xa67 is the engine's
 * own code: it is called on the machine, with the state written back first since it reads the samples
 * there.
 */
#include "port.h"
#include "klatt.h"

#include <string.h>

#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

#define STATE_SIZE 0x1c00u
#define SYNTH_ADDR 0x1013caf0u
#define FRAME_BYTES 0x100u      /* the original's frame below its return address */
/* state fields (klatt.c) */
#define OUT_CALLBACK 0xa67u
#define S_GAIN      0x004a
#define S_OUT       0x070au
#define S_NBLOCK    0x14f3
#define S_OUT_ON    0x1add

typedef struct {
    cpu *c;
    uint32_t state_addr;
    uint8_t *state;
    uint8_t scratch[8][1024];
    int next;
    klatt_trace tr;         /* the original's stack frame */
    int32_t called;         /* the block whose output called the callback last */
} guest;

/* image data (tables, the handle string): within one page it is read in place, else copied */
static const void *img(void *user, uint32_t addr, size_t n)
{
    guest *g = (guest *)user;
    if ((addr & (PAGE_SIZE - 1)) + n <= PAGE_SIZE) return MP(g->c, addr);
    if (n > sizeof g->scratch[0]) return NULL;
    uint8_t *b = g->scratch[g->next++ & 7];
    x86_read(g->c, addr, b, (uint32_t)n);
    return b;
}

/* The original's stack below its frame when it outputs a block: its call of FUN_1013c6c0(state, count),
 * which keeps { count, samples } in two locals and saves edi (and, with output on, ebx and esi while it
 * applies the gain). The engine's callback is then called from there (see output). */
static void output_frame(guest *g, int32_t count, int gain_calls)
{
    cpu *c = g->c;
    const uint32_t x = g->tr.stack;                 /* the original's esp in its frame */
    wr32(c, x - 0x04, (uint32_t)count);
    wr32(c, x - 0x08, g->state_addr);
    wr32(c, x - 0x0c, 0x1013e83fu);                 /* the return address */
    wr32(c, x - 0x18, g->tr.edi);
    if (!g->state[S_OUT_ON]) return;
    wr32(c, x - 0x1c, g->tr.ebx);
    wr32(c, x - 0x20, g->state_addr);
    wr32(c, x - 0x14, (uint32_t)count);
    wr32(c, x - 0x10, g->state_addr + S_OUT);
    if (gain_calls) wr32(c, x - 0x24, 0x1013c704u); /* _ftol's return address */
}

static int gain_calls(const guest *g, int32_t count)
{
    float gain;
    memcpy(&gain, g->state + S_GAIN, 4);
    return (double)gain != 1.0 && count > 0;
}

/* the engine's output callback: (cookie, &{count, samples}) cdecl, returning a byte. It is called where
 * the original calls it, with the original's registers (it saves them on the stack). */
static uint8_t output(void *user, uint32_t cookie, void *samples, int32_t count)
{
    guest *g = (guest *)user;
    cpu *c = g->c;
    (void)samples;                                  /* the callback reads them from state+0x70a */
    x86_write(c, g->state_addr, g->state, STATE_SIZE);
    output_frame(g, count, gain_calls(g, count));
    g->called = g->tr.blocks;
    uint32_t fn = rd32(c, g->state_addr + OUT_CALLBACK);
    const uint32_t x = g->tr.stack;
    uint32_t save = c->esp, ebx = c->ebx, ebp = c->ebp, esi = c->esi, edi = c->edi;
    c->esp = x - 0x18;
    push32(c, x - 0x14);                            /* &{ count, samples } */
    push32(c, cookie);
    push32(c, 0x1013c728u);                         /* the return address in FUN_1013c6c0 */
    c->eax = x - 0x14;
    c->ecx = cookie;
    c->ebx = g->tr.ebx;
    c->ebp = g->tr.ebp;
    c->esi = g->state_addr;
    c->edi = g->state_addr;
    x86_icall(c, fn);
    uint32_t r = c->eax;
    c->esp = save;
    c->ebx = ebx;
    c->ebp = ebp;
    c->esi = esi;
    c->edi = edi;
    x86_read(c, g->state_addr, g->state, STATE_SIZE);
    return (uint8_t)r;
}

PORT_FN(1013caf0)
{
    if (x86_on_enter) x86_on_enter(c, SYNTH_ADDR);  /* frame dumps and voice effects see it first */
    static THREAD_LOCAL uint8_t state[STATE_SIZE];
    static THREAD_LOCAL guest g;
    uint32_t state_addr = rd32(c, c->esp + 4), frame_addr = rd32(c, c->esp + 8);
    float frame[KLATT_FRAME_SIZE];
    x86_read(c, state_addr, state, STATE_SIZE);
    x86_read(c, frame_addr, frame, sizeof frame);
    g.c = c;
    g.state_addr = state_addr;
    g.state = state;
    g.tr.stack = c->esp - FRAME_BYTES;             /* sub esp, 0xf0 and four pushes */
    g.called = -1;
    klatt_ctx ctx = { img, output, &g, state_addr, &g.tr };
    int r = klatt_synth(&ctx, state, frame);
    x86_write(c, state_addr, state, STATE_SIZE);
    if (r == 1) {
        /* what the original leaves on its stack: edi, saved after the handle check (ebx, ebp and esi are
         * the prologue's, written by PORT_FN), its locals, and the frame of its last output call */
        wr32(c, g.tr.stack, c->edi);
        for (uint32_t i = 0; i < KLATT_FRAME_BYTES; i++)
            if (g.tr.written[i]) wr8(c, g.tr.stack + i, g.tr.frame[i]);
        if (g.tr.blocks > 0 && g.called != g.tr.blocks) {
            int32_t n;
            memcpy(&n, state + S_NBLOCK, 4);
            output_frame(&g, n, gain_calls(&g, n));
        }
    }
    c->eax = (c->eax & 0xffffff00u) | ((uint32_t)r & 0xffu);
    c->esp += 4;                                     /* ret (cdecl) */
}
