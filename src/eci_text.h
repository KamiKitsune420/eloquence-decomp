/* eci_text - ECI.DLL's text handling, as pure functions (notes/eci_api.md 3.4, 3.10, 8.1) */
#ifndef ECI_TEXT_H
#define ECI_TEXT_H

/* FUN_1000ddc5: the text as the engine gets it (byte table, newlines, escapes). malloc'd. */
char *eci_prepare(const char *in, int annotations);

/* FUN_10013080's character count for a string handed to processSentences: its length, +1 if it does
 * not end in a space, -1 per backslash escape (a backslash and the byte after it) */
int eci_char_count(const char *s);

/* voice parameter ranges (0x1001d348) and ECI parameter ranges (0x1001d2c0) */
extern const int eci_voice_range[8][2];
extern const int eci_param_range[17][2];

/* real-world units (FUN_10001923): p 2 pitch (Hz), 6 speed (words/min), 7 volume; other p: v */
int eci_to_real(int p, int v);
/* the inverse (FUN_10001a29): the ECI value in lo..hi whose real-world value is nearest to r */
int eci_from_real(int p, int r, int lo, int hi);

/* FUN_100083c1 on a text with annotations: `da `pp `ts `ty `l `vX and `vN (presets) update params/voice
 * (and, when given, the second, last-sent, copies); `vb `vs `vv numbers are rewritten in place (units:
 * eciRealWorldUnits) */
typedef struct {
    char name[31];
    int p[8];
    int defined;
    int fx;             /* this port's voice effects (eci.c ext_voices): 0 none, else 1 + index */
    int rw[3];          /* real-world pitch, speed, volume */
} eci_voice;

void eci_voice_realworld(eci_voice *v, int p);     /* p = -1: all three */
void eci_parse_annotations(eci_voice *voice, int *params, eci_voice *voice2, int *params2, int units,
                           char *text, const eci_voice *presets /* 8 */);

#endif
