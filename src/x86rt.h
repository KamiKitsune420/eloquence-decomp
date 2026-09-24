/* x86rt - the machine the recompiled engine code runs on (tools/x2c.py emits C against this).
 *
 * A 32-bit x86 in software, as much of it as compiled MSVC code uses: eight registers, lazily computed
 * flags, the x87 stack in exact 80-bit arithmetic (fx80), and a sparse 32-bit address space of 4 KB pages
 * created on first touch. Guest functions become C functions `void f_XXXXXXXX(cpu *c)`; a guest call pushes
 * a return address on the guest stack and calls the C function, a guest `ret` pops it and returns, so the
 * guest stack holds exactly what it held on the real machine.
 */
#ifndef X86RT_H
#define X86RT_H

#include <stdint.h>
#include <string.h>

#include "fx80.h"

#define PAGE_BITS 12
#define PAGE_SIZE (1u << PAGE_BITS)
#define NPAGES (1u << (32 - PAGE_BITS))

typedef struct cpu cpu;
typedef void (*guest_fn)(cpu *c);

struct cpu {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    /* flags: after an arithmetic/logic op, cf and of are computed, zf/sf/pf come from res (fl_size
     * bytes); after sahf/fcomi/popf they are explicit (fl_explicit) */
    uint32_t fl_res;
    uint8_t fl_size, fl_cf, fl_of, fl_explicit, fl_zf, fl_sf, fl_pf, df;
    /* x87 */
    fx80 st[8];
    unsigned ftop;
    uint16_t fcw;
    uint8_t c0, c1, c2, c3;
    /* memory */
    uint8_t **pages;
    uint32_t fs_base;
    /* calls the recompiler could not resolve statically (vtables, callbacks) */
    guest_fn (*resolve)(cpu *c, uint32_t target);
    void *user;
    /* guest heap: size classes of powers of two, free lists, bump pointer */
    uint32_t heap_top, heap_free[32];
    /* per machine, so that several can run: crt.c's guest-side variables (errno, _iob, ...) and the
     * host jmp_bufs standing in for guest ones */
    uint32_t crt_vars[8];
    void *crt_files;                 /* crt.c: the open files (guest FILE * -> host) */
    struct x86_jbtab *jb;
};

/* ---------------------------------------------------------------- memory */
uint8_t *x86_page(cpu *c, uint32_t addr);                  /* creates the page if missing */

static inline uint8_t *MP(cpu *c, uint32_t a)
{
    uint8_t *p = c->pages[a >> PAGE_BITS];
    if (!p) p = x86_page(c, a);
    return p + (a & (PAGE_SIZE - 1));
}

static inline uint8_t rd8(cpu *c, uint32_t a) { return *MP(c, a); }
static inline void wr8(cpu *c, uint32_t a, uint8_t v) { *MP(c, a) = v; }
static inline uint16_t rd16(cpu *c, uint32_t a)
{
    if ((a & (PAGE_SIZE - 1)) <= PAGE_SIZE - 2) { uint16_t v; memcpy(&v, MP(c, a), 2); return v; }
    return (uint16_t)(rd8(c, a) | (rd8(c, a + 1) << 8));
}
static inline uint32_t rd32(cpu *c, uint32_t a)
{
    if ((a & (PAGE_SIZE - 1)) <= PAGE_SIZE - 4) { uint32_t v; memcpy(&v, MP(c, a), 4); return v; }
    return (uint32_t)rd16(c, a) | ((uint32_t)rd16(c, a + 2) << 16);
}
static inline uint64_t rd64(cpu *c, uint32_t a) { return (uint64_t)rd32(c, a) | ((uint64_t)rd32(c, a + 4) << 32); }
static inline void wr16(cpu *c, uint32_t a, uint16_t v)
{
    if ((a & (PAGE_SIZE - 1)) <= PAGE_SIZE - 2) { memcpy(MP(c, a), &v, 2); return; }
    wr8(c, a, (uint8_t)v);
    wr8(c, a + 1, (uint8_t)(v >> 8));
}
static inline void wr32(cpu *c, uint32_t a, uint32_t v)
{
    if ((a & (PAGE_SIZE - 1)) <= PAGE_SIZE - 4) { memcpy(MP(c, a), &v, 4); return; }
    wr16(c, a, (uint16_t)v);
    wr16(c, a + 2, (uint16_t)(v >> 16));
}
static inline void wr64(cpu *c, uint32_t a, uint64_t v) { wr32(c, a, (uint32_t)v); wr32(c, a + 4, (uint32_t)(v >> 32)); }

void x86_write(cpu *c, uint32_t a, const void *src, uint32_t n);
void x86_read(cpu *c, uint32_t a, void *dst, uint32_t n);

static inline void push32(cpu *c, uint32_t v) { c->esp -= 4; wr32(c, c->esp, v); }
static inline uint32_t pop32(cpu *c) { uint32_t v = rd32(c, c->esp); c->esp += 4; return v; }

/* ---------------------------------------------------------------- flags */
static inline uint32_t szmask(int size) { return size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu; }
static inline uint32_t szsign(int size) { return size == 1 ? 0x80u : size == 2 ? 0x8000u : 0x80000000u; }

static inline void fl_set(cpu *c, int size, uint32_t res, int cf, int of)
{
    c->fl_explicit = 0;
    c->fl_size = (uint8_t)size;
    c->fl_res = res & szmask(size);
    c->fl_cf = (uint8_t)(cf != 0);
    c->fl_of = (uint8_t)(of != 0);
}

static inline int F_CF(cpu *c) { return c->fl_cf; }
static inline int F_OF(cpu *c) { return c->fl_of; }
static inline int F_ZF(cpu *c) { return c->fl_explicit ? c->fl_zf : c->fl_res == 0; }
static inline int F_SF(cpu *c) { return c->fl_explicit ? c->fl_sf : (c->fl_res & szsign(c->fl_size)) != 0; }
static inline int F_PF(cpu *c)
{
    if (c->fl_explicit) return c->fl_pf;
    uint8_t b = (uint8_t)c->fl_res;
    b ^= b >> 4; b ^= b >> 2; b ^= b >> 1;
    return !(b & 1);
}

/* arithmetic that sets flags; size in bytes */
static inline uint32_t op_add(cpu *c, int size, uint32_t a, uint32_t b)
{
    uint32_t m = szmask(size), r = (a + b) & m;
    fl_set(c, size, r, r < (a & m), ((a ^ r) & (b ^ r) & szsign(size)) != 0);
    return r;
}
static inline uint32_t op_adc(cpu *c, int size, uint32_t a, uint32_t b)
{
    uint32_t m = szmask(size), cin = c->fl_cf, r = (a + b + cin) & m;
    fl_set(c, size, r, r < (a & m) || (cin && r == (a & m)), ((a ^ r) & (b ^ r) & szsign(size)) != 0);
    return r;
}
static inline uint32_t op_sub(cpu *c, int size, uint32_t a, uint32_t b)
{
    uint32_t m = szmask(size), r = (a - b) & m;
    fl_set(c, size, r, (a & m) < (b & m), ((a ^ b) & (a ^ r) & szsign(size)) != 0);
    return r;
}
static inline uint32_t op_sbb(cpu *c, int size, uint32_t a, uint32_t b)
{
    uint32_t m = szmask(size), cin = c->fl_cf, r = (a - b - cin) & m;
    fl_set(c, size, r, (a & m) < (b & m) || (cin && (a & m) == (b & m)), ((a ^ b) & (a ^ r) & szsign(size)) != 0);
    return r;
}
static inline uint32_t op_logic(cpu *c, int size, uint32_t r) { fl_set(c, size, r, 0, 0); return r & szmask(size); }
static inline uint32_t op_inc(cpu *c, int size, uint32_t a)
{
    uint32_t r = (a + 1) & szmask(size);
    int cf = c->fl_cf;
    fl_set(c, size, r, cf, r == szsign(size));
    return r;
}
static inline uint32_t op_dec(cpu *c, int size, uint32_t a)
{
    uint32_t r = (a - 1) & szmask(size);
    int cf = c->fl_cf;
    fl_set(c, size, r, cf, r == szsign(size) - 1);
    return r;
}
static inline uint32_t op_neg(cpu *c, int size, uint32_t a)
{
    uint32_t m = szmask(size), r = (0 - a) & m;
    fl_set(c, size, r, (a & m) != 0, r == szsign(size));
    return r;
}

uint32_t op_shift(cpu *c, int kind, int size, uint32_t a, uint32_t count);   /* 0 shl 1 shr 2 sar 3 rol 4 ror */
uint32_t op_shd(cpu *c, int left, uint32_t dst, uint32_t src, uint32_t count);  /* shld/shrd, 32-bit */

/* conditions: the x86 condition codes 0..15 (o no b ae e ne be a s ns p np l ge le g) */
int x86_cond(cpu *c, int cc);

/* ---------------------------------------------------------------- x87 */
static inline fx_env FENV(cpu *c) { return fx_env_from_cw(c->fcw); }
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(cpu *c, fx80 v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(cpu *c) { c->ftop = (c->ftop + 1) & 7; }
static inline void fcom_set(cpu *c, fx80 a, fx80 b)
{
    int r = fx_cmp(a, b);
    c->c0 = (uint8_t)(r == -1 || r == 2);
    c->c2 = (uint8_t)(r == 2);
    c->c3 = (uint8_t)(r == 0 || r == 2);
}
static inline void fcomi_set(cpu *c, fx80 a, fx80 b)
{
    int r = fx_cmp(a, b);
    c->fl_explicit = 1;
    c->fl_zf = (uint8_t)(r == 0 || r == 2);
    c->fl_pf = (uint8_t)(r == 2);
    c->fl_cf = (uint8_t)(r == -1 || r == 2);
    c->fl_of = 0;
    c->fl_sf = 0;
}
static inline uint16_t fsw(cpu *c)
{
    return (uint16_t)((c->c0 << 8) | (c->c1 << 9) | (c->c2 << 10) | ((c->ftop & 7) << 11) | (c->c3 << 14));
}
void x86_sahf(cpu *c);
uint32_t x86_lahf(cpu *c);

/* ---------------------------------------------------------------- string instructions (df = 0) */
void x86_rep_movs(cpu *c, int size, int rep);
void x86_rep_stos(cpu *c, int size, int rep);
void x86_repne_scas(cpu *c, int size, int rep);
void x86_repe_cmps(cpu *c, int size, int rep);

/* ---------------------------------------------------------------- calls */
void x86_icall(cpu *c, uint32_t target);            /* an indirect call: the return address is pushed */
void x86_fail(cpu *c, uint32_t at, const char *what);

/* guest functions the recompiled code provides, and C-runtime imports this runtime provides */
typedef struct { uint32_t addr; guest_fn fn; } fn_entry;
extern const fn_entry x86_functions[];
extern const unsigned x86_nfunctions;
guest_fn x86_lookup(uint32_t addr);

/* the image's imports: IAT slot, dll, name, implementation (NULL for data imports) */
typedef struct { uint32_t slot; const char *dll, *name; guest_fn fn; } import_entry;
extern const import_entry x86_imports[];
extern const unsigned x86_nimports;
#define X86_IMPORT_BASE 0xf0000000u      /* what an IAT slot holds: X86_IMPORT_BASE + index */
void x86_bind_imports(cpu *c);          /* fill the IAT (function imports; data imports via crt) */

/* call a guest function from the host: pushes `nargs` 32-bit args (last first) and a return address,
 * runs it, and for cdecl removes the arguments again; returns eax */
uint32_t x86_call(cpu *c, uint32_t addr, int cdecl_, int nargs, const uint32_t *args);

/* guest memory for the host: a deterministic heap (malloc/new), and strings */
uint32_t x86_alloc(cpu *c, uint32_t size);
void     x86_dealloc(cpu *c, uint32_t p);
uint32_t x86_alloc_size(cpu *c, uint32_t p);
uint32_t x86_strdup(cpu *c, const char *s);
void     x86_get_string(cpu *c, uint32_t a, char *out, uint32_t cap);

/* setjmp/longjmp: the host jmp_buf that goes with a guest one */
#include <setjmp.h>
jmp_buf *x86_jmpbuf(cpu *c, uint32_t guest_buf);

/* debugging: functions recompiled with --hook call this first (NULL: nothing) */
extern void (*x86_on_enter)(cpu *c, uint32_t fn);

cpu *x86_new(void);
void x86_free(cpu *c);

#endif
