/* tracks - the parameter tracks' queues (ENU.SYN FUN_10130ac0 .. FUN_10130cb0): the side of the ring
 * buffers that framer.c reads (framer_queue_pop / _shrink / _empty), hand-ported: create, reset, free,
 * push, grow. See framer.c for the queue layout:
 *
 *   +0 p   entries (32-bit), malloc'd       +4 u16 capacity      +6 u16 head (next read)
 *   +8 u16 tail (next write)                +a u16 initial capacity (never shrunk below)
 *
 * The queues live in the engine's memory and are handled by the C runtime's allocator there, so the
 * allocations go through the machine (the image's imports), at the esp the original has at each call.
 */
#include "port.h"

#define IAT_REALLOC 0x1014409cu
#define IAT_FREE    0x1014410cu
#define IAT_MALLOC  0x10144110u
#define IAT_MEMMOVE 0x10144134u

enum { Q_BUF = 0, Q_CAP = 4, Q_HEAD = 6, Q_TAIL = 8, Q_MIN = 0xa };

/* a cdecl call of an import with esp at `sp` before the arguments are pushed. Returns eax. */
static uint32_t import_at(cpu *c, uint32_t sp, uint32_t iat, uint32_t ret, int n, const uint32_t *args)
{
    uint32_t save = c->esp;
    c->esp = sp;
    for (int i = n - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, ret);
    x86_icall(c, rd32(c, iat));
    c->esp = save;
    return c->eax;
}

/* FUN_10130b00 (thiscall): a queue of `cap` entries. Returns the queue (eax). */
uint32_t track_queue_init(cpu *c, uint32_t sp, uint32_t q, uint16_t cap)
{
    wr16(c, q + Q_CAP, cap);
    wr16(c, q + Q_MIN, cap);
    wr32(c, q + Q_BUF, 0);
    wr16(c, q + Q_HEAD, 0);
    wr16(c, q + Q_TAIL, 0);
    uint32_t n = (uint32_t)cap << 2;
    uint32_t p = import_at(c, sp - 4, IAT_MALLOC, 0x10130b30u, 1, &n);
    wr32(c, q + Q_BUF, p);
    if (!p) wr16(c, q + Q_CAP, 0);
    return q;
}

/* FUN_10130b50 (fastcall): free the entries. Returns eax as the original (free's, or 0). */
uint32_t track_queue_free(cpu *c, uint32_t sp, uint32_t q)
{
    uint32_t p = rd32(c, q + Q_BUF);
    if (!p) return 0;
    uint32_t r = import_at(c, sp - 4, IAT_FREE, 0x10130b60u, 1, &p);
    wr32(c, q + Q_BUF, 0);
    wr16(c, q + Q_CAP, 0);
    wr16(c, q + Q_TAIL, 0);
    wr16(c, q + Q_HEAD, 0);
    return r;
}

/* FUN_10130b80 (fastcall): empty the queue at its initial capacity (new entries). al 1, 0 if out of
 * memory. */
uint32_t track_queue_reset(cpu *c, uint32_t sp, uint32_t q)
{
    uint32_t p = rd32(c, q + Q_BUF);
    if (p) import_at(c, sp - 4, IAT_FREE, 0x10130b90u, 1, &p);
    uint32_t n = (uint32_t)rd16(c, q + Q_MIN) << 2;
    p = import_at(c, sp - 4, IAT_MALLOC, 0x10130ba3u, 1, &n);
    wr32(c, q + Q_BUF, p);
    if (!p) return 0;
    uint16_t cap = rd16(c, q + Q_MIN);
    wr16(c, q + Q_TAIL, 0);
    wr16(c, q + Q_CAP, cap);
    wr16(c, q + Q_HEAD, 0);
    return (p & 0xffffff00u) | 1;
}

/* FUN_10130cb0 (fastcall): double the capacity of a full queue, the entries unrolled to start at 0.
 * al 1, 0 without entries or if realloc fails. */
uint32_t track_queue_grow(cpu *c, uint32_t sp, uint32_t q)
{
    uint32_t p = rd32(c, q + Q_BUF);
    if (!p) return p & 0xffffff00u;
    uint16_t cap2 = (uint16_t)(rd16(c, q + Q_CAP) << 1);
    uint32_t a[3] = { p, (uint32_t)cap2 << 2, 0 };
    uint32_t np = import_at(c, sp - 0x10, IAT_REALLOC, 0x10130cdcu, 2, a);
    /* the tail part after the old capacity, then everything from the head down to 0 */
    a[0] = np + 4u * rd16(c, q + Q_CAP);
    a[1] = np;
    a[2] = (uint32_t)rd16(c, q + Q_TAIL) << 2;
    import_at(c, sp - 0x18, IAT_MEMMOVE, 0x10130cfbu, 3, a);
    a[0] = np;
    a[1] = np + 4u * rd16(c, q + Q_HEAD);
    a[2] = (uint32_t)rd16(c, q + Q_CAP) << 2;
    uint32_t r = import_at(c, sp - 0x24, IAT_MEMMOVE, 0x10130d12u, 3, a);
    if (!np) return r & 0xffffff00u;
    uint16_t cap = rd16(c, q + Q_CAP);
    wr32(c, q + Q_BUF, np);
    wr16(c, q + Q_HEAD, 0);
    wr16(c, q + Q_TAIL, cap);
    wr16(c, q + Q_CAP, cap2);
    return (r & 0xffffff00u) | 1;
}

void f_10130cb0(cpu *c);

/* FUN_10130bf0 (thiscall): append an entry, growing a full queue. al 1, 0 without entries or if it could
 * not grow (the entry is then dropped). */
uint32_t track_queue_push(cpu *c, uint32_t sp, uint32_t q, uint32_t v)
{
    uint32_t p = rd32(c, q + Q_BUF);
    if (!p) return 0;
    wr32(c, p + 4u * rd16(c, q + Q_TAIL), v);
    wr16(c, q + Q_TAIL, (uint16_t)(rd16(c, q + Q_TAIL) + 1));
    if (rd16(c, q + Q_TAIL) == rd16(c, q + Q_CAP)) wr16(c, q + Q_TAIL, 0);
    uint16_t t = rd16(c, q + Q_TAIL);
    uint32_t eax = (p & 0xffff0000u) | t;
    if (t == rd16(c, q + Q_HEAD)) {
        uint32_t save = c->esp;
        c->esp = sp - 4;
        c->ecx = q;
        push32(c, 0x10130c31u);
        f_10130cb0(c);
        c->esp = save;
        eax = c->eax;
        if (!(eax & 0xff)) {
            t = rd16(c, q + Q_TAIL);
            if (t == 0) wr16(c, q + Q_TAIL, (uint16_t)(rd16(c, q + Q_CAP) - 1));
            else wr16(c, q + Q_TAIL, (uint16_t)(t - 1));
            return 0;
        }
    }
    return (eax & 0xffffff00u) | 1;
}

/* FUN_10130ac0: free the frame builder's cursor (instance + 0x64 -> its first word) and its buffer */
void track_cursor_free(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t cur = rd32(c, rd32(c, eng + 0x64));
    if (!cur) return;
    uint32_t b = rd32(c, cur);
    if (b) import_at(c, sp - 0xc, IAT_FREE, 0x10130adfu, 1, &b);
    import_at(c, sp - 0xc, IAT_FREE, 0x10130ae5u, 1, &cur);
    wr32(c, rd32(c, eng + 0x64), 0);
}

/* ---------------------------------------------------------------- adapters */
PORT_FN(10130b00) { uint32_t q = c->ecx; port_ret(c, track_queue_init(c, c->esp, q, (uint16_t)ARG(0)), 4); }
PORT_FN(10130b50) { port_ret(c, track_queue_free(c, c->esp, c->ecx), 0); }
PORT_FN(10130b80) { port_ret(c, track_queue_reset(c, c->esp, c->ecx), 0); }
PORT_FN(10130cb0) { port_ret(c, track_queue_grow(c, c->esp, c->ecx), 0); }
PORT_FN(10130bf0) { uint32_t q = c->ecx; port_ret(c, track_queue_push(c, c->esp, q, ARG(0)), 4); }
PORT_FN(10130ac0) { track_cursor_free(c, c->esp, ARG(0)); port_ret(c, c->eax, 0); }
