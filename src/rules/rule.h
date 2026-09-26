/* rule - what the hand-written Delta rules (src/rules/*.c) are written against.
 *
 * A rule is a function the engine calls with one argument, the engine instance, and that returns 0 (done)
 * or 0x5e (failed). It is a pattern with alternatives over the delta (see src/rules.h): it enters a rule
 * frame, sets the streams it looks at, and then runs its alternatives as labelled steps; the rule runtime
 * keeps the choice points, and after each step `rule_next` says which label to go on at (by backtracking
 * if the step did not say).
 *
 * The runtime keeps pointers into the rule's frame (its frame record, the variables it registers, its
 * jmp_buf), so the frame is in guest memory where the original has it: `fp` is the original's frame
 * pointer (the entry esp - 4) and the rule's locals are at fp - k; `sp` is the esp of its body, below
 * them. Calls go to the runtime's C functions directly; they get the esp of a call from the body (the
 * arguments are written where a cdecl call has them), so whatever they keep on the stack is below the
 * rule's frame. Calls to rules or engine functions not written by hand yet go through the machine the
 * same way (rule_call).
 *
 * The contract (notes/port.md, "The whole engine hand-written"): the rule does what the original does to
 * the engine's memory and returns what it returns; the stack below its entry is its own business (the
 * tester leaves out what depends on uninitialized bytes: DIFFTEST_UNINIT=1).
 */
#ifndef RULE_H
#define RULE_H

#include <setjmp.h>

#include "../x86rt.h"
#include "../port.h"
#include "../rules.h"

/* a hand-written rule in the place of the engine's function at `a` (listed in src/rules/hand.h) */
#define RULE_FN(a) void PORT_FN_NAME(a)(cpu *c)

/* what a rule returns */
enum { RULE_DONE = 0, RULE_FAILED = 0x5e };

typedef struct rule {
    cpu *c;
    uint32_t eng;
    uint32_t fp;                        /* the original's frame pointer: locals at fp - k */
    uint32_t sp;                        /* the body's esp */
    uint32_t ebx, esi, edi, ebp;        /* the caller's registers, as the rule returns them */
} rule;

/* The rule's frame: `frame_bytes` below fp (the original's locals and saved registers) */
static inline rule rule_open(cpu *c, uint32_t frame_bytes)
{
    rule r;
    r.c = c;
    r.fp = c->esp - 4;
    r.eng = rd32(c, c->esp + 4);
    r.sp = r.fp - frame_bytes;
    r.ebx = c->ebx;
    r.esi = c->esi;
    r.edi = c->edi;
    r.ebp = c->ebp;
    c->esp = r.sp;
    return r;
}

/* return v to the caller (eax), with its registers and esp as a cdecl return leaves them */
static inline void rule_return(rule *r, uint32_t v)
{
    cpu *c = r->c;
    c->ebx = r->ebx;
    c->esi = r->esi;
    c->edi = r->edi;
    c->ebp = r->ebp;
    c->eax = v;
    c->esp = r->fp + 8;
}

/* The catch point for the runtime's longjmp (a failing rule aborts to the innermost rule's jmp_buf):
 * `if (RULE_CATCH(r, jb)) ...` - 0 when set, nonzero when a longjmp arrived (locals changed since then
 * are indeterminate there, as with any setjmp). The guest jmp_buf gets what _setjmp3 keeps (the longjmp
 * restores the machine's registers and esp from it). */
static inline jmp_buf *rule_arm(rule *r, uint32_t jb)
{
    cpu *c = r->c;
    wr32(c, jb + 0, r->fp);
    wr32(c, jb + 4, r->ebx);
    wr32(c, jb + 8, r->edi);
    wr32(c, jb + 12, r->esi);
    wr32(c, jb + 16, r->sp);
    wr32(c, jb + 20, 0);
    wr32(c, jb + 24, rd32(c, c->fs_base));
    return x86_jmpbuf(c, jb);
}
#define RULE_CATCH(r, jb) setjmp(*rule_arm(&(r), (jb)))

/* ------------------------------------------------------------------ calls */

/* the esp of a call with n cdecl arguments from the body (the arguments written where they go, the
 * return address slot below them); the machine's esp is set to it for the call */
static inline uint32_t rule_call_sp(rule *r, int n, const uint32_t *a)
{
    uint32_t sp = r->sp - 4u * (uint32_t)n;
    for (int i = 0; i < n; i++) wr32(r->c, sp + 4u * (uint32_t)i, a[i]);
    r->c->esp = sp - 4;
    return sp - 4;
}
static inline void rule_call_end(rule *r) { r->c->esp = r->sp; }

/* a guest function (a rule or engine function not written by hand yet) called through the machine */
static inline uint32_t rule_call(rule *r, guest_fn f, int n, const uint32_t *a)
{
    rule_call_sp(r, n, a);
    wr32(r->c, r->c->esp, 0);           /* the return address slot */
    f(r->c);
    rule_call_end(r);
    return r->c->eax;
}

/* ------------------------------------------------------------------ a rule */

/* Where a rule keeps what the runtime points into, below its frame pointer (the original's layout, which
 * depends on how many variables the rule has): its frame record, jmp_buf, and the seen/scope/slot byte
 * arrays; and how far its frame reaches (locals and saved registers: the body's esp is fp - bytes). */
typedef struct rule_layout {
    uint16_t bytes, record, jb, seen, scope, slots;
} rule_layout;

/* Run a rule: enter (a failure to enter, or the runtime aborting it with longjmp, fails it), run the body,
 * leave, and return the body's result to the caller. */
void rule_run(cpu *c, const rule_layout *L, uint32_t (*body)(rule *r));

/* RULE(address, bytes, record, jb, seen, scope, slots) { body }: the rule's body, a function of `rule *r`
 * returning RULE_DONE or RULE_FAILED */
#define RULE(a, ...)                                                                                    \
    static uint32_t rule_body_##a(rule *r);                                                             \
    RULE_FN(a)                                                                                          \
    {                                                                                                   \
        static const rule_layout layout = { __VA_ARGS__ };                                              \
        rule_run(c, &layout, rule_body_##a);                                                            \
    }                                                                                                   \
    static uint32_t rule_body_##a(rule *r)

/* ------------------------------------------------------------------ the rules' global variables */

/* The rule language's global variables live in the engine instance; a short one at offset `off`. (Named
 * by offset until what they hold is known.) */
static inline int16_t rule_global16(const rule *r, uint32_t off) { return (int16_t)rd16(r->c, r->eng + off); }

/* Another rule (or engine function) with the instance as its argument, called with the machine's esp at
 * fp - at: where the original's is at that call. (The original leaves earlier calls' arguments on the
 * stack, so its callees' frames are that much deeper; a rule's frame addresses stay in the rule state -
 * its registered variables - so they have to be the original's.) */
static inline uint32_t rule_sub(rule *r, uint32_t at, guest_fn f)
{
    cpu *c = r->c;
    const uint32_t sp = r->fp - at;
    wr32(c, sp, r->eng);
    wr32(c, sp - 4, 0);                 /* the return address slot */
    c->esp = sp - 4;
    f(c);
    rule_call_end(r);
    return c->eax;
}

/* the same with n arguments (the instance first) */
static inline uint32_t rule_subn(rule *r, uint32_t at, guest_fn f, int n, const uint32_t *a)
{
    cpu *c = r->c;
    const uint32_t sp = r->fp - at;
    for (int i = 0; i < n; i++) wr32(c, sp + 4u * (uint32_t)i, a[i]);
    wr32(c, sp - 4, 0);
    c->esp = sp - 4;
    f(c);
    rule_call_end(r);
    return c->eax;
}

/* ------------------------------------------------------------------ arguments and variables */

/* the rule's argument i after the instance (rules with parameters get pointers to their caller's values) */
static inline uint32_t rule_arg(const rule *r, int i) { return rd32(r->c, r->fp + 0xc + 4u * (uint32_t)i); }

/* a value's sync mark (a value: short type, then the mark pointer) */
static inline uint32_t rule_mark(const rule *r, uint32_t val) { return rd32(r->c, val + 2); }

/* hand back a result: the caller's value `out` gets the mark of the rule's variable `var` */
static inline void rule_out(rule *r, uint32_t out, uint32_t var) { wr32(r->c, out + 2, rule_mark(r, var)); }

/* ------------------------------------------------------------------ the runtime, by name */

uint32_t rl_leave_at(cpu *c, uint32_t sp, uint32_t eng);
int32_t  rl_backtrack_sp(cpu *c, uint32_t sp, uint32_t eng, int32_t depth);
uint32_t rl_match_string_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t len, uint32_t str);
uint32_t rl_goto_val_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t v, uint32_t s, int back, uint32_t ret);
void     rl_set_ab(cpu *c, uint32_t eng, uint32_t a, uint32_t b);
uint32_t rl_advance_to_a_at(cpu *c, uint32_t sp, uint32_t eng);
void     rl_mark_here_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t label, uint32_t val);
void     rl_succeed(cpu *c, uint32_t eng, uint32_t label);
uint32_t rl_insert_list(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t n, uint32_t p, uint32_t k,
                        guest_fn insert, uint32_t base);
void     f_10132730(cpu *c);

/* Enter the rule: its frame record, the slot/scope/seen byte arrays and its jmp_buf (all in its frame).
 * Nonzero if the rule is not to run (aborting). */
static inline int rule_enter(rule *r, uint32_t frame, uint32_t slots, uint32_t scope, uint32_t seen, uint32_t jb)
{
    const uint32_t a[6] = { r->eng, frame, slots, scope, seen, jb };
    rule_call_sp(r, 6, a);
    int v = rl_enter(r->c, r->eng, frame, slots, scope, seen, jb);
    rule_call_end(r);
    return v;
}

/* Leave it: the control stack and the rule state back as they were at rule_enter */
static inline void rule_leave(rule *r)
{
    const uint32_t a[1] = { r->eng };
    uint32_t sp = rule_call_sp(r, 1, a);
    rl_leave_at(r->c, sp, r->eng);
    rule_call_end(r);
}

/* the streams the rule looks at: n stream numbers (bytes) at `streams` */
static inline void rule_scope(rule *r, uint8_t n, uint32_t streams)
{
    rl_set_scope(r->c, r->eng, n, streams);
}

/* the rule has matched: its alternatives are done with, go on at `label` if it is retried */
static inline void rule_succeed(rule *r, uint32_t label)
{
    rl_succeed(r->c, r->eng, label);
}

/* Which label to go on at: the one a step set (ENG_RESULT), else the next choice point's (backtracking
 * `depth` levels of marks) */
static inline uint32_t rule_next(rule *r, int32_t depth)
{
    cpu *c = r->c;
    uint32_t label = rd32(c, r->eng + ENG_RESULT);
    if (label) {
        wr32(c, r->eng + ENG_RESULT, 0);
        return label;
    }
    const uint32_t a[2] = { r->eng, (uint32_t)depth };
    uint32_t sp = rule_call_sp(r, 2, a);
    label = (uint32_t)rl_backtrack_sp(c, sp, r->eng, depth);
    rule_call_end(r);
    return label;
}

/* Move the cursor back to the sync mark in variable `var` (a value: short type, pointer) in stream s.
 * 0 if it got there. */
static inline uint32_t rule_back_to(rule *r, uint32_t var, uint8_t s)
{
    const uint32_t a[3] = { r->eng, var, s };
    uint32_t sp = rule_call_sp(r, 3, a);
    uint32_t v = rl_goto_val_at(r->c, sp, r->eng, var, s, 1, 0);
    rule_call_end(r);
    return v;
}

/* Match `len` symbols (bytes at `str`) against field 0 of stream s from the cursor on. 0 if they match. */
static inline uint32_t rule_match(rule *r, uint8_t s, uint8_t len, uint32_t str)
{
    const uint32_t a[4] = { r->eng, s, len, str };
    uint32_t sp = rule_call_sp(r, 4, a);
    uint32_t v = rl_match_string_at(r->c, sp, r->eng, s, len, str);
    rule_call_end(r);
    return v;
}

/* var := the cursor's sync mark; a choice point for `label` */
static inline void rule_mark_here(rule *r, uint32_t label, uint32_t var)
{
    const uint32_t a[3] = { r->eng, label, var };
    uint32_t sp = rule_call_sp(r, 3, a);
    rl_mark_here_at(r->c, sp, r->eng, label, var);
    rule_call_end(r);
}

/* sync variable A := the sync mark in variable `var` */
static inline void rule_set_a(rule *r, uint32_t var)
{
    cpu *c = r->c;
    wr8(c, r->eng + ENG_SYNC_A + SV_STATE, 1);
    wr32(c, r->eng + ENG_SYNC_A + SV_MARK, rd32(c, var + 2));
    wr32(c, r->eng + ENG_SYNC_A + SV_OFFSET, 0);
}

/* Move the cursor forward to sync variable A. 0 if it got there. */
static inline uint32_t rule_advance_to_a(rule *r)
{
    const uint32_t a[1] = { r->eng };
    uint32_t sp = rule_call_sp(r, 1, a);
    uint32_t v = rl_advance_to_a_at(r->c, sp, r->eng);
    rule_call_end(r);
    return v;
}

/* A := the mark in variable a, B := the one in b */
static inline void rule_set_ab(rule *r, uint32_t a, uint32_t b)
{
    rl_set_ab(r->c, r->eng, a, b);
}

/* Put n tokens between A and B in stream s, their field 0 the symbols (bytes, one per int at `list`).
 * 0 if done. */
static inline uint32_t rule_insert(rule *r, uint8_t s, uint8_t n, uint32_t list)
{
    const uint32_t a[5] = { r->eng, s, n, list, 0 };
    uint32_t sp = rule_call_sp(r, 5, a);
    uint32_t v = rl_insert_list(r->c, sp, r->eng, s, n, list, 0, f_10132730, 0x10132a20u);
    rule_call_end(r);
    return v;
}

int rl_var_init_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t var, uint32_t src, int16_t type);

/* A variable of the rule (in its frame, at `var`): a copy of the value at `src` (the caller's, for a
 * parameter), of the given type, registered with the runtime. */
static inline void rule_var(rule *r, uint32_t var, uint32_t src, int16_t type)
{
    const uint32_t a[4] = { r->eng, var, src, (uint32_t)(int32_t)type };
    uint32_t sp = rule_call_sp(r, 4, a);
    rl_var_init_at(r->c, sp, r->eng, var, src, type);
    rule_call_end(r);
}

/* a depth mark on the control stack (rule_next's depth counts them) */
static inline void rule_push_down(rule *r) { rl_push_mark(r->c, r->eng, CS_DOWN); }

/* a choice point: backtracking comes back with `label` */
static inline void rule_push_retry(rule *r, uint32_t label) { rl_push_retry(r->c, r->eng, label); }

/* the cursor, restored when backtracking over it */
static inline void rule_push_pos(rule *r) { rl_push_pos(r->c, r->eng); }

/* drop the control stack's top entry */
static inline void rule_drop_top(rule *r) { rl_drop_top(r->c, r->eng); }

#endif
