/* eloq_run - run the recompiled ENU.SYN the way ECI.DLL drives it, and write what it says.
 *
 *   eloq_run pkg/ENU.SYN "text" out.wav
 *
 * The call sequence is the one `ecisay --trace-engine` recorded from the real ECI.DLL (notes/port.md):
 * DllMain, getObject(1), the interface's start / callbacks / annotation strings, then the text.
 * Callbacks into "ECI" are host functions at addresses the engine is given (0xe0000000 + n).
 */
#include "x86rt.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { M_ADDREF = 1, M_RELEASE, M_START, M_END, M_PROCESS_SENTENCES, M_PROCESS_REMAINING, M_GETLASTERROR,
       M_RESTART, M_READPHONEMES, M_READERROR, M_FLUSH, M_CLEARINPUT, M_SETABORT, M_OUTPUTPLAYING, M_PAUSE,
       M_SYNTH_TO_FILE, M_SYNTH_TO_CALLBACK, M_DURATION_CB, M_WORD_CB, M_INDEX_CB, M_PHONEME_CB, M_ANNO_CB,
       M_INSERT_INDEX, M_INSERT_DELAYED_INDEX, M_WANT_PHONEME_INDICES, M_CLOSE };

#define CB_SYNTH   0xe0000001u
#define CB_WORD    0xe0000002u
#define CB_INDEX   0xe0000003u
#define CB_PHONEME 0xe0000004u
#define CB_ANNO    0xe0000005u
#define USER       0x0badc0deu

static int16_t *g_pcm;
static size_t g_n, g_cap;
static int g_reported;

static void keep(cpu *c, uint32_t p, uint32_t n)
{
    if (g_n + n > g_cap) {
        g_cap = (g_n + n) * 2 + 4096;
        g_pcm = (int16_t *)realloc(g_pcm, g_cap * sizeof *g_pcm);
    }
    for (uint32_t i = 0; i < n; i++) {          /* int32 samples; ECI.DLL clips them to 16 bits */
        int32_t v = (int32_t)rd32(c, p + 4 * i);
        g_pcm[g_n++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

/* cdecl (a, b, user): which of a, b is the count and which the samples is found out on the first call */
static void cb_synth(cpu *c)
{
    uint32_t a = rd32(c, c->esp + 4), b = rd32(c, c->esp + 8);
    if (!g_reported) {
        fprintf(stderr, "synth callback: a=%08x b=%08x user=%08x\n", a, b, rd32(c, c->esp + 12));
        g_reported = 1;
    }
    if (a < 0x100000 && b > 0x100000) keep(c, b, a);
    else if (b < 0x100000 && a > 0x100000) keep(c, a, b);
    c->eax = 1;
    c->esp += 4;
}

static void cb_other(cpu *c) { c->eax = 1; c->esp += 4; }

static guest_fn resolve(cpu *c, uint32_t t)
{
    (void)c;
    if (t == CB_SYNTH) return cb_synth;
    if (t >= 0xe0000000u && t <= 0xe00000ffu) return cb_other;
    return NULL;
}

static uint32_t g_obj;
static FILE *g_frames;

static float g_rate;       /* ELOQ_RATE: run the synthesizer at another sample rate (an experiment) */
static float g_formant;    /* ELOQ_FORMANT: scale the formant frequencies (an experiment) */

/* ELOQ_FX: voice effects on every synthesizer frame (an experiment). Comma-separated:
 *   formant=S  scale F1..F8        bw=S       scale their bandwidths     pitch=S   scale F0
 *   mono=HZ    one pitch            vib=C:HZ   vibrato, cents and rate    jitter=C  random pitch wander
 *   whisper    no voicing, aspiration instead                        breath=DB more aspiration when voiced
 *   oq=V       open quotient (56: normal; low = buzzy, high = soft)
 * Frame slots: 0 length ms, 1 F0*10, 2 voicing amplitude, 3 open quotient, 7 aspiration, 8 frication,
 * 9/10 13/14 15/16 17/18 .. 25/26 F1..F8 and bandwidths. */
static struct {
    int on, whisper;
    float formant, bw, pitch, mono, vib_c, vib_hz, jitter, breath, oq;
    double t, wander;
    unsigned rng;
} g_fx = { 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 12345 };

static void fx_parse(const char *s)
{
    char buf[512];
    strncpy(buf, s, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *k = strtok(buf, ","); k; k = strtok(NULL, ",")) {
        char *v = strchr(k, '=');
        float x = v ? (float)atof(v + 1) : 0;
        if (!strncmp(k, "formant", 7)) g_fx.formant = x;
        else if (!strncmp(k, "bw", 2)) g_fx.bw = x;
        else if (!strncmp(k, "pitch", 5)) g_fx.pitch = x;
        else if (!strncmp(k, "mono", 4)) g_fx.mono = x;
        else if (!strncmp(k, "vib", 3) && v) { g_fx.vib_c = x; char *r = strchr(v, ':'); g_fx.vib_hz = r ? (float)atof(r + 1) : 5; }
        else if (!strncmp(k, "jitter", 6)) g_fx.jitter = x;
        else if (!strncmp(k, "whisper", 7)) g_fx.whisper = 1;
        else if (!strncmp(k, "breath", 6)) g_fx.breath = x;
        else if (!strncmp(k, "oq", 2)) g_fx.oq = x;
    }
    g_fx.on = 1;
}

static void fx_frame(cpu *c)
{
    uint32_t frame = rd32(c, c->esp + 8);
    float f[64];
    x86_read(c, frame, f, sizeof f);
    static const int fslot[8] = { 9, 13, 15, 17, 19, 21, 23, 25 };
    for (int k = 0; k < 8; k++) { f[fslot[k]] *= g_fx.formant; f[fslot[k] + 1] *= g_fx.bw; }
    double f0 = f[1] / 10.0 * g_fx.pitch;
    if (g_fx.mono > 0) f0 = g_fx.mono;
    if (g_fx.vib_c != 0) f0 *= pow(2.0, g_fx.vib_c / 1200.0 * sin(2 * 3.14159265358979 * g_fx.vib_hz * g_fx.t));
    if (g_fx.jitter != 0) {
        g_fx.rng = g_fx.rng * 1103515245u + 12345u;
        g_fx.wander = 0.9 * g_fx.wander + 0.1 * (((g_fx.rng >> 16) & 0x7fff) / 16384.0 - 1.0) * g_fx.jitter * 4;
        f0 *= pow(2.0, g_fx.wander / 1200.0);
    }
    f[1] = (float)(f0 * 10.0);
    if (g_fx.whisper) {
        if (f[2] + 14 > f[7]) f[7] = f[2] + 14;    /* aspiration is ~12 dB weaker than voicing at the same number */
        f[2] = 0;
    } else if (g_fx.breath != 0 && f[2] > 0) {
        f[7] += g_fx.breath;
    }
    if (g_fx.oq > 0) f[3] = g_fx.oq;
    g_fx.t += f[0] / 1000.0;
    x86_write(c, frame, f, sizeof f);
}

/* with a frames file: every frame the synthesizer receives (64 floats at [esp+8]).
 * With ELOQ_RATE: the synthesizer's init (FUN_1013c7f0) gets its config by value at [esp+8], the
 * sample rate as a float at +4; everything rate-dependent is derived from it there. */
static void on_enter(cpu *c, uint32_t fn)
{
    if (fn == 0x1013c7f0u && g_rate > 0) {
        uint32_t bits;
        memcpy(&bits, &g_rate, 4);
        wr32(c, c->esp + 8 + 4, bits);
        return;
    }
    if (fn == 0x1013caf0u && g_fx.on) fx_frame(c);
    if (fn == 0x1013caf0u && g_formant > 0) {
        /* ELOQ_FORMANT: scale the formant frequencies F1..F8 (frame floats 9, 13, 15, 17, .. 25) */
        static const int slots[8] = { 9, 13, 15, 17, 19, 21, 23, 25 };
        uint32_t frame = rd32(c, c->esp + 8);
        for (int k = 0; k < 8; k++) {
            uint32_t a = frame + 4u * (uint32_t)slots[k], bits = rd32(c, a);
            float v;
            memcpy(&v, &bits, 4);
            v *= g_formant;
            memcpy(&bits, &v, 4);
            wr32(c, a, bits);
        }
    }
    if (fn != 0x1013caf0u || !g_frames) return;
    uint32_t frame = rd32(c, c->esp + 8);
    float f[64];
    x86_read(c, frame, f, sizeof f);
    fwrite(f, 4, 64, g_frames);
}

static uint32_t method(cpu *c, int slot, int nargs, uint32_t a1, uint32_t a2)
{
    uint32_t args[3] = { g_obj, a1, a2 };
    uint32_t fn = rd32(c, rd32(c, g_obj) + 4u * (uint32_t)slot);
    return x86_call(c, fn, 0, nargs + 1, args);
}

static uint32_t text_method(cpu *c, int slot, const char *s)
{
    uint32_t p = x86_strdup(c, s);
    uint32_t r = method(c, slot, 1, p, 0);
    x86_dealloc(c, p);
    return r;
}

/* ECI.DLL's text preparation (FUN_1000ddc5 and helpers) before the text reaches the engine.
 * Bytes are mapped through its table (controls and some cp1252 bytes to space, 0x91/0x92 to '),
 * a newline becomes a space unless a space precedes it (then it is dropped), and the characters the
 * engine treats specially are escaped:  | -> "\| "   ^ -> "\\^ "   \ -> "\\\\"   and with annotations
 * off also ` -> "\` ".  With annotations on, a backtick stays an annotation unless it starts `g, `i or
 * `ui, and a backslash before \ or ` just quotes that character. */
static const unsigned char eci_map_hi[] = { 129, 130, 131, 132, 134, 135, 136, 137, 139, 141, 142, 143, 144,
                                            155, 157, 158, 159, 255 };

static unsigned char eci_map(unsigned char b)
{
    if (b < 32 && b != 10) return ' ';
    if (b == 127) return ' ';
    if (b == 145 || b == 146) return '\'';
    for (size_t i = 0; i < sizeof eci_map_hi; i++)
        if (b == eci_map_hi[i]) return ' ';
    return b;
}

static char *eci_prepare(const char *in, int annotations)
{
    size_t n = strlen(in);
    unsigned char *s = (unsigned char *)malloc(n + 1);
    char *o = (char *)malloc(5 * n + 10), *w = o;
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

static void put(FILE *f, uint32_t v, int n) { for (int i = 0; i < n; i++) fputc((v >> (8 * i)) & 0xff, f); }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: eloq_run ENU.SYN|- \"text\" out.wav [frames.bin]    (-: the data compiled in)\n");
        return 2;
    }
    if (argc > 4) { g_frames = fopen(argv[4], "wb"); x86_on_enter = on_enter; }
    if (getenv("ELOQ_FX")) { fx_parse(getenv("ELOQ_FX")); x86_on_enter = on_enter; }
    if (getenv("ELOQ_FORMANT")) { g_formant = (float)atof(getenv("ELOQ_FORMANT")); x86_on_enter = on_enter; }
    if (getenv("ELOQ_RATE")) { g_rate = (float)atof(getenv("ELOQ_RATE")); x86_on_enter = on_enter; }
    cpu *c = x86_new();
    image_info img;
    if (!strcmp(argv[1], "-")) {
#ifdef ELOQ_EMBEDDED
        image_load_sections(c, enu_sections, enu_nsections);
        img = enu_info;
#else
        fprintf(stderr, "built without the data (tools/embed_image.py, then rebuild)\n");
        return 2;
#endif
    } else if (image_load(c, argv[1], &img)) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    x86_bind_imports(c);
    c->resolve = resolve;

    uint32_t dllmain_args[3] = { img.base, 1, 0 };
    uint32_t ok = x86_call(c, 0x10142e49u, 0, 3, dllmain_args);
    fprintf(stderr, "DllMain -> %u\n", ok);
    uint32_t out = x86_alloc(c, 4);
    uint32_t go_args[2] = { 1, out };
    x86_call(c, 0x1012b3e0u, 1, 2, go_args);
    g_obj = rd32(c, out);
    fprintf(stderr, "getObject -> %08x\n", g_obj);
    if (!g_obj) return 1;

    method(c, M_ADDREF, 0, 0, 0);
    method(c, M_START, 0, 0, 0);
    method(c, M_WANT_PHONEME_INDICES, 1, 0, 0);
    method(c, M_ANNO_CB, 2, CB_ANNO, USER);
    method(c, M_WORD_CB, 2, 0, USER);
    text_method(c, M_PROCESS_REMAINING, "`v1 `ts0 `da1 `ty1 `pp1");
    method(c, M_WORD_CB, 2, CB_WORD, USER);
    method(c, M_INDEX_CB, 2, CB_INDEX, USER);
    method(c, M_PHONEME_CB, 2, CB_PHONEME, USER);
    method(c, M_ANNO_CB, 2, CB_ANNO, USER);
    method(c, M_SYNTH_TO_CALLBACK, 2, CB_SYNTH, USER);
    method(c, M_CLEARINPUT, 0, 0, 0);
    method(c, M_FLUSH, 1, 0, 0);
    text_method(c, M_PROCESS_REMAINING, "");
    for (int k = 0; k < 2; k++) {
        method(c, M_WORD_CB, 2, 0, USER);
        text_method(c, M_PROCESS_REMAINING, "`esr1");
        method(c, M_WORD_CB, 2, CB_WORD, USER);
    }
    /* eciCopyVoice(preset): ECI.DLL sends each voice parameter that differs from the current (Voice1)
     * as its own `v annotation, in the order gender head pitch fluctuation roughness breathiness speed volume */
    static const int presets[8][8] = {
        { 0, 50, 65, 30, 0, 0, 50, 92 }, { 1, 50, 81, 30, 0, 50, 50, 100 }, { 1, 22, 93, 35, 0, 0, 50, 90 },
        { 0, 86, 56, 47, 0, 0, 50, 93 }, { 0, 50, 69, 34, 0, 0, 70, 92 },  { 1, 56, 89, 35, 0, 40, 70, 95 },
        { 1, 45, 68, 30, 3, 40, 50, 90 }, { 0, 30, 61, 44, 18, 20, 50, 90 } };
    const char *voice = getenv("ELOQ_VOICE");
    int v = voice ? atoi(voice) : 1;
    if (v >= 1 && v <= 8)
        for (int k = 0; k < 8; k++)
            if (presets[v - 1][k] != presets[0][k]) {
                char a[16];
                snprintf(a, sizeof a, "`v%c%d", "ghbfrysv"[k], presets[v - 1][k]);
                text_method(c, M_PROCESS_SENTENCES, a);
            }
    char *text = eci_prepare(argv[2], getenv("ELOQ_ANNOT") != NULL);
    text_method(c, M_PROCESS_SENTENCES, text);
    free(text);
    text_method(c, M_PROCESS_REMAINING, "");
    method(c, M_FLUSH, 1, 1, 0);
    method(c, M_FLUSH, 1, 0, 0);

    extern long crt_math_calls[3];
    fprintf(stderr, "%zu samples; log/exp/pow calls %ld/%ld/%ld\n", g_n, crt_math_calls[0], crt_math_calls[1],
            crt_math_calls[2]);
    unsigned rate = g_rate > 0 ? (unsigned)g_rate : 11025;
    FILE *f = fopen(argv[3], "wb");
    if (!f) return 1;
    fwrite("RIFF", 1, 4, f); put(f, (uint32_t)(36 + 2 * g_n), 4); fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4); put(f, 1, 2); put(f, 1, 2); put(f, rate, 4); put(f, 2 * rate, 4); put(f, 2, 2); put(f, 16, 2);
    fwrite("data", 1, 4, f); put(f, (uint32_t)(2 * g_n), 4);
    fwrite(g_pcm, 2, g_n, f);
    fclose(f);
    return 0;
}
