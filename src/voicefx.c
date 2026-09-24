/* voicefx - see voicefx.h */
#include "voicefx.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#define strtok_s strtok_r
#endif

/* tunes, as notes; the old MacinTalk novelty voices sang like this, a note per syllable */
static const struct { const char *name, *notes; } tunes[] = {
    /* Beethoven, Ode to Joy */
    { "goodnews", "E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 E4 D4 D4 E4 E4 F4 G4 G4 F4 E4 D4 C4 C4 D4 E4 D4 C4 C4" },
    /* Chopin, funeral march (the Mac's Bad News sang it) */
    { "badnews", "C3 C3 C3 C3 D#3 D3 D3 C3 C3 B2 C3 C3 C3 C3 C3 D#3 D3 D3 C3 C3 B2 C3" },
    /* Grieg, In the Hall of the Mountain King (the Mac's Cellos) */
    { "cellos", "E3 F#3 G3 A3 B3 G3 B3 A#3 F#3 A#3 A3 F3 A3 E3 F#3 G3 A3 B3 G3 B3 E4 D4 B3 G3 B3 D4" },
    /* Westminster chimes (the Mac's Bells) */
    { "bells", "E4 C4 D4 G3 G3 D4 E4 C4 E4 D4 C4 G3 G3 D4 E4 C4" },
    /* Old Hundredth (a hymn, for the organ) */
    { "organ", "G3 G3 F#3 E3 D3 G3 A3 B3 B3 B3 A3 G3 C4 B3 A3 G3" },
};

void voicefx_init(voicefx *v, int rate)
{
    memset(v, 0, sizeof *v);
    v->formant = v->bw = v->pitch = v->flat = 1.0f;
    v->rng = 12345;
    v->note = -1;
    v->hold = 1;
    v->rate = rate > 0 ? rate : 11025;
}

/* "C4", "F#3", "Bb2" -> Hz (0 if not a note) */
static float note_hz(const char *s)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    char c = (char)toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') return 0;
    int semi = base[c - 'A'], i = 1;
    if (s[i] == '#') { semi++; i++; }
    else if (s[i] == 'b') { semi--; i++; }
    int midi = (atoi(s + i) + 1) * 12 + semi;
    return (float)(440.0 * pow(2.0, (midi - 69) / 12.0));
}

static void set_tune(voicefx *v, const char *notes)
{
    char buf[1024], *save = NULL;
    strncpy(buf, notes, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    v->ntune = 0;
    for (char *n = strtok_s(buf, " ", &save); n && v->ntune < 256; n = strtok_s(NULL, " ", &save)) {
        float hz = note_hz(n);
        if (hz > 0) v->tune[v->ntune++] = hz;
    }
}

void voicefx_parse(voicefx *v, const char *spec)
{
    char buf[1024], *save = NULL;
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *k = strtok_s(buf, ",", &save); k; k = strtok_s(NULL, ",", &save)) {
        while (*k == ' ') k++;
        char *val = strchr(k, '=');
        float x = val ? (float)atof(val + 1) : 0;
        const char *r = val ? strchr(val, ':') : NULL;
        if (!strncmp(k, "formant", 7)) v->formant = x;
        else if (!strncmp(k, "bw", 2)) v->bw = x;
        else if (!strncmp(k, "pitch", 5)) v->pitch = x;
        else if (!strncmp(k, "mono", 4)) v->mono = x;
        else if (!strncmp(k, "flat", 4)) v->flat = x;
        else if (!strncmp(k, "vib", 3) && val) { v->vib_c = x; v->vib_hz = r ? (float)atof(r + 1) : 5; }
        else if (!strncmp(k, "jitter", 6)) v->jitter = x;
        else if (!strncmp(k, "growl", 5) && val) { v->growl_db = x; v->growl_hz = r ? (float)atof(r + 1) : 30; }
        else if (!strncmp(k, "whisper", 7)) v->whisper = 1;
        else if (!strncmp(k, "breath", 6)) v->breath = x;
        else if (!strncmp(k, "oq", 2)) v->oq = x;
        else if (!strncmp(k, "transpose", 9)) v->transpose = x;
        else if (!strncmp(k, "crush", 5)) v->crush = x;
        else if (!strncmp(k, "hold", 4)) v->hold = x >= 1 ? (int)x : 1;
        else if (!strncmp(k, "hp", 2)) v->hp = x;
        else if (!strncmp(k, "tune", 4) && val) {
            for (size_t i = 0; i < sizeof tunes / sizeof tunes[0]; i++)
                if (!strcmp(val + 1, tunes[i].name)) set_tune(v, tunes[i].notes);
        } else if (!strncmp(k, "notes", 5) && val) set_tune(v, val + 1);
    }
    v->on = 1;
}

void voicefx_phoneme(voicefx *v, const char *sym)
{
    if (!v->ntune || !sym || !strchr("iIeE@AacoUuHxXRYWO", sym[0])) return;
    v->note = (v->note + 1) % v->ntune;
    v->started = 1;
}

void voicefx_frame(voicefx *v, float *f)
{
    if (!v->on) return;
    static const int fslot[8] = { 9, 13, 15, 17, 19, 21, 23, 25 };
    for (int k = 0; k < 8; k++) {
        f[fslot[k]] *= v->formant;
        f[fslot[k] + 1] *= v->bw;
    }
    double f0 = f[1] / 10.0 * v->pitch;
    if (v->flat < 1.0f && f0 > 0) {          /* around a slow running average of the voice's own pitch */
        if (v->f0_avg <= 0) v->f0_avg = f0;
        if (f[2] > 0) v->f0_avg = 0.995 * v->f0_avg + 0.005 * f0;
        f0 = v->f0_avg * pow(f0 / v->f0_avg, v->flat);
    }
    if (v->mono > 0) f0 = v->mono;
    if (v->ntune) f0 = v->tune[v->started ? v->note : 0];
    f0 *= pow(2.0, v->transpose / 12.0);
    if (v->vib_c != 0) f0 *= pow(2.0, v->vib_c / 1200.0 * sin(2 * 3.14159265358979 * v->vib_hz * v->t));
    if (v->jitter != 0) {
        v->rng = v->rng * 1103515245u + 12345u;
        v->wander = 0.9 * v->wander + 0.1 * (((v->rng >> 16) & 0x7fff) / 16384.0 - 1.0) * v->jitter * 4;
        f0 *= pow(2.0, v->wander / 1200.0);
    }
    f[1] = (float)(f0 * 10.0);
    if (v->growl_db != 0 && f[2] > 0) {
        double m = 0.5 + 0.5 * sin(2 * 3.14159265358979 * v->growl_hz * v->t);
        f[2] -= (float)(v->growl_db * m);
        if (f[2] < 0) f[2] = 0;
    }
    if (v->whisper) {
        if (f[2] + 14 > f[7]) f[7] = f[2] + 14;     /* aspiration is ~12 dB weaker than voicing at the same number */
        f[2] = 0;
    } else if (v->breath != 0 && f[2] > 0) {
        f[7] += v->breath;
    }
    if (v->oq > 0) f[3] = v->oq;
    v->t += f[0] / 1000.0;
}

int voicefx_has_audio(const voicefx *v) { return v->on && (v->crush > 0 || v->hold > 1 || v->hp > 0); }

void voicefx_audio(voicefx *v, int32_t *s, int n)
{
    if (!voicefx_has_audio(v)) return;
    double a = v->hp > 0 ? 1.0 / (1.0 + 2 * 3.14159265358979 * v->hp / v->rate) : 0;
    double step = v->crush > 0 ? pow(2.0, 16.0 - v->crush) : 1;
    for (int i = 0; i < n; i++) {
        double x = s[i];
        if (v->hp > 0) {                     /* y = a (y' + x - x') */
            double y = a * (v->hp_y + x - v->hp_x);
            v->hp_x = x;
            v->hp_y = y;
            x = y;
        }
        if (v->hold > 1) {
            if (v->hold_n++ % v->hold == 0) v->held = (int32_t)x;
            x = v->held;
        }
        if (v->crush > 0) x = floor(x / step + 0.5) * step;
        s[i] = (int32_t)x;
    }
}
