/* rules_int - what the rule runtime's files (rules*.c) share: guest calls the way the original makes
 * them, and small accessors of the engine's structures (rules.h). */
#ifndef RULES_INT_H
#define RULES_INT_H

#include <string.h>

#include "rules.h"
#include "port.h"

/* engine functions not ported (yet): called through the machine */
void f_1013a040(cpu *c);
void f_1013b5b0(cpu *c);   /* resolve a sync variable's pending offset */
/* ported ones called through the machine (their frames as the original has them) */
void f_10131cd0(cpu *c);
void f_10131d70(cpu *c);

#define IAT_LONGJMP 0x101440a0u

/* -------------------------------------------------------------------------------- small helpers */

#define RS(eng) rd32(c, (eng) + ENG_RS)
/* the esp a function is entered with when the original calls it with esp at x and n arguments pushed */
#define AT(x, n) ((x) - 4u * (n) - 4u)
#define WS(eng) rd32(c, (eng) + ENG_WS)

/* engine functions the ports call through the machine where the original calls them (so that the
 * arguments, the return address and the callee's frame land on the stack where the original's do; the
 * callees are hand ports too, entered through their adapters) */
void f_10131520(cpu *c);    /* rl_ref_at */
void f_10138b80(cpu *c);    /* rl_push */
void f_10138c60(cpu *c);    /* rl_pop */
void f_10138920(cpu *c);    /* rl_compare */
void f_10138510(cpu *c);    /* rl_trail_ref */
void f_101314f0(cpu *c);    /* rl_trail_at */
void f_10138730(cpu *c);    /* rl_assign */
void f_10130e80(cpu *c);    /* rl_throw */
void f_10135d00(cpu *c);    /* rl_step */
void f_10134690(cpu *c);    /* rl_set_a */
void f_10135b20(cpu *c);    /* a stream's field type */
void f_10136a20(cpu *c);    /* rl_delete_range */
void f_101310f0(cpu *c);    /* rl_start_at */
void f_10135e40(cpu *c);    /* rl_step_to */
void f_10135f70(cpu *c);    /* rl_skip_marks */
void f_10136570(cpu *c);    /* rl_insert_token */
void f_101385f0(cpu *c);    /* rl_add */
void f_101386e0(cpu *c);    /* rl_is_negative */
void f_101313d0(cpu *c);    /* rl_utterance_reset */
void f_101355d0(cpu *c);    /* the rule state's status */
void f_10135900(cpu *c);    /* rl_ws_new */
void f_101364c0(cpu *c);    /* rl_token_init */
void f_10138d40(cpu *c);    /* pool_alloc for a stream's token */
void f_101360a0(cpu *c);    /* rl_next_token */

/* a cdecl call of an engine function with esp at `sp` before the arguments are pushed; `ret` is the
 * original's return address. Returns eax. */
static inline uint32_t call_at(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, int nargs, const uint32_t *args)
{
    uint32_t save = c->esp;
    c->esp = sp;
    uint32_t r = guest_call(c, f, ret, nargs, args);
    c->esp = save;
    return r;
}

static inline uint32_t call2(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b)
{
    uint32_t args[2] = { a, b };
    return call_at(c, sp, f, ret, 2, args);
}

static inline uint32_t call3(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d)
{
    uint32_t args[3] = { a, b, d };
    return call_at(c, sp, f, ret, 3, args);
}

/* an indirect call (a stream's getter/setter) with one argument */
static inline uint32_t icall1(cpu *c, uint32_t sp, uint32_t target, uint32_t ret, uint32_t a)
{
    uint32_t save = c->esp;
    c->esp = sp;
    push32(c, a);
    push32(c, ret);
    x86_icall(c, target);
    c->esp = save;
    return c->eax;
}

/* a cdecl call of one of the image's imports (through its IAT slot) with esp at `sp` before the arguments
 * are pushed. Returns eax. */
static inline uint32_t import_call(cpu *c, uint32_t sp, uint32_t iat, uint32_t ret, int nargs, const uint32_t *args)
{
    uint32_t save = c->esp;
    c->esp = sp;
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, ret);
    x86_icall(c, rd32(c, iat));
    c->esp = save;
    return c->eax;
}

/* msvcrt's longjmp(buf, 1), called from `sp` with return address `ret`; does not return */
static inline void guest_longjmp(cpu *c, uint32_t sp, uint32_t buf, uint32_t ret)
{
    c->esp = sp;
    push32(c, 1);
    push32(c, buf);
    push32(c, ret);
    x86_icall(c, rd32(c, IAT_LONGJMP));
    x86_fail(c, ret, "longjmp returned");
}

/* byte copies the way the original's moves do them (the areas never overlap) */
static inline void copy(cpu *c, uint32_t dst, uint32_t src, uint32_t n)
{
    for (; n >= 4; n -= 4, dst += 4, src += 4) wr32(c, dst, rd32(c, src));
    for (; n; n--, dst++, src++) wr8(c, dst, rd8(c, src));
}

/* the cursor: 4-byte mark, then stream, direction, fresh (7 bytes, copied as 4 + 2 + 1) */
static inline void copy_pos(cpu *c, uint32_t dst, uint32_t src)
{
    wr32(c, dst, rd32(c, src));
    wr16(c, dst + 4, rd16(c, src + 4));
    wr8(c, dst + 6, rd8(c, src + 6));
}

/* a sync variable: 10 bytes, copied as 4 + 4 + 2 */
static inline void copy_sv(cpu *c, uint32_t dst, uint32_t src)
{
    wr32(c, dst, rd32(c, src));
    wr32(c, dst + 4, rd32(c, src + 4));
    wr16(c, dst + 8, rd16(c, src + 8));
}

/* stream descriptor fields */
static inline uint32_t stream_desc(uint32_t s) { return STREAM_TABLE + STREAM_SIZE * s; }
static inline uint32_t field_desc(cpu *c, uint32_t s, uint32_t f)
{
    return rd32(c, stream_desc(s) + SD_FIELDS) + FIELD_SIZE * f;
}
static inline uint32_t getter(cpu *c, uint32_t s, uint32_t f) { return rd32(c, rd32(c, stream_desc(s) + SD_GETTERS) + 4 * f); }

/* a sync mark's links in stream s */
static inline uint32_t fwd_link(cpu *c, uint32_t mark, uint32_t s) { return rd32(c, mark + 0xc + 4 * s); }
static inline uint32_t back_link(cpu *c, uint32_t rs, uint32_t mark, uint32_t s)
{
    return rd32(c, mark + 4 * (rd32(c, rs + RS_BACK) + s));
}
/* the link the cursor follows: forward or backward in its stream */
static inline uint32_t cursor_link(cpu *c, uint32_t rs, uint32_t mark, uint32_t s)
{
    return rd8(c, rs + RS_POS_BACK) ? back_link(c, rs, mark, s) : fwd_link(c, mark, s);
}
static inline int is_mark(cpu *c, uint32_t e) { return (rd8(c, e) & 2) != 0; }

/* the control stack grows down: take `size` more bytes (WS_TOP, then WS_TOP_OFF, as the original) */
static inline uint32_t cs_push(cpu *c, uint32_t eng, uint32_t size_field)
{
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_TOP, rd32(c, ws + WS_TOP) - rd32(c, ws + size_field));
    ws = WS(eng);
    wr32(c, ws + WS_TOP_OFF, rd32(c, ws + WS_TOP_OFF) - rd32(c, ws + size_field));
    return rd32(c, ws + WS_TOP);
}

/* ... and give them back (WS_TOP_OFF first here, as rl_backtrack does) */
static inline void cs_pop(cpu *c, uint32_t eng, uint32_t size)
{
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_TOP_OFF, rd32(c, ws + WS_TOP_OFF) + size);
    ws = WS(eng);
    wr32(c, ws + WS_TOP, rd32(c, ws + WS_TOP) + size);
}
static inline void cs_pop_field(cpu *c, uint32_t eng, uint32_t size_field)
{
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_TOP_OFF, rd32(c, ws + WS_TOP_OFF) + rd32(c, ws + size_field));
    ws = WS(eng);
    wr32(c, ws + WS_TOP, rd32(c, ws + WS_TOP) + rd32(c, ws + size_field));
}

/* WS_TOP_OFF follows WS_TOP (after the top is set directly) */
static inline void cs_sync_off(cpu *c, uint32_t eng)
{
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_TOP_OFF, rd32(c, ws + WS_CHUNK_OFF) - rd32(c, rd32(c, ws + WS_CHUNK) + 0x10) + rd32(c, ws + WS_TOP));
}


/* the value core (rules.c) */
uint32_t rl_trail_ref(cpu *c, uint32_t eng, uint32_t ref);
void     rl_assign(cpu *c, uint32_t eng, uint32_t dst, uint32_t src);
void     rl_compare(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
void     rl_compare_sp(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b);
int32_t  rl_backtrack_sp(cpu *c, uint32_t sp, uint32_t eng, int32_t depth);
void     rl_assign_sp(cpu *c, uint32_t sp, uint32_t eng, uint32_t dst, uint32_t src);
void     rl_push(cpu *c, uint32_t eng, uint32_t ref);
uint32_t rl_pop(cpu *c, uint32_t eng, uint32_t ref);
uint32_t rl_mark_key(uint32_t m);
int      rl_is_negative(cpu *c, uint32_t ref);
void     rl_add(cpu *c, uint32_t eng, uint32_t dst, uint32_t src, uint32_t argslot);

/* rules.c, at a given entry esp */
uint32_t rl_leave_at(cpu *c, uint32_t sp, uint32_t eng);
void     rl_throw_at(cpu *c, uint32_t sp, uint32_t eng);
void     rl_ref_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t ref, uint32_t val);
uint32_t rl_trail_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val);
uint32_t rl_goto_a_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, int back, int fresh, uint32_t ret);
void     rl_set_a(cpu *c, uint32_t eng, uint32_t v);

/* msvcrt _ftol: truncate to int64, keep the low 32 bits (0 for the integer indefinite) */
static inline uint32_t ftol32(uint64_t bits)
{
    double v;
    memcpy(&v, &bits, 8);
    if (!(v > -9223372036854775808.0 && v < 9223372036854775808.0)) return 0;
    return (uint32_t)(uint64_t)(int64_t)v;
}

static inline void store_double(cpu *c, uint32_t a, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    wr64(c, a, b);
}

/* a value's type; a token value's field reference is used up after use (-1: the token itself) */
static inline int16_t val_type(cpu *c, uint32_t v) { return (int16_t)rd16(c, v); }
static inline void val_release(cpu *c, uint32_t v)
{
    if (val_type(c, v) >= 0) wr16(c, v + 2, 0xffff);
}

#endif
