/* rules_guest - the hand-written rule runtime (rules.c) in the place of the recompiled functions.
 *
 * Each adapter takes the cdecl arguments from the guest stack (ARG(n)), calls the C function and returns
 * eax as the original leaves it (see src/ported.h for which ones the callers read at all). `sp` is the
 * esp the function was entered with, for the functions that use their own stack frame as the original
 * does (rules.c).
 */
#include "rules.h"
#include "port.h"
#include "rules_int.h"      /* call_at, call2, call3: calls through the machine */

/* rules.c: the functions that work at a given entry esp */
uint32_t rl_leave_at(cpu *c, uint32_t sp, uint32_t eng);
void     rl_throw_at(cpu *c, uint32_t sp, uint32_t eng);
void     rl_ref_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t ref, uint32_t val);
int      rl_var_init_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var, uint32_t src, int16_t type);
uint32_t rl_var_init_sync(cpu *c, uint32_t eng, uint32_t var);
uint32_t rl_trail_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val);
void     rl_push_val_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val);
void     rl_push_const_at(cpu *c, uint32_t sp, uint32_t eng, int16_t type, uint32_t ret);
void     rl_pop_into_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var);
void     rl_set_short_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var);
uint32_t rl_touch_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val);
void     rl_assign_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t dst, uint32_t src);
void     rl_compare_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b, uint32_t ret);
uint32_t rl_cmp_val_short_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val, uint32_t k, int op, uint32_t base);
uint32_t rl_cmp_stack_at(cpu *c, uint32_t sp, uint32_t eng, int op, uint32_t base);
uint32_t rl_field_ne_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, uint8_t v);
uint32_t rl_push_field_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f);
uint32_t rl_match_string_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t len, uint32_t str);
uint32_t rl_match_shorts_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t len, uint32_t p);
uint32_t rl_goto_a_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, int back, int fresh, uint32_t ret);
uint32_t rl_goto_val_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t v, uint32_t s, int back, uint32_t ret);
void     rl_set_ab(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
void     rl_set_a(cpu *c, uint32_t eng, uint32_t v);
void     rl_set_b(cpu *c, uint32_t eng, uint32_t v);
uint32_t rl_advance_to_a_at(cpu *c, uint32_t sp, uint32_t eng);
uint32_t rl_align(cpu *c, uint32_t sp, uint32_t eng, uint32_t label, uint32_t n, uint32_t streams);
uint32_t rl_align1(cpu *c, uint32_t sp, uint32_t eng, uint32_t label, uint32_t s);
void     rl_mark_here_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t label, uint32_t val);
void     rl_succeed(cpu *c, uint32_t eng, uint32_t label);
void     rl_commit(cpu *c, uint32_t eng, uint32_t label);
void     rl_set_labels(cpu *c, uint32_t eng, uint32_t next, uint32_t fail);

uint32_t rl_trail_ref(cpu *c, uint32_t eng, uint32_t ref);
void     rl_assign(cpu *c, uint32_t eng, uint32_t dst, uint32_t src);
void     rl_compare(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
void     rl_push(cpu *c, uint32_t eng, uint32_t ref);
uint32_t rl_pop(cpu *c, uint32_t eng, uint32_t ref);
uint32_t rl_mark_key(uint32_t m);
int      rl_is_negative(cpu *c, uint32_t ref);
void     rl_add(cpu *c, uint32_t eng, uint32_t dst, uint32_t src, uint32_t argslot);

void f_1013a040(cpu *c);

#define RET(v) port_ret(c, (v), 0)

/* ---------------------------------------------------------------- rule frames, control stack */
PORT_FN(101315e0) { RET((uint32_t)rl_enter(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); }
PORT_FN(10131790) { RET(rl_leave_at(c, c->esp, ARG(0))); }
PORT_FN(10130e80) { rl_throw_at(c, c->esp, ARG(0)); }
PORT_FN(101311a0) { RET((uint32_t)rl_backtrack_sp(c, c->esp, ARG(0), (int32_t)ARG(1))); }
PORT_FN(10131f90) { rl_cut(c, ARG(0)); RET(c->eax); }
PORT_FN(10131ff0) { rl_push_retry(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10132040) { uint32_t eng = ARG(0); rl_push_retry(c, eng, ARG(1)); rl_push_pos(c, eng); RET(c->eax); }
PORT_FN(101320e0) { rl_push_mark(c, ARG(0), CS_DOWN); RET(c->eax); }
PORT_FN(10132120) { rl_push_mark(c, ARG(0), CS_UP); RET(c->eax); }
/* push esi; push eng; call 101320e0; push n; push eng; call 10131ff0 / 10132040 - through the machine, the
 * second call's arguments below the first one's (the original removes them together) */
void f_101320e0(cpu *c);
void f_10131ff0(cpu *c);
void f_10132040(cpu *c);
static void push_down_then(cpu *c, guest_fn then, uint32_t ret1, uint32_t ret2)
{
    uint32_t eng = ARG(0), n = ARG(1), sp = c->esp - 4;
    uint32_t a1[1] = { eng };
    call_at(c, sp, f_101320e0, ret1, 1, a1);
    call2(c, sp - 4, then, ret2, eng, n);
}
PORT_FN(101326c0) { push_down_then(c, f_10131ff0, 0x101326cbu, 0x101326d6u); RET(c->eax); }
PORT_FN(101326e0) { push_down_then(c, f_10132040, 0x101326ebu, 0x101326f6u); RET(c->eax); }
PORT_FN(10132ac0) { rl_succeed(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101338d0) { rl_commit(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101345e0) { rl_set_labels(c, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10133300) { rl_mark_here_at(c, c->esp, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10132f80) { RET(rl_align(c, c->esp, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10134b60) { RET(rl_align1(c, c->esp, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10132160)
{
    uint32_t a = ARG(0);
    RET(guest_call(c, f_1013a040, 0x1013216au, 1, &a));
}

/* ---------------------------------------------------------------- variables and values */
PORT_FN(10130ea0) { RET((uint32_t)rl_var_init_at(c, c->esp, ARG(0), ARG(1), ARG(2), (int16_t)ARG(3))); }
PORT_FN(101310b0) { RET(rl_var_init_sync(c, ARG(0), ARG(1))); }
PORT_FN(10131520) { rl_ref_at(c, c->esp, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(101314f0) { RET(rl_trail_at(c, c->esp, ARG(0), ARG(1))); }
PORT_FN(10131cd0) { rl_push_val_at(c, c->esp, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10131d10) { rl_push_const_at(c, c->esp, ARG(0), T_SYM8, 0x10131d36u); RET(c->eax); }
PORT_FN(10131d40) { rl_push_const_at(c, c->esp, ARG(0), T_INT, 0x10131d66u); RET(c->eax); }
PORT_FN(10131d70) { rl_push_const_at(c, c->esp, ARG(0), T_SHORT, 0x10131d96u); RET(c->eax); }
PORT_FN(10131e90) { rl_pop_into_at(c, c->esp, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101325e0) { rl_set_short_at(c, c->esp, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101332d0) { RET(rl_touch_at(c, c->esp, ARG(0), ARG(1))); }
PORT_FN(10133250) { rl_assign_at(c, c->esp, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10132be0) { rl_compare_at(c, c->esp, ARG(0), ARG(1), ARG(2), 0x10132c1au); RET(c->eax); }
PORT_FN(10132b80) { RET(rd8(c, rd32(c, ARG(0) + ENG_RS) + RS_CMP) != 0); }
PORT_FN(10132ba0) { RET(rd8(c, rd32(c, ARG(0) + ENG_RS) + RS_CMP) == 0); }
/* push esi; call FUN_10132be0(eng, a, b); call FUN_10132b80(eng) - through the machine, with the original's
 * registers (esi eng, ecx a, eax b) */
void f_10132be0(cpu *c);
void f_10132b80(cpu *c);
PORT_FN(10132350)
{
    uint32_t eng = ARG(0), a = ARG(1), b = ARG(2), esi = c->esi, sp = c->esp - 4;
    c->esi = eng;
    c->ecx = a;
    c->eax = b;
    call3(c, sp, f_10132be0, 0x10132365u, eng, a, b);
    uint32_t a1[1] = { eng };
    uint32_t r = call_at(c, sp - 0xc, f_10132b80, 0x1013236bu, 1, a1);
    c->esi = esi;
    RET(r);
}
PORT_FN(10134280)
{
    uint32_t eng = ARG(0);
    rl_compare_at(c, c->esp, eng, ARG(1), ARG(2), 0x101342bau);
    uint32_t rs = rd32(c, eng + ENG_RS);
    if (rd8(c, rs + RS_CMP)) { RET(0); return; }
    wr32(c, eng + ENG_RESULT, rd32(c, rs + RS_NEXT));
    RET(2);
}

/* comparisons of a variable with a short constant, and of the two top stack values */
#define CMP_VS(a, op) PORT_FN(a) { RET(rl_cmp_val_short_at(c, c->esp, ARG(0), ARG(1), ARG(2), op, 0x##a##u)); }
CMP_VS(10132170, 0)
CMP_VS(101321d0, 1)
CMP_VS(10132230, 2)
CMP_VS(10132290, 3)
CMP_VS(101322f0, 4)
#define CMP_ST(a, op) PORT_FN(a) { RET(rl_cmp_stack_at(c, c->esp, ARG(0), op, 0x##a##u)); }
CMP_ST(10132370, 0)
CMP_ST(101323c0, 4)
CMP_ST(10132410, 2)
CMP_ST(10132460, 1)
CMP_ST(101324b0, 3)
CMP_ST(10132500, 5)

/* ---------------------------------------------------------------- the cursor */

/* The cursor movements keep the cursor's stream in their first argument's slot (the engine pointer is
 * in a register by then): the original overwrites it on entry. Returns the engine pointer. */
static uint32_t stream_in_arg0(cpu *c)
{
    uint32_t eng = ARG(0);
    wr32(c, c->esp + 4, rd8(c, rd32(c, eng + ENG_RS) + RS_POS_STREAM));
    return eng;
}

PORT_FN(10135d00)
{
    uint32_t across = ARG(1), check = ARG(2), eng = stream_in_arg0(c);
    RET((c->eax & 0xffffff00u) | (uint32_t)rl_step(c, eng, (int)across, (int)check));
}
PORT_FN(10135e40)
{
    uint32_t stop = ARG(1), check = ARG(2), eng = stream_in_arg0(c);
    rl_step_to(c, eng, stop, (int)check);
    RET(c->eax);
}
PORT_FN(10135f70)
{
    uint32_t check = ARG(1), eng = stream_in_arg0(c);
    rl_skip_marks(c, eng, (int)check);
    RET(c->eax);
}
PORT_FN(101360a0)
{
    uint32_t check = ARG(1), eng = stream_in_arg0(c);
    RET((c->eax & 0xffffff00u) | (uint32_t)rl_next_token(c, eng, (int)check));
}
/* these two call the stepping functions through the machine, as the original does (its arguments, return
 * address and the callee's frame are then the original's on the stack) */
void f_101360a0(cpu *c);
void f_10135d00(cpu *c);
PORT_FN(10134ca0) { RET(!(call2(c, c->esp, f_101360a0, 0x10134cacu, ARG(0), 1) & 0xff)); }
PORT_FN(10134c80) { RET(!(call3(c, c->esp, f_10135d00, 0x10134c8eu, ARG(0), 0, 1) & 0xff)); }
PORT_FN(10132550) { RET(rl_field_ne_at(c, c->esp, ARG(0), ARG(1), ARG(2), (uint8_t)ARG(3))); }
PORT_FN(10131da0) { RET(rl_push_field_at(c, c->esp, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10132c40) { RET(rl_match_string_at(c, c->esp, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10132de0) { RET(rl_match_shorts_at(c, c->esp, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10133550) { RET(rl_goto_a_at(c, c->esp, ARG(0), ARG(1), 0, 1, 0x10133567u)); }
PORT_FN(101335e0) { RET(rl_goto_a_at(c, c->esp - 4, ARG(0), ARG(1), 1, 1, 0x101335fdu)); }
PORT_FN(101337b0) { RET(rl_goto_a_at(c, c->esp, ARG(0), ARG(1), 0, 0, 0x101337c7u)); }
PORT_FN(10133840) { RET(rl_goto_a_at(c, c->esp, ARG(0), ARG(1), 1, 0, 0x10133857u)); }
PORT_FN(10133670) { RET(rl_goto_val_at(c, c->esp, ARG(0), ARG(1), ARG(2), 0, 0x101336a0u)); }
PORT_FN(10133710) { RET(rl_goto_val_at(c, c->esp, ARG(0), ARG(1), ARG(2), 1, 0x10133740u)); }
PORT_FN(10132700) { rl_set_ab(c, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10134690) { rl_set_a(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101346b0) { rl_set_b(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10132f20) { RET(rl_advance_to_a_at(c, c->esp, ARG(0))); }

/* ---------------------------------------------------------------- the value core */
PORT_FN(10138510) { RET(rl_trail_ref(c, ARG(0), ARG(1))); }
void rl_assign_sp(cpu *c, uint32_t sp, uint32_t eng, uint32_t dst, uint32_t src);
PORT_FN(10138730) { rl_assign_sp(c, c->esp, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
void rl_compare_sp(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b);
PORT_FN(10138920) { rl_compare_sp(c, c->esp, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10138b80) { rl_push(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10138c60) { RET(rl_pop(c, ARG(0), ARG(1))); }
PORT_FN(101385f0) { rl_add(c, ARG(0), ARG(1), ARG(2), c->esp + 12); RET(c->eax); }
PORT_FN(101386e0) { RET((uint32_t)rl_is_negative(c, ARG(1))); }
PORT_FN(101359b0) { RET(rl_mark_key(ARG(0))); }

/* ---------------------------------------------------------------- rules_ops.c */
uint32_t rl_start_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t s, uint32_t v);
uint32_t rl_start_back_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                            uint32_t s, uint32_t v);
uint32_t rl_start_back_token2(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                              uint32_t s, uint32_t v);
uint32_t rl_start_back_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label,
                             uint32_t s, uint32_t v);
uint32_t rl_start_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label, uint32_t s,
                       uint32_t v, uint32_t stop, int back);
uint32_t rl_start_span(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t label, uint32_t s,
                       uint32_t v, uint32_t stop, int back);
void     rl_assign_then(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t dst, uint32_t src);
uint32_t rl_for_step(cpu *c, uint32_t sp, uint32_t eng, uint32_t next, uint32_t fail, uint32_t a, uint32_t limit,
                     uint32_t step);
uint32_t rl_for_done(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t limit, uint32_t step);
void     rl_cmp_popped_byte(cpu *c, uint32_t sp, uint32_t eng);
uint32_t rl_take_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t var);
void     rl_set_a_offset(cpu *c, uint32_t sp, uint32_t eng, uint8_t s, uint32_t n);
void     rl_sv_bound(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t s, int where, uint32_t base);
void     rl_sv_move(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t n, int with_eng, uint32_t base);
uint32_t rl_a_bound(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, int kind, int where, uint32_t base);
uint32_t rl_a_move(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, int with_eng, uint32_t r1, uint32_t r2);
uint32_t rl_get_sv(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t val, uint32_t base);
void     rl_edit_a(cpu *c, uint32_t sp, uint32_t eng, uint32_t k, uint32_t base);
void     rl_edit_ba(cpu *c, uint32_t sp, uint32_t eng, uint32_t k, uint32_t sv, guest_fn edit, uint32_t base);
void     rl_edit_a_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t k);
void     rl_edit_bytes(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, uint32_t bytes, uint32_t v);
uint32_t rl_insert_list(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t n, uint32_t p, uint32_t k,
                        guest_fn insert, uint32_t base);
uint32_t rl_delete_between(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t k);
uint32_t rl_set_field_val(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, uint32_t val, uint32_t k);
uint32_t rl_set_field_const(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, int16_t type, uint32_t k,
                            uint32_t base);
uint32_t rl_insert_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k);
void     rl_insert_value_b(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k);
void     rl_insert_text(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t n, uint32_t p, uint32_t k,
                        int b_first, uint32_t base);
uint32_t rl_edit_count(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t val, uint32_t k);
void     rl_measure(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t var);
uint32_t rl_match_table(cpu *c, uint32_t sp, uint32_t eng, uint32_t out, int16_t n);
uint32_t rl_ab_test(cpu *c, uint32_t sp, uint32_t eng);
void     rl_ab_call(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b, uint32_t d, uint32_t e);
void     rl_lookup(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t flag, uint32_t base);
uint32_t rl_val_call(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val);
void     rl_set_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t val);
uint32_t rl_emit(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b);
void     rl_emit_val(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val);
void     rl_mark_set(cpu *c, uint32_t eng, uint32_t m, uint32_t s, int on);

void f_10132730(cpu *c);
void f_101328a0(cpu *c);
void f_1013ba30(cpu *c);
void f_1013b8c0(cpu *c);

#define SP c->esp

/* match starts: (eng, next, fail, label, stream, v[, stop]) */
PORT_FN(101310f0) { RET(rl_start_at(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))); }
PORT_FN(10133960) { RET(rl_start_back_step(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); }
PORT_FN(10133a50) { RET(rl_start_back_token2(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); }
PORT_FN(10133bb0) { RET(rl_start_back_token(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); }
PORT_FN(10133ce0) { RET(rl_start_step(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0)); }
PORT_FN(10133fc0) { RET(rl_start_step(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 1)); }
PORT_FN(10133e10) { RET(rl_start_span(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0)); }
PORT_FN(101340f0) { RET(rl_start_span(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 1)); }

/* values */
PORT_FN(10134300) { rl_assign_then(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); RET(c->eax); }
PORT_FN(10134400) { RET(rl_for_step(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))); }
PORT_FN(10134500) { RET(rl_for_done(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10131f00) { rl_cmp_popped_byte(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(101333c0) { RET(rl_take_token(c, SP, ARG(0), ARG(1))); }

/* sync variables */
PORT_FN(101346d0) { rl_set_a_offset(c, SP, ARG(0), (uint8_t)ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10134720) { rl_sv_bound(c, SP, ARG(0), ARG(0) + ENG_SYNC_A, ARG(1), 1, 0x10134720u); RET(c->eax); }
PORT_FN(10134780) { rl_sv_bound(c, SP, ARG(0), ARG(0) + ENG_SYNC_B, ARG(1), 1, 0x10134780u); RET(c->eax); }
PORT_FN(101347e0) { rl_sv_bound(c, SP, ARG(0), ARG(0) + ENG_SYNC_A, ARG(1), 0, 0x101347e0u); RET(c->eax); }
PORT_FN(10134840) { rl_sv_bound(c, SP, ARG(0), ARG(0) + ENG_SYNC_B, ARG(1), 0, 0x10134840u); RET(c->eax); }
PORT_FN(101348a0) { rl_sv_move(c, SP, ARG(0), ARG(0) + ENG_SYNC_A, ARG(1), 0, 0x101348a0u); RET(c->eax); }
PORT_FN(101348e0) { rl_sv_move(c, SP, ARG(0), ARG(0) + ENG_SYNC_B, ARG(1), 0, 0x101348e0u); RET(c->eax); }
PORT_FN(10134920) { rl_sv_move(c, SP, ARG(0), ARG(0) + ENG_SYNC_A, ARG(1), 1, 0x10134920u); RET(c->eax); }
PORT_FN(10134960) { RET(rl_a_bound(c, SP, ARG(0), ARG(1), 0, 1, 0x10134960u)); }
PORT_FN(101349c0) { RET(rl_a_bound(c, SP, ARG(0), ARG(1), 1, 0, 0x101349c0u)); }
PORT_FN(10134a20) { RET(rl_a_move(c, SP, ARG(0), ARG(1), 0, 0x10134a31u, 0x10134a45u)); }
PORT_FN(10134a60) { RET(rl_a_move(c, SP, ARG(0), ARG(1), 1, 0x10134a72u, 0x10134a87u)); }
PORT_FN(10134aa0) { RET(rl_get_sv(c, SP, ARG(0), ARG(0) + ENG_SYNC_A, ARG(1), 0x10134aa0u)); }
PORT_FN(10134b00) { RET(rl_get_sv(c, SP, ARG(0), ARG(0) + ENG_SYNC_B, ARG(1), 0x10134b00u)); }
/* FUN_10135a40 / 10135a60 (eng, A, s) through the machine, as the original calls them (eax eng, ecx A) */
void f_10135a40(cpu *c);
void f_10135a60(cpu *c);
static void mark_set_a(cpu *c, guest_fn f, uint32_t ret)
{
    uint32_t eng = ARG(0), s = ARG(1), a = rd32(c, eng + ENG_SYNC_A);
    c->eax = eng;
    c->ecx = a;
    call3(c, c->esp, f, ret, eng, a, s);
}
PORT_FN(10134d90) { mark_set_a(c, f_10135a40, 0x10134da3u); RET(c->eax); }
PORT_FN(10134db0) { mark_set_a(c, f_10135a60, 0x10134dc3u); RET(c->eax); }
PORT_FN(10135a40) { rl_mark_set(c, ARG(0), ARG(1), ARG(2), 1); RET(c->eax); }
PORT_FN(10135a60) { rl_mark_set(c, ARG(0), ARG(1), ARG(2), 0); RET(c->eax); }

/* editing the delta */
PORT_FN(10134e40) { rl_edit_a(c, SP, ARG(0), ARG(1), 0x10134e40u); RET(c->eax); }
PORT_FN(10134e80) { rl_edit_ba(c, SP, ARG(0), ARG(1), ARG(0) + ENG_SYNC_B, f_1013ba30, 0x10134e80u); RET(c->eax); }
PORT_FN(10134ec0) { rl_edit_ba(c, SP, ARG(0), ARG(1), ARG(0) + ENG_SYNC_A, f_1013b8c0, 0x10134ec0u); RET(c->eax); }
PORT_FN(10134fd0) { rl_edit_a_token(c, SP, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10134dd0) { rl_edit_bytes(c, SP, ARG(0), ARG(1), ARG(2), ARG(3)); RET(c->eax); }
PORT_FN(10132a20) { RET(rl_insert_list(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), f_10132730, 0x10132a20u)); }
PORT_FN(10132a70) { RET(rl_insert_list(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), f_101328a0, 0x10132a70u)); }
PORT_FN(10135010) { RET(rl_delete_between(c, SP, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10135060) { RET(rl_set_field_val(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))); }
PORT_FN(10135150) { RET(rl_set_field_const(c, SP, ARG(0), ARG(1), ARG(2), T_SYM8, ARG(4), 0x10135150u)); }
PORT_FN(101351d0) { RET(rl_set_field_const(c, SP, ARG(0), ARG(1), ARG(2), T_SHORT, ARG(4), 0x101351d0u)); }
PORT_FN(10135250) { RET(rl_insert_value(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10135470) { rl_insert_value_b(c, SP, ARG(0), ARG(1), ARG(2), ARG(3)); RET(c->eax); }
PORT_FN(101353d0) { rl_insert_text(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 1, 0x101353d0u); RET(c->eax); }
PORT_FN(10135420) { rl_insert_text(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0, 0x10135420u); RET(c->eax); }
PORT_FN(10133480) { RET(rl_edit_count(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10135660) { rl_measure(c, SP, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10135820) { RET(rl_match_table(c, SP, ARG(0), ARG(1), (int16_t)ARG(2))); }
PORT_FN(10132bc0) { RET(rl_ab_test(c, SP, ARG(0))); }
PORT_FN(10133170) { rl_ab_call(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); RET(c->eax); }
PORT_FN(10133110) { rl_lookup(c, SP, ARG(0), ARG(1), 0, 0x10133110u); RET(c->eax); }
PORT_FN(10133140) { rl_lookup(c, SP, ARG(0), ARG(1), 1, 0x10133140u); RET(c->eax); }
PORT_FN(101330b0) { RET(rl_val_call(c, SP, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10133210) { rl_set_token(c, SP, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101331c0) { RET(rl_emit(c, SP, ARG(0), ARG(1), ARG(2))); }
PORT_FN(101331e0) { rl_emit_val(c, SP, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10135890)
{
    uint32_t eng = ARG(0);
    wr32(c, eng + ENG_RESULT, (uint32_t)(int32_t)(int16_t)rd16(c, rd32(c, eng + ENG_RS) + RS_MATCH_COUNT));
    RET(eng);
}
PORT_FN(101355d0)
{
    uint32_t eng = ARG(0);
    wr8(c, rd32(c, eng + ENG_RS) + RS_STATUS, 0xff);
    RET(eng);
}

/* ---------------------------------------------------------------- rules_delta.c and more of rules_ops.c */
uint32_t rl_elem_bit2(cpu *c, uint32_t e);
uint32_t rl_ring_flag(cpu *c, uint32_t m);
uint32_t rl_ring_set_next(cpu *c, uint32_t m, uint32_t to);
uint32_t rl_ring_set_prev(cpu *c, uint32_t eng, uint32_t m, uint32_t to);
uint32_t rl_ring_insert_before(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
uint32_t rl_ring_insert_after(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
uint32_t rl_ring_remove(cpu *c, uint32_t eng, uint32_t a);
uint32_t rl_next_mark(cpu *c, uint32_t m, int8_t s);
uint32_t rl_prev_mark(cpu *c, uint32_t eng, uint32_t m, int8_t s);
uint32_t rl_stream_type(cpu *c, uint32_t s);
uint32_t rl_mark_number(cpu *c, uint32_t sp, uint32_t eng, uint32_t m);
uint32_t rl_last_mark_fwd(cpu *c, uint32_t m, uint32_t s);
uint32_t rl_last_mark_back(cpu *c, uint32_t eng, uint32_t m, uint32_t s);
uint32_t rl_set_fields(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t from, uint32_t to, uint32_t value,
                       uint32_t fslot);
void     rl_token_init(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t dst, uint32_t src);
uint32_t rl_delete_range(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t right, uint32_t left);
uint32_t rl_insert_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t right, uint32_t left, uint32_t ref,
                         uint32_t argslot);
uint32_t rl_reset_stream(cpu *c, uint32_t sp, uint32_t eng, uint32_t s);
void     rl_ws_new(cpu *c, uint32_t sp, uint32_t eng);
void     rl_ws_free(cpu *c, uint32_t sp, uint32_t eng);
void     rl_new(cpu *c, uint32_t sp, uint32_t eng);
void     rl_free(cpu *c, uint32_t sp, uint32_t eng);
void     rl_reset_globals(cpu *c, uint32_t sp, uint32_t eng);
uint32_t rl_utterance_reset(cpu *c, uint32_t sp, uint32_t eng);
uint32_t rl_start(cpu *c, uint32_t sp, uint32_t eng, int32_t n, uint32_t p);
uint32_t rl_reach(cpu *c, uint32_t sp, uint32_t eng, uint32_t v);
void     rl_reset_streams(cpu *c, uint32_t sp, uint32_t eng, uint32_t n, uint32_t list);
uint32_t rl_match_pattern(cpu *c, uint32_t sp, uint32_t eng, int16_t n, uint32_t out1, uint32_t out2);
uint32_t rl_mod(cpu *c, uint32_t sp, uint32_t eng, uint32_t va, uint32_t vb, uint32_t out);

PORT_FN(10135a80) { RET(rl_elem_bit2(c, ARG(0))); }
PORT_FN(10135b00) { RET(rl_ring_flag(c, ARG(0))); }
PORT_FN(10135a90) { RET(rl_ring_set_next(c, ARG(0), ARG(1))); }
PORT_FN(10135ab0) { RET(rl_ring_set_prev(c, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10135ae0) { uint32_t e = ARG(0); wr32(c, e + 4, rd32(c, e + 4) | 1); RET(e); }
PORT_FN(10135af0) { uint32_t e = ARG(0); wr32(c, e + 4, rd32(c, e + 4) & ~1u); RET(e); }
PORT_FN(10135b10) { uint32_t e = ARG(0); wr32(c, e + 4, rd32(c, e + 4) & ~2u); RET(e); }
PORT_FN(10135b40) { RET(rl_ring_insert_before(c, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10135bb0) { RET(rl_ring_insert_after(c, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10135c20) { RET(rl_ring_remove(c, ARG(0), ARG(1))); }
PORT_FN(101359f0) { RET(rl_next_mark(c, ARG(0), (int8_t)ARG(1))); }
PORT_FN(10135a10) { RET(rl_prev_mark(c, ARG(0), ARG(1), (int8_t)ARG(2))); }
/* it leaves ecx 9 s and edx the field descriptor, which callers push later without reloading them */
PORT_FN(10135b20)
{
    int32_t s = (int8_t)ARG(0);
    c->ecx = (uint32_t)(s * 9);
    c->edx = rd32(c, stream_desc((uint32_t)s) + SD_FIELDS);
    RET(rl_stream_type(c, ARG(0)));
}
PORT_FN(101359c0) { RET(rl_mark_number(c, SP, ARG(0), ARG(1))); }
PORT_FN(10136200) { RET(rl_last_mark_fwd(c, ARG(0), ARG(1))); }
PORT_FN(10136240) { RET(rl_last_mark_back(c, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10136280) { RET(rl_set_fields(c, SP, ARG(0), ARG(1), ARG(3), ARG(4), ARG(5), SP + 12)); }
PORT_FN(101364c0) { rl_token_init(c, SP, ARG(0), ARG(1), ARG(2), ARG(3)); RET(c->eax); }
PORT_FN(10136a20) { RET(rl_delete_range(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10136570)
{
    uint32_t r = rl_insert_token(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), SP + 4);
    RET((c->eax & 0xffffff00u) | r);
}
PORT_FN(10135c70) { RET((c->eax & 0xffffff00u) | rl_reset_stream(c, SP, ARG(0), ARG(1))); }
PORT_FN(10135900) { rl_ws_new(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10135970) { rl_ws_free(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10131c10) { rl_new(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10131c60) { rl_free(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10131b40) { rl_reset_globals(c, c->esp, ARG(0)); RET(c->eax); }
PORT_FN(101313d0) { RET(rl_utterance_reset(c, SP, ARG(0))); }
PORT_FN(101355e0) { RET(rl_start(c, SP, ARG(0), (int32_t)ARG(1), ARG(2))); }
PORT_FN(10134cc0) { RET(rl_reach(c, SP, ARG(0), ARG(1))); }
PORT_FN(10134f00) { rl_reset_streams(c, SP, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
PORT_FN(10135730) { RET(rl_match_pattern(c, SP, ARG(0), (int16_t)ARG(1), ARG(2), ARG(3))); }
PORT_FN(101358b0) { RET(rl_mod(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10130e50) { uint32_t eng = ARG(0); wr8(c, rd32(c, eng + ENG_RS) + RS_ABORT, 1); RET(eng); }
PORT_FN(10130e60) { uint32_t eng = ARG(0); wr8(c, rd32(c, eng + ENG_RS) + RS_ABORT, 0); RET(eng); }
PORT_FN(10130e70)
{
    uint32_t eng = ARG(0);
    RET((eng & 0xffffff00u) | rd8(c, rd32(c, eng + ENG_RS) + RS_ABORT));
}

/* ---------------------------------------------------------------- lists of tokens, printing */
uint32_t rl_insert_items(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t p, uint32_t n, uint32_t k5, int shorts);
void     rl_print_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t val);
PORT_FN(10132730) { RET(rl_insert_items(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0)); }
PORT_FN(101328a0) { RET(rl_insert_items(c, SP, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 1)); }
PORT_FN(101319a0) { rl_print_value(c, SP, ARG(0), ARG(1), ARG(2)); RET(c->eax); }
