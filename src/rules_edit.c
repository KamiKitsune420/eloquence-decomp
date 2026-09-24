/* rules_edit - inserting and removing sync marks, deleting runs of elements, and keeping the ring of
 * sync marks consistent (ENU.SYN FUN_10136680 .. FUN_10137360), hand-ported.
 *
 * Ring bookkeeping is on when rs+0x10fa is set: each mark's ring flags (word 1, bit 0 "a boundary of a
 * single stream", bit 1 "marked") and its word 2 bit 1 ("orphan": no longer a boundary where it
 * mattered). rs+0x1142 is a byte per stream (streams whose boundaries count as "weak"), ws+0x4f a list of
 * streams (0xff-terminated) the current rule inserts into. With rs+0x10f6 set, a mark left without a
 * boundary is reported (FUN_1012d320, the error hook) and the operation fails.
 *
 * Three records of 13 bytes at ws+0x08, 0x15, 0x22 describe ring runs for FUN_10137360: +0 byte
 * direction (0xff backward, 1 forward, 0, 2 both), +1 int "unflagged mark seen", +5 first mark, +9 last.
 *
 * As in rules.c: memory accesses in the original's order, eax as the original leaves it where callers
 * read it, calls into the machine at the esp the original has there (`sp` is the esp a function was
 * entered with).
 */
#include "rules_int.h"

#define IAT_FREE   0x1014410cu
#define IAT_MALLOC 0x10144110u
#define RING_COUNT 0x101939e0u

void f_1012d320(cpu *c);   /* report an error (the rules' error hook) */
void f_10138ce0(cpu *c);
void f_10138d60(cpu *c);
void f_10136680(cpu *c);
void f_10136970(cpu *c);
void f_10136fa0(cpu *c);
void f_10137360(cpu *c);
void f_10136e00(cpu *c);
void f_101385f0(cpu *c);
void f_101364c0(cpu *c);

enum { RS_RING = 0x10fa, RS_REPORT = 0x10f6, RS_WEAK = 0x1142, WS_INSERT_LIST = 0x4f };
enum { RUN_DIR = 0, RUN_FLAG = 1, RUN_FIRST = 5, RUN_LAST = 9, RUN_SIZE = 13 };
#define WS_RUN(k) (8u + RUN_SIZE * (k))

static uint32_t ringprev_at(cpu *c, uint32_t eng, uint32_t m) { return m + 4 * rd32(c, RS(eng) + RS_BACK) - 8; }
static void set_link(cpu *c, uint32_t a, uint32_t to) { wr32(c, a, (rd32(c, a) & 3) | to); }

/* FUN_10136680: check a mark that is losing boundaries: it must still be flagged (ring bits) and, if it
 * is a boundary of several streams, not only of weak ones; otherwise report (FUN_1012d320 with the text
 * `what`) and fail. al 1 ok, 0 reported. */
uint32_t ring_check_mark(cpu *c, uint32_t sp, uint32_t eng, uint32_t m, uint32_t s, uint32_t what)
{
    uint32_t n = 0, weak = 0;
    if ((rd8(c, m + 4) & 1) || ((rd32(c, m + 4) >> 1) & 1)) {
        uint32_t ns = rd8(c, eng + ENG_NSTREAMS);
        if (!ns) return 1;
        uint32_t rs = RS(eng);
        uint32_t p = m + 4 * rd32(c, rs + RS_BACK);
        for (uint32_t i = 0; i < ns; i++, p += 4) {
            if (!(rd8(c, p) & 1)) continue;
            n++;
            if (rd8(c, i + rd32(c, rs + RS_WEAK))) weak++;
        }
        if ((int32_t)weak >= (int32_t)n || (int32_t)n <= 1) return 1;
    }
    uint32_t args[4] = { eng, m, s & 0xff, what };
    return call_at(c, sp - 0xc, f_1012d320, 0x10136706u, 4, args) & 0xffffff00u;
}

/* FUN_10136720: insert a new sync mark into stream s between element `right` and element `left` (tokens
 * or marks), a boundary of s; with ring bookkeeping, put it into the ring and flag the marks passed
 * (report `0x10192eec` / `0x10192ee8` texts on failure). Returns the mark, 0 on failure. The original
 * keeps the right-hand mark in its eng argument slot (sp + 4) and the previous ring mark in its right
 * slot (sp + 12). */
uint32_t ring_insert_mark(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t right, uint32_t left)
{
    uint32_t m = call_at(c, sp - 0x10, f_10138ce0, 0x1013672eu, 1, &eng);
    if (!m) return 0;
    uint32_t si = s & 0xff;
    uint32_t a = m + 4 * (rd32(c, RS(eng) + RS_BACK) + si);
    wr32(c, a, rd32(c, a) | 1);
    if (rd8(c, rd32(c, RS(eng) + RS_WEAK) + si)) wr32(c, m + 4, rd32(c, m + 4) | 2);
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    uint32_t rmark;
    if (right && is_mark(c, right)) {
        wr32(c, sp + 4, right);
        set_link(c, right + 4 * (rd32(c, RS(eng) + RS_BACK) + si), m);
    } else {
        uint32_t w = rd32(c, right);
        wr32(c, right + 4, m);
        wr32(c, sp + 4, w & ~3u);
    }
    rmark = rd32(c, sp + 4);
    set_link(c, m + 4 * si + 0xc, right);
    uint32_t lmark;
    if (left && is_mark(c, left)) {
        set_link(c, left + 4 * si + 0xc, m);
        lmark = left;
    } else {
        lmark = rd32(c, left + 4) & ~3u;
        wr32(c, left, m);
    }
    set_link(c, m + 4 * (rd32(c, RS(eng) + RS_BACK) + si), left);
    uint32_t rs = RS(eng);
    if (rd32(c, rs + RS_RING)) {
        uint32_t off = 4 * rd32(c, rs + RS_BACK) - 8;
        uint32_t pr = rd32(c, rmark + off) & ~3u;
        wr32(c, sp + 12, pr);
        if (pr != lmark) {
            int orphan = 0;
            if (rd8(c, rd32(c, rs + RS_WEAK) + si)) {
                orphan = 1;
            } else {
                for (uint32_t e = pr; e != lmark; e = rd32(c, e + off) & ~3u) {
                    if (!(rd8(c, e + 4) & 1) && !((rd32(c, e + 4) >> 1) & 1)) {
                        orphan = 1;
                        break;
                    }
                }
            }
            if (!orphan) {
                for (uint32_t e = rd32(c, sp + 12); e != lmark;
                     e = rd32(c, e + 4 * rd32(c, RS(eng) + RS_BACK) - 8) & ~3u) {
                    wr32(c, e + 8, rd32(c, e + 8) | 2);
                    if (rd32(c, RS(eng) + RS_REPORT) && !(rd8(c, e + 4) & 1)) {
                        uint32_t args[4] = { eng, e, rd32(c, sp + 8), 0x10192eecu };
                        if (!(call_at(c, sp - 0x10, f_10136680, 0x10136893u, 4, args) & 0xff)) return 0;
                    }
                }
            } else {
                wr32(c, m + 8, rd32(c, m + 8) | 2);
                if (rd32(c, RS(eng) + RS_REPORT) && !(rd8(c, m + 4) & 1)) {
                    uint32_t args[4] = { eng, m, rd32(c, sp + 8), 0x10192ee8u };
                    if (!(call_at(c, sp - 0x10, f_10136680, 0x10136958u, 4, args) & 0xff)) return 0;
                }
            }
        }
        /* into the ring before the right-hand mark */
        uint32_t off2 = 4 * rd32(c, RS(eng) + RS_BACK) - 8;
        uint32_t r = rd32(c, sp + 4);
        uint32_t p = rd32(c, off2 + r) & ~3u;
        set_link(c, off2 + m, p);
        set_link(c, p + 4, m);
        set_link(c, ringprev_at(c, eng, r), m);
        set_link(c, m + 4, r);
        wr32(c, RING_COUNT, rd32(c, RING_COUNT) + 1);
    }
    wr32(c, RS(eng) + 0x1146, 0);
    return m;
}

/* FUN_10136970: recompute a mark's ring flags from its boundaries: bit 1 (marked) when it is a boundary
 * of a stream in the insert list and no strong stream counted after, bit 0 when it is a boundary of
 * exactly one counted stream */
void ring_flags(cpu *c, uint32_t eng, uint32_t m)
{
    int32_t n = 0;
    int marked = 0;
    uint32_t p = rd32(c, WS(eng) + WS_INSERT_LIST);
    int8_t b = (int8_t)rd8(c, p);
    if (b > -1) {
        uint32_t base = rd32(c, RS(eng) + RS_BACK);
        do {
            if (rd8(c, m + 4 * ((uint32_t)(int32_t)b + base)) & 1) {
                n++;
                marked = 1;
            }
            b = (int8_t)rd8(c, p + 1);
            p++;
        } while (b > -1);
    }
    int32_t i = (int32_t)rd8(c, eng + ENG_NSTREAMS) - 1;
    if (i >= 0) {
        uint32_t rs = RS(eng);
        uint32_t a = m + 4 * (rd32(c, rs + RS_BACK) + (uint32_t)i);
        for (; i >= 0; i--, a -= 4) {
            if (!(rd8(c, a) & 1)) continue;
            if (!rd8(c, rd32(c, rs + RS_WEAK) + (uint32_t)i)) {
                marked = 0;
                n++;
            }
            if (n > 1 && !marked) break;
        }
    }
    uint32_t w = rd32(c, m + 4);
    w = n == 1 ? w | 1 : w & ~1u;
    wr32(c, m + 4, w);
    wr32(c, m + 4, marked ? w | 2 : w & ~2u);
}

/* FUN_10137360: extend a ring run (record `run`): from its first mark in its direction, over the marks
 * that are no boundary of any stream the neighbouring mark is a boundary of; the run's last mark is the
 * last one passed, its flag set if a passed mark had no ring flags. The original keeps its loop index in
 * its run argument slot's low byte (sp + 8). */
void ring_extend(cpu *c, uint32_t sp, uint32_t eng, uint32_t run)
{
    uint32_t buf = sp - 0x64;
    uint8_t dir = rd8(c, run + RUN_DIR);
    uint32_t e = rd32(c, run + RUN_FIRST);
    wr32(c, run + RUN_LAST, e);
    int fwd = dir == 1;
    wr32(c, sp - 0x68, (uint32_t)fwd);
    uint32_t nb = fwd ? rd32(c, e + 4) & ~3u : rd32(c, e + 4 * rd32(c, RS(eng) + RS_BACK) - 8) & ~3u;
    uint8_t ns = rd8(c, eng + ENG_NSTREAMS);
    uint32_t cnt = 0;
    if (ns) {
        uint32_t a = nb + 4 * rd32(c, RS(eng) + RS_BACK);
        for (uint8_t k = 0; k < ns; k++, a += 4)
            if (rd8(c, a) & 1) wr8(c, buf + cnt++, k);
    }
    for (;;) {
        if ((int32_t)cnt > 0) {
            uint32_t base = rd32(c, RS(eng) + RS_BACK);
            uint32_t k = 0;
            for (;;) {
                if (rd8(c, e + 4 * (rd8(c, buf + k) + base)) & 1) return;
                wr8(c, sp + 8, (uint8_t)(k + 1));
                k = rd32(c, sp + 8) & 0xff;
                if ((int32_t)k >= (int32_t)cnt) break;
            }
        }
        if (!(rd8(c, e + 4) & 1) || !((rd32(c, e + 4) >> 1) & 1)) wr32(c, run + RUN_FLAG, 1);
        wr32(c, run + RUN_LAST, e);
        e = fwd ? rd32(c, e + 4 * rd32(c, RS(eng) + RS_BACK) - 8) & ~3u : rd32(c, e + 4) & ~3u;
    }
}

/* FUN_10136fa0: a mark m stops being a boundary of stream s: with ring bookkeeping, find the neighbouring
 * unorphaned marks R (next) and L (previous), decide from their boundaries in the streams from s on which
 * ring runs are affected, extend them (FUN_10137360) and orphan the marks in the chosen run, reporting
 * (0x10192ef0) as FUN_10136680 does. 1, or 0 when the change is not allowed. The original keeps L in its
 * eng slot (sp + 4) and a loop index in its m slot (sp + 8). */
uint32_t ring_unbound(cpu *c, uint32_t sp, uint32_t eng, uint32_t m, uint32_t s)
{
    uint32_t cnt = sp - 0x14, fl1 = sp - 0x10, fl2 = sp - 0xc, rsl = sp - 8, sml = sp - 4;
    wr32(c, fl2, 0);
    wr32(c, cnt, 0);
    wr32(c, fl1, 0);
    uint32_t rs = RS(eng);
    if (!rd32(c, rs + RS_RING)) return 1;
    uint32_t r = rd32(c, m + 4) & ~3u;
    wr32(c, rsl, r);
    while ((rd32(c, r + 8) >> 1) & 1) {
        r = rd32(c, r + 4) & ~3u;
        wr32(c, rsl, r);
    }
    uint32_t base = rd32(c, rs + RS_BACK);
    uint32_t l = rd32(c, m + 4 * base - 8) & ~3u;
    wr32(c, sp + 4, l);
    while ((rd32(c, l + 8) >> 1) & 1) {
        l = rd32(c, l + 4 * base - 8) & ~3u;
        wr32(c, sp + 4, l);
    }
    uint32_t sm = s & 0xff;
    int32_t i = (int32_t)rd8(c, eng + ENG_NSTREAMS) - 1;
    wr32(c, sml, sm);
    if (i >= (int32_t)sm) {
        for (; i >= (int32_t)sm; i--) {
            uint32_t ra = r + 4 * (base + (uint32_t)i), la = l + 4 * (base + (uint32_t)i);
            if (rd8(c, m + 4 * (base + (uint32_t)i)) & 1) {
                if (rd8(c, ra) & 1) {
                    if (rd32(c, fl1) || (rd8(c, la) & 1)) return 0;
                    wr32(c, cnt, rd32(c, cnt) + 1);
                } else if (!rd32(c, fl1) && (rd8(c, la) & 1)) {
                    if (rd32(c, cnt)) return 0;
                    wr32(c, fl1, 1);
                }
            } else if (!rd32(c, fl2) && (rd8(c, ra) & 1) && (rd8(c, la) & 1)) {
                wr32(c, fl2, 1);
            }
        }
        l = rd32(c, sp + 4);
        r = rd32(c, rsl);
        sm = rd32(c, sml);
    }
    wr32(c, sp + 8, 0);
    if ((int32_t)sm > 0) {
        for (uint32_t k = 0;;) {
            uint32_t ra = r + 4 * (base + k), la = l + 4 * (base + k);
            if (rd8(c, m + 4 * (base + k)) & 1) {
                if (rd8(c, ra) & 1) {
                    wr32(c, cnt, rd32(c, cnt) + 1);
                    if (rd32(c, fl1) || (rd8(c, la) & 1)) return 0;
                } else if (!rd32(c, fl1)) {
                    if (rd8(c, la) & 1) {
                        if (rd32(c, cnt)) return 0;
                        wr32(c, fl1, 1);
                    } else if (rd32(c, cnt)) {
                        return 0;
                    }
                }
            } else if (!rd32(c, fl2) && (rd8(c, ra) & 1) && (rd8(c, la) & 1)) {
                wr32(c, fl2, 1);
            }
            k = rd32(c, sp + 8) + 1;
            wr32(c, sp + 8, k);
            if ((int32_t)k >= (int32_t)rd32(c, sml)) break;
        }
        l = rd32(c, sp + 4);
    }
    uint32_t f1 = rd32(c, fl1), n = rd32(c, cnt);
    if (f1 && n) return 0;
    uint32_t ws;
    if (rd32(c, fl2)) {
        wr8(c, WS(eng) + WS_RUN(0) + RUN_DIR, 0);
        wr32(c, WS(eng) + WS_RUN(0) + RUN_LAST, m);
        wr32(c, WS(eng) + WS_RUN(0) + RUN_FIRST, m);
        uint32_t w = rd32(c, m + 4);
        wr32(c, WS(eng) + WS_RUN(0) + RUN_FLAG, (!((w >> 1) & 1) && !(rd8(c, m + 4) & 1)) ? 1 : 0);
        if (f1) {
            wr32(c, WS(eng) + WS_RUN(1) + RUN_FIRST, r);
            wr8(c, WS(eng) + WS_RUN(1) + RUN_DIR, 0xff);
        } else if (n) {
            wr32(c, WS(eng) + WS_RUN(1) + RUN_FIRST, l);
            wr8(c, WS(eng) + WS_RUN(1) + RUN_DIR, 1);
        } else {
            wr8(c, WS(eng) + WS_RUN(1) + RUN_DIR, 2);
        }
    } else if (f1) {
        wr32(c, WS(eng) + WS_RUN(0) + RUN_FIRST, r);
        wr8(c, WS(eng) + WS_RUN(0) + RUN_DIR, 0xff);
        wr32(c, WS(eng) + WS_RUN(1) + RUN_FIRST, m);
        wr8(c, WS(eng) + WS_RUN(1) + RUN_DIR, 1);
    } else {
        if (!n) return 0;
        wr32(c, WS(eng) + WS_RUN(0) + RUN_FIRST, l);
        wr8(c, WS(eng) + WS_RUN(0) + RUN_DIR, 1);
        wr32(c, WS(eng) + WS_RUN(1) + RUN_FIRST, m);
        wr8(c, WS(eng) + WS_RUN(1) + RUN_DIR, 0xff);
    }
    ws = WS(eng);
    if (rd8(c, ws + WS_RUN(0))) call2(c, sp - 0x24, f_10137360, 0x1013722fu, eng, ws + WS_RUN(0));
    ws = WS(eng);
    uint8_t d1 = rd8(c, ws + WS_RUN(1));
    if (d1 == 0xff || d1 == 1) {
        call2(c, sp - 0x24, f_10137360, 0x101372d6u, eng, ws + WS_RUN(1));
    } else if (d1 == 2) {
        if (rd32(c, ws + WS_RUN(0) + RUN_FIRST) == r && rd8(c, ws + WS_RUN(0)) == 0xff) {
            wr32(c, ws + WS_RUN(2) + RUN_FIRST, r);
            wr8(c, WS(eng) + WS_RUN(2), 0xff);
            ws = WS(eng);
            wr32(c, ws + WS_RUN(2) + RUN_LAST, rd32(c, ws + WS_RUN(0) + RUN_LAST));
            ws = WS(eng);
            wr32(c, ws + WS_RUN(2) + RUN_FLAG, rd32(c, ws + WS_RUN(0) + RUN_FLAG));
        } else {
            wr32(c, ws + WS_RUN(2) + RUN_FIRST, r);
            wr8(c, WS(eng) + WS_RUN(2), 0xff);
            call2(c, sp - 0x24, f_10137360, 0x10137292u, eng, WS(eng) + WS_RUN(2));
        }
        wr8(c, WS(eng) + WS_RUN(1), 1);
        wr32(c, WS(eng) + WS_RUN(1) + RUN_FIRST, rd32(c, sp + 4));
        call2(c, sp - 0x24, f_10137360, 0x101372b3u, eng, WS(eng) + WS_RUN(1));
        ws = WS(eng);
        wr32(c, ws + WS_RUN(1) + RUN_FIRST, rd32(c, ws + WS_RUN(2) + RUN_LAST));
        ws = WS(eng);
        wr32(c, ws + WS_RUN(1) + RUN_FLAG, rd32(c, ws + WS_RUN(1) + RUN_FLAG) | rd32(c, ws + WS_RUN(2) + RUN_FLAG));
    }
    ws = WS(eng);
    uint32_t k = rd32(c, ws + WS_RUN(0) + RUN_FLAG) != 0;
    uint32_t run = ws + WS_RUN(k);
    int8_t dir = (int8_t)rd8(c, run);
    uint32_t e = rd32(c, run + RUN_FIRST);
    for (;;) {
        wr32(c, e + 8, rd32(c, e + 8) | 2);
        if (rd32(c, RS(eng) + RS_REPORT) && !(rd8(c, e + 4) & 1)) {
            uint32_t args[4] = { eng, e, rd32(c, sp + 12), 0x10192ef0u };
            if (!(call_at(c, sp - 0x24, f_10136680, 0x10137325u, 4, args) & 0xff)) return 0;
        }
        if (e == rd32(c, run + RUN_LAST)) return 1;
        e = dir >= 0 ? rd32(c, e + 4 * rd32(c, RS(eng) + RS_BACK) - 8) & ~3u : rd32(c, e + 4) & ~3u;
    }
}

/* FUN_10136e00: delete the elements of stream s (ws+0x46) from `from` leftwards to `to`: tokens freed,
 * sync marks unlinked from s (freed if they bound nothing else, else their ring flags recomputed and the
 * ring fixed up by FUN_10136fa0), and the two sides joined. al 1, 0 if `from` is null or the join fails.
 * The original keeps the right-hand side in its eng argument slot (sp + 4). */
uint32_t delete_run(cpu *c, uint32_t sp, uint32_t eng, uint32_t from, uint32_t to)
{
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    if (!from) return 0;
    uint32_t w = rd32(c, from);
    if (w & 2) w = rd32(c, from + 0xc + 4 * (uint32_t)(int32_t)(int8_t)rd8(c, WS(eng) + 0x46));
    uint32_t outer = w & ~3u;
    wr32(c, sp + 4, outer);
    uint32_t e = from, next;
    for (;;) {
        if (!e) return 0;
        if (is_mark(c, e)) {
            int32_t si = (int8_t)rd8(c, WS(eng) + 0x46);
            uint32_t a = e + 4 * ((uint32_t)si + rd32(c, RS(eng) + RS_BACK));
            uint32_t lw = rd32(c, a);
            uint8_t flags = rd8(c, e + 4);
            next = lw & ~3u;
            wr32(c, a, lw & ~1u);
            si = (int8_t)rd8(c, WS(eng) + 0x46);
            wr32(c, e + 0xc + 4 * (uint32_t)si, rd32(c, e + 0xc + 4 * (uint32_t)si) & 3);
            si = (int8_t)rd8(c, WS(eng) + 0x46);
            a = e + 4 * ((uint32_t)si + rd32(c, RS(eng) + RS_BACK));
            wr32(c, a, rd32(c, a) & 3);
            if (!(flags & 1)) {
                call2(c, sp - 0x10, f_10136970, 0x10136ef6u, eng, e);
                uint32_t sb = (c->ecx & 0xffffff00u) | rd8(c, WS(eng) + 0x46);
                call3(c, sp - 0x18, f_10136fa0, 0x10136f04u, eng, e, sb);
                goto step;
            }
            uint32_t rs = RS(eng);
            if (rd32(c, rs + RS_RING)) {
                uint32_t off = 4 * rd32(c, rs + RS_BACK) - 8;
                uint32_t rn = rd32(c, e + 4) & ~3u, pv = rd32(c, off + e) & ~3u;
                set_link(c, off + rn, pv);
                set_link(c, pv + 4, rn);
                wr32(c, RING_COUNT, rd32(c, RING_COUNT) + 1);
            }
        } else {
            next = rd32(c, e + 4) & ~3u;
        }
        call3(c, sp - 0x10, f_10138d60, 0x10136f18u, eng, e, 0);
    step:
        if (e == to) break;
        e = next;
    }
    outer = rd32(c, sp + 4);
    if (outer && is_mark(c, outer)) {
        set_link(c, outer + 4 * ((uint32_t)(int32_t)(int8_t)rd8(c, WS(eng) + 0x46) + rd32(c, RS(eng) + RS_BACK)), next);
    } else {
        if (!next || !is_mark(c, next)) return 0;
        wr32(c, outer + 4, next);
    }
    if (next && is_mark(c, next)) {
        set_link(c, next + 0xc + 4 * (uint32_t)(int32_t)(int8_t)rd8(c, WS(eng) + 0x46), outer);
        return 1;
    }
    wr32(c, next, outer);
    return 1;
}

/* FUN_10136b10: take sync mark m out of stream s (with the run between its neighbours deleted by
 * FUN_10136e00); if both neighbours are tokens and the stream merges (descriptor +0x33), the left
 * token's field 0 is added into the right one's (FUN_101385f0, FUN_101364c0) and the left token freed
 * first. al 1. The original keeps the left element in its eng slot (sp + 4). */
uint32_t unmark(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t m, uint32_t k4)
{
    wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
    wr8(c, WS(eng) + 0x46, (uint8_t)s);
    wr32(c, WS(eng) + 0x3a, m);
    wr32(c, WS(eng) + 0x36, m);
    uint32_t ws = WS(eng);
    uint32_t e = rd32(c, m + 0xc + 4 * (uint32_t)(int32_t)(int8_t)rd8(c, ws + 0x46)) & ~3u;
    if (e && !(rd32(c, e) & 2)) e = rd32(c, e) & ~3u;
    wr32(c, ws + 0x3e, e);
    ws = WS(eng);
    e = rd32(c, m + 4 * ((uint32_t)(int32_t)(int8_t)rd8(c, ws + 0x46) + rd32(c, RS(eng) + RS_BACK))) & ~3u;
    if (e && !is_mark(c, e)) e = rd32(c, e + 4) & ~3u;
    wr32(c, ws + 0x42, e);
    uint32_t si = s & 0xff;
    if (!(rd8(c, m + 4 * (si + rd32(c, RS(eng) + RS_BACK))) & 1)) return 1;
    uint32_t nx = rd32(c, m + 0xc + 4 * si) & ~3u;
    uint32_t pv = rd32(c, m + 4 * (si + rd32(c, RS(eng) + RS_BACK))) & ~3u;
    wr32(c, sp + 4, pv);
    if (!(nx && is_mark(c, nx)) && !(pv && is_mark(c, pv))) {
        uint32_t d = stream_desc(si);
        uint32_t fd = rd32(c, stream_desc((uint32_t)(int32_t)(int8_t)s) + SD_FIELDS);
        uint16_t t = rd16(c, fd + FD_TYPE);
        uint32_t r0 = sp - 0x10, r1 = sp - 8;
        wr16(c, r0 + 4, t);
        wr16(c, r1 + 4, t);
        uint8_t fl = rd8(c, rd32(c, d + SD_FIELDS) + FD_FLAG);
        wr8(c, r0 + 6, fl);
        wr8(c, r1 + 6, fl);
        uint32_t get0 = rd32(c, rd32(c, d + SD_GETTERS));
        wr32(c, r1, icall1(c, sp - 0x20, get0, 0x10136c1au, nx + 8));
        wr32(c, r0, icall1(c, sp - 0x24, rd32(c, rd32(c, d + SD_GETTERS)), 0x10136c2fu, rd32(c, sp + 4) + 8));
        if (rd8(c, d + 0x33)) {
            call3(c, sp - 0x20, f_101385f0, 0x10136c51u, eng, r0, r1);
            uint32_t args[4] = { eng, rd32(c, sp + 8), rd32(c, r1), rd32(c, r0) };
            call_at(c, sp - 0x2c, f_101364c0, 0x10136c66u, 4, args);
            uint32_t left = rd32(c, sp + 4);
            wr32(c, rd32(c, eng + ENG_OUT) + 0x1b5, 1);
            uint32_t pl = rd32(c, left + 4) & ~3u;
            set_link(c, m + 4 * (rd32(c, RS(eng) + RS_BACK) + si), pl);
            set_link(c, pl + 4 * si + 0xc, m);
            call3(c, sp - 0x3c, f_10138d60, 0x10136caau, eng, left, 0);
        }
    }
    ws = WS(eng);
    uint32_t args[4] = { eng, rd32(c, ws + 0x36), rd32(c, ws + 0x3a), k4 };
    call_at(c, sp - 0x20, f_10136e00, 0x10136cc3u, 4, args);
    return 1;
}

/* FUN_10136cd0: the weak-stream flags (rs+0x1142, cleared) and the insert list (ws+0x4f, empty). al 1,
 * 0 if out of memory. */
uint32_t ring_tables_new(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t n = rd8(c, eng + ENG_NSTREAMS);
    uint32_t p = import_call(c, sp - 8, IAT_MALLOC, 0x10136ce7u, 1, &n);
    wr32(c, RS(eng) + RS_WEAK, p);
    n = rd8(c, eng + ENG_NSTREAMS);
    p = import_call(c, sp - 0xc, IAT_MALLOC, 0x10136cfbu, 1, &n);
    wr32(c, WS(eng) + WS_INSERT_LIST, p);
    if (!rd32(c, RS(eng) + RS_WEAK) || !rd32(c, WS(eng) + WS_INSERT_LIST)) return 0;
    for (uint32_t i = 1; i <= rd8(c, eng + ENG_NSTREAMS); i++) wr8(c, rd32(c, RS(eng) + RS_WEAK) + i - 1, 0);
    wr8(c, rd32(c, WS(eng) + WS_INSERT_LIST), 0xff);
    return 1;
}

/* FUN_10136d60 */
void ring_tables_free(cpu *c, uint32_t sp, uint32_t eng)
{
    uint32_t p = rd32(c, WS(eng) + WS_INSERT_LIST);
    if (p) {
        import_call(c, sp - 8, IAT_FREE, 0x10136d79u, 1, &p);
        wr32(c, WS(eng) + WS_INSERT_LIST, 0);
    }
    p = rd32(c, RS(eng) + RS_WEAK);
    if (p) {
        import_call(c, sp - 8, IAT_FREE, 0x10136d96u, 1, &p);
        wr32(c, RS(eng) + RS_WEAK, 0);
    }
}

/* FUN_10136db0: add stream s to the insert list (once) */
void insert_list_add(cpu *c, uint32_t eng, uint32_t s)
{
    uint32_t p = rd32(c, WS(eng) + WS_INSERT_LIST), i = 0;
    int8_t b = (int8_t)rd8(c, p);
    while (b != -1) {
        if ((int32_t)b == (int32_t)s) return;
        b = (int8_t)rd8(c, p + i + 1);
        i++;
    }
    wr8(c, p + i, (uint8_t)s);
    wr8(c, rd32(c, WS(eng) + WS_INSERT_LIST) + i + 1, 0xff);
}

/* ---------------------------------------------------------------- adapters */
#define SP c->esp
#define RET(v) port_ret(c, (v), 0)

PORT_FN(10136680) { RET((c->eax & 0xffffff00u) | ring_check_mark(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10136720) { RET(ring_insert_mark(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10136970) { ring_flags(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10137360) { ring_extend(c, SP, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10136fa0) { RET(ring_unbound(c, SP, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10136e00) { RET((c->eax & 0xffffff00u) | delete_run(c, SP, ARG(0), ARG(1), ARG(2))); }
PORT_FN(10136b10) { RET((c->eax & 0xffffff00u) | unmark(c, SP, ARG(0), ARG(1), ARG(2), ARG(3))); }
PORT_FN(10136cd0) { RET((c->eax & 0xffffff00u) | ring_tables_new(c, SP, ARG(0))); }
PORT_FN(10136d60) { ring_tables_free(c, SP, ARG(0)); RET(c->eax); }
PORT_FN(10136db0) { insert_list_add(c, ARG(0), ARG(1)); RET(c->eax); }
PORT_FN(10136df0) { uint32_t eng = ARG(0); wr8(c, rd32(c, rd32(c, eng + ENG_WS) + WS_INSERT_LIST), 0xff); RET(eng); }
