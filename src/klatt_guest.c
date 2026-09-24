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
#define OUT_CALLBACK 0xa67u

typedef struct {
    cpu *c;
    uint32_t state_addr;
    uint8_t *state;
    uint8_t scratch[8][1024];
    int next;
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

/* the engine's output callback: (cookie, &{count, samples}) cdecl, returning a byte */
static uint8_t output(void *user, uint32_t cookie, void *samples, int32_t count)
{
    guest *g = (guest *)user;
    cpu *c = g->c;
    (void)samples;                                  /* the callback reads them from state+0x70a */
    x86_write(c, g->state_addr, g->state, STATE_SIZE);
    uint32_t fn = rd32(c, g->state_addr + OUT_CALLBACK);
    uint32_t save = c->esp;
    c->esp -= 8;                                    /* FUN_1013c6c0's locals: { count, samples } */
    wr32(c, c->esp, (uint32_t)count);
    wr32(c, c->esp + 4, g->state_addr + 0x70au);
    uint32_t args[2] = { cookie, c->esp };
    uint32_t r = x86_call(c, fn, 1, 2, args);
    c->esp = save;
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
    klatt_ctx ctx = { img, output, &g, state_addr };
    int r = klatt_synth(&ctx, state, frame);
    x86_write(c, state_addr, state, STATE_SIZE);
    c->eax = (c->eax & 0xffffff00u) | ((uint32_t)r & 0xffu);
    c->esp += 4;                                     /* ret (cdecl) */
}
