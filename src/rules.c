/* rules - the rule runtime library of ENU.SYN, hand-ported (see rules.h for the data structures).
 *
 * Every function follows the original's memory accesses in order (the engine's structures are shared
 * with the recompiled rule code, and the rules' variables live in the rule functions' frames), and
 * returns what the original leaves in eax. Where the original passes the address of one of its locals
 * to engine code, or writes into its own argument slots, the port does the same at the same guest stack
 * address: such functions take `sp`, the esp they were entered with (the return address at sp, the
 * cdecl arguments from sp + 4).
 */

#include "rules_int.h"

/* -------------------------------------------------------------------------------- rule frames */

/* FUN_101315e0: enter a rule. Opens a new variable frame, saves the rule state into the rule's frame
 * record `frame` and pushes a frame entry (type 7) on the control stack; the rule's scope arrays and
 * its jmp_buf become current. 0, or 1 without a frame record or when the variable stack is full. */
int rl_enter(cpu *c, uint32_t eng, uint32_t frame, uint32_t slot, uint32_t scope, uint32_t seen, uint32_t jmpbuf)
{
    wr32(c, eng + ENG_RESULT, 0);
    if (!frame) return 1;
    uint32_t rs = RS(eng);
    int32_t n = (int32_t)rd32(c, rs + RS_NVARS);
    if (n >= 999) return 1;
    wr32(c, rs + RS_VARS + 4 * (uint32_t)n, rd32(c, rs + RS_FRAME));
    rs = RS(eng);
    wr32(c, rs + RS_NVARS, rd32(c, rs + RS_NVARS) + 1);
    rs = RS(eng);
    wr32(c, rs + RS_FRAME, rd32(c, rs + RS_NVARS));

    wr32(c, frame + FR_SAVED, rd32(c, RS(eng) + RS_SAVED));
    rs = RS(eng);
    wr32(c, frame + FR_FAIL, rd32(c, rs + RS_FAIL));
    wr32(c, frame + FR_NEXT, rd32(c, rs + RS_NEXT));
    wr8(c, frame + FR_TRAIL, rd8(c, RS(eng) + RS_TRAIL));
    wr32(c, frame + FR_RULE_TOP, rd32(c, RS(eng) + RS_RULE_TOP));
    wr32(c, frame + FR_TOP, rd32(c, WS(eng) + WS_TOP));
    wr32(c, frame + FR_CUT, rd32(c, WS(eng) + WS_CUT));
    wr8(c, frame + FR_NSCOPE, rd8(c, RS(eng) + RS_NSCOPE));
    wr32(c, frame + FR_JMPBUF, rd32(c, RS(eng) + RS_JMPBUF));
    copy_pos(c, frame + FR_POS, RS(eng) + RS_POS);
    copy_sv(c, frame + FR_SYNC_A, eng + ENG_SYNC_A);
    copy_sv(c, frame + FR_SYNC_B, eng + ENG_SYNC_B);
    wr8(c, frame + FR_CMP, rd8(c, RS(eng) + RS_CMP));
    wr8(c, frame + FR_EVAL_TOP, rd8(c, WS(eng) + WS_EVAL_TOP));

    uint32_t top = cs_push(c, eng, WS_SZ_FRAME);
    wr8(c, top, CS_FRAME);
    wr32(c, top + 1, frame);
    wr32(c, top + 9, rd32(c, eng + ENG_SCOPE));
    wr32(c, eng + ENG_SCOPE, scope);
    wr32(c, top + 5, rd32(c, eng + ENG_SLOT));
    wr32(c, eng + ENG_SLOT, slot);
    wr32(c, top + 0xd, rd32(c, eng + ENG_SEEN));
    wr32(c, eng + ENG_SEEN, seen);
    wr32(c, RS(eng) + RS_RULE_TOP, rd32(c, WS(eng) + WS_TOP));
    wr32(c, RS(eng) + RS_JMPBUF, jmpbuf);
    return 0;
}

/* FUN_10131790: leave a rule: drop its variable frame and control stack, restore the rule state from its
 * frame record. If the rule was entered behind a barrier (a type 8 entry), the output status becomes 0xea
 * and the result says whether an abort is pending; otherwise a pending abort longjmps to the enclosing
 * rule. `sp` is the esp the original was entered with. Returns eax: 1 if there was no variable frame
 * (else 0), or the abort flag behind a barrier. */
uint32_t rl_leave_at(cpu *c, uint32_t sp, uint32_t eng)
{
    int barrier = 0;
    uint32_t result;
    uint32_t rs = RS(eng);
    if ((int32_t)rd32(c, rs + RS_NVARS) <= 0) {
        result = 1;
    } else {
        wr32(c, rs + RS_NVARS, rd32(c, rs + RS_FRAME));
        rs = RS(eng);
        wr32(c, rs + RS_NVARS, rd32(c, rs + RS_NVARS) - 1);
        rs = RS(eng);
        wr32(c, rs + RS_FRAME, rd32(c, rs + RS_VARS + 4 * rd32(c, rs + RS_NVARS)));
        result = 0;
    }
    uint32_t ws = WS(eng);
    uint32_t rule_top = rd32(c, RS(eng) + RS_RULE_TOP);
    if (rd8(c, rd32(c, ws + WS_SZ_FRAME) + rule_top) == CS_BARRIER) barrier = 1;
    if (rd32(c, ws + WS_CHUNK)) {
        wr32(c, ws + WS_TOP, rule_top);
        cs_sync_off(c, eng);
    }
    uint32_t fr = rd32(c, rule_top + 1);
    uint32_t top = rd32(c, WS(eng) + WS_TOP);
    wr32(c, eng + ENG_SCOPE, rd32(c, top + 9));
    wr32(c, eng + ENG_SLOT, rd32(c, top + 5));
    wr32(c, eng + ENG_SEEN, rd32(c, top + 0xd));
    wr32(c, RS(eng) + RS_SAVED, rd32(c, fr + FR_SAVED));
    rs = RS(eng);
    wr32(c, rs + RS_FAIL, rd32(c, fr + FR_FAIL));
    wr32(c, rs + RS_NEXT, rd32(c, fr + FR_NEXT));
    wr8(c, RS(eng) + RS_TRAIL, rd8(c, fr + FR_TRAIL));
    wr32(c, RS(eng) + RS_RULE_TOP, rd32(c, fr + FR_RULE_TOP));
    ws = WS(eng);
    if (rd32(c, ws + WS_CHUNK)) {
        wr32(c, ws + WS_TOP, rd32(c, fr + FR_TOP));
        cs_sync_off(c, eng);
    }
    wr32(c, WS(eng) + WS_CUT, rd32(c, fr + FR_CUT));
    wr8(c, RS(eng) + RS_NSCOPE, rd8(c, fr + FR_NSCOPE));
    wr32(c, RS(eng) + RS_JMPBUF, rd32(c, fr + FR_JMPBUF));
    copy_pos(c, RS(eng) + RS_POS, fr + FR_POS);
    copy_sv(c, eng + ENG_SYNC_A, fr + FR_SYNC_A);
    copy_sv(c, eng + ENG_SYNC_B, fr + FR_SYNC_B);
    wr8(c, RS(eng) + RS_CMP, rd8(c, fr + FR_CMP));
    wr8(c, WS(eng) + WS_EVAL_TOP, rd8(c, fr + FR_EVAL_TOP));
    wr32(c, RS(eng) + RS_11AC, 0);
    if (barrier) {
        wr32(c, rd32(c, eng + ENG_OUT) + 0x1a4, 0xea);
        return rd8(c, RS(eng) + RS_ABORT) != 0;
    }
    rs = RS(eng);
    if (rd8(c, rs + RS_ABORT)) {
        wr8(c, rs + RS_ABORT, 1);
        guest_longjmp(c, sp - 16, rd32(c, RS(eng) + RS_JMPBUF), 0x10131991u);
    }
    return result;
}

/* FUN_10130e80: abort: every rule returns (RS_ABORT), starting with a longjmp to the current rule */
void rl_throw_at(cpu *c, uint32_t sp, uint32_t eng)
{
    wr8(c, RS(eng) + RS_ABORT, 1);
    guest_longjmp(c, sp, rd32(c, RS(eng) + RS_JMPBUF), 0x10130ea0u);
}

/* FUN_101311a0: backtrack. Pops the control stack, undoing (restoring the cursor, trailed values, the
 * cut point), up to a choice point: returns its label. A choice point of type 3 only counts if the cursor
 * can move on (rl_step). `depth` counts nested marks (types 4 and 6) to skip first. -1 at the rule's frame
 * or when aborting. */
int32_t rl_backtrack(cpu *c, uint32_t eng, int32_t depth)
{
    if (rd8(c, RS(eng) + RS_ABORT)) return -1;
    for (;;) {
        uint32_t ws = WS(eng);
        uint32_t top = rd32(c, ws + WS_TOP);
        switch ((int8_t)rd8(c, top)) {
        case CS_RETRY:
            cs_pop_field(c, eng, WS_SZ_LABEL);
            if (depth == 0) return (int32_t)rd32(c, top + 1);
            break;
        case CS_POS:
            cs_pop_field(c, eng, WS_SZ_POS);
            copy_pos(c, RS(eng) + RS_POS, top + 1);
            break;
        case CS_UNDO: {
            uint32_t size = ((rd32(c, top + 7) - 1) | 1) + rd32(c, ws + WS_SZ_UNDO) + 1;
            cs_pop(c, eng, size);
            uint32_t n = rd32(c, top + 7);
            copy(c, rd32(c, top + 3), top + rd32(c, WS(eng) + WS_SZ_UNDO), n);
            break;
        }
        case CS_NEXT:
            cs_pop_field(c, eng, WS_SZ_LABEL);
            if (depth == 0 && rl_step(c, eng, 0, 1)) return (int32_t)rd32(c, top + 1);
            break;
        case CS_DOWN:
            cs_pop_field(c, eng, WS_SZ_MARK);
            if (depth > 0) depth--;
            break;
        case CS_CUTPT:
            cs_pop_field(c, eng, WS_SZ_CUTPT);
            wr32(c, WS(eng) + WS_CUT, rd32(c, top + 1));
            break;
        case CS_UP:
            cs_pop_field(c, eng, WS_SZ_MARK);
            depth++;
            break;
        default:        /* the rule's frame (7), or anything else */
            return -1;
        }
    }
}

/* the cut point as a stack position: at a barrier it is the rule's frame */
static int cut_target(cpu *c, uint32_t eng, uint32_t *to)
{
    uint32_t ws = WS(eng), cut = rd32(c, ws + WS_CUT);
    int barrier = rd8(c, cut) == CS_BARRIER;
    if (!rd32(c, ws + WS_CHUNK)) return 0;
    *to = barrier ? rd32(c, RS(eng) + RS_RULE_TOP) : cut;
    return 1;
}

/* FUN_10131f90: cut: drop the choice points above the cut point */
void rl_cut(cpu *c, uint32_t eng)
{
    uint32_t to;
    if (!cut_target(c, eng, &to)) return;
    wr32(c, WS(eng) + WS_TOP, to);
    cs_sync_off(c, eng);
}

/* FUN_10131ff0: push a choice point resumed at `label` */
void rl_push_retry(cpu *c, uint32_t eng, uint32_t label)
{
    uint32_t top = cs_push(c, eng, WS_SZ_LABEL);
    wr8(c, top, CS_RETRY);
    wr32(c, top + 1, label);
}

/* push the cursor (restored when backtracking over it) */
void rl_push_pos(cpu *c, uint32_t eng)
{
    uint32_t top = cs_push(c, eng, WS_SZ_POS);
    wr8(c, top, CS_POS);
    copy_pos(c, top + 1, RS(eng) + RS_POS);
}

/* push a choice point resumed at `label` if the cursor can move on */
void rl_push_next(cpu *c, uint32_t eng, uint32_t label)
{
    uint32_t top = cs_push(c, eng, WS_SZ_LABEL);
    wr8(c, top, CS_NEXT);
    wr32(c, top + 1, label);
}

/* FUN_101320e0 (CS_DOWN) / FUN_10132120 (CS_UP) */
void rl_push_mark(cpu *c, uint32_t eng, int type)
{
    uint32_t top = cs_push(c, eng, WS_SZ_MARK);
    wr8(c, top, (uint8_t)type);
}

/* FUN_10132ac0: the rule matched: go on at `label` after a cut, trailing variable writes from now on */
void rl_succeed(cpu *c, uint32_t eng, uint32_t label)
{
    wr32(c, RS(eng) + RS_NEXT, label);
    rl_cut(c, eng);
    rl_push_retry(c, eng, rd32(c, RS(eng) + RS_NEXT));
    wr8(c, RS(eng) + RS_TRAIL, 1);
}

/* FUN_101338d0: the same without the choice point, variable writes not trailed */
void rl_commit(cpu *c, uint32_t eng, uint32_t label)
{
    wr32(c, RS(eng) + RS_NEXT, label);
    rl_cut(c, eng);
    wr8(c, RS(eng) + RS_TRAIL, 0);
}

/* FUN_101345e0: set the rule's labels and cut */
void rl_set_labels(cpu *c, uint32_t eng, uint32_t next, uint32_t fail)
{
    wr32(c, RS(eng) + RS_FAIL, fail);
    wr32(c, RS(eng) + RS_NEXT, next);
    rl_cut(c, eng);
    wr32(c, WS(eng) + 0x8f, 0);
}

/* -------------------------------------------------------------------------------- variables and values */




/* FUN_10131520: resolve the value at `val` into the ref at `ref`: its data address, type and flag. A
 * token field goes through the stream's getter. Negative types other than -6..-3 leave the address. */
void rl_ref_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t ref, uint32_t val)
{
    (void)eng;
    int16_t t = val_type(c, val);
    if (t < 0) {
        wr16(c, ref + 4, (uint16_t)t);
        if (t >= T_SYNC && t <= T_INT) wr32(c, ref, val + 2);
        wr8(c, ref + 6, 0);
        return;
    }
    int16_t f = (int16_t)rd16(c, val + 2);
    if (f == -1) {
        wr32(c, ref, val + 4);
        wr16(c, ref + 4, (uint16_t)t);
        wr8(c, ref + 6, 0);
        return;
    }
    /* the getter is called with esp as the original has it (entry - 8: edi, esi saved) */
    uint32_t data = icall1(c, sp - 8, getter(c, (uint32_t)(int32_t)t, (uint32_t)(int32_t)f), 0x1013158cu, val + 4);
    wr32(c, ref, data);
    uint32_t fd = field_desc(c, (uint32_t)(int32_t)val_type(c, val), (uint32_t)(int32_t)f);
    wr16(c, ref + 4, rd16(c, fd + FD_TYPE));
    fd = field_desc(c, (uint32_t)(int32_t)val_type(c, val), (uint32_t)(int32_t)f);
    wr8(c, ref + 6, rd8(c, fd + FD_FLAG));
}

void rl_ref(cpu *c, uint32_t eng, uint32_t ref, uint32_t val) { rl_ref_at(c, c->esp, eng, ref, val); }

/* FUN_10130ea0: initialize the variable `var` of type `type` from the value `src` (converting) and, for
 * a sync mark, register it (RS_VARS). 0, or 1 for a type it cannot convert or a full variable stack.
 * `sp`: the original writes a converted int into its `src` argument slot (sp + 12). */
int rl_var_init_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var, uint32_t src, int16_t type)
{
    uint32_t ref = sp - 8;
    wr16(c, var, (uint16_t)type);
    switch (type) {
    case T_SYNC: {
        wr32(c, var + 2, rd32(c, src + 2));
        uint32_t rs = RS(eng);
        int32_t n = (int32_t)rd32(c, rs + RS_NVARS);
        if (n >= 999) return 1;
        wr32(c, rs + RS_VARS + 4 * (uint32_t)n, var);
        rs = RS(eng);
        wr32(c, rs + RS_NVARS, rd32(c, rs + RS_NVARS) + 1);
        return 0;
    }
    case T_DOUBLE: {
        int16_t st = val_type(c, src);
        if (st == T_DOUBLE) {
            wr32(c, var + 2, rd32(c, src + 2));
            wr32(c, var + 6, rd32(c, src + 6));
            return 0;
        }
        if (st == T_SHORT) {
            int32_t v = (int16_t)rd16(c, src + 2);
            wr32(c, sp + 12, (uint32_t)v);
            store_double(c, var + 2, (double)v);
            return 0;
        }
        if (st == T_INT) {
            store_double(c, var + 2, (double)(int32_t)rd32(c, src + 2));
            return 0;
        }
        if (st < 0) return 1;
        rl_ref_at(c, AT(sp - 0x14, 3), eng, ref, src);
        int32_t v = (int16_t)rd16(c, rd32(c, ref));
        wr32(c, sp + 12, (uint32_t)v);
        store_double(c, var + 2, (double)v);
        val_release(c, src);
        return 0;
    }
    case T_SHORT: {
        int16_t st = val_type(c, src);
        if (st == T_DOUBLE) { wr16(c, var + 2, (uint16_t)ftol32(rd64(c, src + 2))); return 0; }
        if (st == T_SHORT || st == T_INT) { wr16(c, var + 2, rd16(c, src + 2)); return 0; }
        if (st < 0) return 1;
        rl_ref_at(c, AT(sp - 0x14, 3), eng, ref, src);
        wr16(c, var + 2, rd16(c, rd32(c, ref)));
        val_release(c, src);
        return 0;
    }
    case T_INT: {
        int16_t st = val_type(c, src);
        if (st == T_DOUBLE) { wr32(c, var + 2, ftol32(rd64(c, src + 2))); return 0; }
        if (st == T_SHORT) { wr32(c, var + 2, (uint32_t)(int32_t)(int16_t)rd16(c, src + 2)); return 0; }
        if (st == T_INT) { wr32(c, var + 2, rd32(c, src + 2)); return 0; }
        if (st < 0) return 1;
        rl_ref_at(c, AT(sp - 0x14, 3), eng, ref, src);
        wr32(c, var + 2, (uint32_t)(int32_t)(int16_t)rd16(c, rd32(c, ref)));
        val_release(c, src);
        return 0;
    }
    default:
        return 1;
    }
}

/* FUN_101310b0: a sync mark variable, initially null, registered. Returns eax as the original: al 1 if
 * registered (the rule state's address above it), else 0 (the variable's address above it). */
uint32_t rl_var_init_sync(cpu *c, uint32_t eng, uint32_t var)
{
    wr32(c, var + 2, 0);
    wr16(c, var, (uint16_t)T_SYNC);
    uint32_t rs = RS(eng);
    int32_t n = (int32_t)rd32(c, rs + RS_NVARS);
    if (n >= 999) return var & 0xffffff00u;
    wr32(c, rs + RS_VARS + 4 * (uint32_t)n, var);
    rs = RS(eng);
    wr32(c, rs + RS_NVARS, rd32(c, rs + RS_NVARS) + 1);
    return (rs & 0xffffff00u) | 1;
}

/* FUN_101314f0: trail the value at `val` (its current content goes on the control stack). Returns
 * FUN_10138510's eax, which the original leaves. */
uint32_t rl_trail_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val)
{
    uint32_t ref = sp - 8;
    rl_ref_at(c, AT(sp - 0xc, 3), eng, ref, val);
    return rl_trail_ref(c, eng, ref);
}

/* FUN_10131cd0: push the value at `val` on the value stack */
void rl_push_val_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val)
{
    uint32_t ref = sp - 8;
    rl_ref_at(c, AT(sp - 0x10, 3), eng, ref, val);
    rl_push(c, eng, ref);
    val_release(c, val);
}

/* FUN_10131d10 / 10131d40 / 10131d70: push a constant (the argument at sp + 8) of type -1, -3, -4 */
void rl_push_const_at(cpu *c, uint32_t sp, uint32_t eng, int16_t type, uint32_t ret)
{
    uint32_t ref = sp - 8;
    wr16(c, ref + 4, (uint16_t)type);
    wr32(c, ref, sp + 8);
    wr8(c, ref + 6, 0);
    (void)ret;
    rl_push(c, eng, ref);
}

/* FUN_10131e90: pop the value stack into the variable `var` */
void rl_pop_into_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var)
{
    uint32_t popped = sp - 0x10, ref = sp - 8;
    rl_pop(c, eng, popped);
    if (rd8(c, RS(eng) + RS_TRAIL)) rl_trail_at(c, AT(sp - 0x18, 2), eng, var);
    rl_ref_at(c, AT(sp - 0x18, 3), eng, ref, var);
    rl_assign(c, eng, ref, popped);
    val_release(c, var);
}

/* FUN_101325e0: var := the short at sp + 12 (trailed if trailing is on) */
void rl_set_short_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var)
{
    int16_t v = (int16_t)rd16(c, sp + 12);
    if (rd8(c, RS(eng) + RS_TRAIL)) rl_trail_at(c, AT(sp - 0x18, 2), eng, var);
    int16_t t = val_type(c, var);
    switch (t) {
    case T_SYNC:
    case T_INT:
        wr32(c, var + 2, (uint32_t)(int32_t)v);
        return;
    case T_DOUBLE:
        wr32(c, sp + 12, (uint32_t)(int32_t)v);
        store_double(c, var + 2, (double)v);
        return;
    case T_SHORT:
        wr16(c, var + 2, (uint16_t)v);
        return;
    default:
        break;
    }
    if (t < 0) {
        rl_throw_at(c, AT(sp - 0x18, 1), eng);
        return;
    }
    uint32_t cref = sp - 0x10, ref = sp - 8;
    wr16(c, cref + 4, (uint16_t)T_SHORT);
    wr32(c, cref, sp + 12);
    wr8(c, cref + 6, 0);
    rl_ref_at(c, AT(sp - 0x18, 3), eng, ref, var);
    rl_assign(c, eng, ref, cref);
    val_release(c, var);
}

/* FUN_101332d0: trail the variable if trailing is on; the value's field reference is used up. Returns
 * eax as the original leaves it (callers keep its upper bits in a byte argument). */
uint32_t rl_touch_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val)
{
    uint32_t eax = eng;
    if (rd8(c, RS(eng) + RS_TRAIL)) eax = rl_trail_at(c, AT(sp - 4, 2), eng, val);
    val_release(c, val);
    return eax;
}

/* FUN_10133250: dst := src (trailed if trailing is on) */
void rl_assign_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t dst, uint32_t src)
{
    uint32_t rd = sp - 8, rsrc = sp - 0x10;
    if (rd8(c, RS(eng) + RS_TRAIL)) rl_trail_at(c, AT(sp - 0x1c, 2), eng, dst);
    rl_ref_at(c, AT(sp - 0x1c, 3), eng, rd, dst);
    rl_ref_at(c, AT(sp - 0x28, 3), eng, rsrc, src);
    rl_assign(c, eng, rd, rsrc);
    val_release(c, dst);
    val_release(c, src);
}

/* FUN_10132be0: compare a and b (RS_CMP) */
void rl_compare_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b, uint32_t ret)
{
    uint32_t ra = sp - 8, rb = sp - 0x10;
    rl_ref_at(c, AT(sp - 0x1c, 3), eng, ra, a);
    rl_ref_at(c, AT(sp - 0x28, 3), eng, rb, b);
    (void)ret;
    rl_compare(c, eng, ra, rb);
    val_release(c, a);
    val_release(c, b);
}

/* FUN_10138c60 twice and FUN_10138920: compare the two top values of the value stack (popped) */
static void compare_popped(cpu *c, uint32_t sp, uint32_t eng, uint32_t a, uint32_t b, uint32_t r1, uint32_t r2,
                           uint32_t r3)
{
    (void)sp; (void)r1; (void)r2; (void)r3;
    rl_pop(c, eng, a);
    rl_pop(c, eng, b);
    rl_compare(c, eng, a, b);
}

/* how the comparison predicates read RS_CMP */
static int cmp_test(int8_t v, int op)
{
    switch (op) {
    case 0: return v != 0;
    case 1: return v == -1;
    case 2: return v != 1;
    case 3: return v != -1;
    case 4: return v == 0;
    default: return v == 1;
    }
}

/* FUN_10132170 family: push val, push the short constant, compare the two */
uint32_t rl_cmp_val_short_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t val, uint32_t k, int op, uint32_t base)
{
    uint32_t args[2] = { eng, val };
    call_at(c, sp - 0x14, f_10131cd0, base + 0x13, 2, args);
    args[1] = k;
    call_at(c, sp - 0x1c, f_10131d70, base + 0x1e, 2, args);
    compare_popped(c, sp - 0x24, eng, sp - 8, sp - 0x10, base + 0x29, base + 0x34, base + 0x44);
    return (uint32_t)cmp_test((int8_t)rd8(c, RS(eng) + RS_CMP), op);
}

/* FUN_10132370 family: compare the two top values of the value stack */
uint32_t rl_cmp_stack_at(cpu *c, uint32_t sp, uint32_t eng, int op, uint32_t base)
{
    compare_popped(c, sp - 0x14, eng, sp - 8, sp - 0x10, base + 0x13, base + 0x1e, base + 0x2e);
    return (uint32_t)cmp_test((int8_t)rd8(c, RS(eng) + RS_CMP), op);
}

/* -------------------------------------------------------------------------------- the cursor */

/* The scope check the cursor movements share: before leaving a sync mark, every stream in the rule's
 * scope must not end at it (bit 0 of its backward link), nor be marked (bit 1) unless it is the cursor's
 * stream or the rule has matched in it; the "matched" flags are cleared on the way. Returns the number of
 * scope streams checked, or -1 to stop. */
static int scope_check(cpu *c, uint32_t eng, uint32_t mark, uint32_t cur_stream)
{
    int i = 0;
    uint32_t rs = RS(eng);
    if (rd8(c, rs + RS_NSCOPE) == 0) return 0;
    do {
        uint8_t s = rd8(c, rd32(c, eng + ENG_SCOPE) + (uint32_t)i);
        uint32_t base = rd32(c, rs + RS_BACK);
        if (rd8(c, mark + 4 * (base + s)) & 1) return -1;
        if (((rd32(c, mark + 4 * (base + (uint32_t)(int32_t)(int8_t)s)) >> 1) & 1) && cur_stream != s
            && rd8(c, rd32(c, eng + ENG_SEEN) + (uint32_t)i) == 0)
            return -1;
        wr8(c, rd32(c, eng + ENG_SEEN) + (uint32_t)i, 0);
        rs = RS(eng);
        i++;
    } while (i < (int)rd8(c, rs + RS_NSCOPE));
    return i;
}

static void clear_seen_from(cpu *c, uint32_t eng, int i)
{
    while (i < (int)rd8(c, RS(eng) + RS_NSCOPE)) {
        wr8(c, rd32(c, eng + ENG_SEEN) + (uint32_t)i, 0);
        i++;
    }
}

/* FUN_10135d00: move the cursor to the next sync mark in its stream and direction. A token in between
 * is crossed only with `across`. With `check_scope` (and a cursor that has moved before), the rule's scope
 * must allow leaving the current mark. 1 if moved, 0 if not. */
int rl_step(cpu *c, uint32_t eng, int across, int check_scope)
{
    uint32_t rs = RS(eng);
    uint32_t cur = rd8(c, rs + RS_POS_STREAM);
    uint32_t mark = rd32(c, rs + RS_POS_MARK);
    int i = 0;
    if (rd8(c, rs + RS_NSCOPE) && check_scope && !rd8(c, rs + RS_POS_FRESH)) {
        i = scope_check(c, eng, mark, cur);
        if (i < 0) return 0;
    }
    if (!mark) return 0;
    rs = RS(eng);
    int back = rd8(c, rs + RS_POS_BACK);
    uint32_t next = cursor_link(c, rs, mark, cur) & ~3u;
    if (!next) return 0;
    uint32_t w = rd32(c, next);
    if (!(w & 2)) {
        if (!across) return 0;
        next = back ? rd32(c, next + 4) & ~3u : w & ~3u;
    }
    wr32(c, rs + RS_POS_MARK, next);
    wr8(c, RS(eng) + RS_POS_FRESH, 0);
    clear_seen_from(c, eng, i);
    return 1;
}

/* FUN_10135e40: move the cursor over sync marks until `stop` (or a token, or the end). 1 if it got to
 * stop or a token, 0 at the end or when the scope forbids. */
int rl_step_to(cpu *c, uint32_t eng, uint32_t stop, int check_scope)
{
    uint32_t rs = RS(eng);
    uint32_t cur = rd8(c, rs + RS_POS_STREAM);
    uint32_t mark = rd32(c, rs + RS_POS_MARK);
    if (!mark) return 0;
    for (;;) {
        int i = 0;
        rs = RS(eng);
        if (rd8(c, rs + RS_NSCOPE) && check_scope && !rd8(c, rs + RS_POS_FRESH)) {
            i = scope_check(c, eng, mark, cur);
            if (i < 0) return 0;
        }
        rs = RS(eng);
        uint32_t next = cursor_link(c, rs, mark, cur) & ~3u;
        if (!next) return 0;
        if (!is_mark(c, next)) return 1;
        wr32(c, rs + RS_POS_MARK, next);
        wr8(c, RS(eng) + RS_POS_FRESH, 0);
        clear_seen_from(c, eng, i);
        mark = next;
        if (mark == stop) return 1;
    }
}

/* FUN_10135f70: move the cursor over sync marks up to the next token. 1 if a token follows, 0 at the
 * end or when the scope forbids. */
int rl_skip_marks(cpu *c, uint32_t eng, int check_scope)
{
    uint32_t rs = RS(eng);
    uint32_t cur = rd8(c, rs + RS_POS_STREAM);
    uint32_t mark = rd32(c, rs + RS_POS_MARK);
    if (!mark) return 0;
    for (;;) {
        int i = 0;
        rs = RS(eng);
        if (rd8(c, rs + RS_NSCOPE) && check_scope && !rd8(c, rs + RS_POS_FRESH)) {
            i = scope_check(c, eng, mark, cur);
            if (i < 0) return 0;
        }
        rs = RS(eng);
        uint32_t next = cursor_link(c, rs, mark, cur) & ~3u;
        if (!next) return 0;
        if (!is_mark(c, next)) return 1;
        wr32(c, rs + RS_POS_MARK, next);
        wr8(c, RS(eng) + RS_POS_FRESH, 0);
        clear_seen_from(c, eng, i);
        mark = next;
    }
}

/* FUN_101360a0: move the cursor over the next token (to the sync mark after it), passing sync marks on
 * the way. 1 if it crossed a token, 0 at the end or when the scope forbids. */
int rl_next_token(cpu *c, uint32_t eng, int check_scope)
{
    uint32_t rs = RS(eng);
    uint32_t cur = rd8(c, rs + RS_POS_STREAM);
    uint32_t mark = rd32(c, rs + RS_POS_MARK);
    if (!mark) return 0;
    for (;;) {
        int i = 0;
        if (rd8(c, rs + RS_NSCOPE) && check_scope && !rd8(c, rs + RS_POS_FRESH)) {
            i = scope_check(c, eng, mark, cur);
            if (i < 0) return 0;
        }
        rs = RS(eng);
        uint32_t next = cursor_link(c, rs, mark, cur) & ~3u;
        if (!next) return 0;
        wr32(c, rs + RS_POS_MARK, next);
        wr8(c, RS(eng) + RS_POS_FRESH, 0);
        clear_seen_from(c, eng, i);
        rs = RS(eng);
        if (!is_mark(c, next)) {
            uint32_t after = rd8(c, rs + RS_POS_BACK) ? rd32(c, next + 4) : rd32(c, next);
            wr32(c, rs + RS_POS_MARK, after & ~3u);
            return 1;
        }
        mark = next;
    }
}

/* FUN_10132550: does the token at the cursor (the next one in its direction) have field `f` of stream
 * `s` different from the byte `v`? 1 also when there is no token. */
uint32_t rl_field_ne_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f, uint8_t v)
{
    uint32_t rs = RS(eng);
    uint32_t e = rd32(c, rs + RS_POS_MARK);
    for (;;) {
        e = cursor_link(c, rs, e, rd8(c, rs + RS_POS_STREAM)) & ~3u;
        if (!e) return 1;
        if (!is_mark(c, e)) break;
    }
    uint32_t p = icall1(c, sp - 8, getter(c, s & 0xff, f & 0xff), 0x101325c7u, e + 8);
    return rd8(c, p) != v;
}

/* FUN_10131da0: push field `f` of the token at the cursor (skipping sync marks). 0, or 1 if there is no
 * token. */
uint32_t rl_push_field_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t f)
{
    s &= 0xff;
    f &= 0xff;
    uint32_t ref = sp - 8;
    uint32_t fd = field_desc(c, s, f);
    wr16(c, ref + 4, rd16(c, fd + FD_TYPE));
    uint32_t rs = RS(eng);
    wr8(c, ref + 6, rd8(c, fd + FD_FLAG));
    uint32_t e = rd32(c, rs + RS_POS_MARK);
    for (;;) {
        e = cursor_link(c, rs, e, rd8(c, rs + RS_POS_STREAM)) & ~3u;
        if (!e) return 1;
        if (!is_mark(c, e)) break;
    }
    wr32(c, ref, icall1(c, sp - 0x18, getter(c, s, f), 0x10131e6fu, e + 8));
    rl_push(c, eng, ref);
    return 0;
}

/* FUN_10132c40: match the string (len bytes at str) against field 0 of stream s, token by token from the
 * cursor on. 0 if all match, 1 if not. */
uint32_t rl_match_string_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t len, uint32_t str)
{
    s &= 0xff;
    uint32_t end = str + (len & 0xff);
    uint32_t fd = field_desc(c, s, 0);
    uint32_t get0 = rd32(c, rd32(c, stream_desc(s) + SD_GETTERS));
    if ((int16_t)rd16(c, fd + FD_TYPE) == -1) {
        /* byte symbols: compared directly */
        while (str < end) {
            uint32_t rs = RS(eng);
            uint32_t e = cursor_link(c, rs, rd32(c, rs + RS_POS_MARK), rd8(c, rs + RS_POS_STREAM)) & ~3u;
            if (!e) return 1;
            if (!is_mark(c, e)) {
                uint32_t p = icall1(c, sp - 0x20, get0, 0x10132cdeu, e + 8);
                if (rd8(c, p) != rd8(c, str)) return 1;
                str++;
            }
            if (!rl_step(c, eng, 1, 1)) return 1;
        }
        return 0;
    }
    /* anything else: through the value comparison, the string byte as a symbol */
    uint32_t ra = sp - 8, rb = sp - 0x10;
    wr16(c, ra + 4, 0xffff);
    uint8_t flag = rd8(c, fd + FD_FLAG);
    wr8(c, ra + 6, flag);
    wr8(c, rb + 6, flag);
    wr16(c, rb + 4, rd16(c, fd + FD_TYPE));
    while (str < end) {
        uint32_t rs = RS(eng);
        uint32_t e = cursor_link(c, rs, rd32(c, rs + RS_POS_MARK), rd8(c, rs + RS_POS_STREAM)) & ~3u;
        if (!e) return 1;
        uint32_t next = str;
        if (!is_mark(c, e)) {
            next = str + 1;
            wr32(c, ra, str);
            wr32(c, rb, icall1(c, sp - 0x20, get0, 0x10132d89u, e + 8));
            rl_compare(c, eng, ra, rb);
            if (rd8(c, RS(eng) + RS_CMP)) return 1;
        }
        if (!rl_step(c, eng, 1, 1)) return 1;
        str = next;
    }
    return 0;
}

/* FUN_10132de0: match 16-bit values (len bytes at p, big-endian sign-magnitude) against field 0 of
 * stream s token by token; each value is decoded into the original's `p` argument slot (sp + 16). */
uint32_t rl_match_shorts_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t len, uint32_t p)
{
    s &= 0xff;
    uint32_t ra = sp - 8, rb = sp - 0x10;
    wr32(c, ra, sp + 16);
    uint32_t end = p + (len & 0xff);
    wr16(c, ra + 4, (uint16_t)T_SHORT);
    uint32_t fd = field_desc(c, s, 0);
    wr8(c, ra + 6, rd8(c, fd + FD_FLAG));
    wr16(c, rb + 4, rd16(c, fd + FD_TYPE));
    wr8(c, rb + 6, rd8(c, fd + FD_FLAG));
    uint32_t get0 = rd32(c, rd32(c, stream_desc(s) + SD_GETTERS));
    while (p < end) {
        uint32_t rs = RS(eng);
        uint32_t e = cursor_link(c, rs, rd32(c, rs + RS_POS_MARK), rd8(c, rs + RS_POS_STREAM)) & ~3u;
        if (!e) return 1;
        if (!is_mark(c, e)) {
            uint8_t b0 = rd8(c, p), b1 = rd8(c, p + 1);
            uint32_t v = ((uint32_t)(b0 & 0x7f) << 8) | b1;
            if (b0 & 0x80) v = 0u - v;
            wr32(c, sp + 16, v);
            p += 2;
            wr32(c, rb, icall1(c, sp - 0x20, get0, 0x10132eb7u, e + 8));
            rl_compare(c, eng, ra, rb);
            if (rd8(c, RS(eng) + RS_CMP)) return 1;
        }
        if (!rl_step(c, eng, 1, 1)) return 1;
    }
    return 0;
}

/* resolve a sync variable with a pending offset: FUN_1013b5b0 through the machine */
static int32_t sv_resolve(cpu *c, uint32_t sp, uint32_t eng, uint32_t sv, uint32_t ret)
{
    return (int32_t)call2(c, sp, f_1013b5b0, ret, eng, sv);
}

/* FUN_10133550 family: put the cursor on sync variable A in stream s, direction `back`, `fresh`. 0, or 1
 * if A is null, unresolvable or not a boundary of s. */
uint32_t rl_goto_a_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, int back, int fresh, uint32_t ret)
{
    uint32_t sv = eng + ENG_SYNC_A;
    if (!(rd8(c, sv + SV_STATE) & 1)) {
        int32_t r = sv_resolve(c, sp - 8, eng, sv, ret);
        if (r >= 0 && r <= 2) return 1;
        wr8(c, sv + SV_STATE, 1);
    }
    uint32_t m = rd32(c, sv);
    if (!m) return 1;
    uint32_t rs = RS(eng);
    if (!(rd8(c, m + 4 * (rd32(c, rs + RS_BACK) + (s & 0xff))) & 1)) return 1;
    wr32(c, rs + RS_POS_MARK, m);
    wr8(c, RS(eng) + RS_POS_STREAM, (uint8_t)s);
    wr8(c, RS(eng) + RS_POS_BACK, (uint8_t)back);
    wr8(c, RS(eng) + RS_POS_FRESH, (uint8_t)fresh);
    return 0;
}

/* FUN_10133670 / 10133710: sync variable A := the sync value v (resolved), then the cursor on it (fresh) */
uint32_t rl_goto_val_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t v, uint32_t s, int back, uint32_t ret)
{
    uint32_t sv = eng + ENG_SYNC_A;
    wr8(c, sv + SV_STATE, 1);
    uint32_t m = rd32(c, v + 2);
    wr32(c, sv + SV_OFFSET, 0);
    wr32(c, sv, m);
    return rl_goto_a_at(c, sp - 4, eng, s, back, 1, ret);
}

/* FUN_10132700: A := a, B := b (resolved) */
void rl_set_ab(cpu *c, uint32_t eng, uint32_t a, uint32_t b)
{
    wr8(c, eng + ENG_SYNC_B + SV_STATE, 1);
    wr8(c, eng + ENG_SYNC_A + SV_STATE, 1);
    wr32(c, eng + ENG_SYNC_A, rd32(c, a + 2));
    wr32(c, eng + ENG_SYNC_B, rd32(c, b + 2));
    wr32(c, eng + ENG_SYNC_B + SV_OFFSET, 0);
    wr32(c, eng + ENG_SYNC_A + SV_OFFSET, 0);
}

/* FUN_10134690 / FUN_101346b0: A := v / B := v */
void rl_set_a(cpu *c, uint32_t eng, uint32_t v)
{
    wr8(c, eng + ENG_SYNC_A + SV_STATE, 1);
    wr32(c, eng + ENG_SYNC_A, rd32(c, v + 2));
    wr32(c, eng + ENG_SYNC_A + SV_OFFSET, 0);
}
void rl_set_b(cpu *c, uint32_t eng, uint32_t v)
{
    wr8(c, eng + ENG_SYNC_B + SV_STATE, 1);
    wr32(c, eng + ENG_SYNC_B, rd32(c, v + 2));
    wr32(c, eng + ENG_SYNC_A + SV_OFFSET, 0);
}

/* FUN_10132f20: move the cursor forward (across tokens) to sync variable A. 0 if it got there, 1 if A is
 * null or out of reach. */
uint32_t rl_advance_to_a_at(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t sv = eng + ENG_SYNC_A;
    if (!rd32(c, sv)) return 1;
    if (rd8(c, sv + SV_STATE) & 2) sv_resolve(c, sp - 8, eng, sv, 0x10132f45u);
    while (rd32(c, RS(eng) + RS_POS_MARK) != rd32(c, sv))
        if (!rl_step(c, eng, 0, 1)) return 1;
    return 0;
}

/* FUN_10132f80: move the cursor (without crossing tokens) to a mark that is a boundary of all n streams
 * in `streams`; then push a choice point for `label` and the cursor, and flag the streams as matched.
 * 0, or 1 if the cursor cannot move on. */
uint32_t rl_align(cpu *c, uint32_t eng, uint32_t label, uint32_t n, uint32_t streams)
{
    int32_t cnt = (int32_t)(n & 0xff);
    int ok;
    do {
        ok = 1;
        for (int32_t i = 0; i < cnt && ok; i++) {
            uint32_t rs = RS(eng);
            uint32_t s = rd8(c, streams + (uint32_t)i);
            if (!(rd8(c, rd32(c, rs + RS_POS_MARK) + 4 * (rd32(c, rs + RS_BACK) + s)) & 1)) {
                ok = 0;
                if (!rl_step(c, eng, 0, 1)) return 1;
            }
        }
    } while (!ok);
    rl_push_next(c, eng, label);
    rl_push_pos(c, eng);
    for (int32_t i = 0; i < cnt; i++) {
        uint32_t s = rd8(c, streams + (uint32_t)i);
        wr8(c, rd32(c, eng + ENG_SEEN) + rd8(c, rd32(c, eng + ENG_SLOT) + s), 1);
    }
    return 0;
}

/* FUN_10134b60: move the cursor (without crossing tokens) to a boundary of stream s, push a choice point
 * for `label` and the cursor, flag s as matched and make it the cursor's stream. 0, or 1 at the end. */
uint32_t rl_align1(cpu *c, uint32_t eng, uint32_t label, uint32_t s)
{
    uint32_t si = s & 0xff;
    for (;;) {
        uint32_t rs = RS(eng);
        if (rd8(c, rd32(c, rs + RS_POS_MARK) + 4 * (rd32(c, rs + RS_BACK) + si)) & 1) break;
        if (!rl_step(c, eng, 0, 1)) return 1;
    }
    rl_push_next(c, eng, label);
    rl_push_pos(c, eng);
    wr8(c, rd32(c, eng + ENG_SEEN) + rd8(c, rd32(c, eng + ENG_SLOT) + si), 1);
    wr8(c, RS(eng) + RS_POS_STREAM, (uint8_t)s);
    return 0;
}

/* FUN_10133300: val := the cursor's mark (trailed if trailing is on); push a choice point for `label`
 * and the cursor */
void rl_mark_here_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t label, uint32_t val)
{
    if (rd8(c, RS(eng) + RS_TRAIL)) rl_trail_at(c, AT(sp - 8, 2), eng, val);
    wr32(c, val + 2, rd32(c, RS(eng) + RS_POS_MARK));
    rl_push_next(c, eng, label);
    rl_push_pos(c, eng);
}

/* -------------------------------------------------------------------------------- the value core
 * refs (address, type, flag) read, written, compared, stacked (FUN_10138510 .. FUN_10138c60) */

/* bytes a value of a type takes: tokens their stream's token size */
static uint32_t type_size(cpu *c, int16_t t)
{
    switch (t) {
    case T_SYNC: case T_INT: case T_SYM16: return 4;
    case T_DOUBLE: return 8;
    case T_SHORT: return 2;
    case T_SYM8: return 1;
    default: return rd32(c, stream_desc((uint32_t)(int32_t)t) + SD_TOKEN_SIZE);
    }
}

/* FUN_10138510: trail the value a ref points at: an undo entry (type 2) with its address, type, size
 * and content (the size rounded up to even) goes on the control stack. Returns the original's eax, the
 * rounded size. */
uint32_t rl_trail_ref(cpu *c, uint32_t eng, uint32_t ref)
{
    uint32_t n = type_size(c, (int16_t)rd16(c, ref + 4));
    uint32_t m = (n - 1) | 1;
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_TOP, rd32(c, ws + WS_TOP) + (0xffffffffu - rd32(c, ws + WS_SZ_UNDO) - m));
    ws = WS(eng);
    uint32_t delta = 0xffffffffu - rd32(c, ws + WS_SZ_UNDO) - m;
    uint32_t top = rd32(c, ws + WS_TOP);
    wr32(c, ws + WS_TOP_OFF, rd32(c, ws + WS_TOP_OFF) + delta);
    wr8(c, top, CS_UNDO);
    uint16_t t = rd16(c, ref + 4);
    wr32(c, top + 7, n);
    wr16(c, top + 1, t);
    wr32(c, top + 3, rd32(c, ref));
    copy(c, top + rd32(c, WS(eng) + WS_SZ_UNDO), rd32(c, ref), m + 1);
    return m + 1;
}

/* the "undefined" markers of the conversions: int 0x80000001, short 0x8001, double 2^-1022 */
#define UNDEF_INT   0x80000001u
#define UNDEF_SHORT 0x8001u
#define UNDEF_DHI   0x00100000u

static double load_double(cpu *c, uint32_t a)
{
    uint64_t b = rd64(c, a);
    double v;
    memcpy(&v, &b, 8);
    return v;
}

static uint32_t ftol_double(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    return ftol32(bits);
}

static void store_undef_double(cpu *c, uint32_t d)
{
    wr32(c, d, 0);
    wr32(c, d + 4, UNDEF_DHI);
}

/* FUN_10138730: *dst := *src, converting between int, short and double (keeping the undefined marker);
 * other types are copied as they are. */
void rl_assign(cpu *c, uint32_t eng, uint32_t dst, uint32_t src)
{
    (void)eng;
    int16_t td = (int16_t)rd16(c, dst + 4);
    switch (td) {
    case T_SYNC:
        wr32(c, rd32(c, dst), rd32(c, rd32(c, src)));
        return;
    case T_DOUBLE: {
        int16_t ts = (int16_t)rd16(c, src + 4);
        if (ts == T_INT) {
            uint32_t v = rd32(c, rd32(c, src));
            if (v == UNDEF_INT) store_undef_double(c, rd32(c, dst));
            else store_double(c, rd32(c, dst), (double)(int32_t)v);
        } else if (ts == T_SHORT) {
            uint16_t v = rd16(c, rd32(c, src));
            if (v == UNDEF_SHORT) store_undef_double(c, rd32(c, dst));
            else store_double(c, rd32(c, dst), (double)(int16_t)v);
        } else if (ts == T_DOUBLE) {
            uint32_t a = rd32(c, src), d = rd32(c, dst);
            wr32(c, d, rd32(c, a));
            wr32(c, d + 4, rd32(c, a + 4));
        }
        return;
    }
    case T_SHORT: {
        int16_t ts = (int16_t)rd16(c, src + 4);
        if (ts == T_DOUBLE) {
            uint32_t a = rd32(c, src);
            if (rd32(c, a) == 0 && rd32(c, a + 4) == UNDEF_DHI) wr16(c, rd32(c, dst), UNDEF_SHORT);
            else wr16(c, rd32(c, dst), (uint16_t)ftol32(rd64(c, a)));
        } else if (ts == T_SHORT || ts == T_INT) {
            wr16(c, rd32(c, dst), rd16(c, rd32(c, src)));
        }
        return;
    }
    case T_INT: {
        int16_t ts = (int16_t)rd16(c, src + 4);
        if (ts == T_DOUBLE) {
            uint32_t a = rd32(c, src);
            if (rd32(c, a) == 0 && rd32(c, a + 4) == UNDEF_DHI) wr32(c, rd32(c, dst), UNDEF_INT);
            else wr32(c, rd32(c, dst), ftol32(rd64(c, a)));
        } else if (ts == T_SHORT) {
            wr32(c, rd32(c, dst), (uint32_t)(int32_t)(int16_t)rd16(c, rd32(c, src)));
        } else if (ts == T_INT) {
            wr32(c, rd32(c, dst), rd32(c, rd32(c, src)));
        }
        return;
    }
    case T_SYM16:
        wr16(c, rd32(c, dst), rd16(c, rd32(c, src)));
        return;
    case T_SYM8:
        wr8(c, rd32(c, dst), rd8(c, rd32(c, src)));
        return;
    default:
        copy(c, rd32(c, dst), rd32(c, src), rd32(c, stream_desc((uint32_t)(int32_t)td) + SD_TOKEN_SIZE));
        return;
    }
}

/* FUN_101359b0: a sync mark as compared: the address without flags, -1 for null */
uint32_t rl_mark_key(uint32_t m) { return m ? m & ~3u : 0xffffffffu; }

static void set_cmp(cpu *c, uint32_t eng, int v) { wr8(c, RS(eng) + RS_CMP, (uint8_t)v); }
static int cmp3(int64_t x, int64_t y) { return x < y ? -1 : x == y ? 0 : 1; }

/* FUN_10138920: compare *a with *b into RS_CMP (-1, 0, 1). Numbers compare across int, short and double
 * as the original does (a double only with a double or an int; NaN as the x87 compare leaves it); tokens
 * of the same stream byte by byte; other type pairs leave RS_CMP alone (numbers) or make it 1. */
void rl_compare(cpu *c, uint32_t eng, uint32_t a, uint32_t b)
{
    int16_t ta = (int16_t)rd16(c, a + 4);
    switch (ta) {
    case T_SYM8:
        set_cmp(c, eng, cmp3(rd8(c, rd32(c, a)), rd8(c, rd32(c, b))));
        return;
    case T_SYM16:
        set_cmp(c, eng, cmp3((int16_t)rd16(c, rd32(c, a)), (int16_t)rd16(c, rd32(c, b))));
        return;
    case T_INT: {
        int16_t tb = (int16_t)rd16(c, b + 4);
        if (tb == T_SHORT) {
            int32_t y = (int16_t)rd16(c, rd32(c, b));
            set_cmp(c, eng, cmp3((int32_t)rd32(c, rd32(c, a)), y));
        } else if (tb == T_INT) {
            set_cmp(c, eng, cmp3((int32_t)rd32(c, rd32(c, a)), (int32_t)rd32(c, rd32(c, b))));
        }
        return;
    }
    case T_SHORT: {
        int16_t tb = (int16_t)rd16(c, b + 4);
        if (tb == T_SHORT)
            set_cmp(c, eng, cmp3((int16_t)rd16(c, rd32(c, a)), (int16_t)rd16(c, rd32(c, b))));
        else if (tb == T_INT)
            set_cmp(c, eng, cmp3((int16_t)rd16(c, rd32(c, a)), (int32_t)rd32(c, rd32(c, b))));
        return;
    }
    case T_DOUBLE: {
        int16_t tb = (int16_t)rd16(c, b + 4);
        if (tb == T_DOUBLE) {
            double x = load_double(c, rd32(c, a)), y = load_double(c, rd32(c, b));
            /* fcomp: C0 (below, or unordered) -> -1, else C3 (equal) -> 0, else 1 */
            if (x < y || x != x || y != y) set_cmp(c, eng, -1);
            else set_cmp(c, eng, x == y ? 0 : 1);
        } else if (tb == T_INT) {
            double y = (double)(int32_t)rd32(c, rd32(c, b));
            double x = load_double(c, rd32(c, a));
            /* the int compared with the double: neither C0 nor C3 (int above) -> -1; C3 (equal, or
             * unordered) -> 0; else 1 */
            if (y > x) set_cmp(c, eng, -1);
            else set_cmp(c, eng, (y == x || x != x) ? 0 : 1);
        }
        return;
    }
    case T_SYNC:
        set_cmp(c, eng, rl_mark_key(rd32(c, rd32(c, a))) == rl_mark_key(rd32(c, rd32(c, b))) ? 0 : 1);
        return;
    default: {
        if ((int16_t)rd16(c, b + 4) != ta) {
            set_cmp(c, eng, 1);
            return;
        }
        uint32_t n = rd32(c, stream_desc((uint32_t)(int32_t)ta) + SD_TOKEN_SIZE);
        uint32_t pa = rd32(c, a), pb = rd32(c, b);
        int r = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t x = rd8(c, pa + i), y = rd8(c, pb + i);
            if (x != y) {
                r = x < y ? -1 : 1;
                break;
            }
        }
        set_cmp(c, eng, r);
        return;
    }
    }
}

/* FUN_101386e0: is the number negative (a double below 0, or NaN) */
int rl_is_negative(cpu *c, uint32_t ref)
{
    int16_t t = (int16_t)rd16(c, ref + 4);
    if (t == T_DOUBLE) {
        double x = load_double(c, rd32(c, ref));
        return x < 0.0 || x != x;
    }
    if (t == T_SHORT) return (int16_t)rd16(c, rd32(c, ref)) < 0;
    if (t == T_INT) return (int32_t)rd32(c, rd32(c, ref)) < 0;
    return 0;
}

/* FUN_101385f0: *dst += *src for int, short and double (an int or a short plus a double is truncated by
 * _ftol). `argslot`: where the original keeps a short converted to int (its src argument slot; 0: not
 * written, when the caller is a port). */
void rl_add(cpu *c, uint32_t eng, uint32_t dst, uint32_t src, uint32_t argslot)
{
    (void)eng;
    int16_t td = (int16_t)rd16(c, dst + 4);
    if (td == T_INT) {
        int16_t ts = (int16_t)rd16(c, src + 4);
        if (ts == T_INT) {
            uint32_t s = rd32(c, rd32(c, src)), d = rd32(c, dst);
            wr32(c, d, rd32(c, d) + s);
        } else if (ts == T_SHORT) {
            uint32_t s = (uint32_t)(int32_t)(int16_t)rd16(c, rd32(c, src)), d = rd32(c, dst);
            wr32(c, d, rd32(c, d) + s);
        } else if (ts == T_DOUBLE) {
            uint32_t d = rd32(c, dst);
            wr32(c, d, ftol_double((double)(int32_t)rd32(c, d) + load_double(c, rd32(c, src))));
        }
    } else if (td == T_SHORT) {
        int16_t ts = (int16_t)rd16(c, src + 4);
        if (ts == T_SHORT || ts == T_INT) {
            uint16_t s = rd16(c, rd32(c, src));
            uint32_t d = rd32(c, dst);
            wr16(c, d, (uint16_t)(rd16(c, d) + s));
        } else if (ts == T_DOUBLE) {
            uint32_t d = rd32(c, dst);
            int32_t x = (int16_t)rd16(c, d);
            if (argslot) wr32(c, argslot, (uint32_t)x);
            wr16(c, d, (uint16_t)ftol_double((double)x + load_double(c, rd32(c, src))));
        }
    } else if (td == T_DOUBLE) {
        int16_t ts = (int16_t)rd16(c, src + 4);
        uint32_t d;
        if (ts == T_INT) {
            uint32_t s = rd32(c, src);
            d = rd32(c, dst);
            store_double(c, d, (double)(int32_t)rd32(c, s) + load_double(c, d));
        } else if (ts == T_SHORT) {
            uint32_t s = rd32(c, src);
            d = rd32(c, dst);
            int32_t x = (int16_t)rd16(c, s);
            if (argslot) wr32(c, argslot, (uint32_t)x);
            store_double(c, d, (double)x + load_double(c, d));
        } else if (ts == T_DOUBLE) {
            uint32_t s = rd32(c, src);
            d = rd32(c, dst);
            store_double(c, d, load_double(c, s) + load_double(c, d));
        }
    }
}

/* FUN_10138b80: push the value a ref points at on the value stack */
void rl_push(cpu *c, uint32_t eng, uint32_t ref)
{
    uint32_t ws = WS(eng);
    wr8(c, ws + WS_EVAL_TOP, (uint8_t)(rd8(c, ws + WS_EVAL_TOP) + 1));
    ws = WS(eng);
    uint32_t cell = rd32(c, ws + WS_EVAL) + 10u * (uint32_t)(int32_t)(int8_t)rd8(c, ws + WS_EVAL_TOP);
    int16_t t = (int16_t)rd16(c, ref + 4);
    wr16(c, cell + 8, (uint16_t)t);
    switch (t) {
    case T_SYM8: wr8(c, cell, rd8(c, rd32(c, ref))); break;
    case T_INT: wr32(c, cell, rd32(c, rd32(c, ref))); break;
    case T_SHORT: case T_SYM16: wr16(c, cell, rd16(c, rd32(c, ref))); break;
    case T_DOUBLE: {
        uint32_t p = rd32(c, ref);
        wr32(c, cell, rd32(c, p));
        wr32(c, cell + 4, rd32(c, p + 4));
        break;
    }
    default: break;
    }
}

/* FUN_10138c60: pop the value stack into a ref (pointing at the cell, valid until the next push).
 * Returns the original's eax, the ref. */
uint32_t rl_pop(cpu *c, uint32_t eng, uint32_t ref)
{
    uint32_t ws = WS(eng);
    uint32_t cell = rd32(c, ws + WS_EVAL) + 10u * (uint32_t)(int32_t)(int8_t)rd8(c, ws + WS_EVAL_TOP);
    uint16_t t = rd16(c, cell + 8);
    wr8(c, ref + 6, 0);
    wr16(c, ref + 4, t);
    if ((int16_t)t >= T_DOUBLE && (int16_t)t <= T_SYM8) {
        ws = WS(eng);
        wr32(c, ref, rd32(c, ws + WS_EVAL) + 10u * (uint32_t)(int32_t)(int8_t)rd8(c, ws + WS_EVAL_TOP));
    }
    ws = WS(eng);
    wr8(c, ws + WS_EVAL_TOP, (uint8_t)(rd8(c, ws + WS_EVAL_TOP) - 1));
    return ref;
}
