/* eci_text - see eci_text.h */
#include "eci_text.h"
#include "fx80.h"
#include "x87math.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int eci_voice_range[8][2] = { { 0, 1 }, { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 }, { 0, 100 },
                                    { 0, 250 }, { 0, 100 } };
/* eciSampleRate (5): 0..1 in the original; 2..4 are this port's 22050 / 44100 / 48000 */
const int eci_param_range[17][2] = { { 0, 1 }, { 0, 1 }, { 0, 3 }, { 0, 1 }, { 0, 100 }, { 0, 4 }, { 0, 100 },
                                     { 0, 1 }, { 0, 1 }, { 0, 0x7fffffff }, { 0, 1 }, { 0, 1 }, { 0, 1 },
                                     { 2, 0x7fffffff }, { 220, 0x7fffffff }, { 0, 0x7fffffff },
                                     { 220, 0x7fffffff } };

/* ---------------------------------------------------------------- FUN_1000ddc5 */
static unsigned char eci_map(unsigned char b)
{
    static const unsigned char hi[] = { 129, 130, 131, 132, 134, 135, 136, 137, 139, 141, 142, 143, 144,
                                        155, 157, 158, 159, 255 };
    if (b < 32 && b != 10) return ' ';
    if (b == 127) return ' ';
    if (b == 145 || b == 146) return '\'';
    for (size_t i = 0; i < sizeof hi; i++)
        if (b == hi[i]) return ' ';
    return b;
}

char *eci_prepare(const char *in, int annotations)
{
    size_t n = strlen(in);
    unsigned char *s = (unsigned char *)malloc(n + 1);
    char *o = (char *)malloc(5 * n + 10), *w = o;
    if (!s || !o) { free(s); free(o); return NULL; }
    for (size_t i = 0; i <= n; i++) s[i] = i < n ? eci_map((unsigned char)in[i]) : 0;
    for (size_t i = 0; i < n; ) {
        unsigned char ch = s[i];
        if (ch == '\n') {
            if (i == 0 || s[i - 1] != ' ') s[i] = ' ';
            else i++;
            continue;
        }
        if (annotations && ch == '\\' && (s[i + 1] == '\\' || s[i + 1] == '`')) ch = s[++i];
        else if (annotations && ch == '`') {
            if (s[i + 1] == 'g' || s[i + 1] == 'i' || (s[i + 1] == 'u' && s[i + 2] == 'i')) {
                w += sprintf(w, "\\` ");
                i++;
            } else
                *w++ = (char)s[i++];
            continue;
        }
        if (ch == '|' || ch == '`') w += sprintf(w, "\\%c ", ch);
        else if (ch == '^') w += sprintf(w, "\\\\^ ");
        else if (ch == '\\') w += sprintf(w, "\\\\\\\\");
        else *w++ = (char)ch;
        i++;
    }
    *w = 0;
    free(s);
    return o;
}

/* ---------------------------------------------------------------- FUN_10013080 */
int eci_char_count(const char *s)
{
    size_t n = strlen(s);
    int count = (int)n + (n == 0 || s[n - 1] != ' ');   /* n == 0 reads the byte before; never happens */
    int esc = 0;
    for (size_t i = 0; s[i]; i++) {
        if (esc) esc = 0;
        else if (s[i] == '\\') { count--; esc = 1; }
    }
    return count;
}

/* ---------------------------------------------------------------- FUN_10001923 / FUN_10001a29 */
static fx80 fd(double v) { uint64_t b; memcpy(&b, &v, 8); return fx_from_f64(b); }

int eci_to_real(int p, int v)
{
    if (p < 0 || p > 7) return v;
    if (v > eci_voice_range[p][1]) v = eci_voice_range[p][1];
    if (v < eci_voice_range[p][0]) v = eci_voice_range[p][0];
    fx_env e = { FX_PC53, FX_RN }, rz = { FX_PC53, FX_RZ };
    if (p == 2) {
        fx80 t = fx_mul(fx_from_i64(v), fd(0.06306456), e);
        uint64_t tb = fx_to_f64(t, e);                           /* passed to pow as a double */
        double td;
        memcpy(&td, &tb, 8);
        fx80 r = x87m_pow(2.0, td, 0x027f);
        r = fx_mul(r, fd(4.889761232), e);
        r = fx_add(r, fd(35.11023877), e);
        r = fx_add(r, fx_from_f32(0x3c23d70au), e);            /* 0.01f */
        return (int)fx_to_int(r, rz, 32);
    }
    if (p == 6) {
        fx80 a = fx_add(fx_mul(fx_from_i64(v), fd(1.406), e), fd(70.25), e);
        fx80 b = fx_mul(fx_from_i64((int64_t)v * v), fd(0.014), e);
        fx80 r = fx_add(fx_add(a, b, e), fd(0.5), e);
        return (int)fx_to_int(r, rz, 32);
    }
    if (p == 7) {
        fx80 r = x87m_pow(1.11728696759, (double)v, 0x027f);
        return (int)fx_to_int(r, rz, 32);
    }
    return v;
}

int eci_from_real(int p, int r, int lo, int hi)
{
    if (hi > eci_voice_range[p][1]) hi = eci_voice_range[p][1];
    if (lo < eci_voice_range[p][0]) lo = eci_voice_range[p][0];
    if (!(p == 2 || p == 6 || p == 7)) return r;
    if (lo == hi) return lo;
    if (hi - lo == 1) return eci_to_real(p, hi) - r < r - eci_to_real(p, lo) ? hi : lo;
    int mid = (lo + hi) / 2;
    if (r < eci_to_real(p, mid)) return eci_from_real(p, r, lo, mid);
    return eci_from_real(p, r, mid, hi);
}

void eci_voice_realworld(eci_voice *v, int p)
{
    if (p == -1 || p == 2) v->rw[0] = eci_to_real(2, v->p[2]);
    if (p == -1 || p == 6) v->rw[1] = eci_to_real(6, v->p[6]);
    if (p == -1 || p == 7) v->rw[2] = eci_to_real(7, v->p[7]);
}

/* ---------------------------------------------------------------- FUN_100083c1 */
static int scan(const char *s, int *v, int *n)
{
    *n = 0;
    return sscanf(s, "%i%n", v, n) == 1;
}

static void set_param(int *params, int *params2, int k, int v)
{
    if (v >= eci_param_range[k][0] && v <= eci_param_range[k][1]) {
        params[k] = v;
        if (params2) params2[k] = v;
    }
}

/* FUN_10007ba0: "a[.b[.c]]" -> bytes; returns the characters used (0: none) */
static int parse_lang(const char *s, unsigned *a, unsigned *b, unsigned *c)
{
    int n = 0, k;
    *a = *b = *c = 0;
    if (sscanf(s, "%u%n", a, &k) != 1) return 0;
    n = k;
    if (s[n] == '.' && sscanf(s + n + 1, "%u%n", b, &k) == 1) {
        n += 1 + k;
        if (s[n] == '.' && sscanf(s + n + 1, "%u%n", c, &k) == 1) n += 1 + k;
    }
    return n;
}

void eci_parse_annotations(eci_voice *voice, int *params, eci_voice *voice2, int *params2, int units,
                           char *t, const eci_voice *presets)
{
    size_t i = 0;
    while (t[i]) {
        if (!(t[i] == '`' && (i == 0 || t[i - 1] != '\\'))) { i++; continue; }
        int v, n;
        char c1 = t[i + 1], c2 = c1 ? t[i + 2] : 0;
        if (c1 == 'd' && c2 == 'a') {
            i += 3;
            if (scan(t + i, &v, &n)) { set_param(params, params2, 3, v); i += n; }
        } else if (c1 == 'p' && c2 == 'p') {
            i += 3;
            if (scan(t + i, &v, &n)) { set_param(params, params2, 11, v); i += n; }
        } else if (c1 == 't' && (c2 == 's' || c2 == 'y')) {
            i += 3;
            if (scan(t + i, &v, &n)) { set_param(params, params2, c2 == 's' ? 2 : 10, v); i += n; }
        } else if (c1 == 'l') {
            unsigned a, b, c;
            i += 2;
            n = parse_lang(t + i, &a, &b, &c);
            if (n > 0) {
                if (a == 1 && b == 0) {             /* the only language compiled in: 1.0 */
                    params[9] = (int)(a << 16 | c << 8 | b);
                    if (params2) params2[9] = params[9];
                }
                i += (size_t)n;
            }
        } else if (c1 == 'v' && c2 == 'g') {
            i += 3;
            if (scan(t + i, &v, &n)) {
                if (v >= 0 && v <= 1) { voice->p[0] = v; if (voice2) voice2->p[0] = v; }
                i += n;
            }
        } else if (c1 == 'v' && (c2 == 'h' || c2 == 'f' || c2 == 'r' || c2 == 'y')) {
            int k = c2 == 'h' ? 1 : c2 == 'f' ? 3 : c2 == 'r' ? 4 : 5;
            i += 3;
            if (scan(t + i, &v, &n)) {
                if (v > eci_voice_range[k][1]) v = eci_voice_range[k][1];
                if (v >= eci_voice_range[k][0]) { voice->p[k] = v; if (voice2) voice2->p[k] = v; }
                i += n;
            }
        } else if (c1 == 'v' && (c2 == 'b' || c2 == 's' || c2 == 'v')) {
            int k = c2 == 'b' ? 2 : c2 == 's' ? 6 : 7;
            i += 3;
            if (scan(t + i, &v, &n)) {
                if (v >= 0) {
                    int r = units == 1 ? eci_from_real(k, v, 0, 250) : v;
                    if (r > eci_voice_range[k][1]) r = eci_voice_range[k][1];
                    int digits = 0;
                    for (int x = r; ; x /= 10) { digits++; if (x < 10) break; }
                    if (n < digits) {
                        r = 0;
                        for (int d = 0; d < n; d++) r = r * 10 + 9;
                        digits = n;
                    }
                    voice->p[k] = r;
                    if (voice2) voice2->p[k] = r;
                    char keep = t[i + digits], buf[16];
                    sprintf(buf, "%i", r);
                    memcpy(t + i, buf, (size_t)digits);
                    t[i + digits] = keep;
                    memset(t + i + digits, ' ', (size_t)(n - digits));
                }
                i += n;
            }
        } else if (c1 == 'v') {
            i += 2;
            if (scan(t + i, &v, &n)) {
                if (v > 0 && v < 9 && presets[v - 1].defined) {
                    *voice = presets[v - 1];
                    if (voice2) *voice2 = presets[v - 1];
                }
                i += n;
            }
        } else
            i++;
    }
}
