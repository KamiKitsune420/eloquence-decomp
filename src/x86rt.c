/* x86rt - see x86rt.h */
#include "x86rt.h"

#include <stdio.h>
#include <stdlib.h>

void (*x86_on_enter)(cpu *c, uint32_t fn);

cpu *x86_new(void)
{
    cpu *c = (cpu *)calloc(1, sizeof *c);
    if (!c) abort();
    c->pages = (uint8_t **)calloc(NPAGES, sizeof *c->pages);
    if (!c->pages) abort();
    c->fcw = 0x027f;           /* what the engine runs with: 53-bit precision, round to nearest */
    /* a thread block for fs: [fs:0] is the SEH chain (empty), [fs:0x18] the block itself */
    c->fs_base = 0x7ffd0000u;
    wr32(c, c->fs_base, 0xffffffffu);
    wr32(c, c->fs_base + 0x18, c->fs_base);
    c->esp = 0x08000000u;      /* the stack grows down from 128 MB */
    return c;
}

/* guest jmp_buf address -> the host jmp_buf the recompiled setjmp uses */
struct x86_jbtab { unsigned n; struct { uint32_t guest; jmp_buf *host; } e[512]; };

void x86_free(cpu *c)
{
    if (!c) return;
    for (uint32_t i = 0; i < NPAGES; i++) free(c->pages[i]);
    free(c->pages);
    if (c->jb) {
        for (unsigned i = 0; i < c->jb->n; i++) free(c->jb->e[i].host);
        free(c->jb);
    }
    free(c->crt_files);
    free(c);
}

uint8_t *x86_page(cpu *c, uint32_t addr)
{
    uint32_t i = addr >> PAGE_BITS;
    if (!c->pages[i]) {
        c->pages[i] = (uint8_t *)calloc(1, PAGE_SIZE);
        if (!c->pages[i]) { fprintf(stderr, "x86rt: out of memory\n"); abort(); }
    }
    return c->pages[i];
}

void x86_write(cpu *c, uint32_t a, const void *src, uint32_t n)
{
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint32_t room = PAGE_SIZE - (a & (PAGE_SIZE - 1)), k = n < room ? n : room;
        memcpy(MP(c, a), s, k);
        a += k; s += k; n -= k;
    }
}

void x86_read(cpu *c, uint32_t a, void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        uint32_t room = PAGE_SIZE - (a & (PAGE_SIZE - 1)), k = n < room ? n : room;
        memcpy(d, MP(c, a), k);
        a += k; d += k; n -= k;
    }
}

/* ---------------------------------------------------------------- shifts */
uint32_t op_shift(cpu *c, int kind, int size, uint32_t a, uint32_t count)
{
    uint32_t m = szmask(size), sgn = szsign(size), bits = (uint32_t)size * 8;
    count &= 31;
    a &= m;
    if (!count) return a;                     /* flags unchanged */
    uint32_t r;
    int cf, of;
    switch (kind) {
    case 0:        /* shl */
        r = count >= bits ? 0 : (a << count) & m;
        cf = count <= bits ? (int)((a >> (bits - count)) & 1) : 0;
        of = ((r & sgn) != 0) ^ cf;
        break;
    case 1:        /* shr */
        r = count >= bits ? 0 : a >> count;
        cf = count <= bits ? (int)((a >> (count - 1)) & 1) : 0;
        of = (a & sgn) != 0;
        break;
    case 2: {      /* sar */
        int32_t sa = size == 1 ? (int8_t)a : size == 2 ? (int16_t)a : (int32_t)a;
        if (count >= bits) {
            r = sa < 0 ? m : 0;
            cf = sa < 0;
        } else {
            r = (uint32_t)(sa >> count) & m;
            cf = (int)((sa >> (count - 1)) & 1);
        }
        of = 0;
        break;
    }
    case 3: {      /* rol */
        uint32_t n = count % bits;
        r = n ? ((a << n) | (a >> (bits - n))) & m : a;
        cf = (int)(r & 1);
        of = ((r & sgn) != 0) ^ cf;
        c->fl_cf = (uint8_t)cf;
        c->fl_of = (uint8_t)of;
        return r;               /* rotates leave ZF/SF/PF alone */
    }
    default: {     /* ror */
        uint32_t n = count % bits;
        r = n ? ((a >> n) | (a << (bits - n))) & m : a;
        cf = (r & sgn) != 0;
        of = ((r & sgn) != 0) ^ ((r & (sgn >> 1)) != 0);
        c->fl_cf = (uint8_t)cf;
        c->fl_of = (uint8_t)of;
        return r;
    }
    }
    fl_set(c, size, r, cf, of);
    return r;
}

uint32_t op_shd(cpu *c, int left, uint32_t dst, uint32_t src, uint32_t count)
{
    count &= 31;
    if (!count) return dst;
    uint32_t r;
    int cf;
    if (left) {
        r = (dst << count) | (src >> (32 - count));
        cf = (int)((dst >> (32 - count)) & 1);
    } else {
        r = (dst >> count) | (src << (32 - count));
        cf = (int)((dst >> (count - 1)) & 1);
    }
    fl_set(c, 4, r, cf, ((r ^ dst) & 0x80000000u) != 0);
    return r;
}

int x86_cond(cpu *c, int cc)
{
    int r;
    switch (cc >> 1) {
    case 0: r = F_OF(c); break;
    case 1: r = F_CF(c); break;
    case 2: r = F_ZF(c); break;
    case 3: r = F_CF(c) || F_ZF(c); break;
    case 4: r = F_SF(c); break;
    case 5: r = F_PF(c); break;
    case 6: r = F_SF(c) != F_OF(c); break;
    default: r = F_ZF(c) || F_SF(c) != F_OF(c); break;
    }
    return (cc & 1) ? !r : r;
}

void x86_sahf(cpu *c)
{
    uint32_t ah = (c->eax >> 8) & 0xff;
    c->fl_explicit = 1;
    c->fl_sf = (uint8_t)((ah >> 7) & 1);
    c->fl_zf = (uint8_t)((ah >> 6) & 1);
    c->fl_pf = (uint8_t)((ah >> 2) & 1);
    c->fl_cf = (uint8_t)(ah & 1);
}

uint32_t x86_lahf(cpu *c)
{
    return (uint32_t)((F_SF(c) << 7) | (F_ZF(c) << 6) | (F_PF(c) << 2) | 2 | F_CF(c));
}

/* ---------------------------------------------------------------- string instructions */
void x86_rep_movs(cpu *c, int size, int rep)
{
    uint32_t n = rep ? c->ecx : 1;
    while (n) {
        if (size == 1) wr8(c, c->edi, rd8(c, c->esi));
        else if (size == 2) wr16(c, c->edi, rd16(c, c->esi));
        else wr32(c, c->edi, rd32(c, c->esi));
        c->esi += (uint32_t)size;
        c->edi += (uint32_t)size;
        n--;
        if (rep) c->ecx--;
    }
}

void x86_rep_stos(cpu *c, int size, int rep)
{
    uint32_t n = rep ? c->ecx : 1;
    while (n) {
        if (size == 1) wr8(c, c->edi, (uint8_t)c->eax);
        else if (size == 2) wr16(c, c->edi, (uint16_t)c->eax);
        else wr32(c, c->edi, c->eax);
        c->edi += (uint32_t)size;
        n--;
        if (rep) c->ecx--;
    }
}

static uint32_t rdn(cpu *c, int size, uint32_t a)
{
    return size == 1 ? rd8(c, a) : size == 2 ? rd16(c, a) : rd32(c, a);
}

void x86_repne_scas(cpu *c, int size, int rep)
{
    uint32_t v = c->eax & szmask(size);
    if (!rep) { op_sub(c, size, v, rdn(c, size, c->edi)); c->edi += (uint32_t)size; return; }
    while (c->ecx) {
        op_sub(c, size, v, rdn(c, size, c->edi));
        c->edi += (uint32_t)size;
        c->ecx--;
        if (F_ZF(c)) break;
    }
}

void x86_repe_cmps(cpu *c, int size, int rep)
{
    if (!rep) {
        op_sub(c, size, rdn(c, size, c->esi), rdn(c, size, c->edi));
        c->esi += (uint32_t)size;
        c->edi += (uint32_t)size;
        return;
    }
    while (c->ecx) {
        op_sub(c, size, rdn(c, size, c->esi), rdn(c, size, c->edi));
        c->esi += (uint32_t)size;
        c->edi += (uint32_t)size;
        c->ecx--;
        if (!F_ZF(c)) break;
    }
}

/* ---------------------------------------------------------------- calls */
guest_fn x86_lookup(uint32_t addr)
{
    unsigned lo = 0, hi = x86_nfunctions;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (x86_functions[mid].addr == addr) return x86_functions[mid].fn;
        if (x86_functions[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

void x86_bind_imports(cpu *c)
{
    uint32_t crt_data_import(cpu *c, const char *name);
    for (unsigned i = 0; i < x86_nimports; i++) {
        const import_entry *e = &x86_imports[i];
        wr32(c, e->slot, e->fn ? X86_IMPORT_BASE + i : crt_data_import(c, e->name));
    }
}

uint32_t x86_call(cpu *c, uint32_t addr, int cdecl_, int nargs, const uint32_t *args)
{
    for (int i = nargs - 1; i >= 0; i--) push32(c, args[i]);
    push32(c, 0xfffffff0u);                  /* a return address nobody jumps to */
    x86_icall(c, addr);
    if (cdecl_) c->esp += 4u * (uint32_t)nargs;
    return c->eax;
}

/* ---------------------------------------------------------------- guest heap */
#define HEAP_BASE 0x30000000u
#define HEAP_END  0x70000000u

uint32_t x86_alloc(cpu *c, uint32_t size)
{
    int k = 4;                                /* smallest block: 16 bytes */
    while ((1u << k) < size + 8) k++;
    if (k > 30) x86_fail(c, size, "allocation too large");
    uint32_t p = c->heap_free[k];
    if (p) {
        c->heap_free[k] = rd32(c, p);
    } else {
        if (!c->heap_top) c->heap_top = HEAP_BASE;
        p = c->heap_top;
        c->heap_top += 1u << k;
        if (c->heap_top > HEAP_END) x86_fail(c, size, "guest heap exhausted");
    }
    wr32(c, p, (uint32_t)k);
    wr32(c, p + 4, size);
    /* fresh memory reads as zero on first use; recycled blocks are cleared for determinism */
    for (uint32_t i = 8; i < (1u << k); i += 4) wr32(c, p + i, 0);
    return p + 8;
}

void x86_dealloc(cpu *c, uint32_t p)
{
    if (!p) return;
    uint32_t b = p - 8, k = rd32(c, b);
    if (k < 4 || k > 30) x86_fail(c, p, "free of a pointer the heap did not hand out");
    wr32(c, b, c->heap_free[k]);
    c->heap_free[k] = b;
}

uint32_t x86_alloc_size(cpu *c, uint32_t p) { return rd32(c, p - 4); }

uint32_t x86_strdup(cpu *c, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, p = x86_alloc(c, n);
    x86_write(c, p, s, n);
    return p;
}

void x86_get_string(cpu *c, uint32_t a, char *out, uint32_t cap)
{
    uint32_t i = 0;
    for (; i + 1 < cap; i++) {
        char ch = (char)rd8(c, a + i);
        out[i] = ch;
        if (!ch) return;
    }
    if (cap) out[i] = 0;
}

/* ---------------------------------------------------------------- setjmp */
jmp_buf *x86_jmpbuf(cpu *c, uint32_t guest_buf)
{
    struct x86_jbtab *t = c->jb;
    if (!t && !(t = c->jb = (struct x86_jbtab *)calloc(1, sizeof *t))) abort();
    for (unsigned i = 0; i < t->n; i++)
        if (t->e[i].guest == guest_buf) return t->e[i].host;
    if (t->n == 512) x86_fail(c, guest_buf, "too many jmp_bufs");
    t->e[t->n].guest = guest_buf;
    if (!(t->e[t->n].host = (jmp_buf *)malloc(sizeof(jmp_buf)))) abort();
    return t->e[t->n++].host;
}

void x86_icall(cpu *c, uint32_t target)
{
    if (target >= X86_IMPORT_BASE && target - X86_IMPORT_BASE < x86_nimports) {
        guest_fn f = x86_imports[target - X86_IMPORT_BASE].fn;
        if (f) { f(c); return; }
    }
    guest_fn f = x86_lookup(target);
    if (!f && c->resolve) f = c->resolve(c, target);
    if (!f) x86_fail(c, target, "indirect call to an unknown address");
    f(c);
}

void x86_fail(cpu *c, uint32_t at, const char *what)
{
    fprintf(stderr, "x86rt: %s at 0x%08x (esp 0x%08x)\n", what, at, c ? c->esp : 0);
    abort();
}
