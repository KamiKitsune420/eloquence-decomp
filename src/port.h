/* port - what the hand ports share: how an adapter stands in for a recompiled engine function.
 *
 * tools/x2c.py --replace (the list is src/ported.h) renames the recompiled function to f_XXXXXXXX_recomp;
 * the hand port defines the adapter with PORT_FN(XXXXXXXX), which is f_XXXXXXXX in the engine builds and
 * port_XXXXXXXX in the differential tester's build (src/difftest.c, -DDIFFTEST), where f_XXXXXXXX runs
 * both and compares them.
 *
 * An adapter is entered as the guest's `call` left it: return address at [esp], cdecl arguments above
 * (ARG(0) is the first). It returns with the original's calling convention: result in eax, esp past the
 * return address (and, for stdcall/thiscall, past the arguments the original's `ret N` pops), ebx, esi,
 * edi, ebp as they were.
 */
#ifndef PORT_H
#define PORT_H

#include "x86rt.h"

#ifdef DIFFTEST
#define PORT_FN(a) void port_##a(cpu *c)
#else
#define PORT_FN(a) void f_##a(cpu *c)
#endif

/* the n-th 32-bit argument of a function entered by a guest call */
#define ARG(n) rd32(c, c->esp + 4u + 4u * (uint32_t)(n))

/* return from an adapter: eax, and `pop` bytes of arguments besides the return address */
static inline void port_ret(cpu *c, uint32_t eax, uint32_t pop)
{
    c->eax = eax;
    c->esp += 4u + pop;
}

/* call a guest function (recompiled, or another port) the way the original does: cdecl arguments pushed
 * last to first below the current esp, a return address, the call, the arguments removed again.
 * `ret` is the original's return address (only the callee's scratch can see it). Returns eax. */
static inline uint32_t guest_call(cpu *c, guest_fn f, uint32_t ret, int nargs, const uint32_t *args)
{
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, ret);
    f(c);
    c->esp += 4u * (uint32_t)nargs;
    return c->eax;
}

#endif
