/* engine - see engine.h */
#include "engine.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLLMAIN   0x10142e49u
#define GETOBJECT 0x1012b3e0u

struct eng {
    cpu *c;
    uint32_t obj;
    eng_handler handler;
    void *user;
    float rate;                      /* eng_set_rate; 0: as the engine says */
    void (*frame_hook)(void *user, float frame[64]);
    void *frame_user;
};

/* the synthesizer's init (FUN_1013c7f0, recompiled with an entry hook) gets its config by value at
 * [esp+8]; the sample rate is the float at +4, and everything rate-dependent is derived from it there */
#define SYNTH_INIT 0x1013c7f0u
#define SYNTH 0x1013caf0u            /* the synthesizer: (state, float frame[64]) every 5 ms */

static void on_enter(cpu *c, uint32_t fn)
{
    eng *e = (eng *)c->user;
    if (!e) return;
    if (fn == SYNTH_INIT && e->rate > 0) {
        uint32_t bits;
        memcpy(&bits, &e->rate, 4);
        wr32(c, c->esp + 8 + 4, bits);
    } else if (fn == SYNTH && e->frame_hook) {
        uint32_t frame = rd32(c, c->esp + 8);
        float f[64];
        x86_read(c, frame, f, sizeof f);
        e->frame_hook(e->frame_user, f);
        x86_write(c, frame, f, sizeof f);
    }
}

void eng_set_rate(eng *e, float hz)
{
    e->rate = hz;
    x86_on_enter = on_enter;
}

void eng_set_frame_hook(eng *e, void (*hook)(void *user, float frame[64]), void *user)
{
    e->frame_hook = hook;
    e->frame_user = user;
    x86_on_enter = on_enter;
}

/* the host side of callback address ENG_CB_BASE + n: hand it to the ECI layer, then pop the return
 * address and what the handler says the callee removes */
static void cb_entry(cpu *c, unsigned n)
{
    eng *e = (eng *)c->user;
    uint32_t pop = e->handler ? e->handler(e, c, n) : 0;
    c->esp += 4 + pop;
}

#define CB(n) static void cb_##n(cpu *c) { cb_entry(c, n); }
CB(0) CB(1) CB(2) CB(3) CB(4) CB(5) CB(6) CB(7) CB(8) CB(9) CB(10) CB(11) CB(12) CB(13) CB(14) CB(15)
static const guest_fn cb_stubs[16] = { cb_0, cb_1, cb_2, cb_3, cb_4, cb_5, cb_6, cb_7,
                                       cb_8, cb_9, cb_10, cb_11, cb_12, cb_13, cb_14, cb_15 };

static guest_fn resolve(cpu *c, uint32_t target)
{
    (void)c;
    if (target >= ENG_CB_BASE && target < ENG_CB_BASE + 16) return cb_stubs[target - ENG_CB_BASE];
    return NULL;
}

eng *eng_new(const char *syn_path, eng_handler handler, void *user)
{
    eng *e = (eng *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->handler = handler;
    e->user = user;
    e->c = x86_new();
    e->c->user = e;
    e->c->resolve = resolve;
    if (syn_path) {
        if (image_load(e->c, syn_path, NULL)) { eng_free(e); return NULL; }
    } else {
#ifdef ELOQ_EMBEDDED
        image_load_sections(e->c, enu_sections, enu_nsections);
#else
        eng_free(e);
        return NULL;
#endif
    }
    x86_bind_imports(e->c);
    uint32_t dllmain_args[3] = { 0x10000000u, 1, 0 };
    if (!x86_call(e->c, DLLMAIN, 0, 3, dllmain_args)) { eng_free(e); return NULL; }
    uint32_t out = x86_alloc(e->c, 4);
    uint32_t go_args[2] = { 2, out };               /* as ECI.DLL: getObject(2, &obj), then start() */
    x86_call(e->c, GETOBJECT, 1, 2, go_args);
    e->obj = rd32(e->c, out);
    x86_dealloc(e->c, out);
    if (!e->obj || eng_call(e, ENG_START, 0, 0, 0, 0, 0)) { eng_free(e); return NULL; }
    return e;
}

void eng_free(eng *e)
{
    if (!e) return;
    x86_free(e->c);
    free(e);
}

cpu *eng_cpu(eng *e) { return e->c; }
void *eng_user(eng *e) { return e->user; }

uint32_t eng_call(eng *e, int slot, int nargs, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    uint32_t args[5] = { e->obj, a1, a2, a3, a4 };
    uint32_t fn = rd32(e->c, rd32(e->c, e->obj) + 4u * (uint32_t)slot);
    return x86_call(e->c, fn, 0, nargs + 1, args);          /* stdcall: the method removes its arguments */
}

uint32_t eng_call_text(eng *e, int slot, const char *s)
{
    uint32_t p = x86_strdup(e->c, s);
    uint32_t r = eng_call(e, slot, 1, p, 0, 0, 0);
    x86_dealloc(e->c, p);
    return r;
}
