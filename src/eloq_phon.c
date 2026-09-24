/* eloq_phon - what Eloquence says, as timed phonemes and synthesizer frames (to drive, or be driven by,
 * another voice).
 *
 *   eloq_phon "text" out.txt [out.wav]
 *
 * The text goes to the engine as is (annotations work: `[.1hE.0lo] is phonetic input).
 * out.txt:  "P <sample> <code>"   a phoneme starts at that output sample (11025 Hz); code = its bytes
 *           "W <sample> <n>"      the engine's word callback
 *           "F <sample> <f0> <av>" every synthesizer frame (5 ms): F0 in Hz, voicing amplitude
 *           "E <sample>"          the end
 * ELOQ_FRAMES_OUT=file   every frame as (int32 sample, 64 floats)
 * ELOQ_FRAMES_IN=file    64-float frames that replace, one by one, what the synthesizer is given; after the
 *                        last one it gets silence, and the audio ends where they ran out
 * The phonemes come the way ECI.DLL gets them: the engine's phoneme callback (code, delay) is turned into a
 * delayed index, and the index callback fires when the audio reaches that phoneme.
 */
#include "engine.h"
#include "voicefx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CB_SYNTH = 1, CB_WORD, CB_INDEX, CB_PHONEME, CB_ANNO };
#define SYNTH 0x1013caf0u

static FILE *g_out, *g_fout;
static long long g_samples, g_cut = -1;
static short *g_pcm;
static size_t g_cap;
static uint32_t g_codes[65536];
static unsigned g_nslots = 1;
static eng *g_e;
static float (*g_fin)[64];
static long g_nfin, g_ifin;
static voicefx g_vfx;

static float g_rate;   /* ELOQ_RATE: the synthesizer at another sample rate */

static void on_enter(cpu *c, uint32_t fn)
{
    if (fn == 0x1013c7f0u && g_rate > 0) {           /* the synthesizer's init: its config's rate (+4) */
        uint32_t bits;
        memcpy(&bits, &g_rate, 4);
        wr32(c, c->esp + 8 + 4, bits);
        return;
    }
    if (fn != SYNTH) return;
    uint32_t frame = rd32(c, c->esp + 8);
    float f[64];
    x86_read(c, frame, f, sizeof f);
    /* the synthesizer's output switch (state+0x1add): frames given while it is off make no sound, so
     * replacement frames are only spent while it is on */
    uint32_t state = rd32(c, c->esp + 4);
    if (g_fin && !rd8(c, state + 0x1add)) {
        fprintf(g_out, "F %lld %.2f %.1f off\n", g_samples, f[1] / 10.0f, f[2]);
        return;
    }
    if (g_fin) {
        if (g_ifin < g_nfin) {
            memcpy(f, g_fin[g_ifin], sizeof f);
        } else {
            if (g_cut < 0) g_cut = g_samples;
            memcpy(f, g_fin[g_nfin - 1], sizeof f);
            f[2] = 0.0f;                                   /* no voicing: silence */
        }
        g_ifin++;
        x86_write(c, frame, f, sizeof f);
    }
    if (g_vfx.on) {
        voicefx_frame(&g_vfx, f);
        x86_write(c, frame, f, sizeof f);
    }
    if (g_fout) {
        int32_t s = (int32_t)g_samples;
        fwrite(&s, 4, 1, g_fout);
        fwrite(f, 4, 64, g_fout);
    }
    fprintf(g_out, "F %lld %.2f %.1f\n", g_samples, f[1] / 10.0f, f[2]);
}

static uint32_t handler(eng *e, cpu *c, unsigned n)
{
    (void)e;
    if (n == CB_SYNTH) {
        uint32_t count = eng_arg(c, 0), p = eng_arg(c, 1);
        if (g_samples + count > (long long)g_cap) {
            g_cap = (size_t)(g_samples + count) * 2 + 65536;
            g_pcm = (short *)realloc(g_pcm, g_cap * sizeof *g_pcm);
        }
        for (uint32_t i = 0; i < count; i++) {
            int32_t v = (int32_t)rd32(c, p + 4 * i);
            voicefx_audio(&g_vfx, &v, 1);
            g_pcm[g_samples + i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        g_samples += count;
    } else if (n == CB_PHONEME) {
        uint32_t code = eng_arg(c, 0), delay = eng_arg(c, 1);
        unsigned slot = g_nslots++ & 0xffff;
        g_codes[slot] = code;
        eng_call(g_e, ENG_INSERT_DELAYED_SYNTHESIS_INDEX, 2, slot, delay, 0, 0);
    } else if (n == CB_INDEX) {
        uint32_t code = g_codes[eng_arg(c, 0) & 0xffff];
        char s[5] = { 0 };
        for (int k = 0; k < 4; k++) s[k] = (char)((code >> (8 * k)) & 0xff);
        for (int k = 0; k < 4; k++) if ((unsigned char)s[k] < 33 && s[k]) s[k] = '_';
        fprintf(g_out, "P %lld %s\n", g_samples, s[0] ? s : "?");
        voicefx_phoneme(&g_vfx, s);
    } else if (n == CB_WORD) {
        fprintf(g_out, "W %lld %u\n", g_samples, eng_arg(c, 0));
    }
    return 0;
}

static void put(FILE *f, uint32_t v, int n) { for (int i = 0; i < n; i++) fputc((v >> (8 * i)) & 0xff, f); }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: eloq_phon \"text\" out.txt [out.wav]\n"); return 2; }
    g_out = fopen(argv[2], "w");
    if (!g_out) return 1;
    if (getenv("ELOQ_FRAMES_OUT")) g_fout = fopen(getenv("ELOQ_FRAMES_OUT"), "wb");
    if (getenv("ELOQ_FRAMES_IN")) {
        FILE *fi = fopen(getenv("ELOQ_FRAMES_IN"), "rb");
        if (!fi) { fprintf(stderr, "cannot read the frames\n"); return 1; }
        fseek(fi, 0, SEEK_END);
        g_nfin = ftell(fi) / 256;
        fseek(fi, 0, SEEK_SET);
        g_fin = (float(*)[64])malloc((size_t)g_nfin * 256);
        if (!g_fin || !g_nfin || fread(g_fin, 256, (size_t)g_nfin, fi) != (size_t)g_nfin) return 1;
        fclose(fi);
    }
    if (getenv("ELOQ_RATE")) g_rate = (float)atof(getenv("ELOQ_RATE"));
    voicefx_init(&g_vfx, g_rate > 0 ? (int)g_rate : 11025);
    if (getenv("ELOQ_FX")) voicefx_parse(&g_vfx, getenv("ELOQ_FX"));
    g_e = eng_new(NULL, handler, NULL);
    if (!g_e) { fprintf(stderr, "no engine\n"); return 1; }
    x86_on_enter = on_enter;
    eng *e = g_e;
    /* as ECI.DLL sets it up (notes/eci_api.md 3.1), with phoneme indices wanted */
    eng_call(e, ENG_WANT_PHONEME_INDICES, 1, 0, 0, 0, 0);
    eng_call(e, ENG_REGISTER_ANNO_CALLBACK, 2, ENG_CB_BASE + CB_ANNO, 0, 0, 0);
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    eng_call_text(e, ENG_PROCESS_REMAINING, "`v1 `ts0 `da1 `ty1 `pp1");
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, ENG_CB_BASE + CB_WORD, 0, 0, 0);
    eng_call(e, ENG_REGISTER_INDEX_CALLBACK, 2, ENG_CB_BASE + CB_INDEX, 0, 0, 0);
    eng_call(e, ENG_REGISTER_PHONEME_CALLBACK, 2, ENG_CB_BASE + CB_PHONEME, 0, 0, 0);
    eng_call(e, ENG_SET_SYNTH_TO_CALLBACK, 2, ENG_CB_BASE + CB_SYNTH, 0, 0, 0);
    eng_call(e, ENG_CLEAR_INPUT, 0, 0, 0, 0, 0);
    eng_call(e, ENG_FLUSH, 1, 0, 0, 0, 0);
    eng_call(e, ENG_PROCESS_REMAINING, 1, 0, 0, 0, 0);
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    eng_call_text(e, ENG_PROCESS_REMAINING, "`esr1");
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, ENG_CB_BASE + CB_WORD, 0, 0, 0);
    /* sendParameters' wantPhonemeIndices(1) */
    eng_call(e, ENG_PROCESS_REMAINING, 1, 0, 0, 0, 0);
    eng_call(e, ENG_WANT_PHONEME_INDICES, 1, 1, 0, 0, 0);
    eng_call_text(e, ENG_PROCESS_SENTENCES, argv[1]);
    eng_call(e, ENG_PROCESS_REMAINING, 1, 0, 0, 0, 0);
    if (g_fin && g_cut < 0) fprintf(stderr, "eloq_phon: only %ld of %ld frames were used - give more text\n", g_ifin, g_nfin);
    if (g_cut >= 0) g_samples = g_cut;
    if (g_fout) fclose(g_fout);
    fprintf(g_out, "E %lld\n", g_samples);
    fclose(g_out);
    if (argc > 3) {
        FILE *f = fopen(argv[3], "wb");
        if (f) {
            uint32_t bytes = (uint32_t)(g_samples * 2);
            fwrite("RIFF", 1, 4, f); put(f, 36 + bytes, 4); fwrite("WAVEfmt ", 1, 8, f);
            put(f, 16, 4); put(f, 1, 2); put(f, 1, 2); put(f, g_rate > 0 ? (uint32_t)g_rate : 11025, 4); put(f, 2 * (g_rate > 0 ? (uint32_t)g_rate : 11025), 4); put(f, 2, 2); put(f, 16, 2);
            fwrite("data", 1, 4, f); put(f, bytes, 4);
            fwrite(g_pcm, 2, (size_t)g_samples, f);
            fclose(f);
        }
    }
    return 0;
}
