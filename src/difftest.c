/* difftest - the differential tester: every hand-ported engine function against the recompiled original,
 * call by call, over a whole run of eloq_run.
 *
 *   build\x64\difftest.exe - "text" out.wav            (same arguments and ELOQ_* variables as eloq_run)
 *   DIFFTEST=all | none | 101311a0,10131790 ...        which ported functions to check (default all)
 *   DIFFTEST_VERBOSE=n                                 print the first n mismatches in full (default 3)
 *
 * The functions checked are those of src/ported.h and, in builds with ELOQ_LIFTED, the lifted rules of
 * src/gen/lifted.h (tools/delta_lift.py).
 *
 * Built by tools/build_engine.py (x64|x86) difftest: all of the engine's code compiled with -DX86_WTRACK
 * (memory writes are tracked) and the hand ports with -DDIFFTEST (their adapters are port_XXXXXXXX), in
 * build\<arch>\enu_wt\. tools/difftest.py runs it over the whole test matrix and sums the reports.
 *
 * For each call of a ported function (src/ported.h) that is being checked:
 *   1. the registers (and x87, heap, crt variables) are saved and write tracking starts: the first write to
 *      each 256-byte block of guest memory saves the block's old content (x86_wtrack);
 *   2. the recompiled original f_XXXXXXXX_recomp runs (everything it calls runs recompiled too);
 *   3. its result is saved: registers, and the current content of every block it wrote;
 *   4. the machine is put back (blocks, registers, and the host's own state: the audio collected so far,
 *      the voice-effect state, the maths counters);
 *   5. the hand port runs, on the same state;
 *   6. the two results are compared: eax (per its flags in ported.h: all, al only, none; edx for 64-bit
 *      results), ebx esi edi ebp esp, the x87 control word, stack top and live registers, the guest heap's
 *      state, crt's variables, every byte either run wrote (except the scratch stack below the entry esp),
 *      the audio each produced, and whether and where each left by longjmp.
 * The hand port's state is the one that continues.
 *
 * Nesting: a ported function the hand port calls (through the machine) is checked the same way, one level
 * deeper; its writes are merged into the enclosing level's log afterwards. Inside a recompiled run
 * everything is recompiled and nothing is checked.
 *
 * longjmp: the engine's rules abort with longjmp (FUN_10130e80, FUN_10131790). A longjmp that leaves the
 * function being checked is caught (crt's x86_longjmp_hook), compared like a return, and taken again
 * once the hand port has done the same.
 *
 * Per function the report gives calls, checked calls, mismatches and the first mismatch in detail; the
 * last line is DIFFTEST_RESULT calls C checked K bad B.
 */
#include "x86rt.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* eloq_run itself: its main becomes eloq_main; its state (the audio collected, the voice effects) is
 * the host state saved and compared here */
#define main eloq_main
#include "eloq_run.c"
#undef main

enum { DT_AL = 1, DT_VOID = 2, DT_EDX = 4 };

typedef struct {
    uint32_t addr;
    int flags;
    int enabled;
    int lifted;                     /* from gen/lifted.h */
    long calls, checked, bad;
    char first[1024];
} dt_fn;

static dt_fn g_fns[] = {
#define PORTED(a, fl) { 0x##a##u, fl, 1, 0, 0, 0, 0, "" },
#include "ported.h"
#undef PORTED
#ifdef ELOQ_LIFTED
#define PORTED(a, fl) { 0x##a##u, fl, 1, 1, 0, 0, 0, "" },
#include "gen/lifted.h"           /* the lifted rules (tools/delta_lift.py) */
#endif
#undef PORTED
};
#define NFNS (sizeof g_fns / sizeof g_fns[0])

static void dt_call(cpu *c, dt_fn *e, guest_fn port, guest_fn recomp);

/* the dispatchers: f_XXXXXXXX, called by the engine, runs the check */
#define PORTED(a, fl)                                                                   \
    void port_##a(cpu *c);                                                              \
    void f_##a##_recomp(cpu *c);                                                        \
    void f_##a(cpu *c)                                                                  \
    {                                                                                   \
        static dt_fn *e;                                                                \
        if (!e) for (unsigned i = 0; i < NFNS; i++) if (g_fns[i].addr == 0x##a##u) e = &g_fns[i]; \
        dt_call(c, e, port_##a, f_##a##_recomp);                                        \
    }
#include "ported.h"
#ifdef ELOQ_LIFTED
#include "gen/lifted.h"           /* the lifted rules (tools/delta_lift.py) */
#endif
#undef PORTED

/* ---------------------------------------------------------------- write tracking */
#define BLK (1u << WT_BITS)
#define NBLK (1u << (32 - WT_BITS))

typedef struct { uint32_t blk, old_stamp; uint8_t pre[BLK]; } wlog;

typedef struct level {
    uint32_t serial;
    wlog *log;
    size_t n, cap;
} level;

#define MAXDEPTH 16
static level g_lv[MAXDEPTH];
static int g_depth;                 /* active checks */
static uint32_t *g_stamp;
static uint32_t g_serial;
static int g_recomp;                /* inside a recompiled reference run */
static int g_scratch;               /* DIFFTEST_SCRATCH: compare the scratch below the entry esp too */
static int g_rtrecomp;              /* DIFFTEST_RTRECOMP: the hand ports (not the lifted rules) run recompiled */

static void blk_read(cpu *c, uint32_t blk, uint8_t *out)
{
    uint8_t *p = c->pages[blk >> (PAGE_BITS - WT_BITS)];
    if (p) memcpy(out, p + ((blk << WT_BITS) & (PAGE_SIZE - 1)), BLK);
    else memset(out, 0, BLK);
}

static void blk_write(cpu *c, uint32_t blk, const uint8_t *in)
{
    uint8_t *p = c->pages[blk >> (PAGE_BITS - WT_BITS)];
    if (!p) {
        static const uint8_t zero[BLK];
        if (!memcmp(in, zero, BLK)) return;       /* absent pages read as zero */
        p = x86_page(c, blk << WT_BITS);
    }
    memcpy(p + ((blk << WT_BITS) & (PAGE_SIZE - 1)), in, BLK);
}

static wlog *log_add(level *L)
{
    if (L->n == L->cap) {
        L->cap = L->cap ? 2 * L->cap : 64;
        L->log = (wlog *)realloc(L->log, L->cap * sizeof *L->log);
        if (!L->log) { fprintf(stderr, "difftest: out of memory\n"); exit(3); }
    }
    return &L->log[L->n++];
}

/* DIFFTEST_WATCH=addr (debugging): every write into that address's block is reported with the writer's esp
 * and return address, and the byte's value before the write */
static uint32_t g_watch, g_watch_serial;
static int g_watch_on = -1;

void x86_wtrack(cpu *c, uint32_t a)
{
    level *L = &g_lv[g_depth - 1];
    uint32_t b = a >> WT_BITS;
    if (g_watch_on < 0) {
        const char *e = getenv("DIFFTEST_WATCH");
        g_watch_on = e != NULL;
        if (e) g_watch = (uint32_t)strtoul(e, NULL, 16);
    }
    if (g_watch_on && b == g_watch >> WT_BITS) {
        static uint32_t prev = 0;
        if (prev) fprintf(stderr, "WROTE %s %08x = %08x\n", g_recomp ? "orig" : "port", prev, rd32(c, prev & ~3u));
        prev = a;
        fprintf(stderr, "WATCH %s depth %d write %08x esp %08x ret %08x byte %02x\n", g_recomp ? "orig" : "port",
                g_depth, a, c->esp, rd32(c, c->esp), rd8(c, g_watch));
        {   /* the code addresses on the stack above: the writer's callers */
            int k = 0;
            for (uint32_t sp = c->esp; sp < c->esp + 0x400 && k < 4; sp += 4) {
                uint32_t v = rd32(c, sp);
                if (v >= 0x10001000u && v < 0x10144000u) { fprintf(stderr, "    [%08x] %08x\n", sp, v); k++; }
            }
        }
        if (g_watch_serial == L->serial) { g_stamp[b] = 0; return; }   /* logged at this level already */
        g_watch_serial = L->serial;
        wlog *w = log_add(L);
        w->blk = b;
        w->old_stamp = g_stamp[b];
        blk_read(c, b, w->pre);
        g_stamp[b] = 0;                  /* call again on the next write */
        return;
    }
    wlog *w = log_add(L);
    w->blk = b;
    w->old_stamp = g_stamp[b];
    blk_read(c, b, w->pre);
    g_stamp[b] = L->serial;
}

static void level_push(cpu *c)
{
    if (!g_stamp) {
        g_stamp = (uint32_t *)calloc(NBLK, sizeof *g_stamp);
        if (!g_stamp) { fprintf(stderr, "difftest: out of memory\n"); exit(3); }
    }
    level *L = &g_lv[g_depth++];
    L->serial = ++g_serial;
    L->n = 0;
    c->wt_stamp = g_stamp;
    c->wt_gen = L->serial;
}

/* the finished level's blocks become the enclosing level's (with their content from before, unless the
 * enclosing level already has them) */
static void level_pop(cpu *c)
{
    level *L = &g_lv[--g_depth];
    if (!g_depth) {
        c->wt_stamp = NULL;
        return;
    }
    level *P = &g_lv[g_depth - 1];
    for (size_t i = 0; i < L->n; i++) {
        wlog *w = &L->log[i];
        if (w->old_stamp != P->serial) {
            wlog *v = log_add(P);
            v->blk = w->blk;
            v->old_stamp = w->old_stamp;
            memcpy(v->pre, w->pre, BLK);
        }
        g_stamp[w->blk] = P->serial;
    }
    c->wt_gen = P->serial;
}

/* ---------------------------------------------------------------- the host's state (eloq_run's) */
extern long crt_math_calls[3];

typedef struct {
    size_t pcm_n;
    long math[3];
    int reported;
    uint8_t fx[sizeof g_fx];
} host_state;

static void host_save(host_state *h)
{
    h->pcm_n = g_n;
    memcpy(h->math, crt_math_calls, sizeof h->math);
    h->reported = g_reported;
    memcpy(h->fx, &g_fx, sizeof g_fx);
}

static void host_restore(const host_state *h)
{
    g_n = h->pcm_n;
    memcpy(crt_math_calls, h->math, sizeof h->math);
    g_reported = h->reported;
    memcpy(&g_fx, h->fx, sizeof g_fx);
}

/* ---------------------------------------------------------------- the check */
typedef struct {
    jmp_buf jb;
    uint32_t entry_esp;
    uint32_t lj_buf;
} frame;

static frame *g_frame_stack[MAXDEPTH];

static void lj_hook(cpu *c, uint32_t buf)
{
    if (!g_depth) return;
    frame *f = g_frame_stack[g_depth - 1];
    if (c->esp > f->entry_esp) {                  /* the jump leaves the function being checked */
        f->lj_buf = buf;
        longjmp(f->jb, 1);
    }
}

static void keep_host_fields(cpu *c, const cpu *now)
{
    c->pages = now->pages;
    c->resolve = now->resolve;
    c->user = now->user;
    c->crt_files = now->crt_files;
    c->jb = now->jb;
    c->wt_stamp = now->wt_stamp;
    c->wt_gen = now->wt_gen;
}

static int fx_eq(fx80 a, fx80 b) { return a.sig == b.sig && a.exp == b.exp && a.sign == b.sign; }

static long g_verbose_left = 3;
static long g_total_calls, g_total_checked, g_total_bad;

#define REPORT(...) do {                                                              \
        if (!bad++) len = snprintf(msg, sizeof msg, "call %ld: ", e->calls);          \
        if (len < (int)sizeof msg - 1) len += snprintf(msg + len, sizeof msg - len, __VA_ARGS__); \
    } while (0)

static void dt_call(cpu *c, dt_fn *e, guest_fn port, guest_fn recomp)
{
    if (g_recomp || (g_rtrecomp && !e->lifted)) {
        static int calllog = -1;         /* debugging: DIFFTEST_CALLLOG=1 logs these calls inside a check */
        if (calllog < 0) calllog = getenv("DIFFTEST_CALLLOG") != NULL;
        if (calllog && g_depth) {
            uint32_t a0 = rd32(c, c->esp + 4), a1 = rd32(c, c->esp + 8), ra = rd32(c, c->esp);
            recomp(c);
            fprintf(stderr, "CALL %s %08x ret %08x (%08x, %08x) -> %08x\n", g_recomp ? "orig" : "port", e->addr, ra,
                    a0, a1, c->eax);
            return;
        }
        recomp(c);
        return;
    }
    e->calls++;
    g_total_calls++;
    if (!e->enabled || g_depth >= MAXDEPTH) { port(c); return; }
    e->checked++;
    if (g_watch_on > 0) fprintf(stderr, "CHECK %08x call %ld\n", e->addr, e->calls);
    if (getenv("DIFFTEST_WATCH_RS") && g_depth == 0) {       /* debugging: watch the rule cursor */
        g_watch = rd32(c, rd32(c, c->esp + 4) + 0x5c) + 0xfc6;
        g_watch_on = 1;
        fprintf(stderr, "WATCH_RS %08x\n", g_watch);
    }
    g_total_checked++;

    uint32_t entry = c->esp;
    cpu pre = *c;
    host_state hpre, ha, hb;
    host_save(&hpre);
    frame fr;
    fr.entry_esp = entry;
    level_push(c);
    g_frame_stack[g_depth - 1] = &fr;
    level *L = &g_lv[g_depth - 1];

    /* the original */
    volatile int a_lj = 0;
    g_recomp = 1;
    if (!setjmp(fr.jb)) recomp(c);
    else a_lj = 1;
    g_recomp = 0;
    uint32_t a_buf = fr.lj_buf;
    cpu A = *c;
    host_save(&ha);
    size_t na = L->n;
    uint8_t *adata = (uint8_t *)malloc(na * BLK + 1);
    int16_t *apcm = NULL;
    size_t apcm_n = g_n > hpre.pcm_n ? g_n - hpre.pcm_n : 0;
    if (apcm_n) {
        apcm = (int16_t *)malloc(apcm_n * sizeof *apcm);
        memcpy(apcm, g_pcm + hpre.pcm_n, apcm_n * sizeof *apcm);
    }
    for (size_t i = 0; i < na; i++) blk_read(c, L->log[i].blk, adata + i * BLK);

    /* back to where it started */
    for (size_t i = na; i-- > 0;) blk_write(c, L->log[i].blk, L->log[i].pre);
    {
        cpu now = *c;
        *c = pre;
        keep_host_fields(c, &now);
    }
    host_restore(&hpre);

    /* the hand port */
    volatile int b_lj = 0;
    fr.lj_buf = 0;
    if (!setjmp(fr.jb)) port(c);
    else b_lj = 1;
    L = &g_lv[g_depth - 1];
    uint32_t b_buf = fr.lj_buf;
    host_save(&hb);

    /* compare */
    char msg[1024];
    int len = 0, bad = 0;
    if (a_lj != b_lj) REPORT("original %s, port %s; ", a_lj ? "longjmp" : "returned", b_lj ? "longjmp" : "returned");
    else if (a_lj && a_buf != b_buf) REPORT("longjmp to %08x vs %08x; ", a_buf, b_buf);
    uint32_t mask = (e->flags & DT_VOID) ? 0 : (e->flags & DT_AL) ? 0xffu : 0xffffffffu;
    if ((A.eax ^ c->eax) & mask) REPORT("eax %08x vs %08x; ", A.eax, c->eax);
    if ((e->flags & DT_EDX) && A.edx != c->edx) REPORT("edx %08x vs %08x; ", A.edx, c->edx);
    if (A.ebx != c->ebx) REPORT("ebx %08x vs %08x; ", A.ebx, c->ebx);
    if (A.esi != c->esi) REPORT("esi %08x vs %08x; ", A.esi, c->esi);
    if (A.edi != c->edi) REPORT("edi %08x vs %08x; ", A.edi, c->edi);
    if (A.ebp != c->ebp) REPORT("ebp %08x vs %08x; ", A.ebp, c->ebp);
    if (A.esp != c->esp) REPORT("esp %08x vs %08x; ", A.esp, c->esp);
    if (A.fcw != c->fcw) REPORT("fcw %04x vs %04x; ", A.fcw, c->fcw);
    if (A.ftop != c->ftop) REPORT("x87 top %u vs %u; ", A.ftop, c->ftop);
    else {
        unsigned live = (pre.ftop - A.ftop) & 7;       /* registers pushed by the call */
        for (unsigned i = 0; i < live; i++)
            if (!fx_eq(A.st[(A.ftop + i) & 7], c->st[(c->ftop + i) & 7])) REPORT("st%u differs; ", i);
    }
    if (A.heap_top != c->heap_top || memcmp(A.heap_free, c->heap_free, sizeof A.heap_free))
        REPORT("heap state differs (top %08x vs %08x); ", A.heap_top, c->heap_top);
    if (memcmp(A.crt_vars, c->crt_vars, sizeof A.crt_vars)) REPORT("crt variables differ; ");
    if (memcmp(ha.math, hb.math, sizeof ha.math)) REPORT("maths call counts differ; ");
    if (memcmp(ha.fx, hb.fx, sizeof ha.fx) || ha.reported != hb.reported) REPORT("host state differs; ");
    {
        size_t bn = g_n > hpre.pcm_n ? g_n - hpre.pcm_n : 0;
        if (bn != apcm_n || (bn && memcmp(apcm, g_pcm + hpre.pcm_n, bn * sizeof *apcm)))
            REPORT("audio differs (%zu vs %zu samples); ", apcm_n, bn);
    }
    unsigned nd = 0;
    for (size_t i = 0; i < L->n; i++) {
        const uint8_t *ra = i < na ? adata + i * BLK : L->log[i].pre;
        uint8_t rb[BLK];
        blk_read(c, L->log[i].blk, rb);
        if (!memcmp(ra, rb, BLK)) continue;
        uint32_t base = L->log[i].blk << WT_BITS;
        for (unsigned k = 0; k < BLK; k++) {
            uint32_t at = base + k;
            if (ra[k] == rb[k]) continue;
            if (at < entry && at >= entry - 0x01000000u) {
                /* the scratch below the entry esp: compared only with DIFFTEST_SCRATCH=1 (debugging: the
                 * uninitialized bytes a later caller reads) */
                if (!g_scratch) continue;
                if (nd++ < 6) REPORT("[esp-%x=%08x] %02x vs %02x; ", entry - at, at, ra[k], rb[k]);
                continue;
            }
            if (nd++ < 6) REPORT("[%08x] %02x vs %02x; ", at, ra[k], rb[k]);
        }
    }
    if (nd > 6) REPORT("(%u bytes differ); ", nd);
    if (bad) {
        e->bad++;
        g_total_bad++;
        if (!e->first[0]) snprintf(e->first, sizeof e->first, "%s", msg);
        if (g_verbose_left > 0) {
            g_verbose_left--;
            fprintf(stderr, "DIFFTEST MISMATCH %08x %s\n", e->addr, msg);
        }
    }
    free(adata);
    free(apcm);

    level_pop(c);
    if (b_lj) {                                   /* leave the way the port did */
        lj_hook(c, b_buf);
        longjmp(*x86_jmpbuf(c, b_buf), 1);
    }
}

static void dt_setup(void)
{
    const char *s = getenv("DIFFTEST");
    if (s && strcmp(s, "all")) {
        for (unsigned i = 0; i < NFNS; i++) g_fns[i].enabled = 0;
        if (strcmp(s, "none")) {
            char buf[4096];
            snprintf(buf, sizeof buf, "%s", s);
            for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", ")) {
                uint32_t a = (uint32_t)strtoul(t, NULL, 16);
                int found = 0;
                for (unsigned i = 0; i < NFNS; i++) if (g_fns[i].addr == a) g_fns[i].enabled = found = 1;
                if (!found) fprintf(stderr, "difftest: %s is not a ported function (src/ported.h)\n", t);
            }
        }
    }
    g_rtrecomp = getenv("DIFFTEST_RTRECOMP") != NULL;
    g_scratch = getenv("DIFFTEST_SCRATCH") != NULL;
    const char *v = getenv("DIFFTEST_VERBOSE");
    if (v) g_verbose_left = atol(v);
    x86_longjmp_hook = lj_hook;
}

static void dt_report(void)
{
    for (unsigned i = 0; i < NFNS; i++) {
        dt_fn *e = &g_fns[i];
        if (!e->calls) continue;
        fprintf(stderr, "DIFFTEST %08x calls %ld checked %ld bad %ld%s%s\n", e->addr, e->calls, e->checked, e->bad,
                e->first[0] ? " first: " : "", e->first);
    }
    fprintf(stderr, "DIFFTEST_RESULT calls %ld checked %ld bad %ld\n", g_total_calls, g_total_checked, g_total_bad);
}

int main(int argc, char **argv)
{
    if (argc > 4) {
        fprintf(stderr, "difftest: no frames file (its writes could not be taken back)\n");
        return 2;
    }
    dt_setup();
    int r = eloq_main(argc, argv);
    dt_report();
    return r;
}
