/* rules_delta - the delta itself: sync marks and tokens linked into their streams (rules.h), the ring of
 * all sync marks, inserting and deleting tokens, setting fields over a range, and the rule state's
 * set-up and tear-down.
 *
 * As in rules.c: memory accesses in the original's order, eax as the original leaves it, calls into the
 * machine at the esp the original has there (`sp` is the esp a function was entered with).
 *
 * The ring of sync marks: every sync mark is on a doubly linked list of all marks in text order, next at
 * +4 and previous at 4 (B - 2), B = rs[RS_BACK] (the word just before the backward stream links), with
 * the same two flag bits as the stream links. WS_START (+4) and WS_END (+0) of the workspace are its
 * first and last marks. Every change to it counts in a global (0x101939e0).
 */
#include "rules_int.h"

void f_10136e00(cpu *c);   /* delete the tokens between two marks */
void f_10138d40(cpu *c);   /* allocate an element of a stream (FUN_10139b80) */
void f_10139f10(cpu *c);
void f_10138d70(cpu *c);   /* set up the delta: the two end marks */
void f_10140f40(cpu *c);
void f_10140ff0(cpu *c);
void f_1012f0b0(cpu *c);
void f_1012f340(cpu *c);
void f_10141350(cpu *c);
void f_10141410(cpu *c);
void f_1012aed0(cpu *c);
void f_1012af10(cpu *c);
void f_10141040(cpu *c);

#define IAT_MEMMOVE 0x10144134u
#define IAT_MALLOC  0x10144110u
#define IAT_FREE    0x1014410cu
#define RING_COUNT  0x101939e0u

/* the ring links of a sync mark */
static uint32_t ring_prev_at(cpu *c, uint32_t eng, uint32_t m) { return m + 4 * rd32(c, RS(eng) + RS_BACK) - 8; }
static void set_link(cpu *c, uint32_t a, uint32_t to) { wr32(c, a, (rd32(c, a) & 3) | to); }
static uint32_t ring_count(cpu *c)
{
    uint32_t n = rd32(c, RING_COUNT) + 1;
    wr32(c, RING_COUNT, n);
    return n;
}

/* ------------------------------------------------------------------------ small element helpers */

/* FUN_10135a80: bit 1 of an element's word 2 */
uint32_t rl_elem_bit2(cpu *c, uint32_t e) { return (rd32(c, e + 8) >> 1) & 1; }
/* FUN_10135b00: bit 1 of the ring's next link (a mark flagged) */
uint32_t rl_ring_flag(cpu *c, uint32_t m) { return (rd32(c, m + 4) >> 1) & 1; }

/* FUN_10135a90: next := to (in the ring). Returns the original's eax (m). */
uint32_t rl_ring_set_next(cpu *c, uint32_t m, uint32_t to)
{
    set_link(c, m + 4, to);
    return m;
}

/* FUN_10135ab0: previous := to (in the ring). Returns the original's eax (the link's address). */
uint32_t rl_ring_set_prev(cpu *c, uint32_t eng, uint32_t m, uint32_t to)
{
    uint32_t a = ring_prev_at(c, eng, m);
    set_link(c, a, to);
    return a;
}

/* FUN_10135b40: put mark a into the ring before b */
uint32_t rl_ring_insert_before(cpu *c, uint32_t eng, uint32_t a, uint32_t b)
{
    uint32_t off = 4 * rd32(c, RS(eng) + RS_BACK) - 8;
    uint32_t prev = rd32(c, off + b) & ~3u;
    set_link(c, off + a, prev);
    set_link(c, prev + 4, a);
    set_link(c, ring_prev_at(c, eng, b), a);
    set_link(c, a + 4, b);
    return ring_count(c);
}

/* FUN_10135bb0: put mark a into the ring after b */
uint32_t rl_ring_insert_after(cpu *c, uint32_t eng, uint32_t a, uint32_t b)
{
    uint32_t next = rd32(c, b + 4) & ~3u;
    set_link(c, a + 4, next);
    set_link(c, ring_prev_at(c, eng, next), a);
    set_link(c, b + 4, a);
    set_link(c, ring_prev_at(c, eng, a), b);
    return ring_count(c);
}

/* FUN_10135c20: take mark a out of the ring */
uint32_t rl_ring_remove(cpu *c, uint32_t eng, uint32_t a)
{
    uint32_t off = 4 * rd32(c, RS(eng) + RS_BACK) - 8;
    uint32_t next = rd32(c, a + 4) & ~3u;
    uint32_t prev = rd32(c, off + a) & ~3u;
    set_link(c, off + next, prev);
    set_link(c, prev + 4, next);
    return ring_count(c);
}

/* FUN_101359f0: the mark after m in stream s (over a token), or the token's... as the original: the
 * forward link, and if that is a token, the token's right mark */
uint32_t rl_next_mark(cpu *c, uint32_t m, int8_t s)
{
    uint32_t e = rd32(c, m + 0xc + 4 * (uint32_t)(int32_t)s) & ~3u;
    if (e && !(rd32(c, e) & 2)) e = rd32(c, e) & ~3u;
    return e;
}

/* FUN_10135a10: the mark before m in stream s (over a token) */
uint32_t rl_prev_mark(cpu *c, uint32_t eng, uint32_t m, int8_t s)
{
    uint32_t e = rd32(c, m + 4 * (rd32(c, RS(eng) + RS_BACK) + (uint32_t)(int32_t)s)) & ~3u;
    if (e && !is_mark(c, e)) e = rd32(c, e + 4) & ~3u;
    return e;
}

/* FUN_10135b20: the type of field 0 of stream s, in ax (the original's upper half: 19 s >> 16) */
uint32_t rl_stream_type(cpu *c, uint32_t s)
{
    int32_t k = 19 * (int32_t)(int8_t)s;
    return ((uint32_t)k & 0xffff0000u) | rd16(c, rd32(c, stream_desc((uint32_t)(int32_t)(int8_t)s) + SD_FIELDS) + FD_TYPE);
}

/* FUN_101359c0: a number for a sync mark (FUN_10139f10), -1 for null */
uint32_t rl_mark_number(cpu *c, uint32_t sp, uint32_t eng, uint32_t m)
{
    if (!m) return 0xffffffffu;
    return call3(c, sp, f_10139f10, 0x101359e1u, eng, m, rd32(c, WS(eng) + 0x87));
}

/* ------------------------------------------------------------------------ walking and editing */

/* FUN_10136200: from mark m forward in stream s over sync marks: the last mark before a token (or the
 * end) */
uint32_t rl_last_mark_fwd(cpu *c, uint32_t m, uint32_t s)
{
    uint32_t off = 4 * (s & 0xff) + 0xc, r = m;
    uint32_t l = rd32(c, off + m);
    while ((l & ~3u) && is_mark(c, l & ~3u)) {
        r = l & ~3u;
        l = rd32(c, off + r);
    }
    return r;
}

/* FUN_10136240: the same backward */
uint32_t rl_last_mark_back(cpu *c, uint32_t eng, uint32_t m, uint32_t s)
{
    uint32_t off = 4 * (rd32(c, RS(eng) + RS_BACK) + (s & 0xff)), r = m;
    uint32_t l = rd32(c, off + m);
    while ((l & ~3u) && is_mark(c, l & ~3u)) {
        r = l & ~3u;
        l = rd32(c, off + r);
    }
    return r;
}

/* FUN_10136280: set a field of stream s of every token from mark `from` leftwards to mark `to` (or the
 * start) to `value`, through the stream's setters. The field number is at `fslot` (the original's own
 * argument slot), whose address is published at ws+0x2f for the setters. Returns 1. */
uint32_t rl_set_fields(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t from, uint32_t to, uint32_t value,
                       uint32_t fslot)
{
    wr16(c, WS(eng) + 0x33, 0xffff);
    wr32(c, WS(eng) + 0x2f, fslot);
    wr8(c, WS(eng) + 0x35, 0);
    uint32_t e = from;
    if (e != rd32(c, WS(eng) + WS_START)) {
        do {
            if (e == to) break;
            e = rd32(c, e + 4 * (rd32(c, RS(eng) + RS_BACK) + (s & 0xff))) & ~3u;
            if (!e || !is_mark(c, e)) {
                /* the field number is read back from its slot: the setters may see it through ws+0x2f */
                uint32_t set = rd32(c, rd32(c, stream_desc(s & 0xff) + SD_SETTERS) + 4 * (rd32(c, fslot) & 0xff));
                uint32_t save = c->esp;
                c->esp = sp - 0x10;
                push32(c, value);
                push32(c, e + 8);
                push32(c, 0x101362fau);
                x86_icall(c, set);
                c->esp = save;
                e = rd32(c, e + 4) & ~3u;
            }
        } while (e != rd32(c, WS(eng) + WS_START));
    }
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    return 1;
}

/* msvcrt memmove on guest memory (the direction as crt.c's) */
static void guest_memmove(cpu *c, uint32_t d, uint32_t s, uint32_t n)
{
    if (d < s) for (uint32_t i = 0; i < n; i++) wr8(c, d + i, rd8(c, s + i));
    else for (uint32_t i = n; i > 0; i--) wr8(c, d + i - 1, rd8(c, s + i - 1));
}

/* FUN_101364c0: token data dst of stream s := the stream's default token, then field 0 set from src
 * through the setter, then for a symbol field the symbol's record from the stream's table */
void rl_token_init(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t dst, uint32_t src)
{
    (void)eng;
    uint32_t d = stream_desc(s & 0xff);
    guest_memmove(c, dst, rd32(c, d + SD_DEFAULT), rd32(c, d + SD_TOKEN_SIZE));
    uint32_t save = c->esp;
    c->esp = sp - 0x1c;
    push32(c, src);
    push32(c, dst);
    push32(c, 0x101364ffu);
    x86_icall(c, rd32(c, rd32(c, d + SD_SETTERS)));
    c->esp = save;
    uint32_t sym = rd32(c, d + SD_SYMBOLS);
    if (!sym) return;
    int16_t t = (int16_t)rd16(c, rd32(c, d + SD_FIELDS) + FD_TYPE);
    if (t == T_SYM16)
        guest_memmove(c, dst, rd32(c, d + SD_SYMBOL_SIZE) * (uint32_t)(int32_t)(int16_t)rd16(c, src) + sym,
                      rd32(c, d + SD_SYMBOL_COPY));
    else if (t == T_SYM8)
        guest_memmove(c, dst, rd32(c, d + SD_SYMBOL_SIZE) * rd8(c, src) + sym, rd32(c, d + SD_SYMBOL_COPY));
}

/* FUN_10136a20: delete the tokens of stream s between mark `left` and mark `right` (FUN_10136e00 does it;
 * the range is kept at ws+0x36..0x46). Returns eax as the original: al 1. */
uint32_t rl_delete_range(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t right, uint32_t left)
{
    uint32_t ws = WS(eng);
    wr8(c, ws + 0x46, (uint8_t)s);
    wr32(c, WS(eng) + 0x3e, right);
    wr32(c, WS(eng) + 0x42, left);
    ws = WS(eng);
    int32_t si = (int8_t)rd8(c, ws + 0x46);
    uint32_t e = rd32(c, rd32(c, ws + 0x3e) + 4 * (rd32(c, RS(eng) + RS_BACK) + (uint32_t)si)) & ~3u;
    if (e && !is_mark(c, e)) e = rd32(c, e + 4) & ~3u;
    wr32(c, ws + 0x36, e);
    ws = WS(eng);
    si = (int8_t)rd8(c, ws + 0x46);
    e = rd32(c, rd32(c, ws + 0x42) + 0xc + 4 * (uint32_t)si) & ~3u;
    if (e && !(rd32(c, e) & 2)) e = rd32(c, e) & ~3u;
    wr32(c, ws + 0x3a, e);
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    ws = WS(eng);
    si = (int8_t)rd8(c, ws + 0x46);
    if (rd32(c, ws + 0x42) == rd32(c, ws + 0x36)) {
        e = rd32(c, rd32(c, ws + 0x3e) + 4 * (rd32(c, RS(eng) + RS_BACK) + (uint32_t)si)) & ~3u;
        if (e && is_mark(c, e)) return (eng & 0xffffff00u) | 1;
        uint32_t args[4] = { eng, e, e, 0 };
        return (call_at(c, sp - 8, f_10136e00, 0x10136ad6u, 4, args) & 0xffffff00u) | 1;
    }
    uint32_t a = rd32(c, rd32(c, ws + 0x42) + 0xc + 4 * (uint32_t)si) & ~3u;
    uint32_t b = rd32(c, rd32(c, ws + 0x3e) + 4 * (rd32(c, RS(eng) + RS_BACK) + (uint32_t)si)) & ~3u;
    uint32_t args[4] = { eng, b, a, 0 };
    return (call_at(c, sp - 8, f_10136e00, 0x10136b03u, 4, args) & 0xffffff00u) | 1;
}

/* FUN_10136570: insert a token of stream s between mark `left` and mark `right` (deleting what is between
 * them first), its data from the ref: copied for a token value, else made from the stream's default
 * (rl_token_init). al 1, or 0 if no element could be allocated. `argslot`: the original keeps 19 s in its
 * first argument slot (0 when called from a port). */
uint32_t rl_insert_token(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t right, uint32_t left, uint32_t ref,
                         uint32_t argslot)
{
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    uint32_t si = s & 0xff;
    if ((rd32(c, right + 4 * (rd32(c, RS(eng) + RS_BACK) + si)) & ~3u) != left
        || (rd32(c, left + 4 * si + 0xc) & ~3u) != right)
        rl_delete_range(c, AT(sp - 0x10, 4), eng, s, right, left);
    if (argslot) wr32(c, argslot, 19 * si);
    uint32_t tok = call2(c, sp - 0x10, f_10138d40, 0x101365ddu, eng, stream_desc(si));
    if (!tok) return 0;
    set_link(c, right + 4 * (rd32(c, RS(eng) + RS_BACK) + si), tok);
    set_link(c, left + 4 * si + 0xc, tok);
    wr32(c, tok + 4, left);
    wr32(c, tok, right);
    if ((int16_t)rd16(c, ref + 4) >= 0) copy(c, tok + 8, rd32(c, ref), rd32(c, stream_desc(si) + SD_TOKEN_SIZE));
    else rl_token_init(c, AT(sp - 0x10, 4), eng, s, tok + 8, rd32(c, ref));
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    wr32(c, RS(eng) + 0x1146, 0);
    return 1;
}

/* FUN_10135c70: empty stream s (all its tokens between the delta's start and end), and if the stream
 * starts with a default token, insert it. al 0 if that fails, else 1. */
uint32_t rl_reset_stream(cpu *c, uint32_t sp, uint32_t eng, uint32_t s)
{
    uint32_t d = stream_desc((uint32_t)(int32_t)(int8_t)s);
    rl_delete_range(c, AT(sp - 0x14, 4), eng, s, rd32(c, WS(eng) + WS_END), rd32(c, WS(eng) + WS_START));
    if (!rd8(c, d + SD_HAS_DEFAULT)) return 1;
    uint32_t ref = sp - 8;
    uint32_t fd = rd32(c, d + SD_FIELDS);
    wr16(c, ref + 4, rd16(c, fd + FD_TYPE));
    uint8_t flag = rd8(c, fd + FD_FLAG);
    uint32_t def = rd32(c, d + SD_DEFAULT);
    uint32_t get0 = rd32(c, rd32(c, d + SD_GETTERS));
    wr8(c, ref + 6, flag);
    wr32(c, ref, icall1(c, sp - 0x14, get0, 0x10135cceu, def));
    uint32_t ws = WS(eng);
    return rl_insert_token(c, AT(sp - 0x18, 5), eng, s, rd32(c, ws + WS_END), rd32(c, ws + WS_START), ref, 0);
}

/* ------------------------------------------------------------------------ set-up and tear-down */

static uint32_t guest_malloc(cpu *c, uint32_t sp, uint32_t n, uint32_t ret)
{
    uint32_t save = c->esp;
    c->esp = sp;
    push32(c, n);
    push32(c, ret);
    x86_icall(c, rd32(c, IAT_MALLOC));
    c->esp = save;
    return c->eax;
}

static void guest_free(cpu *c, uint32_t sp, uint32_t p, uint32_t ret)
{
    uint32_t save = c->esp;
    c->esp = sp;
    push32(c, p);
    push32(c, ret);
    x86_icall(c, rd32(c, IAT_FREE));
    c->esp = save;
}

static void zero(cpu *c, uint32_t p, uint32_t n)
{
    for (; n >= 4; n -= 4, p += 4) wr32(c, p, 0);
    for (; n; n--, p++) wr8(c, p, 0);
}

/* FUN_10135900: the workspace (0x597 bytes, zeroed, a few fields set) */
void rl_ws_new(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t ws = guest_malloc(c, sp - 4, 0x597, 0x1013590cu);
    wr32(c, eng + ENG_WS, ws);
    zero(c, ws, 0x597);
    wr32(c, WS(eng) + 0xc6, 0x10192ee4u);
    wr32(c, WS(eng) + 0xca, 1);
    wr32(c, WS(eng) + 0x1b9, 0xffffffffu);
    wr32(c, WS(eng) + 0x1bd, 0xffffffffu);
    wr32(c, WS(eng) + 0x539, 0);
}

/* FUN_10135970 */
void rl_ws_free(cpu *c, uint32_t sp, uint32_t eng)
{
    if (!eng || !WS(eng)) return;
    zero(c, WS(eng), 0x597);
    guest_free(c, sp - 8, WS(eng), 0x10135997u);
    wr32(c, eng + ENG_WS, 0);
}

/* FUN_10131c10: create the rule state and everything that hangs off the instance */
void rl_new(cpu *c, uint32_t sp, uint32_t eng)
{
    if (!eng) return;
    uint32_t a = eng;
    call_at(c, sp - 8, f_10140f40, 0x10131c20u, 1, &a);
    uint32_t rs = guest_malloc(c, sp - 0xc, 0x11b2, 0x10131c2bu);
    wr32(c, eng + ENG_RS, rs);
    zero(c, rs, 0x11b2);
    wr8(c, RS(eng) + RS_STATUS, 0xff);                             /* FUN_101355d0 */
    rl_ws_new(c, AT(sp - 0x14, 1), eng);
    call_at(c, sp - 0x18, f_1012f0b0, 0x10131c4du, 1, &a);
    call_at(c, sp - 0x1c, f_10141350, 0x10131c53u, 1, &a);
    call_at(c, sp - 0x20, f_1012aed0, 0x10131c59u, 1, &a);
}

/* FUN_10131c60: free them */
void rl_free(cpu *c, uint32_t sp, uint32_t eng)
{
    if (!eng) return;
    uint32_t a = eng;
    call_at(c, sp - 0xc, f_10140ff0, 0x10131c73u, 1, &a);
    uint32_t rs = RS(eng);
    if (rs) {
        zero(c, rs, 0x11b2);
        guest_free(c, sp - 0xc, RS(eng), 0x10131c92u);
        wr32(c, eng + ENG_RS, 0);
    }
    rl_ws_free(c, AT(sp - 0xc, 1), eng);
    call_at(c, sp - 0x10, f_1012f340, 0x10131ca4u, 1, &a);
    call_at(c, sp - 0x14, f_10141410, 0x10131caau, 1, &a);
    call_at(c, sp - 0x18, f_1012af10, 0x10131cb0u, 1, &a);
    wr32(c, eng + ENG_OUT, 0);
    wr32(c, eng + ENG_RS, 0);
    wr32(c, eng + ENG_WS, 0);
    wr32(c, eng + 0x64, 0);
    wr32(c, eng + 0x68, 0);
    wr32(c, eng + 0x6c, 0);
}

/* the instance's global variables (eng + 0x00 .. 0x24): counts and tables of addresses */
enum { GV_NTOKENS = 0x00, GV_NINTS = 0x04, GV_NSHORTS = 0x08, GV_NDOUBLES = 0x0c, GV_NSYNC = 0x10,
       GV_SYNC = 0x14, GV_TOKENS = 0x18, GV_INTS = 0x1c, GV_SHORTS = 0x20, GV_DOUBLES = 0x24 };

/* the token globals: records of 12 bytes {value address, short type, int size}: type restored, field -1,
 * the token pointer and data zeroed */
static void reset_token_globals(cpu *c, uint32_t eng)
{
    for (int32_t k = 0, off = 0; k < (int32_t)rd32(c, eng + GV_NTOKENS); k++, off += 12) {
        uint32_t rec = rd32(c, eng + GV_TOKENS) + (uint32_t)off;
        wr16(c, rd32(c, rec), rd16(c, rec + 4));
        wr16(c, rd32(c, rd32(c, eng + GV_TOKENS) + (uint32_t)off) + 2, 0xffff);
        rec = rd32(c, eng + GV_TOKENS) + (uint32_t)off;
        zero(c, rd32(c, rec) + 4, rd32(c, rec + 8));
    }
}

/* FUN_10131b40: reset all global variables */
void rl_reset_globals(cpu *c, uint32_t eng)
{
    for (int32_t k = 0; k < (int32_t)rd32(c, eng + GV_NSYNC); k++)
        wr32(c, rd32(c, rd32(c, eng + GV_SYNC) + 4u * (uint32_t)k), 0);
    reset_token_globals(c, eng);
    for (int32_t k = 0; k < (int32_t)rd32(c, eng + GV_NINTS); k++)
        wr32(c, rd32(c, rd32(c, eng + GV_INTS) + 4u * (uint32_t)k), 0);
    for (int32_t k = 0; k < (int32_t)rd32(c, eng + GV_NSHORTS); k++)
        wr16(c, rd32(c, rd32(c, eng + GV_SHORTS) + 4u * (uint32_t)k), 0);
    for (int32_t k = 0; k < (int32_t)rd32(c, eng + GV_NDOUBLES); k++) {
        uint32_t p = rd32(c, rd32(c, eng + GV_DOUBLES) + 4u * (uint32_t)k);
        wr32(c, p, 0);
        wr32(c, p + 4, 0);
    }
}

/* FUN_101313d0: start an utterance: every stream in scope with no match yet, the globals 0x34/0x38 at
 * the delta's end and start, flags cleared, the delta set up again if needed (FUN_10138d70), the token
 * globals reset. al 1, or 0 if the delta could not be set up. */
uint32_t rl_utterance_reset(cpu *c, uint32_t sp, uint32_t eng)
{
    wr8(c, RS(eng) + RS_NSCOPE, 0);
    uint32_t n = rd8(c, eng + ENG_NSTREAMS), i = 0;
    if (n) {
        do {
            i++;
            wr8(c, rd32(c, eng + ENG_SCOPE) + i - 1, 0);
            wr8(c, rd32(c, eng + ENG_SLOT) + i - 1, rd8(c, eng + ENG_NSTREAMS));
            wr8(c, i + rd32(c, eng + ENG_SEEN) - 1, 0);
        } while (i < rd8(c, eng + ENG_NSTREAMS));
    }
    wr8(c, rd32(c, eng + ENG_SEEN) + rd8(c, eng + ENG_NSTREAMS), 0);
    wr32(c, rd32(c, eng + 0x34) + 2, rd32(c, WS(eng) + WS_END));
    wr32(c, rd32(c, eng + 0x38) + 2, rd32(c, WS(eng) + WS_START));
    wr8(c, RS(eng) + 0xfe2, 0);
    wr8(c, RS(eng) + 0xfed, 0);
    wr32(c, RS(eng) + RS_11AC, 0);
    uint8_t st = rd8(c, RS(eng) + RS_STATUS);
    if (st == 0xf9 || st == 0xff) {
        uint32_t r = call2(c, sp - 8, f_10138d70, 0x10131474u, eng, 1);
        if (!(r & 0xff)) return r;
        wr32(c, rd32(c, eng + 0x34) + 2, rd32(c, WS(eng) + WS_END));
        wr32(c, rd32(c, eng + 0x38) + 2, rd32(c, WS(eng) + WS_START));
    }
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 0);
    int32_t ntok = (int32_t)rd32(c, eng + GV_NTOKENS);
    reset_token_globals(c, eng);
    return ((ntok <= 0 ? (uint32_t)ntok : 0u) & 0xffffff00u) | 1;    /* eax as the original leaves it */
}

/* FUN_101355e0: start the rules on an utterance: the output's pending count and pointer, then
 * FUN_10141040 and rl_utterance_reset. 1 if both succeed. */
uint32_t rl_start(cpu *c, uint32_t sp, uint32_t eng, int32_t n, uint32_t p)
{
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1d1, (uint32_t)(n - 1));
    if (n > 1) wr32(c, rd32(c, eng + ENG_OUT) + 0x1d5, rd32(c, p + 4));
    else wr32(c, rd32(c, eng + ENG_OUT) + 0x1d5, 0);
    wr32(c, RS(eng) + 0x10fe, 0);
    uint32_t out = rd32(c, eng + ENG_OUT);
    if (!(call3(c, sp - 4, f_10141040, 0x10135634u, eng, rd32(c, out + 0x1d1), rd32(c, out + 0x1d5)) & 0xff))
        return 0;
    if (!(rl_utterance_reset(c, AT(sp - 4, 1), eng) & 0xff)) return 0;
    return 1;
}
