/* voicefx - voice effects on Eloquence's synthesizer frames (64 floats every 5 ms, see notes/port.md) and
 * on its output samples.
 *
 * A spec is comma-separated.
 *   frames:  formant=S  scale F1..F8       bw=S       their bandwidths      pitch=S   scale F0
 *            mono=HZ    one pitch           flat=R     keep R of the intonation (0 flat .. 1 as is)
 *            vib=C:HZ   vibrato             jitter=C   random pitch wander
 *            growl=DB:HZ  voicing fluttered DB deep at HZ (with Eloquence's roughness: a growl)
 *            whisper    aspiration for voicing       breath=DB  more aspiration when voiced
 *            oq=V       open quotient (56 normal; low = buzzy, high = soft)
 *            tune=NAME  sing a note per vowel (goodnews badnews cellos bells organ); notes=C4 E4 ..; transpose=N
 *   audio:   crush=BITS  fewer bits      hold=N  keep every Nth sample (a lower rate)      hp=HZ  high-pass (1st order)
 * Frame slots: 0 length ms, 1 F0*10, 2 voicing amplitude, 3 open quotient, 7 aspiration, 8 frication,
 * 9/10 13/14 15/16 17/18 .. 25/26 F1..F8 and bandwidths.
 */
#ifndef VOICEFX_H
#define VOICEFX_H

#include <stdint.h>

typedef struct {
    int on, whisper;
    float formant, bw, pitch, mono, flat, vib_c, vib_hz, jitter, growl_db, growl_hz, breath, oq, transpose;
    float crush, hp;
    int hold;
    double t, wander, f0_avg;
    unsigned rng;
    float tune[256];
    int ntune, note, started;
    /* audio state */
    double hp_x, hp_y;
    int32_t held;
    int hold_n;
    int rate;
} voicefx;

void voicefx_init(voicefx *v, int rate);
void voicefx_parse(voicefx *v, const char *spec);
/* a frame just before the synthesizer uses it */
void voicefx_frame(voicefx *v, float f[64]);
/* a phoneme has started (Eloquence's symbol): a vowel moves a tune to its next note */
void voicefx_phoneme(voicefx *v, const char *sym);
/* output samples (the engine's int32, in place) */
void voicefx_audio(voicefx *v, int32_t *s, int n);
int voicefx_has_audio(const voicefx *v);

#endif
