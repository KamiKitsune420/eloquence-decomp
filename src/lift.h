/* lift - what the lifted rule functions (tools/delta_lift.py, output in src/gen/rules_lifted_*.c) are
 * written against.
 *
 * A lifted rule is plain C that calls the rule runtime (src/rules*.c) and other rules by name. It keeps
 * the machine's calling convention at its boundary (entered by a guest `call`: return address at [esp],
 * cdecl arguments above; returns with the result in eax and esp past the return address) and it keeps its
 * stack frame where the original has it, in guest memory: the runtime holds pointers into it (the rule's
 * frame record, its variables, its jmp_buf), so every local lives at the original's address and the
 * frame's bytes are what the original's are.
 *
 *   fp               the frame pointer: the original's ebp (entry esp - 4); locals are fp - k
 *   S(off, ret)      a call site: the arguments go to fp - off (the original's esp at the call), the return
 *                    address `ret` (the original's) below them
 *   lcN(S(..), f, a0, .. aN-1)
 *                    call the guest function f (a hand port, a lifted rule, a recompiled function) with N
 *                    cdecl arguments written where the original pushes them; returns eax
 *   GUEST_SETJMP(fp - off, ret, buf, arg)
 *                    the original's _setjmp3(buf, arg): the host setjmp is taken in this function's frame
 *                    (a longjmp from the runtime comes back here, with c->eax the value), as the
 *                    recompiled code does
 *   LIFT_RETURN(v)   return v in eax
 *
 * The callee-saved registers (ebx esi edi ebp) are not touched: the lifted code keeps what the original
 * keeps in them in C variables, so the machine's registers still hold the caller's values when the rule
 * returns - which is what the original's epilogue restores.
 */
#ifndef LIFT_H
#define LIFT_H

#include <setjmp.h>

#include "x86rt.h"
#include "port.h"

#define LIFTED_FN(a) void PORT_FN_NAME(a)(cpu *c)   /* a lifted rule writes its own prologue */

#define S(off, ret) c, fp - (uint32_t)(off), (uint32_t)(ret)

static inline uint32_t lift_call(cpu *c, uint32_t sp, uint32_t ret, guest_fn f)
{
    wr32(c, sp - 4, ret);
    c->esp = sp - 4;
    f(c);
    return c->eax;
}

static inline uint32_t lc0(cpu *c, uint32_t sp, uint32_t ret, guest_fn f) { return lift_call(c, sp, ret, f); }
static inline uint32_t lc1(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0)
{
    wr32(c, sp, a0);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc2(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc3(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc4(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2); wr32(c, sp + 12, a3);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc5(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2); wr32(c, sp + 12, a3); wr32(c, sp + 16, a4);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc6(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4, uint32_t a5)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2); wr32(c, sp + 12, a3); wr32(c, sp + 16, a4);
    wr32(c, sp + 20, a5);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc7(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2); wr32(c, sp + 12, a3); wr32(c, sp + 16, a4);
    wr32(c, sp + 20, a5); wr32(c, sp + 24, a6);
    return lift_call(c, sp, ret, f);
}
static inline uint32_t lc8(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
{
    wr32(c, sp, a0); wr32(c, sp + 4, a1); wr32(c, sp + 8, a2); wr32(c, sp + 12, a3); wr32(c, sp + 16, a4);
    wr32(c, sp + 20, a5); wr32(c, sp + 24, a6); wr32(c, sp + 28, a7);
    return lift_call(c, sp, ret, f);
}

/* more than 8 arguments */
static inline uint32_t lcn(cpu *c, uint32_t sp, uint32_t ret, guest_fn f, int n, const uint32_t *a)
{
    for (int i = 0; i < n; i++) wr32(c, sp + 4u * (uint32_t)i, a[i]);
    return lift_call(c, sp, ret, f);
}

/* the arguments an imported function gets are on the guest stack too */
void imp__setjmp3(cpu *c);

#define GUEST_SETJMP(sp, ret, buf, arg)                                                                 \
    do {                                                                                                \
        wr32(c, (sp), (buf));                                                                           \
        wr32(c, (sp) + 4, (arg));                                                                       \
        wr32(c, (sp) - 4, (ret));                                                                       \
        c->esp = (sp) - 4;                                                                              \
        if (!setjmp(*x86_jmpbuf(c, (buf)))) imp__setjmp3(c);                                            \
    } while (0)

#define LIFT_RETURN(v)                                                                                  \
    do {                                                                                                \
        c->eax = (v);                                                                                   \
        c->esp = fp + 8;                                                                                \
        return;                                                                                         \
    } while (0)

#endif
