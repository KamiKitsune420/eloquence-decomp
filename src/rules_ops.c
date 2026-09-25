/* rules_ops - the rule operations built on the core (rules.c): starting a match at a sync mark, the
 * sync variables A and B, number conditions, and the entry points that edit the delta (inserting,
 * deleting and changing tokens) around the delta functions at 0x1013a000+ (still recompiled, called
 * through the machine).
 *
 * As in rules.c: memory accesses in the original's order, eax as the original leaves it, and calls into
 * the machine at the esp the original has at that call (`sp` is the esp a function was entered with).
 */
#include "rules_int.h"

/* engine functions not ported (yet): called through the machine */
void f_1013ab30(cpu *c);   /* resolve A and B for a stream (edit preparation); 0 = ok */
void f_1013ae40(cpu *c);
void f_1013af00(cpu *c);
void f_1013afd0(cpu *c);   /* resolve A and B; al = ok */
void f_1013a840(cpu *c);   /* resolve a sync variable; al = ok */
void f_1013a890(cpu *c);
void f_1013a900(cpu *c);
void f_1013a930(cpu *c);
void f_1013a960(cpu *c);
void f_1013a9e0(cpu *c);
void f_1013a250(cpu *c);
void f_1013a570(cpu *c);
void f_1013b0a0(cpu *c);
void f_1013b8c0(cpu *c);
void f_1013ba30(cpu *c);
void f_1013be20(cpu *c);
void f_1013c2d0(cpu *c);   /* insert a sync mark */
void f_10132730(cpu *c);
void f_101328a0(cpu *c);
void f_10136200(cpu *c);
void f_10136240(cpu *c);
void f_10136280(cpu *c);   /* set a field of every token between two marks */
void f_101364c0(cpu *c);
void f_10136570(cpu *c);   /* insert a token */
void f_10136a20(cpu *c);
void f_10136b10(cpu *c);
void f_10137620(cpu *c);
void f_10137d20(cpu *c);
void f_101319a0(cpu *c);
void f_10141b30(cpu *c);
void f_101416f0(cpu *c);
void f_10141730(cpu *c);
void f_101430d0(cpu *c);
void f_10143380(cpu *c);
void f_10135a40(cpu *c);
void f_10135a60(cpu *c);

static inline uint32_t call1(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a)
{
    return call_at(c, sp, f, ret, 1, &a);
}
static uint32_t call4(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    uint32_t args[4] = { a, b, d, e };
    return call_at(c, sp, f, ret, 4, args);
}
static uint32_t call5(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e,
                      uint32_t g)
{
    uint32_t args[5] = { a, b, d, e, g };
    return call_at(c, sp, f, ret, 5, args);
}
static uint32_t call6(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e,
                      uint32_t g, uint32_t h)
{
    uint32_t args[6] = { a, b, d, e, g, h };
    return call_at(c, sp, f, ret, 6, args);
}

#define SV_A(eng) ((eng) + ENG_SYNC_A)
#define SV_B(eng) ((eng) + ENG_SYNC_B)

/* ------------------------------------------------------------------------ starting a match */

/* FUN_101310f0: start matching at the sync value v in stream s: set the rule's labels, turn trailing off,
 * A := v, and put the cursor on it (fresh), flagging s as matched. al 1, or 0 if v is null or not a
 * boundary of s. */
static int start_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t s, uint32_t v)
{
    uint32_t sv = SV_A(eng);
    wr32(c, RS(eng) + RS_FAIL, fail);
    wr32(c, RS(eng) + RS_NEXT, next);
    wr8(c, RS(eng) + RS_TRAIL, 0);
    wr8(c, sv + SV_STATE, 1);
    wr32(c, sv, rd32(c, v + 2));
    if (!(rd8(c, sv + SV_STATE) & 1)) {             /* (never: it was just set) */
        int32_t r = (int32_t)call2(c, sp - 0xc, f_1013b5b0, 0x1013113du, eng, sv);
        if (r >= 0 && r <= 2) return 0;
        wr8(c, sv + SV_STATE, 1);
    }
    uint32_t m = rd32(c, sv);
    if (!m) return 0;
    uint32_t rs = RS(eng);
    if (!(rd8(c, m + 4 * (rd32(c, rs + RS_BACK) + s)) & 1)) return 0;
    wr32(c, rs + RS_POS_MARK, m);
    wr8(c, RS(eng) + RS_POS_STREAM, (uint8_t)s);
    wr8(c, RS(eng) + RS_POS_FRESH, 1);
    wr8(c, rd32(c, eng + ENG_SEEN) + rd8(c, rd32(c, eng + ENG_SLOT) + s), 1);
    return 1;
}

uint32_t rl_start_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t s, uint32_t v)
{
    return (c->eax & 0xffffff00u) | (uint32_t)start_at(c, sp, eng, next, fail, s, v);
}

/* the end of a successful match start: cut, trailing on, the rule goes on at `label`, `out` := the
 * cursor's mark. Returns 2. */
static uint32_t matched(cpu *c, uint32_t eng, uint32_t label, uint32_t out)
{
    rl_cut(c, eng);
    wr32(c, WS(eng) + 0x8f, 0);
    wr8(c, RS(eng) + RS_TRAIL, 1);
    uint32_t rs = RS(eng);
    wr32(c, eng + ENG_RESULT, label);
    wr32(c, out + 2, rd32(c, rs + RS_POS_MARK));
    return 2;
}

/* is the element after the cursor's mark (forward or backward link) a token */
static int token_next(cpu *c, uint32_t eng, int back)
{
    uint32_t rs = RS(eng);
    uint32_t m = rd32(c, rs + RS_POS_MARK), s = rd8(c, rs + RS_POS_STREAM);
    uint32_t e = (back ? back_link(c, rs, m, s) : fwd_link(c, m, s)) & ~3u;
    return e && !is_mark(c, e);
}

/* The match starts (FUN_10133960 .. FUN_101340f0), arguments (eng, next, fail, label, stream, v, stop):
 * start at v in the stream, move the cursor as each one does, and on success cut and go on at `label`
 * with v := the cursor's mark (2); 1 if the start failed, 0 if the move did. */

/* FUN_10133960: one step backward (across a token) */
uint32_t rl_start_back_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                            uint32_t s, uint32_t v)
{
    if (!start_at(c, AT(sp - 8, 5), eng, next, fail, s & 0xff, v)) return 1;
    wr8(c, RS(eng) + RS_POS_BACK, 1);
    if (!rl_step(c, eng, 1, 0)) return 0;
    return matched(c, eng, label, v);
}

/* FUN_10133a50: backward over marks, a token, marks, and another token must follow */
uint32_t rl_start_back_token2(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                              uint32_t s, uint32_t v)
{
    if (!start_at(c, AT(sp - 8, 5), eng, next, fail, s & 0xff, v)) return 1;
    wr8(c, RS(eng) + RS_POS_BACK, 1);
    rl_skip_marks(c, eng, 0);
    if (!token_next(c, eng, 1)) return 0;
    if (!rl_step(c, eng, 1, 0)) return 0;
    rl_skip_marks(c, eng, 0);
    if (!token_next(c, eng, 1)) return 0;
    return matched(c, eng, label, v);
}

/* FUN_10133bb0: backward over marks and one token */
uint32_t rl_start_back_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                             uint32_t s, uint32_t v)
{
    if (!start_at(c, AT(sp - 8, 5), eng, next, fail, s & 0xff, v)) return 1;
    wr8(c, RS(eng) + RS_POS_BACK, 1);
    rl_skip_marks(c, eng, 0);
    if (!token_next(c, eng, 1)) return 0;
    if (!rl_step(c, eng, 1, 0)) return 0;
    return matched(c, eng, label, v);
}

/* FUN_10133ce0 (forward) / FUN_10133fc0 (backward): one step, not onto `stop` */
uint32_t rl_start_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label, uint32_t s,
                       uint32_t v, uint32_t stop, int back)
{
    if (!start_at(c, AT(sp - 0xc, 5), eng, next, fail, s & 0xff, v)) return 1;
    wr8(c, RS(eng) + RS_POS_BACK, (uint8_t)back);
    if (!rl_step(c, eng, 1, 0) || rd32(c, RS(eng) + RS_POS_MARK) == rd32(c, stop + 2)) return 0;
    return matched(c, eng, label, v);
}

/* FUN_10133e10 (forward) / FUN_101340f0 (backward): over marks towards `stop`, a token, one step, marks
 * towards stop again, and a token must follow, never reaching stop */
uint32_t rl_start_span(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label, uint32_t s,
                       uint32_t v, uint32_t stop, int back)
{
    if (!start_at(c, AT(sp - 0xc, 5), eng, next, fail, s & 0xff, v)) return 1;
    wr8(c, RS(eng) + RS_POS_BACK, (uint8_t)back);
    rl_step_to(c, eng, rd32(c, stop + 2), 0);
    if (rd32(c, RS(eng) + RS_POS_MARK) == rd32(c, stop + 2) || !token_next(c, eng, back)) return 0;
    if (!rl_step(c, eng, 1, 0) || rd32(c, RS(eng) + RS_POS_MARK) == rd32(c, stop + 2)) return 0;
    rl_step_to(c, eng, rd32(c, stop + 2), 0);
    if (rd32(c, RS(eng) + RS_POS_MARK) == rd32(c, stop + 2) || !token_next(c, eng, back)) return 0;
    return matched(c, eng, label, v);
}

/* ------------------------------------------------------------------------ conditions on values */

/* FUN_10134300: dst := src (trailed), then the rule's labels and a cut */
void rl_assign_then(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t dst, uint32_t src)
{
    uint32_t rd = sp - 8, rsrc = sp - 0x10;
    /* through the machine as the original: esi eng, edi dst, then ebx src */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi;
    c->esi = eng;
    c->edi = dst;
    c->eax = RS(eng);
    if (rd8(c, RS(eng) + RS_TRAIL)) call2(c, sp - 0x20, f_101314f0, 0x10134323u, eng, dst);
    c->ecx = rd;
    call3(c, sp - 0x20, f_10131520, 0x10134332u, eng, rd, dst);
    c->ebx = src;
    c->edx = rsrc;
    call3(c, sp - 0x2c, f_10131520, 0x10134342u, eng, rsrc, src);
    c->eax = rsrc;
    c->ecx = rd;
    call3(c, sp - 0x38, f_10138730, 0x10134352u, eng, rd, rsrc);
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    wr32(c, RS(eng) + RS_FAIL, fail);
    wr32(c, RS(eng) + RS_NEXT, next);
    rl_cut(c, eng);
    wr32(c, WS(eng) + 0x8f, 0);
    val_release(c, dst);
    val_release(c, src);
}

/* FUN_10134400: with labels set, a := a + step, and continue (2) while a has not passed the limit:
 * a <= limit counting up, a >= limit counting down (step negative) */
uint32_t rl_for_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t a, uint32_t limit,
                     uint32_t step)
{
    uint32_t ra = sp - 0x18, rstep = sp - 0x10, rlim = sp - 8;
    wr32(c, RS(eng) + RS_FAIL, fail);
    wr32(c, RS(eng) + RS_NEXT, next);
    wr8(c, RS(eng) + RS_TRAIL, 0);
    rl_ref_at(c, AT(sp - 0x28, 3), eng, ra, a);
    rl_ref_at(c, AT(sp - 0x34, 3), eng, rstep, step);
    rl_ref_at(c, AT(sp - 0x40, 3), eng, rlim, limit);
    rl_add(c, eng, ra, rstep, 0);
    rl_compare(c, eng, ra, rlim);
    val_release(c, a);
    val_release(c, limit);
    val_release(c, step);
    int8_t cmp = (int8_t)rd8(c, RS(eng) + RS_CMP);
    if (rl_is_negative(c, rstep)) return cmp != -1 ? 2 : 0;
    return cmp != 1 ? 2 : 0;
}

/* FUN_10134500: the loop test without the step: 2 (going on at RS_NEXT) when a has passed the limit */
uint32_t rl_for_done(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t limit, uint32_t step)
{
    uint32_t ra = sp - 0x10, rlim = sp - 0x18, rstep = sp - 8;
    wr8(c, RS(eng) + RS_TRAIL, 0);
    rl_ref_at(c, AT(sp - 0x28, 3), eng, ra, a);
    rl_ref_at(c, AT(sp - 0x34, 3), eng, rlim, limit);
    rl_ref_at(c, AT(sp - 0x40, 3), eng, rstep, step);
    rl_compare(c, eng, ra, rlim);
    val_release(c, a);
    val_release(c, limit);
    val_release(c, step);
    int neg = rl_is_negative(c, rstep);
    uint32_t rs = RS(eng);
    int8_t cmp = (int8_t)rd8(c, rs + RS_CMP);
    if (neg ? cmp == -1 : cmp == 1) {
        wr32(c, eng + ENG_RESULT, rd32(c, rs + RS_NEXT));
        return 2;
    }
    return 0;
}

/* FUN_10131f00: compare the popped value with the byte k (RS_CMP) */
void rl_cmp_popped_byte(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t popped = sp - 0x10, kref = sp - 8;
    uint32_t esi = c->esi;                  /* the original keeps eng in esi across its calls */
    c->esi = eng;
    c->eax = popped;
    call2(c, sp - 0x14, f_10138c60, 0x10131f13u, eng, popped);
    if ((int16_t)rd16(c, popped + 4) == T_SYM8) {
        uint8_t v = rd8(c, rd32(c, popped)), k = rd8(c, sp + 8);
        wr8(c, RS(eng) + RS_CMP, v == k ? 0 : v > k ? 1 : 0xff);
    } else {
        wr16(c, kref + 4, 0xffff);
        wr32(c, kref, sp + 8);
        wr8(c, kref + 6, 0);
        c->eax = popped;
        c->ecx = kref;
        c->edx = sp + 8;
        call3(c, sp - 0x14, f_10138920, 0x10131f7eu, eng, kref, popped);
    }
    c->esi = esi;
}

/* FUN_101333c0: var := the next token forward from the cursor in its stream (the whole token). 0, or 1
 * if there is none. */
uint32_t rl_take_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t var)
{
    uint32_t rs = RS(eng);
    uint32_t s = rd8(c, rs + RS_POS_STREAM);
    uint32_t e = fwd_link(c, rd32(c, rs + RS_POS_MARK), s) & ~3u;
    for (; e; e = rd32(c, e + 0xc + 4 * s) & ~3u)
        if (!is_mark(c, e)) break;
    if (!e) {
        val_release(c, var);
        return 1;
    }
    uint32_t vref = sp - 8, tref = sp - 0x10;
    /* through the machine as the original: edi eng, esi the token, ebx var (pushed here) */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi;
    wr32(c, sp - 0x1c, ebx);
    c->edi = eng;
    c->esi = e;
    c->ebx = var;
    c->eax = vref;
    call3(c, sp - 0x1c, f_10131520, 0x10133420u, eng, vref, var);
    c->esi = e + 8;
    rs = RS(eng);
    wr16(c, tref + 4, rd8(c, rs + RS_POS_STREAM));
    wr32(c, tref, e + 8);
    wr8(c, tref + 6, 0);
    if (rd8(c, rs + RS_TRAIL)) {
        c->edx = vref;
        call2(c, sp - 0x1c, f_10138510, 0x10133454u, eng, vref);
    }
    c->eax = tref;
    c->ecx = vref;
    call3(c, sp - 0x1c, f_10138730, 0x10133467u, eng, vref, tref);
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    val_release(c, var);
    return 0;
}

/* ------------------------------------------------------------------------ sync variables */

/* FUN_101346d0: A := a pending offset of n tokens (a number value) in stream s from its mark */
void rl_set_a_offset(cpu *c, uint32_t sp, uint32_t eng, uint8_t s, uint32_t n)
{
    wr8(c, SV_A(eng) + SV_STREAM, s);
    wr8(c, SV_A(eng) + SV_STATE, 2);
    switch (val_type(c, n)) {
    case T_DOUBLE: wr32(c, SV_A(eng) + SV_OFFSET, ftol32(rd64(c, n + 2))); return;
    case T_SHORT: wr32(c, SV_A(eng) + SV_OFFSET, (uint32_t)(int32_t)(int16_t)rd16(c, n + 2)); return;
    case T_INT: wr32(c, SV_A(eng) + SV_OFFSET, rd32(c, n + 2)); return;
    default: rl_throw_at(c, AT(sp - 4, 1), eng);
    }
}

/* FUN_10134720 / 10134780 / 101347e0 / 10134840: resolve the sync variable (FUN_1013a900; abort if it
 * cannot be) and make its mark a boundary of stream s, inserting one (FUN_1013c2d0) if it is not */
void rl_sv_bound(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t s, int where, uint32_t base)
{
    uint32_t esi = c->esi, edi = c->edi;    /* the original: esi eng, edi sv */
    c->esi = eng;
    c->edi = sv;
    if (!(call2(c, sp - 8, f_1013a900, base + 0x10, eng, sv) & 0xff)) {
        uint32_t a1[1] = { eng };
        call_at(c, sp - 8, f_10130e80, base + 0x1d, 1, a1);     /* longjmps */
    }
    uint32_t m = rd32(c, sv);
    wr32(c, sp - 0xc, c->ebx);              /* push ebx around the stream check */
    if (!(rd8(c, m + 4 * (rd32(c, RS(eng) + RS_BACK) + (s & 0xff))) & 1)) {
        c->ecx = s;
        c->eax = m;
        wr32(c, sv, call5(c, sp - 8, f_1013c2d0, base + 0x4d, eng, (uint32_t)where, 1, m, s));
    }
    c->esi = esi;
    c->edi = edi;
}

/* FUN_101348a0 / 101348e0 (FUN_10136200) and FUN_10134920 (FUN_10136240): resolve the sync variable
 * (FUN_1013a930) and move it with the given function */
void rl_sv_move(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t n, int with_eng, uint32_t base)
{
    uint32_t esi = c->esi, edi = c->edi;       /* the original's registers across its calls: edi eng, esi sv */
    c->edi = eng;
    c->esi = sv;
    if (!(call2(c, sp - 8, f_1013a930, base + 0x10, eng, sv) & 0xff)) rl_throw_at(c, AT(sp - 8, 1), eng);
    uint32_t m = rd32(c, sv);
    wr32(c, sv, with_eng ? call3(c, sp - 8, f_10136240, base + 0x2e, eng, m, n)
                         : call2(c, sp - 8, f_10136200, base + 0x2d, m, n));
    c->esi = esi;
    c->edi = edi;
}

/* FUN_10134960 / 101349c0: resolve A (FUN_1013a9e0, `kind`) and make it a boundary of s. 0, or 1. */
uint32_t rl_a_bound(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, int kind, int where, uint32_t base)
{
    uint32_t sv = SV_A(eng);
    uint32_t esi = c->esi, edi = c->edi;       /* the original's registers across its calls: esi eng, edi sv */
    c->esi = eng;
    c->edi = sv;
    uint32_t r = 1;
    if (!call3(c, sp - 8, f_1013a9e0, base + 0x12, eng, sv, (uint32_t)kind)) {
        uint32_t m = rd32(c, sv);
        wr32(c, sp - 0xc, c->ebx);            /* push ebx around the stream check */
        if (!(rd8(c, m + 4 * (rd32(c, RS(eng) + RS_BACK) + (s & 0xff))) & 1)) {
            c->ecx = s;                        /* the callee's `push ecx` saves these */
            c->eax = m;
            wr32(c, sv, call5(c, sp - 8, f_1013c2d0, base + 0x46, eng, (uint32_t)where, 1, m, s));
        }
        r = 0;
    }
    c->esi = esi;
    c->edi = edi;
    return r;
}

/* FUN_10134a20 / 10134a60: resolve A (FUN_1013a960) and move it (FUN_10136200 / FUN_10136240). 0, or 1.
 * r1, r2: the original's return addresses of the two calls. */
uint32_t rl_a_move(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, int with_eng, uint32_t r1, uint32_t r2)
{
    uint32_t sv = SV_A(eng);
    /* the original keeps sv in esi across its calls; FUN_10134a60 also saves edi and keeps eng in it (its
     * calls are 4 bytes lower) */
    uint32_t esi = c->esi, edi = c->edi, csp = with_eng ? sp - 8 : sp - 4;
    c->esi = sv;
    if (with_eng) c->edi = eng;
    uint32_t r = 1;
    if (!call3(c, csp, f_1013a960, r1, eng, sv, 0)) {
        uint32_t m = rd32(c, sv);
        wr32(c, sv, with_eng ? call3(c, csp, f_10136240, r2, eng, m, n) : call2(c, csp, f_10136200, r2, m, n));
        r = 0;
    }
    c->esi = esi;
    c->edi = edi;
    return r;
}

/* FUN_10134aa0 / 10134b00: val := the sync variable (resolved by FUN_1013a840; trailed). Returns eax. */
uint32_t rl_get_sv(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t val, uint32_t base)
{
    uint32_t esi = c->esi, edi = c->edi, r = val;   /* the original: esi eng, edi sv */
    c->esi = eng;
    c->edi = sv;
    if (!(call2(c, sp - 8, f_1013a840, base + 0x10, eng, sv) & 0xff)) {
        uint32_t a1[1] = { eng };
        call_at(c, sp - 8, f_10130e80, base + 0x1d, 1, a1);     /* longjmps */
    }
    if (rd8(c, RS(eng) + RS_TRAIL)) {
        uint32_t ebx = c->ebx;
        wr32(c, sp - 0xc, ebx);             /* push ebx; ebx val */
        c->ebx = val;
        r = call2(c, sp - 0xc, f_101314f0, base + 0x39, eng, val);
        c->ebx = ebx;
    }
    wr32(c, val + 2, rd32(c, sv));
    c->esi = esi;
    c->edi = edi;
    return r;
}

/* ------------------------------------------------------------------------ editing the delta */

/* resolve (FUN_1013a840, abort if not) then an edit that returns al (abort if 0) */
void rl_edit_a(cpu *c, uint32_t sp, uint32_t eng, uint32_t k, uint32_t base)
{
    /* FUN_10134e40: A resolved, FUN_1013be20(eng, A, k); the original: esi eng, edi A's address */
    uint32_t esi = c->esi, edi = c->edi, ok = 0;
    c->esi = eng;
    c->edi = SV_A(eng);
    if (call2(c, sp - 8, f_1013a840, base + 0x10, eng, SV_A(eng)) & 0xff) {
        c->ecx = rd32(c, SV_A(eng));
        c->eax = k;
        ok = call3(c, sp - 8, f_1013be20, base + 0x25, eng, rd32(c, SV_A(eng)), k) & 0xff;
    }
    if (!ok) {
        uint32_t a1[1] = { eng };
        call_at(c, sp - 8, f_10130e80, base + 0x32, 1, a1);     /* longjmps */
    }
    c->esi = esi;
    c->edi = edi;
}

/* FUN_10134e80 (FUN_1013ba30, B resolved) / FUN_10134ec0 (FUN_1013b8c0, A resolved): an edit between B
 * and A */
void rl_edit_ba(cpu *c, uint32_t sp, uint32_t eng, uint32_t k, uint32_t sv, guest_fn edit, uint32_t base)
{
    /* the original: esi eng, edi sv's address */
    uint32_t esi = c->esi, edi = c->edi, ok = 0;
    c->esi = eng;
    c->edi = sv;
    if (call2(c, sp - 8, f_1013a840, base + 0x10, eng, sv) & 0xff) {
        c->edx = rd32(c, SV_B(eng));
        c->ecx = rd32(c, SV_A(eng));
        c->eax = k;
        ok = call4(c, sp - 8, edit, base + 0x29, eng, rd32(c, SV_B(eng)), rd32(c, SV_A(eng)), k) & 0xff;
    }
    if (!ok) {
        uint32_t a1[1] = { eng };
        call_at(c, sp - 8, f_10130e80, base + 0x36, 1, a1);     /* longjmps */
    }
    c->esi = esi;
    c->edi = edi;
}

/* FUN_10134fd0: A resolved, FUN_10136b10(eng, k, A, 1) */
void rl_edit_a_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t k)
{
    uint32_t esi = c->esi, edi = c->edi, ok = 0;   /* the original: esi eng, edi A's address */
    c->esi = eng;
    c->edi = SV_A(eng);
    if (call2(c, sp - 8, f_1013a840, 0x10134fe0u, eng, SV_A(eng)) & 0xff) {
        c->eax = rd32(c, SV_A(eng));
        c->ecx = k;
        ok = call4(c, sp - 8, f_10136b10, 0x10134ff7u, eng, k, rd32(c, SV_A(eng)), 1) & 0xff;
    }
    if (!ok) {
        uint32_t a1[1] = { eng };
        call_at(c, sp - 8, f_10130e80, 0x10135004u, 1, a1);     /* longjmps */
    }
    c->esi = esi;
    c->edi = edi;
}

/* FUN_10134dd0: for each of the n bytes: A := v, then FUN_1013be20 on the resolved A with the byte (kept
 * in the original's n argument slot, sp + 8, whose other bytes go along) */
void rl_edit_bytes(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, uint32_t bytes, uint32_t v)
{
    uint32_t cnt = n & 0xff;
    wr32(c, sp - 4, cnt);                   /* the count, in the slot `push ecx` made */
    if (!cnt) return;
    /* push ebx, ebp, esi; the loop's registers: edi i, esi eng, ebx A's address, ebp v */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    wr32(c, sp - 0xc, ebx);
    wr32(c, sp - 0x10, ebp);
    wr32(c, sp - 0x14, esi);
    c->esi = eng;
    c->ebx = SV_A(eng);
    c->ebp = v;
    for (uint32_t i = 0; i < cnt; i++) {
        c->edi = i;
        call2(c, sp - 0x14, f_10134690, 0x10134df8u, eng, v);
        wr8(c, sp + 8, rd8(c, bytes + i));
        if (!(call2(c, sp - 0x1c, f_1013a840, 0x10134e0au, eng, SV_A(eng)) & 0xff)
            || !(call3(c, sp - 0x14, f_1013be20, 0x10134e1fu, eng, rd32(c, SV_A(eng)), rd32(c, sp + 8)) & 0xff)) {
            uint32_t a1[1] = { eng };
            call_at(c, sp - 0x14, f_10130e80, 0x10134e2cu, 1, a1);   /* longjmps */
        }
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
}

/* FUN_10132a20 / FUN_10132a70: prepare A and B (FUN_1013ab30); 1 if that fails, else insert the tokens
 * (FUN_10132730 bytes / FUN_101328a0 shorts) and 0 */
uint32_t rl_insert_list(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t n, uint32_t p, uint32_t k,
                        guest_fn insert, uint32_t base)
{
    if (call5(c, sp - 8, f_1013ab30, base + 0x1e, eng, SV_A(eng), SV_B(eng), s, k)) return 1;
    call5(c, sp - 8, insert, base + 0x40, eng, s, p, n, 0);
    return 0;
}

/* FUN_10135010: prepare A and B; 1 if that fails, else delete the tokens between them (FUN_10136a20) */
uint32_t rl_delete_between(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t k)
{
    /* the original's registers: ebp s, esi eng, edi B's address, ebx A's address */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, r = 0;
    c->ebp = s;
    c->esi = eng;
    c->edi = SV_B(eng);
    c->ebx = SV_A(eng);
    c->eax = k;
    if (call5(c, sp - 0x10, f_1013ab30, 0x10135030u, eng, SV_A(eng), SV_B(eng), s, k)) {
        r = 1;
    } else {
        c->ecx = rd32(c, SV_B(eng));
        c->edx = rd32(c, SV_A(eng));
        call4(c, sp - 0x10, f_10136a20, 0x1013504eu, eng, s, rd32(c, SV_A(eng)), rd32(c, SV_B(eng)));
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    return r;
}

/* FUN_10135060: prepare A and B; set field f of stream s of the tokens between them to val (FUN_10136280,
 * twice for a plain number); 1 if the preparation fails */
uint32_t rl_set_field_val(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, uint32_t val, uint32_t k)
{
    /* the original's registers: esi eng, edi s, ebp B's address, then ebx f (pushed on that path) */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, r = 1;
    c->esi = eng;
    c->edi = s;
    c->ebp = SV_B(eng);
    c->eax = SV_A(eng);
    c->ecx = k;
    if (!call5(c, sp - 0x14, f_1013ab30, 0x10135082u, eng, SV_A(eng), SV_B(eng), s, k)) {
        uint32_t ref = sp - 8;
        wr32(c, sp - 0x18, ebx);
        c->eax = ref;
        c->edx = val;
        call3(c, sp - 0x18, f_10131520, 0x101350b6u, eng, ref, val);
        c->ebx = f;
        uint32_t fd = field_desc(c, s & 0xff, f & 0xff);
        if (rd16(c, ref + 4) == rd16(c, fd + FD_TYPE)) {
            for (int twice = 0; twice < 2; twice++) {
                c->eax = rd32(c, ref);
                c->ecx = rd32(c, SV_B(eng));
                c->edx = rd32(c, SV_A(eng));
                call6(c, sp - 0x18, f_10136280, twice ? 0x10135125u : 0x101350ffu, eng, s, f, rd32(c, SV_A(eng)),
                      rd32(c, SV_B(eng)), rd32(c, ref));
                int16_t t = (int16_t)rd16(c, ref + 4);
                if (!(t >= -6 && t < 0)) break;     /* a plain number is set twice */
            }
        }
        r = 0;
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    val_release(c, val);
    return r;
}

/* FUN_10135150 (fields of type -1) / FUN_101351d0 (-4): the same with a constant (the argument at
 * sp + 16, passed by address) */
uint32_t rl_set_field_const(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, int16_t type, uint32_t k,
                            uint32_t base)
{
    /* the original's registers: esi s, edi eng, ebx B's address */
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, r = 1;
    c->esi = s;
    c->edi = eng;
    c->ebx = SV_B(eng);
    c->eax = SV_A(eng);
    c->ecx = k;
    if (!call5(c, sp - 0xc, f_1013ab30, base + 0x1f, eng, SV_A(eng), SV_B(eng), s, k)) {
        wr32(c, sp - 0x10, c->ebp);         /* push ebp for the field lookup */
        uint32_t fd = field_desc(c, s & 0xff, f & 0xff);
        if ((int16_t)rd16(c, fd + FD_TYPE) == type) {
            c->eax = rd32(c, SV_A(eng));
            c->ecx = rd32(c, SV_B(eng));
            c->edx = f;
            call6(c, sp - 0xc, f_10136280, base + 0x72, eng, s, f, rd32(c, SV_A(eng)), rd32(c, SV_B(eng)), sp + 16);
        }
        r = 0;
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    return r;
}

/* FUN_10135250 / FUN_10135470: insert a token of stream s with the value val between A and B
 * (FUN_10136570), converting a number to the stream's type through the rule state's scratch values */
static int insert_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t r1, uint32_t r2)
{
    /* the machine's registers are the original's here (esi eng, edi s, ebx val, ebp as the caller set it);
     * r1 / r2: the return addresses of the two FUN_10136570 calls, the others at fixed distances */
    uint32_t tmp = sp - 0x10, vref = sp - 8;
    uint32_t a1[1] = { s };
    int16_t vt = val_type(c, val);
    if (vt < 0 && vt != (int16_t)call_at(c, sp - 0x20, f_10135b20, r1 - 0xac, 1, a1)) {
        int16_t t = (int16_t)call_at(c, sp - 0x20, f_10135b20, r1 - 0x9a, 1, a1);
        wr16(c, tmp + 4, (uint16_t)t);
        switch (t) {
        case T_SYM8: wr32(c, tmp, RS(eng) + RS_TMP_SYNC); break;
        case T_INT: wr32(c, tmp, RS(eng) + RS_TMP_INT); break;
        case T_SHORT: case T_SYM16: wr32(c, tmp, RS(eng) + RS_TMP_SHORT); break;
        case T_DOUBLE: wr32(c, tmp, RS(eng) + RS_TMP_DOUBLE); break;
        default: break;
        }
        uint32_t fields = rd32(c, stream_desc(s & 0xff) + SD_FIELDS);
        wr8(c, tmp + 6, rd8(c, fields + FD_FLAG));
        c->eax = (s & 0xff) * 19;
        c->ecx = vref;
        c->edx = fields;
        call3(c, sp - 0x20, f_10131520, r1 - 0x24, eng, vref, val);
        c->eax = tmp;
        c->edx = vref;
        call3(c, sp - 0x2c, f_10138730, r1 - 0x14, eng, tmp, vref);
        c->ecx = tmp;
        c->eax = rd32(c, SV_A(eng));
        c->edx = rd32(c, SV_B(eng));
        return (int)(call5(c, sp - 0x38, f_10136570, r1, eng, s, rd32(c, SV_A(eng)), rd32(c, SV_B(eng)), tmp)
                     & 0xff);
    }
    c->ecx = vref;
    call3(c, sp - 0x20, f_10131520, r2 - 0x14, eng, vref, val);
    c->edx = vref;
    c->eax = rd32(c, SV_B(eng));
    c->ecx = rd32(c, SV_A(eng));
    return (int)(call5(c, sp - 0x2c, f_10136570, r2, eng, s, rd32(c, SV_A(eng)), rd32(c, SV_B(eng)), vref) & 0xff);
}

/* the throw of FUN_10135250 / 10135470 through the machine (it longjmps) */
static void insert_throw(cpu *c, uint32_t sp, uint32_t eng, uint32_t ret)
{
    uint32_t a1[1] = { eng };
    call_at(c, sp - 0x20, f_10130e80, ret, 1, a1);
}

uint32_t rl_insert_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k)
{
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, r = 1;
    c->esi = eng;
    c->edi = s;
    c->ebp = SV_B(eng);
    c->eax = SV_A(eng);
    c->ecx = k;
    if (!call5(c, sp - 0x20, f_1013ab30, 0x10135273u, eng, SV_A(eng), SV_B(eng), s, k)) {
        c->ebx = val;
        if (!insert_value(c, sp, eng, s, val, 0x1013535bu, 0x10135380u)) insert_throw(c, sp, eng, 0x1013538du);
        r = 0;
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    val_release(c, val);
    return r;
}

void rl_insert_value_b(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k)
{
    uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
    c->esi = eng;
    c->edi = s;
    c->ebp = SV_A(eng);
    c->eax = SV_B(eng);
    c->ecx = k;
    if (!(call5(c, sp - 0x20, f_1013ae40, 0x10135493u, eng, SV_B(eng), SV_A(eng), s, k) & 0xff))
        insert_throw(c, sp, eng, 0x101354a0u);
    c->ebx = val;
    if (!insert_value(c, sp, eng, s, val, 0x10135563u, 0x1013558cu)) insert_throw(c, sp, eng, 0x10135599u);
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    val_release(c, val);
}

/* FUN_101353d0 (FUN_1013ae40 on B, A) / FUN_10135420 (FUN_1013af00 on A, B): prepare, then FUN_101430d0;
 * abort if either fails */
void rl_insert_text(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t n, uint32_t p, uint32_t k,
                    int b_first, uint32_t base)
{
    uint32_t ok = b_first ? call5(c, sp - 8, f_1013ae40, base + 0x1e, eng, SV_B(eng), SV_A(eng), s, k)
                          : call5(c, sp - 8, f_1013af00, base + 0x1e, eng, SV_A(eng), SV_B(eng), s, k);
    if ((ok & 0xff) && (call5(c, sp - 8, f_101430d0, base + 0x38, eng, s, p, n, 0) & 0xff)) return;
    rl_throw_at(c, AT(sp - 8, 1), eng);
}

/* FUN_10133480: prepare A and B; n := val as an int (in the original's first argument slot, sp + 4),
 * then FUN_1013a250(eng, &A, &B, s, n). 1 if either fails. */
uint32_t rl_edit_count(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k)
{
    if (call5(c, sp - 0x20, f_1013ab30, 0x101334a3u, eng, SV_A(eng), SV_B(eng), s, k)) {
        val_release(c, val);
        return 1;
    }
    uint32_t vref = sp - 8, nref = sp - 0x10;
    wr16(c, nref + 4, (uint16_t)T_INT);
    wr32(c, nref, sp + 4);
    wr8(c, nref + 6, 0);
    rl_ref_at(c, AT(sp - 0x20, 3), eng, vref, val);
    rl_assign(c, eng, nref, vref);
    uint32_t r = call5(c, sp - 0x38, f_1013a250, 0x1013350du, eng, SV_A(eng), SV_B(eng), s, rd32(c, sp + 4));
    val_release(c, val);
    return r ? 1 : 0;
}

/* FUN_10135660: var := FUN_1013a570(eng, &A, &B, s) (0 for "undefined"), both sync variables resolved by
 * FUN_1013a890 first (abort if not); the int goes through the original's first argument slot */
void rl_measure(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t var)
{
    if (!(call3(c, sp - 0x20, f_1013a890, 0x10135677u, eng, SV_A(eng), 0) & 0xff)
        || !(call3(c, sp - 0x20, f_1013a890, 0x1013568au, eng, SV_B(eng), 1) & 0xff))
        rl_throw_at(c, AT(sp - 0x20, 1), eng);
    uint32_t vref = sp - 8, nref = sp - 0x10;
    rl_ref_at(c, AT(sp - 0x20, 3), eng, vref, var);
    uint32_t n = call4(c, sp - 0x2c, f_1013a570, 0x101356bau, eng, SV_A(eng), SV_B(eng), s);
    wr32(c, sp + 4, n);
    if (n == 0x80000001u) wr32(c, sp + 4, 0);
    wr8(c, nref + 6, rd8(c, rd32(c, stream_desc(s & 0xff) + SD_FIELDS) + FD_FLAG));
    wr16(c, nref + 4, (uint16_t)T_INT);
    wr32(c, nref, sp + 4);
    rl_assign(c, eng, vref, nref);
    val_release(c, var);
}

/* FUN_10135820: FUN_10143380 on A..B with the pattern table entry n (eng[0x28] + 30 n); 1 if A or B is
 * null or no match (A and B resolved first, abort if they cannot be) */
uint32_t rl_match_table(cpu *c, uint32_t sp, uint32_t eng, uint32_t out, int16_t n)
{
    uint32_t a = SV_A(eng), b = SV_B(eng);
    if (!rd32(c, a) || !rd32(c, b)) return 1;
    if (!(call3(c, sp - 0xc, f_1013afd0, 0x10135843u, eng, a, b) & 0xff)) rl_throw_at(c, AT(sp - 0xc, 1), eng);
    uint32_t r = call5(c, sp - 0xc, f_10143380, 0x10135876u, eng, rd32(c, a), rd32(c, b),
                       rd32(c, eng + 0x28) + 30u * (uint32_t)(int32_t)n, out);
    return r ? 0 : 1;
}

/* FUN_10132bc0: FUN_1013b0a0(eng, &A, &B) != 0 */
uint32_t rl_ab_test(cpu *c, uint32_t sp, uint32_t eng)
{
    return call3(c, sp, f_1013b0a0, 0x10132bd2u, eng, SV_A(eng), SV_B(eng)) ? 1 : 0;
}

/* FUN_10133170: A and B resolved (abort if not), then FUN_10137d20(eng, a, A, B, b, d, e) */
void rl_ab_call(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    if (!(call3(c, sp - 0xc, f_1013afd0, 0x10133185u, eng, SV_A(eng), SV_B(eng)) & 0xff))
        rl_throw_at(c, AT(sp - 0xc, 1), eng);
    uint32_t args[7] = { eng, a, rd32(c, SV_A(eng)), rd32(c, SV_B(eng)), b, d, e };
    call_at(c, sp - 0xc, f_10137d20, 0x101331b5u, 7, args);
}

/* FUN_10133110 / FUN_10133140: FUN_10141730(eng, FUN_101416f0(eng, a), flag), abort if 0 */
void rl_lookup(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t flag, uint32_t base)
{
    uint32_t p = call2(c, sp - 8, f_101416f0, base + 0x12, eng, a);
    if (!(call3(c, sp - 4, f_10141730, base + 0x1c, eng, p, flag) & 0xff)) rl_throw_at(c, AT(sp - 4, 1), eng);
}

/* FUN_101330b0: FUN_10137620(eng, a, ref of val); 1 if nonzero */
uint32_t rl_val_call(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val)
{
    uint32_t ref = sp - 8;
    rl_ref_at(c, AT(sp - 0x10, 3), eng, ref, val);
    uint32_t r = call3(c, sp - 0x1c, f_10137620, 0x101330d9u, eng, a, ref);
    val_release(c, val);
    return r ? 1 : 0;
}

/* FUN_10133210: trail val if trailing; its token gets the content of the value at sp + 12 (FUN_101364c0
 * with the stream in the low byte of what eax held) */
void rl_set_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t val)
{
    uint32_t eax = RS(eng);
    uint32_t esi = c->esi, edi = c->edi;    /* the original: esi eng, edi val across its calls */
    c->esi = eng;
    c->edi = val;
    if (rd8(c, eax + RS_TRAIL)) eax = call2(c, sp - 8, f_101314f0, 0x1013322eu, eng, val);
    eax = (eax & 0xffffff00u) | rd8(c, val);
    c->eax = eax;                           /* the registers the callee's prologue may save */
    c->ecx = sp + 12;
    c->edx = val + 4;
    call4(c, sp - 8, f_101364c0, 0x10133243u, eng, eax, val + 4, sp + 12);
    c->esi = esi;
    c->edi = edi;
}

/* FUN_101331c0: FUN_10141b30(eng, a, b, 1) */
uint32_t rl_emit(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b)
{
    return call4(c, sp, f_10141b30, 0x101331d6u, eng, a, b, 1);
}

/* FUN_101331e0: FUN_101319a0(eng, a, val) and val's field reference is used up */
void rl_emit_val(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val)
{
    call3(c, sp - 4, f_101319a0, 0x101331f5u, eng, a, val);
    val_release(c, val);
}

/* FUN_10135a40 / FUN_10135a60: set / clear the mark bit (1) of a sync mark's backward link in stream s */
void rl_mark_set(cpu *c, uint32_t eng, uint32_t m, uint32_t s, int on)
{
    uint32_t a = m + 4 * (rd32(c, RS(eng) + RS_BACK) + (uint32_t)(int32_t)(int8_t)s);
    wr32(c, a, on ? rd32(c, a) | 2u : rd32(c, a) & ~2u);
}

/* ------------------------------------------------------------------------ more rule operations */

void f_1013a590(cpu *c);
void f_1013a820(cpu *c);
void f_101390c0(cpu *c);
void f_10139d80(cpu *c);
void f_10138fb0(cpu *c);
void f_101435d0(cpu *c);
void f_10143720(cpu *c);
void f_10143800(cpu *c);
void f_10135c70(cpu *c);

/* FUN_10134cc0: is the sync value v reachable from the cursor over sync marks only (forward if FUN_1013a590
 * says v is after it, backward if FUN_1013a820 says before)? Then v := the cursor's mark when the
 * direction matches the cursor's, and 0; 1 if not (or if v is neither, unless it is the cursor's mark). */
uint32_t rl_reach(cpu *c, uint32_t sp, uint32_t eng, uint32_t v)
{
    uint32_t m = rd32(c, v + 2);
    if (call3(c, sp - 0xc, f_1013a590, 0x10134cdfu, eng, rd32(c, RS(eng) + RS_POS_MARK), m) & 0xff) {
        uint32_t rs = RS(eng), cur = rd32(c, rs + RS_POS_MARK);
        while (m != cur) {
            if (!m || !is_mark(c, m)) return 1;
            m = fwd_link(c, m, rd8(c, rs + RS_POS_STREAM)) & ~3u;
        }
        if (!rd8(c, rs + RS_POS_BACK)) wr32(c, v + 2, cur);
        return 0;
    }
    if (call3(c, sp - 0xc, f_1013a820, 0x10134d3bu, eng, rd32(c, RS(eng) + RS_POS_MARK), m) & 0xff) {
        uint32_t rs = RS(eng), cur = rd32(c, rs + RS_POS_MARK);
        while (m != cur) {
            if (!m || !is_mark(c, m)) return 1;
            m = back_link(c, rs, m, rd8(c, rs + RS_POS_STREAM)) & ~3u;
        }
        if (rd8(c, rs + RS_POS_BACK) == 1) wr32(c, v + 2, cur);
        return 0;
    }
    return rd32(c, RS(eng) + RS_POS_MARK) == m ? 0 : 1;
}

/* FUN_10134f00: reset streams: n = 0 all of them (FUN_10135c70 each, the counter kept in the original's
 * n argument slot, sp + 8), n = the number of streams: the whole delta (FUN_10139d80, FUN_10138fb0), else
 * the n streams listed; abort if one fails; then FUN_101390c0 */
void rl_reset_streams(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, uint32_t list)
{
    uint8_t cnt = (uint8_t)n;
    if (cnt == 0) {
        wr8(c, sp + 8, 0);
        uint8_t k = 0;
        if (rd8(c, eng + ENG_NSTREAMS)) {
            do {
                if (!(call2(c, sp - 8, f_10135c70, 0x10134f2du, eng, rd32(c, sp + 8)) & 0xff))
                    rl_throw_at(c, AT(sp - 8, 1), eng);
                k++;
                wr8(c, sp + 8, k);
            } while (k < rd8(c, eng + ENG_NSTREAMS));
        }
        call_at(c, sp - 8, f_101390c0, 0x10134f53u, 1, &eng);
        return;
    }
    if (cnt == rd8(c, eng + ENG_NSTREAMS)) {
        call3(c, sp - 8, f_10139d80, 0x10134faeu, eng, rd32(c, WS(eng) + WS_START), 0);
        call2(c, sp - 0x14, f_10138fb0, 0x10134fb6u, eng, 1);
        call_at(c, sp - 8, f_101390c0, 0x10134fbfu, 1, &eng);
        return;
    }
    for (uint32_t i = 0; i < cnt; i++) {
        if (!(call2(c, sp - 0xc, f_10135c70, 0x10134f7eu, eng, rd8(c, list + i)) & 0xff))
            rl_throw_at(c, AT(sp - 0xc, 1), eng);
    }
    call_at(c, sp - 8, f_101390c0, 0x10134f99u, 1, &eng);
}

/* FUN_10135730: match A..B against the pattern table entry n (eng[0x2c] + 34 n) with FUN_101435d0; 1 if
 * no match. On a match: RS_MATCH_COUNT := its count, and for the two results (bytes: tokens to go from A,
 * 0xff none) out1 / out2 := the mark reached. The original keeps the result byte in its first argument
 * slot's low byte (sp + 4) and the result index in its n slot (sp + 8). */
uint32_t rl_match_pattern(cpu *c, uint32_t sp, uint32_t eng, int16_t n, uint32_t out1, uint32_t out2)
{
    uint32_t a = eng + ENG_SYNC_A, b = eng + ENG_SYNC_B;
    if (!(call3(c, sp - 0x14, f_1013afd0, 0x10135747u, eng, a, b) & 0xff)) rl_throw_at(c, AT(sp - 0x14, 1), eng);
    uint32_t entry = rd32(c, eng + 0x2c) + 34u * (uint32_t)(int32_t)n;
    uint32_t p = call4(c, sp - 0x14, f_101435d0, 0x10135776u, eng, rd32(c, a), rd32(c, b), entry);
    if (!p) return 1;
    wr16(c, RS(eng) + RS_MATCH_COUNT, rd16(c, p + 2));
    uint32_t k = 0;
    wr32(c, sp + 8, k);
    do {
        uint8_t cnt = rd8(c, p);
        p++;
        wr8(c, sp + 4, cnt);
        wr32(c, sp - 4, p);
        if (cnt != 0xff) {
            uint32_t e = rd32(c, eng + ENG_SYNC_A);
            uint32_t todo = rd32(c, sp + 4) & 0xff;
            for (uint32_t i = 0; i < todo;) {
                if (e && is_mark(c, e)) {
                    e = rd32(c, e + 4 * (rd8(c, entry + 8) + rd32(c, RS(eng) + RS_BACK))) & ~3u;
                } else {
                    e = rd32(c, e + 4) & ~3u;
                    i++;
                }
            }
            uint32_t out = k == 0 ? out1 : out2;
            if (out) wr32(c, out + 2, e);
        }
        k++;
        wr32(c, sp + 8, k);
    } while (k <= 1);
    return 0;
}

/* FUN_101358b0: out := a mod b (both converted to int by FUN_10143720; a in place in the original's a
 * argument slot, b in its local; the result through FUN_10143800). Returns 0. */
uint32_t rl_mod(cpu *c, uint32_t sp, uint32_t eng, uint32_t va, uint32_t vb, uint32_t out)
{
    (void)eng;
    call3(c, sp - 8, f_10143720, 0x101358c4u, va, sp + 8, (uint32_t)-3);
    call3(c, sp - 0x14, f_10143720, 0x101358d5u, vb, sp - 8, (uint32_t)-3);
    int32_t a = (int32_t)rd32(c, sp + 8), bb = (int32_t)rd32(c, sp - 8);
    if (bb == 0 || (a == INT32_MIN && bb == -1)) x86_fail(c, 0x101358deu, "idiv fault");
    wr32(c, sp - 4, (uint32_t)(a % bb));
    call3(c, sp - 0x20, f_10143800, 0x101358f3u, sp - 4, (uint32_t)-3, out);
    return 0;
}

/* ------------------------------------------------------------------------ inserting lists of tokens */

void f_10136720(cpu *c);
void f_10136570(cpu *c);
void f_10136a20(cpu *c);
void f_10137e70(cpu *c);
void f_10140de0(cpu *c);
void f_101359c0(cpu *c);

#define IAT_SPRINTF 0x10144080u

/* the scratch value of a list item as the stream's field 0 type: where the original's FUN_10132730 /
 * FUN_101328a0 keep the converted item (a local or one of their argument slots); 0 for other types */
static uint32_t item_slot(int16_t t, uint32_t sp, int shorts)
{
    switch (t) {
    case T_DOUBLE: return sp - 8;
    case T_SHORT: return shorts ? sp - 0x20 : sp + 16;
    case T_INT: return sp - 0x1c;
    case T_SYM16: return shorts ? sp - 0x20 : sp + 16;
    case T_SYM8: return shorts ? sp + 16 : sp + 8;
    default: return 0;
    }
}

/* FUN_10132730 (bytes) / FUN_101328a0 (16-bit values, big-endian sign-magnitude): insert a token of
 * stream s for each of the n items at p, from sync variable B on, A moving along (FUN_10136720); with
 * n = 0 delete the tokens between A and B instead. Each item is converted to the stream's field 0 type
 * (FUN_10138730) unless it has it. al 1, or 0 when an insertion fails. The original keeps the current
 * byte in its s slot (sp + 8) and p (bytes) or the end (shorts) in its p slot (sp + 12). */
uint32_t rl_insert_items(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t p, uint32_t n, uint32_t k5,
                         int shorts)
{
    uint32_t a = eng + ENG_SYNC_A, b = eng + ENG_SYNC_B;
    if (!(n & 0xff)) {
        uint32_t args[4] = { eng, s, rd32(c, a), rd32(c, b) };
        return (call_at(c, sp - 0x30, f_10136a20, shorts ? 0x101328c6u : 0x10132756u, 4, args) & 0xffffff00u) | 1;
    }
    uint32_t si = s & 0xff;
    uint32_t tref = sp - 0x18, cref = sp - 0x10;
    uint32_t fd = rd32(c, stream_desc(si) + SD_FIELDS);
    int16_t t = (int16_t)rd16(c, fd + FD_TYPE);
    wr16(c, tref + 4, (uint16_t)t);
    wr8(c, tref + 6, rd8(c, fd + FD_FLAG));
    uint32_t slot = item_slot(t, sp, shorts);
    if (!slot) return (uint32_t)(t + 5) & 0xffffff00u;
    wr32(c, tref, slot);
    uint32_t end = p + (n & 0xff);
    if (shorts) {
        wr16(c, cref + 4, (uint16_t)T_SHORT);
        wr32(c, sp + 12, end);
        wr32(c, cref, sp - 0x20);
    } else {
        wr16(c, cref + 4, 0xffff);
        wr32(c, sp - 0x20, end);
        wr32(c, cref, sp + 8);
    }
    wr8(c, cref + 6, rd8(c, tref + 6));
    if (p >= end) return (p & 0xffffff00u) | 1;
    for (;;) {
        if (shorts) {
            uint8_t b0 = rd8(c, p), b1 = rd8(c, p + 1);
            uint32_t v = ((uint32_t)(b0 & 0x7f) << 8) | b1;
            if (b0 & 0x80) v = 0u - v;
            wr32(c, sp - 0x20, v);
            p += 2;
        } else {
            wr8(c, sp + 8, rd8(c, p));
            p++;
            wr32(c, sp + 12, p);
        }
        if (rd16(c, tref + 4) != rd16(c, cref + 4)) rl_assign(c, eng, tref, cref);
        uint32_t args[5] = { eng, shorts ? rd32(c, sp + 8) : s, rd32(c, a), rd32(c, b), tref };
        if (!(call_at(c, sp - 0x30, f_10136570, shorts ? 0x101329cau : 0x10132844u, 5, args) & 0xff))
            return c->eax & 0xffffff00u;
        if (p >= rd32(c, shorts ? sp + 12 : sp - 0x20)) return (p & 0xffffff00u) | 1;
        uint32_t bm = rd32(c, b);
        uint32_t args2[5] = { eng, shorts ? rd32(c, sp + 8) : s, rd32(c, bm + 4 * si + 0xc) & ~3u, bm, k5 };
        uint32_t m = call_at(c, sp - 0x30, f_10136720, shorts ? 0x101329f3u : 0x1013286eu, 5, args2);
        wr32(c, a, m);
        if (!m) return 0;
    }
}

/* FUN_101319a0: write the value val as text to output a (FUN_10141b30): numbers with sprintf, sync
 * marks as NULL / dangling / their number, token fields through FUN_10137e70 (and FUN_10140de0 when the
 * text starts with a backslash). The text is built in the original's 80-byte local (sp - 0x50). */
void rl_print_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val)
{
    uint32_t buf = sp - 0x50;
    int16_t t = val_type(c, val);
    switch (t) {
    case T_INT: {
        uint32_t args[3] = { buf, 0x1019201cu, rd32(c, val + 2) };               /* "%ld" */
        import_call(c, sp - 0x54, IAT_SPRINTF, 0x101319d2u, 3, args);
        call4(c, sp - 0x60, f_10141b30, 0x101319e8u, eng, a, buf, 1);
        return;
    }
    case T_SHORT: {
        uint32_t args[3] = { buf, 0x10155104u, (uint32_t)(int32_t)(int16_t)rd16(c, val + 2) };   /* "%d" */
        import_call(c, sp - 0x54, IAT_SPRINTF, 0x10131a05u, 3, args);
        call4(c, sp - 0x60, f_10141b30, 0x10131a1bu, eng, a, buf, 1);
        return;
    }
    case T_DOUBLE: {
        uint32_t args[4] = { buf, 0x10192018u, rd32(c, val + 2), rd32(c, val + 6) };           /* "%f" */
        import_call(c, sp - 0x54, IAT_SPRINTF, 0x10131a3bu, 4, args);
        call4(c, sp - 0x64, f_10141b30, 0x10131a51u, eng, a, buf, 1);
        return;
    }
    case T_SYNC: {
        uint32_t m = rd32(c, val + 2);
        if (m == 0) {
            call4(c, sp - 0x54, f_10141b30, 0x10131a76u, eng, a, 0x10192010u, 1);              /* "NULL" */
            return;
        }
        if (m == 1) {
            call4(c, sp - 0x54, f_10141b30, 0x10131a98u, eng, a, 0x10192004u, 1);              /* "dangling" */
            return;
        }
        uint32_t num = call2(c, sp - 0x54, f_101359c0, 0x10131aabu, eng, m);
        uint32_t args[3] = { buf, 0x10155104u, num };
        import_call(c, sp - 0x5c, IAT_SPRINTF, 0x10131abcu, 3, args);
        call4(c, sp - 0x68, f_10141b30, 0x10131aceu, eng, a, buf, 1);
        return;
    }
    default: {
        uint16_t f = rd16(c, val + 2);
        if (f == 0xff) return;
        uint32_t args[5] = { eng, val + 4, (uint32_t)(int32_t)t, (uint32_t)(int32_t)(int16_t)f, buf };
        call_at(c, sp - 0x54, f_10137e70, 0x10131af9u, 5, args);
        if (rd8(c, buf) == 0x5c) call3(c, sp - 0x54, f_10140de0, 0x10131b12u, buf, 0, 0);
        call4(c, sp - 0x54, f_10141b30, 0x10131b27u, eng, a, buf, 1);
        return;
    }
    }
}
