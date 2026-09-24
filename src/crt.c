/* crt - the functions ENU.SYN imports (MSVCRT, MSVCIRT, ADVAPI32, KERNEL32, USER32, WINMM), on the
 * recompiled machine.
 *
 * Each is entered as the guest's `call` left it: return address at [esp], arguments above. cdecl ones
 * end with `esp += 4` (the caller removes its arguments), stdcall ones also remove theirs. Results go in
 * eax, edx:eax, or st0 for doubles, as on 32-bit Windows.
 *
 * What the engine does not need to speak (files, the registry, iostreams, C++ exceptions) answers as a
 * machine without those things would, or stops with its name so that any use is noticed at once.
 */
#include "x86rt.h"
#include "x87math.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <io.h>

#define ARG(n) rd32(c, c->esp + 4 + 4 * (n))
#define RET_CDECL() (c->esp += 4)
#define RET_STD(nargs) (c->esp += 4 + 4 * (nargs))

/* counters for the maths functions: if they run while speaking, they have to be exact replicas */
long crt_math_calls[3];

static void st0_double(cpu *c, double v)
{
    uint64_t b;
    memcpy(&b, &v, 8);
    fpush(c, fx_from_f64(b));
}

static double arg_double(cpu *c, int n)       /* a double argument starting at argument slot n */
{
    uint64_t b = (uint64_t)ARG(n) | ((uint64_t)ARG(n + 1) << 32);
    double v;
    memcpy(&v, &b, 8);
    return v;
}

/* ---------------------------------------------------------------- data imports */
/* guest addresses of the variables the engine imports, one set per machine (cpu.crt_vars) */
#define g_errno_at    c->crt_vars[0]
#define g_iob_at      c->crt_vars[1]
#define g_pctype_at   c->crt_vars[2]
#define g_mbmax_at    c->crt_vars[3]
#define g_fdiv_at     c->crt_vars[4]
#define g_openprot_at c->crt_vars[5]

uint32_t crt_data_import(cpu *c, const char *name)
{
    if (!strcmp(name, "_pctype")) {
        if (!g_pctype_at) {
            /* the C locale's table for -1..255; _pctype points at the entry for 0 */
            uint32_t t = x86_alloc(c, 257 * 2);
            for (int ch = 0; ch < 256; ch++) {
                uint16_t f = 0;
                if (ch < 128) {
                    if (ch >= 'A' && ch <= 'Z') f |= 0x1 | 0x100;
                    if (ch >= 'a' && ch <= 'z') f |= 0x2 | 0x100;
                    if (ch >= '0' && ch <= '9') f |= 0x4;
                    if (ch == ' ' || (ch >= 9 && ch <= 13)) f |= 0x8;
                    if (ch > 32 && ch < 127 && !((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) f |= 0x10;
                    if (ch < 32 || ch == 127) f |= 0x20;
                    if (ch == ' ') f |= 0x40;
                    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f')) f |= 0x80;
                }
                wr16(c, t + 2 + 2 * (uint32_t)ch, f);
            }
            g_pctype_at = x86_alloc(c, 4);
            wr32(c, g_pctype_at, t + 2);
        }
        return g_pctype_at;
    }
    if (!strcmp(name, "__mb_cur_max")) {
        if (!g_mbmax_at) { g_mbmax_at = x86_alloc(c, 4); wr32(c, g_mbmax_at, 1); }
        return g_mbmax_at;
    }
    if (!strcmp(name, "_iob")) {
        if (!g_iob_at) g_iob_at = x86_alloc(c, 3 * 32);
        return g_iob_at;
    }
    if (!strcmp(name, "_adjust_fdiv")) {
        if (!g_fdiv_at) g_fdiv_at = x86_alloc(c, 4);           /* 0: no Pentium FDIV workaround */
        return g_fdiv_at;
    }
    if (!strcmp(name, "?openprot@filebuf@@2HB")) {
        if (!g_openprot_at) { g_openprot_at = x86_alloc(c, 4); wr32(c, g_openprot_at, 0x1b6); }
        return g_openprot_at;
    }
    x86_fail(c, 0, name);
    return 0;
}

/* ---------------------------------------------------------------- floating point and numbers */
void imp__ftol(cpu *c)
{
    fx_env e = { FX_PC64, FX_RZ };
    int64_t v = fx_to_int(ST(0), e, 64);
    fpop(c);
    c->eax = (uint32_t)v;
    c->edx = (uint32_t)((uint64_t)v >> 32);
    RET_CDECL();
}

void imp_ldiv(cpu *c)
{
    int32_t n = (int32_t)ARG(0), d = (int32_t)ARG(1);
    if (!d) x86_fail(c, 0, "ldiv by zero");
    c->eax = (uint32_t)(n / d);
    c->edx = (uint32_t)(n % d);
    RET_CDECL();
}

/* ELOQ_MATHLOG=file appends "fn x y result" (hex doubles) for checking against MSVCRT (harness/mathcheck.c) */
static void math_log(int fn, double x, double y, double r)
{
    static FILE *f;
    static int tried;
    if (!tried) {
        const char *p = getenv("ELOQ_MATHLOG");
        tried = 1;
        if (p) f = fopen(p, "a");
    }
    if (!f) return;
    uint64_t a, b, v;
    memcpy(&a, &x, 8); memcpy(&b, &y, 8); memcpy(&v, &r, 8);
    fprintf(f, "%d %016llx %016llx %016llx\n", fn, (unsigned long long)a, (unsigned long long)b, (unsigned long long)v);
    fflush(f);
}

/* msvcrt's x87 log/exp/pow leave an extended-precision st0 (x87math.c); the log records it as a double */
static void math_ret(cpu *c, int fn, double x, double y, fx80 r)
{
    crt_math_calls[fn]++;
    fx_env e = { FX_PC53, FX_RN };
    uint64_t b = fx_to_f64(r, e);
    double d;
    memcpy(&d, &b, 8);
    math_log(fn, x, y, d);
    fpush(c, r);
    RET_CDECL();
}

void imp_log(cpu *c) { double x = arg_double(c, 0); math_ret(c, 0, x, 0, x87m_log(x)); }
void imp_exp(cpu *c) { double x = arg_double(c, 0); math_ret(c, 1, x, 0, x87m_exp(x)); }
void imp_pow(cpu *c)
{
    double x = arg_double(c, 0), y = arg_double(c, 2);
    math_ret(c, 2, x, y, x87m_pow(x, y, c->fcw));
}

static void guest_str(cpu *c, uint32_t a, char *buf, size_t cap) { x86_get_string(c, a, buf, (uint32_t)cap); }

/* ELOQ_TEXTLOG=file appends "# atof <text>" / "# strtod <text>": which strings the engine converts */
static void text_log(const char *fn, const char *s)
{
    const char *p = getenv("ELOQ_TEXTLOG");
    if (!p) return;
    FILE *f = fopen(p, "a");
    if (!f) return;
    fprintf(f, "# %s %s\n", fn, s);
    fclose(f);
}

void imp_atof(cpu *c)
{
    char b[256];
    guest_str(c, ARG(0), b, sizeof b);
    text_log("atof", b);
    st0_double(c, atof(b));
    RET_CDECL();
}

void imp_strtod(cpu *c)
{
    char b[256];
    uint32_t s = ARG(0), endp = ARG(1);
    guest_str(c, s, b, sizeof b);
    text_log("strtod", b);
    char *e;
    double v = strtod(b, &e);
    if (endp) wr32(c, endp, s + (uint32_t)(e - b));
    st0_double(c, v);
    RET_CDECL();
}

void imp_atoi(cpu *c)
{
    char b[256];
    guest_str(c, ARG(0), b, sizeof b);
    c->eax = (uint32_t)atol(b);
    RET_CDECL();
}

void imp_atol(cpu *c) { imp_atoi(c); }

void imp_strtol(cpu *c)
{
    char b[256];
    uint32_t s = ARG(0), endp = ARG(1);
    guest_str(c, s, b, sizeof b);
    char *e;
    long v = strtol(b, &e, (int)ARG(2));
    if (endp) wr32(c, endp, s + (uint32_t)(e - b));
    c->eax = (uint32_t)v;
    RET_CDECL();
}

/* ---------------------------------------------------------------- memory and strings */
void imp_malloc(cpu *c) { c->eax = x86_alloc(c, ARG(0)); RET_CDECL(); }
void imp_op_new(cpu *c) { c->eax = x86_alloc(c, ARG(0)); RET_CDECL(); }
void imp_free(cpu *c) { x86_dealloc(c, ARG(0)); RET_CDECL(); }
void imp_op_delete(cpu *c) { x86_dealloc(c, ARG(0)); RET_CDECL(); }

void imp_calloc(cpu *c)
{
    uint32_t n = ARG(0) * ARG(1);
    c->eax = x86_alloc(c, n);       /* already zeroed */
    RET_CDECL();
}

void imp_realloc(cpu *c)
{
    uint32_t p = ARG(0), n = ARG(1);
    if (!p) { c->eax = x86_alloc(c, n); RET_CDECL(); return; }
    if (!n) { x86_dealloc(c, p); c->eax = 0; RET_CDECL(); return; }
    uint32_t old = x86_alloc_size(c, p), q = x86_alloc(c, n);
    for (uint32_t i = 0; i < old && i < n; i++) wr8(c, q + i, rd8(c, p + i));
    x86_dealloc(c, p);
    c->eax = q;
    RET_CDECL();
}

void imp__strdup(cpu *c)
{
    uint32_t s = ARG(0), n = 0;
    while (rd8(c, s + n)) n++;
    uint32_t p = x86_alloc(c, n + 1);
    for (uint32_t i = 0; i <= n; i++) wr8(c, p + i, rd8(c, s + i));
    c->eax = p;
    RET_CDECL();
}

void imp_memcpy(cpu *c)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    for (uint32_t i = 0; i < n; i++) wr8(c, d + i, rd8(c, s + i));
    c->eax = d;
    RET_CDECL();
}

void imp_memmove(cpu *c)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    if (d < s) for (uint32_t i = 0; i < n; i++) wr8(c, d + i, rd8(c, s + i));
    else for (uint32_t i = n; i > 0; i--) wr8(c, d + i - 1, rd8(c, s + i - 1));
    c->eax = d;
    RET_CDECL();
}

void imp_memset(cpu *c)
{
    uint32_t d = ARG(0), v = ARG(1), n = ARG(2);
    for (uint32_t i = 0; i < n; i++) wr8(c, d + i, (uint8_t)v);
    c->eax = d;
    RET_CDECL();
}

void imp_strncpy(cpu *c)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2), i = 0;
    for (; i < n; i++) {
        uint8_t ch = rd8(c, s + i);
        wr8(c, d + i, ch);
        if (!ch) break;
    }
    for (; i < n; i++) wr8(c, d + i, 0);
    c->eax = d;
    RET_CDECL();
}

void imp__stricmp(cpu *c)
{
    uint32_t a = ARG(0), b = ARG(1);
    int r = 0;
    for (uint32_t i = 0;; i++) {
        int x = rd8(c, a + i), y = rd8(c, b + i);
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) { r = x - y; break; }
        if (!x) break;
    }
    c->eax = (uint32_t)r;
    RET_CDECL();
}

void imp_isspace(cpu *c)
{
    int ch = (int)ARG(0);
    c->eax = (ch == ' ' || (ch >= 9 && ch <= 13)) ? 8 : 0;
    RET_CDECL();
}

void imp__isctype(cpu *c)
{
    int ch = (int)ARG(0);
    uint32_t mask = ARG(1), t = rd32(c, crt_data_import(c, "_pctype"));
    c->eax = (ch >= -1 && ch < 256) ? (rd16(c, t + 2 * (uint32_t)ch) & mask) : 0;
    RET_CDECL();
}

/* ---------------------------------------------------------------- sprintf */

/* format with the guest's arguments at `ap` (a guest address); returns the length */
static uint32_t guest_format(cpu *c, uint32_t dst, uint32_t fmt, uint32_t ap)
{
    char out[4096];
    size_t o = 0;
    for (uint32_t i = 0;; i++) {
        char ch = (char)rd8(c, fmt + i);
        if (!ch) break;
        if (ch != '%') { if (o < sizeof out - 1) out[o++] = ch; continue; }
        char spec[32];
        size_t k = 0;
        spec[k++] = '%';
        for (;;) {
            ch = (char)rd8(c, fmt + ++i);
            if (!ch) { i--; break; }
            if (k < sizeof spec - 2) spec[k++] = ch;
            if (strchr("diouxXcseEfgGp%n", ch)) break;
        }
        spec[k] = 0;
        char piece[512];
        int n = 0;
        char conv = spec[k - 1];
        if (conv == '%') n = snprintf(piece, sizeof piece, "%%");
        else if (conv == 's') {
            char s[1024];
            guest_str(c, rd32(c, ap), s, sizeof s);
            ap += 4;
            n = snprintf(piece, sizeof piece, spec, s);
        } else if (strchr("eEfgG", conv)) {
            uint64_t b = rd64(c, ap);
            double v;
            memcpy(&v, &b, 8);
            ap += 8;
            n = snprintf(piece, sizeof piece, spec, v);
        } else if (conv == 'n') {
            wr32(c, rd32(c, ap), (uint32_t)o);
            ap += 4;
        } else {
            /* integers: drop an 'l' or 'h' length (32-bit either way) */
            char s2[32];
            size_t m = 0;
            for (size_t j = 0; spec[j]; j++)
                if (spec[j] != 'l' && spec[j] != 'h') s2[m++] = spec[j];
            s2[m] = 0;
            uint32_t v = rd32(c, ap);
            ap += 4;
            if (conv == 'd' || conv == 'i') n = snprintf(piece, sizeof piece, s2, (int)v);
            else if (conv == 'c') n = snprintf(piece, sizeof piece, s2, (int)(uint8_t)v);
            else n = snprintf(piece, sizeof piece, s2, (unsigned)v);
        }
        for (int j = 0; j < n && o < sizeof out - 1; j++) out[o++] = piece[j];
    }
    out[o] = 0;
    x86_write(c, dst, out, (uint32_t)o + 1);
    return (uint32_t)o;
}

void imp_sprintf(cpu *c)
{
    c->eax = guest_format(c, ARG(0), ARG(1), c->esp + 12);
    RET_CDECL();
}

void imp_vsprintf(cpu *c)
{
    c->eax = guest_format(c, ARG(0), ARG(1), ARG(2));
    RET_CDECL();
}

/* ---------------------------------------------------------------- setjmp / longjmp
 * The guest's jmp_buf holds ebp, ebx, edi, esi, esp (as after the return), the return address and the
 * SEH registration; the host's jmp_buf (x86_jmpbuf) was set in the calling function's frame by the
 * recompiled call site. */
void imp__setjmp3(cpu *c)
{
    uint32_t b = ARG(0);
    wr32(c, b + 0, c->ebp);
    wr32(c, b + 4, c->ebx);
    wr32(c, b + 8, c->edi);
    wr32(c, b + 12, c->esi);
    wr32(c, b + 16, c->esp + 4);
    wr32(c, b + 20, rd32(c, c->esp));
    wr32(c, b + 24, rd32(c, c->fs_base));
    c->eax = 0;
    RET_CDECL();
}

void imp_longjmp(cpu *c)
{
    uint32_t b = ARG(0), v = ARG(1);
    c->ebp = rd32(c, b + 0);
    c->ebx = rd32(c, b + 4);
    c->edi = rd32(c, b + 8);
    c->esi = rd32(c, b + 12);
    c->esp = rd32(c, b + 16);
    wr32(c, c->fs_base, rd32(c, b + 24));
    c->eax = v ? v : 1;
    if (x86_longjmp_hook) x86_longjmp_hook(c, b);
    longjmp(*x86_jmpbuf(c, b), 1);
}

/* ---------------------------------------------------------------- startup and shutdown */
void imp__initterm(cpu *c)
{
    uint32_t p = ARG(0), end = ARG(1);
    uint32_t ret_esp = c->esp;
    for (; p < end; p += 4) {
        uint32_t f = rd32(c, p);
        if (f) x86_call(c, f, 1, 0, NULL);
    }
    c->esp = ret_esp;
    RET_CDECL();
}

void imp___dllonexit(cpu *c) { c->eax = ARG(0); RET_CDECL(); }
void imp__onexit(cpu *c) { c->eax = ARG(0); RET_CDECL(); }
void imp_clock(cpu *c) { c->eax = 0; RET_CDECL(); }
void imp__errno(cpu *c)
{
    if (!g_errno_at) g_errno_at = x86_alloc(c, 4);
    c->eax = g_errno_at;
    RET_CDECL();
}

/* ---------------------------------------------------------------- files (user dictionaries)
 * A guest FILE * is a 32-byte block in guest memory standing for a host FILE * (cpu.crt_files).
 * The engine's own data never comes from files: only eciLoadDict / eciSaveDict get here. */
#define MAX_FILES 16
typedef struct { uint32_t guest; FILE *host; } crt_file;

static crt_file *files_of(cpu *c)
{
    if (!c->crt_files) c->crt_files = calloc(MAX_FILES, sizeof(crt_file));
    return (crt_file *)c->crt_files;
}

static FILE *host_file(cpu *c, uint32_t g)
{
    crt_file *t = files_of(c);
    for (int i = 0; t && i < MAX_FILES; i++)
        if (t[i].guest == g && t[i].host) return t[i].host;
    return NULL;
}

void imp_fopen(cpu *c)
{
    char name[512], mode[16];
    guest_str(c, ARG(0), name, sizeof name);
    guest_str(c, ARG(1), mode, sizeof mode);
    c->eax = 0;
    crt_file *t = files_of(c);
    for (int i = 0; t && i < MAX_FILES; i++)
        if (!t[i].host) {
            FILE *f = fopen(name, mode);
            if (f) { t[i].host = f; t[i].guest = x86_alloc(c, 32); c->eax = t[i].guest; }
            break;
        }
    RET_CDECL();
}

void imp_fclose(cpu *c)
{
    uint32_t g = ARG(0);
    crt_file *t = files_of(c);
    c->eax = 0xffffffffu;
    for (int i = 0; t && i < MAX_FILES; i++)
        if (t[i].guest == g && t[i].host) {
            c->eax = (uint32_t)fclose(t[i].host);
            t[i].host = NULL;
            x86_dealloc(c, g);
            t[i].guest = 0;
        }
    RET_CDECL();
}

void imp_fflush(cpu *c) { FILE *f = host_file(c, ARG(0)); c->eax = f ? (uint32_t)fflush(f) : 0; RET_CDECL(); }

void imp_fputc(cpu *c)
{
    FILE *f = host_file(c, ARG(1));
    c->eax = f ? (uint32_t)fputc((int)ARG(0), f) : (ARG(0) & 0xff);
    RET_CDECL();
}

void imp_fputs(cpu *c)
{
    FILE *f = host_file(c, ARG(1));
    char b[4096];
    guest_str(c, ARG(0), b, sizeof b);
    c->eax = f ? (uint32_t)fputs(b, f) : 0;
    RET_CDECL();
}

void imp_fgetc(cpu *c) { FILE *f = host_file(c, ARG(0)); c->eax = f ? (uint32_t)fgetc(f) : 0xffffffffu; RET_CDECL(); }

void imp_fgets(cpu *c)
{
    uint32_t buf = ARG(0), n = ARG(1);
    FILE *f = host_file(c, ARG(2));
    c->eax = 0;
    if (f && n > 0 && n < (1u << 20)) {
        char *b = (char *)malloc(n);
        if (b && fgets(b, (int)n, f)) {
            x86_write(c, buf, b, (uint32_t)strlen(b) + 1);
            c->eax = buf;
        }
        free(b);
    }
    RET_CDECL();
}

void imp_getchar(cpu *c) { c->eax = 0xffffffffu; RET_CDECL(); }

/* 32-bit msvcrt's struct _stat: dev 4, ino 2, mode 2, nlink 2, uid 2, gid 2, (2), rdev 4, size 4, times 3 x 4 */
void imp__stat(cpu *c)
{
    char name[512];
    guest_str(c, ARG(0), name, sizeof name);
    struct _stat st;
    c->eax = 0xffffffffu;
    if (_stat(name, &st) == 0) {
        uint32_t p = ARG(1);
        for (uint32_t i = 0; i < 36; i++) wr8(c, p + i, 0);
        wr16(c, p + 6, (uint16_t)st.st_mode);
        wr16(c, p + 8, (uint16_t)st.st_nlink);
        wr32(c, p + 20, (uint32_t)st.st_size);
        wr32(c, p + 24, (uint32_t)st.st_atime);
        wr32(c, p + 28, (uint32_t)st.st_mtime);
        wr32(c, p + 32, (uint32_t)st.st_ctime);
        c->eax = 0;
    }
    RET_CDECL();
}

void imp__chmod(cpu *c)
{
    char name[512];
    guest_str(c, ARG(0), name, sizeof name);
    c->eax = (uint32_t)_chmod(name, (int)ARG(1));
    RET_CDECL();
}

/* ---------------------------------------------------------------- Windows: no registry, no timers */
void imp_RegOpenKeyExA(cpu *c) { c->eax = 2; RET_STD(5); }
void imp_RegQueryValueExA(cpu *c) { c->eax = 2; RET_STD(6); }
void imp_RegSetValueExA(cpu *c) { c->eax = 5; RET_STD(6); }
void imp_RegCreateKeyExA(cpu *c) { c->eax = 5; RET_STD(9); }
void imp_RegCloseKey(cpu *c) { c->eax = 0; RET_STD(1); }
void imp_RegQueryInfoKeyA(cpu *c) { c->eax = 2; RET_STD(12); }
void imp_RegDeleteKeyA(cpu *c) { c->eax = 2; RET_STD(2); }
void imp_RegEnumKeyExA(cpu *c) { c->eax = 259; RET_STD(8); }
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall SearchPathA(const char *, const char *, const char *, unsigned long,
                                                          char *, char **);
static uint32_t host_search_path(const char *path, const char *file, const char *ext, char *out, size_t n, char **part)
{
    return (uint32_t)SearchPathA(path, file, ext, (unsigned long)n, out, part);
}
#else
static uint32_t host_search_path(const char *path, const char *file, const char *ext, char *out, size_t n, char **part)
{
    (void)path; (void)ext;
    FILE *f = fopen(file, "rb");
    if (!f || strlen(file) + 1 > n) { if (f) fclose(f); return 0; }
    fclose(f);
    strcpy(out, file);
    char *s = strrchr(out, '/');
    *part = s ? s + 1 : out;
    return (uint32_t)strlen(out);
}
#endif

/* SearchPathA(path, file, ext, n, buf, &filepart): only the dictionary files are looked for */
void imp_SearchPathA(cpu *c)
{
    char path[1024], file[512], ext[64], out[1024];
    uint32_t gpath = ARG(0), gfile = ARG(1), gext = ARG(2), n = ARG(3), gbuf = ARG(4), gpart = ARG(5);
    if (gpath) guest_str(c, gpath, path, sizeof path);
    guest_str(c, gfile, file, sizeof file);
    if (gext) guest_str(c, gext, ext, sizeof ext);
    char *part = NULL;
    uint32_t r = host_search_path(gpath ? path : NULL, file, gext ? ext : NULL, out, sizeof out, &part);
    if (r && r < n) {
        x86_write(c, gbuf, out, r + 1);
        if (gpart) wr32(c, gpart, part ? gbuf + (uint32_t)(part - out) : 0);
    }
    c->eax = r;
    RET_STD(6);
}
void imp_GetModuleFileNameA(cpu *c)
{
    static const char name[] = "C:\\Eloquence\\ENU.SYN";
    uint32_t buf = ARG(1), n = ARG(2), len = (uint32_t)sizeof name - 1;
    if (n) {
        uint32_t k = len < n - 1 ? len : n - 1;
        x86_write(c, buf, name, k);
        wr8(c, buf + k, 0);
        c->eax = k;
    } else c->eax = 0;
    RET_STD(3);
}
void imp_KillTimer(cpu *c) { c->eax = 1; RET_STD(2); }
void imp_timeKillEvent(cpu *c) { c->eax = 0; RET_STD(1); }
void imp_timeEndPeriod(cpu *c) { c->eax = 0; RET_STD(1); }

/* ---------------------------------------------------------------- what must not happen */
#define REFUSE(name) void imp_##name(cpu *c) { x86_fail(c, 0, #name " is not supported"); }
REFUSE(exit)
REFUSE(_purecall)
REFUSE(terminate)
REFUSE(__CxxFrameHandler)
REFUSE(_except_handler3)
/* ---------------------------------------------------------------- MSVCIRT's ifstream / ofstream
 * Only the user dictionaries use them (FUN_1012fc90 load, FUN_1012ffc0 save). The engine reads the
 * stream state inline, so the objects are laid out as MSVCIRT's: [this] = virtual base table whose
 * entry 1 is the ios offset (0xc for ifstream, 8 for ofstream, as the engine's frames have them),
 * ios+8 = state (1 eof, 2 fail, 4 bad), ios+0x34 = lock count (kept >= 0: no _mtlock), this+4 = _fGline
 * (the inlined getline increments it before calling get, which then takes the delimiter and clears it).
 * Constructors get the hidden "construct virtual bases" flag; members are thiscall (this in ecx). */
enum { IOS_EOF = 1, IOS_FAIL = 2, IOS_BAD = 4 };

static uint32_t ios_of(cpu *c, uint32_t obj) { return obj + rd32(c, rd32(c, obj) + 4); }
static void ios_set(cpu *c, uint32_t obj, uint32_t bits) { uint32_t s = ios_of(c, obj) + 8; wr32(c, s, rd32(c, s) | bits); }

static void stream_ctor(cpu *c, uint32_t off)
{
    uint32_t obj = c->ecx;
    uint32_t vbt = x86_alloc(c, 8);
    wr32(c, vbt, 0);
    wr32(c, vbt + 4, off);
    wr32(c, obj, vbt);
    wr32(c, obj + 4, 0);
    for (uint32_t i = 0; i < 0x50; i++) wr8(c, obj + off + i, 0);
    c->eax = obj;
    RET_STD(1);
}

static crt_file *stream_slot(cpu *c, uint32_t obj, int create)
{
    crt_file *t = files_of(c), *free_slot = NULL;
    for (int i = 0; t && i < MAX_FILES; i++) {
        if (t[i].guest == obj && t[i].host) return &t[i];
        if (!t[i].host && !free_slot) free_slot = &t[i];
    }
    return create ? free_slot : NULL;
}

static void stream_close(cpu *c, uint32_t obj)
{
    crt_file *f = stream_slot(c, obj, 0);
    if (!f) return;
    fclose(f->host);
    f->host = NULL;
    f->guest = 0;
}

static void stream_open(cpu *c, const char *mode)
{
    uint32_t obj = c->ecx;
    char name[512];
    guest_str(c, ARG(0), name, sizeof name);
    crt_file *f = stream_slot(c, obj, 1);
    FILE *h = f ? fopen(name, mode) : NULL;
    if (h) { f->host = h; f->guest = obj; }
    else ios_set(c, obj, IOS_FAIL | IOS_BAD);
    RET_STD(3);
}

void imp___0ifstream__QAE_XZ(cpu *c) { stream_ctor(c, 0xc); }
void imp___0ofstream__QAE_XZ(cpu *c) { stream_ctor(c, 8); }
void imp__open_ifstream__QAEXPBDHH_Z(cpu *c) { stream_open(c, "r"); }
void imp__open_ofstream__QAEXPBDHH_Z(cpu *c) { stream_open(c, "w"); }
void imp__close_ifstream__QAEXXZ(cpu *c) { stream_close(c, c->ecx); RET_CDECL(); }
void imp__close_ofstream__QAEXXZ(cpu *c) { stream_close(c, c->ecx); RET_CDECL(); }
/* the destructors get the ios subobject (or, _vbase_destructor, the whole object): close what is open */
static void stream_dtor(cpu *c)
{
    crt_file *t = files_of(c);
    for (int i = 0; t && i < MAX_FILES; i++)
        if (t[i].host && (t[i].guest == c->ecx || t[i].guest + 8 == c->ecx || t[i].guest + 0xc == c->ecx))
            stream_close(c, t[i].guest);
    RET_CDECL();
}
void imp___1ifstream__UAE_XZ(cpu *c) { stream_dtor(c); }
void imp___1ofstream__UAE_XZ(cpu *c) { stream_dtor(c); }
void imp___1ios__UAE_XZ(cpu *c) { RET_CDECL(); }
void imp____Difstream__QAEXXZ(cpu *c) { stream_dtor(c); }
void imp____Dofstream__QAEXXZ(cpu *c) { stream_dtor(c); }

/* istream::get(char *buf, int n, int delim): up to n-1 characters, the delimiter left in the stream */
void imp__get_istream__IAEAAV1_PADHH_Z(cpu *c)
{
    uint32_t obj = c->ecx, buf = ARG(0);
    int n = (int)ARG(1), delim = (int)ARG(2), got = 0;
    crt_file *f = stream_slot(c, obj, 0);
    uint32_t st = rd32(c, ios_of(c, obj) + 8);
    if (!f || st) {
        ios_set(c, obj, IOS_FAIL);
    } else {
        int ch = 0, gline = rd32(c, obj + 4) != 0;      /* _fGline: set by the inlined getline */
        while (got < n - 1 && (ch = fgetc(f->host)) != EOF) {
            if (ch == delim) {
                if (!gline) ungetc(ch, f->host);        /* get leaves it, getline takes it */
                break;
            }
            wr8(c, buf + (uint32_t)got++, (uint8_t)ch);
        }
        if (ch == EOF) ios_set(c, obj, IOS_EOF);
        if (!got && !(gline && ch == delim)) ios_set(c, obj, IOS_FAIL);
    }
    if (n > 0) wr8(c, buf + (uint32_t)got, 0);
    wr32(c, obj + 4, 0);
    c->eax = obj;
    RET_STD(3);
}

/* ostream::operator<<(const char *) and endl */
void imp___6ostream__QAEAAV0_PBD_Z(cpu *c)
{
    uint32_t obj = c->ecx;
    char s[4096];
    guest_str(c, ARG(0), s, sizeof s);
    crt_file *f = stream_slot(c, obj, 0);
    if (f) fputs(s, f->host);
    else ios_set(c, obj, IOS_FAIL);
    c->eax = obj;
    RET_STD(1);
}

void imp__endl__YAAAVostream__AAV1__Z(cpu *c)
{
    uint32_t obj = ARG(0);
    crt_file *f = stream_slot(c, obj, 0);
    if (f) { fputc('\n', f->host); fflush(f->host); }
    c->eax = obj;
    RET_CDECL();
}

void imp__mtlock(cpu *c) { RET_CDECL(); }
void imp__mtunlock(cpu *c) { RET_CDECL(); }
