/* rules_pool - where the delta's elements and the control stack live: chunks of memory handed out from
 * the top down, with snapshots to roll an utterance's allocations back, and the delta's set-up (its two
 * end marks). ENU.SYN FUN_10138ce0 .. FUN_1013a1a0, hand-ported.
 *
 * A chunk (0x18 bytes, malloc'd, with a malloc'd data block):
 *   +0x00 previous chunk     +0x04 bytes used from the end (starts at the end's misalignment)
 *   +0x08 live elements      +0x0c data block      +0x10 end (data + size - 1)      +0x14 next chunk
 * An element is taken at end - used (8-byte steps); its first word points back to its chunk, the caller
 * gets the address after it. The workspace keeps (WS_*): 0x453 the first chunk, 0x45b the current one,
 * 0x467 the chunk size, 0x457 the control stack's chunk, 0x519 / 0x51d a cache of up to 10 empty chunks,
 * and 10 snapshots of 17 bytes at 0x46f: +0 position (end - used), +4 chunk, +8 free flag, +9 used,
 * +0xd live count (the flag at 0x477 + 17 k).
 *
 * As in rules.c: memory accesses in the original's order, eax as the original leaves it, calls into the
 * machine at the esp the original has there (`sp` is the esp a function was entered with).
 */
#include "rules_int.h"

#define IAT_FREE   0x1014410cu
#define IAT_MALLOC 0x10144110u

enum { CH_PREV = 0, CH_USED = 4, CH_COUNT = 8, CH_DATA = 0xc, CH_END = 0x10, CH_NEXT = 0x14 };
enum { WS_POOL_FIRST = 0x453, WS_POOL_CUR = 0x45b, WS_POOL_SIZE = 0x467, WS_SNAP = 0x46f,
       WS_CACHE_N = 0x519, WS_CACHE = 0x51d, WS_MARK_SIZE = 0x87 };
enum { SN_POS = 0, SN_CHUNK = 4, SN_FREE = 8, SN_USED = 9, SN_COUNT = 0xd, SN_SIZE = 17, SN_N = 10 };

void f_10135c70(cpu *c);
void f_10135ae0(cpu *c);
void f_10135af0(cpu *c);
void f_10135b10(cpu *c);
void f_10135ab0(cpu *c);
void f_10135a90(cpu *c);
void f_10139b80(cpu *c);
void f_10139cf0(cpu *c);
void f_10139e70(cpu *c);
void f_1013a0e0(cpu *c);

static uint32_t free_at(cpu *c, uint32_t sp, uint32_t p, uint32_t ret) { return import_call(c, sp, IAT_FREE, ret, 1, &p); }

/* the end's misalignment, the first `used` of a fresh chunk */
static uint32_t first_used(uint32_t end) { return (end & 7) ? (end & 3) : (end & 3) + 4; }

/* FUN_1013a0e0: a chunk of `size` bytes: from the cache, else malloc'd (0 if out of memory) */
uint32_t pool_chunk_new(cpu *c, uint32_t sp, uint32_t eng, uint32_t size)
{
    uint32_t ch = rd32(c, WS(eng) + WS_CACHE);
    if (ch) {
        uint32_t next = rd32(c, ch + CH_NEXT);
        wr32(c, ch + CH_COUNT, 0);
        wr32(c, WS(eng) + WS_CACHE, next);
        uint32_t ws = WS(eng);
        wr32(c, ws + WS_CACHE_N, rd32(c, ws + WS_CACHE_N) - 1);
        wr32(c, ch + CH_USED, first_used(rd32(c, ch + CH_END)));
        wr32(c, ch + CH_NEXT, 0);
        wr32(c, ch + CH_PREV, 0);
        return ch;
    }
    uint32_t n = 0x18;
    wr32(c, sp - 0x10, c->edi);             /* push edi (saved only on this path) */
    ch = import_call(c, sp - 0x10, IAT_MALLOC, 0x1013a13fu, 1, &n);
    if (!ch) return 0;
    wr32(c, ch + CH_NEXT, 0);
    wr32(c, ch + CH_PREV, 0);
    wr32(c, ch + CH_COUNT, 0);
    uint32_t data = import_call(c, sp - 0x10, IAT_MALLOC, 0x1013a157u, 1, &size);
    wr32(c, ch + CH_DATA, data);
    if (!data) {
        free_at(c, sp - 0x10, ch, 0x1013a168u);
        return 0;
    }
    uint32_t end = data + size - 1;
    wr32(c, ch + CH_END, end);
    wr32(c, ch + CH_USED, first_used(end));
    return ch;
}

static uint32_t chunk_new_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t size, uint32_t ret)
{
    return call2(c, sp, f_1013a0e0, ret, eng, size);
}

/* the snapshots all free */
static void snapshots_free(cpu *c, uint32_t eng)
{
    for (uint32_t k = SN_SIZE; k < SN_SIZE * (SN_N + 1); k += SN_SIZE)
        wr8(c, WS(eng) + WS_SNAP + SN_FREE - SN_SIZE + k, 1);
}

/* FUN_10139b20: the pool with one chunk of `size`. al 1, or 0 if out of memory. */
uint32_t pool_init(cpu *c, uint32_t sp, uint32_t eng, uint32_t size)
{
    uint32_t esi = c->esi, edi = c->edi;    /* the original: esi eng, edi size */
    c->esi = eng;
    c->edi = size;
    uint32_t ch = chunk_new_at(c, sp - 8, eng, size, 0x10139b31u);
    c->esi = esi;
    c->edi = edi;
    wr32(c, WS(eng) + WS_POOL_FIRST, ch);
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_POOL_CUR, rd32(c, ws + WS_POOL_FIRST));
    wr32(c, WS(eng) + WS_POOL_SIZE, size);
    snapshots_free(c, eng);
    ws = WS(eng);
    return (ws & 0xffffff00u) | (rd32(c, ws + WS_POOL_FIRST) != 0);
}

/* FUN_10139b80 (fastcall): an element of `size` bytes (0 if out of memory) */
uint32_t pool_alloc(cpu *c, uint32_t sp, uint32_t eng, int32_t size)
{
    /* its saves (also when entered by the tail jump from FUN_10138d40, which has no prologue of its own) */
    wr32(c, sp - 4, c->ebx);
    wr32(c, sp - 8, c->esi);
    wr32(c, sp - 0xc, c->edi);
    uint32_t ch = rd32(c, WS(eng) + WS_POOL_CUR);
    int32_t n = size + 4;
    uint32_t p;
    if (n <= 0) {
        p = rd32(c, ch + CH_END) - rd32(c, ch + CH_USED);
    } else {
        if (n & 7) n += 8 - (n & 7);
        uint32_t used = rd32(c, ch + CH_USED) + (uint32_t)n;
        wr32(c, ch + CH_USED, used);
        if ((int32_t)used < (int32_t)rd32(c, WS(eng) + WS_POOL_SIZE)) {
            p = rd32(c, ch + CH_END) - used;
        } else {
            wr32(c, ch + CH_USED, used - (uint32_t)n);
            /* the original's registers at the call: ebx eng, esi the chunk, edi n, ecx the size */
            uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi;
            c->ebx = eng;
            c->esi = ch;
            c->edi = (uint32_t)n;
            c->ecx = rd32(c, WS(eng) + WS_POOL_SIZE);
            uint32_t nc = chunk_new_at(c, sp - 0xc, eng, rd32(c, WS(eng) + WS_POOL_SIZE), 0x10139bd1u);
            c->ebx = ebx;
            c->esi = esi;
            c->edi = edi;
            wr32(c, ch + CH_NEXT, nc);
            if (!nc) {
                p = 0;
            } else {
                wr32(c, nc, ch);
                uint32_t nx = rd32(c, ch + CH_NEXT);
                wr32(c, nx + CH_USED, rd32(c, nx + CH_USED) + (uint32_t)n);
                ch = rd32(c, ch + CH_NEXT);
                used = rd32(c, ch + CH_USED);
                p = (int32_t)used <= (int32_t)rd32(c, WS(eng) + WS_POOL_SIZE) ? rd32(c, ch + CH_END) - used : 0;
            }
        }
    }
    if (!p) return 0;
    uint32_t nx = rd32(c, rd32(c, WS(eng) + WS_POOL_CUR) + CH_NEXT);
    if (nx) wr32(c, WS(eng) + WS_POOL_CUR, nx);
    wr32(c, p, rd32(c, WS(eng) + WS_POOL_CUR));
    uint32_t cur = rd32(c, WS(eng) + WS_POOL_CUR);
    wr32(c, cur + CH_COUNT, rd32(c, cur + CH_COUNT) + 1);
    return p + 4;
}

/* FUN_10139c60 (fastcall): give an element back; an emptied chunk other than the current one goes to the
 * cache (up to 10) or is freed. Returns the original's eax. */
uint32_t pool_free(cpu *c, uint32_t sp, uint32_t eng, uint32_t e)
{
    wr32(c, sp - 4, c->esi);                /* push esi (also when entered by FUN_10138d60's tail jump) */
    uint32_t ch = rd32(c, e - 4);
    uint32_t n = rd32(c, ch + CH_COUNT) - 1;
    wr32(c, ch + CH_COUNT, n);
    if (n) return n;
    uint32_t ws = WS(eng);
    uint32_t cur = rd32(c, ws + WS_POOL_CUR);
    if (ch == cur) {
        wr32(c, cur + CH_USED, rd32(c, cur + CH_END) & 3);
        return cur;
    }
    if ((int32_t)rd32(c, ws + WS_CACHE_N) < 10) {
        wr32(c, rd32(c, ch + CH_PREV) + CH_NEXT, rd32(c, ch + CH_NEXT));
        uint32_t nx = rd32(c, ch + CH_NEXT);
        if (nx) wr32(c, nx, rd32(c, ch + CH_PREV));
        wr32(c, ch + CH_NEXT, rd32(c, WS(eng) + WS_CACHE));
        wr32(c, WS(eng) + WS_CACHE, ch);
        ws = WS(eng);
        wr32(c, ws + WS_CACHE_N, rd32(c, ws + WS_CACHE_N) + 1);
        return ws;
    }
    wr32(c, rd32(c, ch + CH_PREV) + CH_NEXT, rd32(c, ch + CH_NEXT));
    uint32_t nx = rd32(c, ch + CH_NEXT);
    if (nx) wr32(c, nx, rd32(c, ch + CH_PREV));
    wr32(c, sp - 8, c->edi);                /* push edi (the import's address is kept in it) */
    free_at(c, sp - 8, rd32(c, ch + CH_DATA), 0x10139ce1u);
    return free_at(c, sp - 0xc, ch, 0x10139ce4u);
}

/* FUN_10139cf0: free every chunk and start again with one */
void pool_reset(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t ch = rd32(c, WS(eng) + WS_POOL_FIRST);
    if (ch) {                               /* push ebx; push ebp (the loop's registers) */
        wr32(c, sp - 0xc, c->ebx);
        wr32(c, sp - 0x10, c->ebp);
    }
    while (ch) {
        uint32_t next = rd32(c, ch + CH_NEXT);
        free_at(c, sp - 0x10, rd32(c, ch + CH_DATA), 0x10139d14u);
        free_at(c, sp - 0x14, ch, 0x10139d17u);
        ch = next;
    }
    uint32_t size = rd32(c, WS(eng) + WS_POOL_SIZE);
    uint32_t esi = c->esi, edi = c->edi;    /* the original: edi eng, esi the size */
    c->edi = eng;
    c->esi = size;
    uint32_t nc = chunk_new_at(c, sp - 8, eng, size, 0x10139d32u);
    c->esi = esi;
    c->edi = edi;
    wr32(c, WS(eng) + WS_POOL_FIRST, nc);
    uint32_t ws = WS(eng);
    wr32(c, ws + WS_POOL_CUR, rd32(c, ws + WS_POOL_FIRST));
    wr32(c, WS(eng) + WS_POOL_SIZE, size);
    snapshots_free(c, eng);
}

/* FUN_1013a1a0: free all chunks (the cache, the pool, the control stack's) */
void pool_free_all(cpu *c, uint32_t sp, uint32_t eng)
{
    static const uint32_t lists[3] = { WS_CACHE, WS_POOL_FIRST, WS_CHUNK };
    static const uint32_t rets[3][2] = { { 0x1013a1cau, 0x1013a1cdu }, { 0x1013a1f0u, 0x1013a1f3u },
                                         { 0x1013a216u, 0x1013a219u } };
    for (int k = 0; k < 3; k++) {
        uint32_t ch = rd32(c, WS(eng) + lists[k]);
        while (ch) {
            uint32_t next = rd32(c, ch + CH_NEXT);
            free_at(c, sp - 0x10, rd32(c, ch + CH_DATA), rets[k][0]);
            free_at(c, sp - 0x14, ch, rets[k][1]);
            ch = next;
        }
    }
    wr32(c, WS(eng) + WS_CHUNK, 0);
    wr32(c, WS(eng) + WS_POOL_CUR, 0);
    wr32(c, WS(eng) + WS_POOL_FIRST, 0);
    wr32(c, WS(eng) + WS_CACHE, 0);
}

/* FUN_10139e70: take a free snapshot of the pool's position. al 1, or 0 if none is free. */
uint32_t pool_snapshot(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t ws = WS(eng);
    for (uint32_t k = 0; k < SN_N; k++) {
        if (!rd8(c, ws + WS_SNAP + SN_FREE + SN_SIZE * k)) continue;
        wr32(c, sp - 0xc, c->ebp);          /* push ebp, push edi when one is found */
        wr32(c, sp - 0x10, c->edi);
        uint32_t o = SN_SIZE * k;
        wr8(c, ws + WS_SNAP + SN_FREE + o, 0);
        ws = WS(eng);
        uint32_t cur = rd32(c, ws + WS_POOL_CUR);
        wr32(c, ws + WS_SNAP + SN_POS + o, rd32(c, cur + CH_END) - rd32(c, cur + CH_USED));
        ws = WS(eng);
        wr32(c, ws + WS_SNAP + SN_USED + o, rd32(c, rd32(c, ws + WS_POOL_CUR) + CH_USED));
        ws = WS(eng);
        wr32(c, ws + WS_SNAP + SN_COUNT + o, rd32(c, rd32(c, ws + WS_POOL_CUR) + CH_COUNT));
        ws = WS(eng);
        uint32_t a = 17 * (k + 0x43);
        wr32(c, a + ws, rd32(c, ws + WS_POOL_CUR));
        return ((k + 0x43) & 0xffffff00u) | 1;
    }
    return ((ws + WS_SNAP + SN_FREE + SN_SIZE * SN_N) & 0xffffff00u);
}

/* FUN_10139d80: roll the pool back to the snapshot taken at position `pos` (freeing the chunks after
 * it); with `release` the snapshot becomes free */
void pool_rollback(cpu *c, uint32_t sp, uint32_t eng, uint32_t pos, uint8_t release)
{
    for (uint32_t k = 0; k < SN_N; k++) {
        uint32_t o = SN_SIZE * k;
        uint32_t ws = WS(eng);
        if (rd8(c, ws + WS_SNAP + SN_FREE + o) || pos != rd32(c, ws + WS_SNAP + SN_POS + o)) continue;
        uint32_t cur = rd32(c, ws + WS_POOL_CUR);
        if (cur != rd32(c, ws + WS_SNAP + SN_CHUNK + o)) {
            while (cur) {
                free_at(c, sp - 0x10, rd32(c, cur + CH_DATA), 0x10139dcau);
                ws = WS(eng);
                wr32(c, ws + WS_POOL_CUR, rd32(c, rd32(c, ws + WS_POOL_CUR) + CH_PREV));
                ws = WS(eng);
                free_at(c, sp - 0x14, rd32(c, rd32(c, ws + WS_POOL_CUR) + CH_NEXT), 0x10139deau);
                ws = WS(eng);
                cur = rd32(c, ws + WS_POOL_CUR);
                if (cur == rd32(c, ws + WS_SNAP + SN_CHUNK + o)) break;
            }
        }
        cur = rd32(c, WS(eng) + WS_POOL_CUR);
        if (!cur) continue;
        wr32(c, cur + CH_USED, rd32(c, WS(eng) + WS_SNAP + SN_USED + o));
        wr32(c, rd32(c, WS(eng) + WS_POOL_CUR) + CH_COUNT, rd32(c, WS(eng) + WS_SNAP + SN_COUNT + o));
        if (release) wr8(c, WS(eng) + WS_SNAP + SN_FREE + o, 1);
        return;
    }
}

/* FUN_10139f10: an element's number: its index within the pool counted in `size` steps (-1 if it is in
 * no chunk, -2 if in a cached one) */
uint32_t pool_index(cpu *c, uint32_t eng, uint32_t e, uint32_t size)
{
    uint32_t ws = WS(eng);
    uint32_t ch = rd32(c, e - 4);
    uint32_t k = 0;
    for (uint32_t p = rd32(c, ws + WS_POOL_FIRST); p; p = rd32(c, p + CH_NEXT), k++) {
        if (p == ch) {
            int32_t q = (int32_t)(rd32(c, ch + CH_END) - e) / (int32_t)size;
            return (rd32(c, ws + WS_POOL_SIZE) / size) * k + (uint32_t)q;
        }
    }
    for (uint32_t p = rd32(c, ws + WS_CACHE); p; p = rd32(c, p + CH_NEXT))
        if (p == ch) return 0xfffffffeu;
    return 0xffffffffu;
}

/* FUN_10139f90: the control stack in a chunk of `size`: its top at the end, a barrier entry (type 8)
 * pushed and made the cut point. al 1, or 0 if out of memory. */
uint32_t cstack_init(cpu *c, uint32_t sp, uint32_t eng, uint32_t size)
{
    uint32_t esi = c->esi, edi = c->edi;    /* the original: esi eng, edi size */
    c->esi = eng;
    c->edi = size;
    uint32_t ch = chunk_new_at(c, sp - 0xc, eng, size, 0x10139fa2u);
    c->esi = esi;
    c->edi = edi;
    wr32(c, WS(eng) + WS_CHUNK, ch);
    uint32_t ws = WS(eng);
    ch = rd32(c, ws + WS_CHUNK);
    wr32(c, ws + WS_TOP, rd32(c, ch + CH_END) - rd32(c, ch + CH_USED));
    wr32(c, WS(eng) + WS_CHUNK_OFF, size);
    ws = WS(eng);
    wr32(c, ws + WS_TOP_OFF, size - rd32(c, rd32(c, ws + WS_CHUNK) + CH_USED));
    uint32_t top = cs_push(c, eng, WS_SZ_MARK);
    wr8(c, top, CS_BARRIER);
    ws = WS(eng);
    wr32(c, ws + WS_CUT, rd32(c, ws + WS_TOP));
    ws = WS(eng);
    uint32_t cc = rd32(c, ws + WS_CHUNK);
    return (cc & 0xffffff00u) | (cc != 0);
}

/* ------------------------------------------------------------------------ the delta's ends */

/* a new sync mark (FUN_10138ce0 inline): zeroed, flagged a mark, ring flags set */
static uint32_t mark_new(cpu *c, uint32_t sp, uint32_t eng, uint32_t ret_alloc, uint32_t ret_set, uint32_t ret_clear,
                         int mirror)
{
    /* mirror: the original's registers as FUN_10138ce0 has them (edi eng at the allocation, then esi the
     * mark and edi its end, where the clearing left it) */
    uint32_t esi = c->esi, edi = c->edi;
    if (mirror) c->edi = eng;
    uint32_t save = c->esp;
    c->esp = sp;
    c->ecx = eng;
    c->edx = rd32(c, WS(eng) + WS_MARK_SIZE);
    push32(c, ret_alloc);
    f_10139b80(c);
    c->esp = save;
    uint32_t m = c->eax;
    if (!m) {
        c->esi = esi;
        c->edi = edi;
        return 0;
    }
    uint32_t n = rd32(c, WS(eng) + WS_MARK_SIZE);
    for (uint32_t i = 0; i + 4 <= n; i += 4) wr32(c, m + i, 0);
    for (uint32_t i = n & ~3u; i < n; i++) wr8(c, m + i, 0);
    wr32(c, m, rd32(c, m) | 2);
    if (mirror) {
        c->esi = m;
        c->edi = m + n;
        c->eax = rd32(c, m);
        c->ecx = 0;
        c->edx = n;
    }
    call_at(c, sp, f_10135ae0, ret_set, 1, &m);
    call_at(c, sp - 4, f_10135b10, ret_clear, 1, &m);
    c->esi = esi;
    c->edi = edi;
    return m;
}

/* FUN_10138ce0 */
uint32_t delta_mark_new(cpu *c, uint32_t sp, uint32_t eng)
{
    return mark_new(c, sp - 8, eng, 0x10138cf6u, 0x10138d26u, 0x10138d2cu, 1);
}

/* link the two end marks in every stream (the counter in the original's first argument slot's low
 * byte, sp + 4), inserting the default tokens with `defaults`; 0 if that fails (FUN_10138d70), the
 * ring closed */
static int link_ends(cpu *c, uint32_t sp, uint32_t eng, uint32_t defaults, uint32_t csp, uint32_t ret_reset,
                     int checked)
{
    wr8(c, sp + 4, 0);
    if (rd8(c, eng + ENG_NSTREAMS)) {
        do {
            uint32_t s = rd32(c, sp + 4) & 0xff;
            wr32(c, stream_desc(s) + 0x19, 0);
            uint32_t a = rd32(c, WS(eng) + WS_END) + 4 * (rd32(c, RS(eng) + RS_BACK) + s);
            wr32(c, a, rd32(c, a) | 1);
            a = rd32(c, WS(eng) + WS_START) + 4 * (rd32(c, RS(eng) + RS_BACK) + s);
            wr32(c, a, rd32(c, a) | 1);
            uint32_t end = rd32(c, WS(eng) + WS_END), start = rd32(c, WS(eng) + WS_START);
            a = end + 4 * (rd32(c, RS(eng) + RS_BACK) + s);
            wr32(c, a, start | (rd32(c, a) & 3));
            start = rd32(c, WS(eng) + WS_START);
            end = rd32(c, WS(eng) + WS_END);
            a = start + 4 * s + 0xc;
            wr32(c, a, (rd32(c, a) & 3) | end);
            if (defaults) {
                /* the original's registers: esi eng, ebp defaults, edi the counter slot, ebx the end mark */
                uint32_t ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp;
                c->esi = eng;
                c->ebp = defaults;
                c->edi = rd32(c, sp + 4);
                c->ebx = rd32(c, WS(eng) + WS_END);
                uint32_t r = call2(c, csp, f_10135c70, ret_reset, eng, rd32(c, sp + 4));
                c->ebx = ebx;
                c->esi = esi;
                c->edi = edi;
                c->ebp = ebp;
                if (checked && !(r & 0xff)) return 0;
            }
            wr8(c, sp + 4, (uint8_t)(rd8(c, sp + 4) + 1));
        } while (rd8(c, sp + 4) < rd8(c, eng + ENG_NSTREAMS));
    }
    return 1;
}

/* FUN_10138fb0: the delta's ends linked again in every stream (defaults inserted with `defaults`) */
void delta_relink(cpu *c, uint32_t sp, uint32_t eng, uint32_t defaults)
{
    uint32_t m = rd32(c, WS(eng) + WS_END);
    call_at(c, sp - 4, f_10135af0, 0x10138fc0u, 1, &m);
    m = rd32(c, WS(eng) + WS_START);
    call_at(c, sp - 8, f_10135af0, 0x10138fccu, 1, &m);
    if (rd8(c, eng + ENG_NSTREAMS)) {       /* push ebx, ebp, edi for the loop */
        wr32(c, sp - 8, c->ebx);
        wr32(c, sp - 0xc, c->ebp);
        wr32(c, sp - 0x10, c->edi);
    }
    link_ends(c, sp, eng, defaults, sp - 0x10, 0x10139079u, 0);
    uint32_t ws = WS(eng);
    call3(c, sp - 4, f_10135ab0, 0x101390a7u, eng, rd32(c, ws + WS_END), rd32(c, ws + WS_START));
    ws = WS(eng);
    call2(c, sp - 0x10, f_10135a90, 0x101390b6u, rd32(c, ws + WS_START), rd32(c, ws + WS_END));
}

/* FUN_101390c0: the cursor nowhere (backward, fresh) */
void cursor_reset(cpu *c, uint32_t eng)
{
    wr32(c, RS(eng) + RS_POS_MARK, 0);
    wr8(c, RS(eng) + RS_POS_STREAM, 0);
    wr8(c, RS(eng) + RS_POS_BACK, 1);
    wr8(c, RS(eng) + RS_POS_FRESH, 1);
}

/* FUN_10138d70: a new delta: the pool reset, two end marks, a snapshot, every stream linked from the
 * start to the end (with its default token if `defaults`), the cursor nowhere. al 1, or 0. */
uint32_t delta_init(cpu *c, uint32_t sp, uint32_t eng, uint32_t defaults)
{
    wr32(c, WS(eng) + WS_MARK_SIZE, 8u * rd8(c, eng + ENG_NSTREAMS) + 0x18);
    call_at(c, sp - 0x10, f_10139cf0, 0x10138d96u, 1, &eng);
    uint32_t m = mark_new(c, sp - 0x10, eng, 0x10138da9u, 0x10138ddeu, 0x10138de4u, 0);
    wr32(c, WS(eng) + WS_END, m);
    m = mark_new(c, sp - 0x10, eng, 0x10138dfcu, 0x10138e2fu, 0x10138e35u, 0);
    wr32(c, WS(eng) + WS_START, m);
    if (!rd32(c, WS(eng) + WS_END) || !rd32(c, WS(eng) + WS_START)) return 0;
    if (!(call_at(c, sp - 0x10, f_10139e70, 0x10138e58u, 1, &eng) & 0xff)) return 0;
    m = rd32(c, WS(eng) + WS_END);
    call_at(c, sp - 0x10, f_10135af0, 0x10138e6eu, 1, &m);
    m = rd32(c, WS(eng) + WS_START);
    call_at(c, sp - 0x14, f_10135af0, 0x10138e7au, 1, &m);
    if (!link_ends(c, sp, eng, defaults, sp - 0x10, 0x10138f21u, 1)) return 0;
    uint32_t ws = WS(eng);
    call3(c, sp - 0x10, f_10135ab0, 0x10138f50u, eng, rd32(c, ws + WS_END), rd32(c, ws + WS_START));
    ws = WS(eng);
    call2(c, sp - 0x1c, f_10135a90, 0x10138f5fu, rd32(c, ws + WS_START), rd32(c, ws + WS_END));
    cursor_reset(c, eng);
    wr32(c, RS(eng) + 0x1146, 1);
    return 1;
}

/* FUN_101390f0: free the value stack */
void eval_free(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t p = rd32(c, WS(eng) + WS_EVAL);
    if (!p) return;
    free_at(c, sp - 4, p, 0x10139109u);
    wr32(c, WS(eng) + WS_EVAL, 0);
}

/* ---------------------------------------------------------------- adapters */
#define SP c->esp
#define RET(v) port_ret(c, (v), 0)

PORT_FN(1013a0e0) { RET(pool_chunk_new(c, SP, ARG(0), ARG(1))); }
PORT_FN(10139b20) { RET(pool_init(c, SP, ARG(0), ARG(1))); }
PORT_FN(10139b80) { RET(pool_alloc(c, SP, c->ecx, (int32_t)c->edx)); }
PORT_FN(10139c60) { RET(pool_free(c, SP, c->ecx, c->edx)); }
PORT_FN(10138d40)
{
    /* a tail jump to FUN_10139b80 with the stream's element size */
    uint32_t eng = ARG(0), desc = ARG(1);
    RET(pool_alloc(c, SP, eng, (int32_t)(rd32(c, desc + SD_TOKEN_SIZE) + 8)));
}
PORT_FN(10138d60) { RET(pool_free(c, SP, ARG(0), ARG(1))); }
PORT_FN(10139cf0) { pool_reset(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(1013a1a0) { pool_free_all(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10139e70) { RET(pool_snapshot(c, SP, ARG(0))); }
PORT_FN(10139d80) { pool_rollback(c, SP, ARG(0), ARG(1), (uint8_t)ARG(2)); RET(c->eax); }
PORT_FN(10139f10) { RET(pool_index(c, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10139f90) { RET(cstack_init(c, SP, ARG(0), ARG(1))); }
PORT_FN(10138ce0) { RET(delta_mark_new(c, SP, ARG(0))); }
PORT_FN(10138fb0) { delta_relink(c, SP, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(101390c0) { cursor_reset(c, ARG(0)); RET(c->eax); }
PORT_FN(10138d70) { RET(delta_init(c, SP, ARG(0), ARG(1))); }
PORT_FN(101390f0) { eval_free(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10139120) { RET(rd8(c, ARG(0) + ENG_NSTREAMS)); }
PORT_FN(10139130) { RET(rd32(c, stream_desc((uint32_t)(int32_t)(int8_t)ARG(0)) + SD_NAME)); }
PORT_FN(10139150)
{
    int32_t s = (int8_t)ARG(0);
    RET(((uint32_t)(19 * s) & 0xffffff00u) | rd8(c, stream_desc((uint32_t)s) + 0x2d));
}
