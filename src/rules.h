/* rules - the runtime library of Eloquence 6.1's rule code (ENU.SYN 0x10130e80-0x10136000 and the
 * helpers it uses), hand-ported to C99 and bit-exact with the original.
 *
 * ENU.SYN's front end (text normalization, letter-to-sound, syllables, stress, intonation, durations,
 * parameter tracks) is rule code compiled from a Delta-like rule language (Hertz's Delta; ETI's rules):
 * hundreds of huge C functions (still recompiled, src/gen/) that call this library for every step.
 * What the rules work on is a **delta**: several parallel **streams** of tokens (char_count, inp, phone,
 * morph, word, inton_phr, klatt, syllable, Ms), aligned at shared **sync marks**. The rules walk the
 * delta with a **cursor**, match tokens, bind **variables**, insert and change tokens, and **backtrack**
 * on failure (a rule is a pattern with alternatives), so the library keeps a control stack of choice
 * points and an undo trail, in the style of a Prolog machine.
 *
 * Everything lives in guest memory (the engine's heap and the rule functions' stack frames) and is shared
 * with the recompiled rule code, so these functions work in place through rd/wr on the machine (x86rt),
 * like framer.c. Calls to functions not ported yet go through the machine (their f_XXXXXXXX).
 *
 * ---------------------------------------------------------------------------------------------------
 * The streams (a table of 9 descriptors of 0x39 bytes at 0x10150be8, one per stream, in .data)
 *
 *   +0x00 name (char *)           "char_count", "inp", "phone", "morph", "word", "inton_phr", "klatt",
 *                                 "syllable", "Ms"
 *   +0x04 fields                  field descriptors, 0x15 bytes each: +0x08 symbol names (char *[]),
 *                                 +0x10 short number of symbols, +0x12 short type, +0x14 byte flag
 *   +0x08 getters                 per field, a function (cdecl, token data -> address of the field)
 *   +0x0c setters                 per field, a function (token data, value)
 *   +0x10 symbol table            for enumerated fields (types -1/-2): entries of +0x25 bytes, +0x29 copied
 *   +0x14 default token           token data a new token starts from
 *   +0x18 byte                    the stream starts with a default token
 *   +0x21 token size              bytes of token data
 *   +0x2d int                     1: the stream's text is read a character at a time (rules_io.c)
 *   +0x31 char, +0x32 char        the quotes of its text (open, close)
 *
 * A field type (like a value type below): >= 0 another stream's token, -1 byte (symbol), -2 short
 * (symbol), -3 int, -4 short, -5 double, -6 sync mark.
 *
 * ---------------------------------------------------------------------------------------------------
 * The delta: elements (on the engine heap)
 *
 * An element is either a token or a sync mark; bit 1 of its first word tells them apart (set: sync mark).
 * Links are pointers with 2 flag bits in the low bits (mask ~3).
 *
 *   token:      +0 link to the sync mark before it     +4 link to the sync mark after it     +8 data
 *   sync mark:  +0 flags (bit 1 set)
 *               +0xc + 4 s          forward link in stream s (the next token or sync mark in stream s)
 *               + 4 (B + s)         backward link in stream s, B = rs[RS_BACK] (number of streams + 3);
 *                                   its bit 0: the mark is a boundary of stream s, bit 1: marked
 *
 * ---------------------------------------------------------------------------------------------------
 * Values (a rule's variables and constants: 2-byte type + data, in the rule function's frame)
 *
 *   short type   -6 sync mark (+2 pointer)   -5 double (+2)   -4 short (+2)   -3 int (+2)
 *                >= 0: a token of stream `type`: +2 short field (-1: the token itself), +4 token pointer
 *
 * A reference ("ref", 8 bytes, how the library passes values around): +0 address of the data, +4 short
 * type, +6 byte flag. rl_ref resolves a value to a ref (a field through the stream's getter).
 *
 * ---------------------------------------------------------------------------------------------------
 * The engine instance (`eng`, the first argument of every function here; the same object framer.c
 * knows as the synthesis instance). The fields the rule runtime uses:
 */
#ifndef RULES_H
#define RULES_H

#include <stdint.h>

#include "x86rt.h"

enum {
    ENG_RESULT     = 0x3c,   /* i  where a rule goes on: a label set by a successful match (see rl_enter) */
    ENG_SYNC_A     = 0x40,   /* the rule's two sync variables, A and B (10 bytes each, see SV_*) */
    ENG_SYNC_B     = 0x4a,
    ENG_OUT        = 0x58,   /* p  output/utterance control (+0x1a4 status, +0x1b5 changed flag) */
    ENG_RS         = 0x5c,   /* p  the rule state (RS_*) */
    ENG_WS         = 0x60,   /* p  the workspace: delta ends, control stack, value stack (WS_*) */
    ENG_SCOPE      = 0x75,   /* p  byte[]: the streams the current rule looks at */
    ENG_SLOT       = 0x7d,   /* p  byte[stream]: the stream's slot in ENG_SEEN */
    ENG_SEEN       = 0x85,   /* p  byte[slot]: the rule has matched in that stream */
    ENG_NSTREAMS   = 0x89    /* b  number of streams */
};

/* a sync variable (ENG_SYNC_A / ENG_SYNC_B) */
enum {
    SV_MARK        = 0,      /* p  the sync mark */
    SV_STREAM      = 4,      /* b  stream of a pending offset */
    SV_OFFSET      = 5,      /* i  pending offset (in tokens) */
    SV_STATE       = 9       /* b  bit 0: resolved; 2: an offset is pending (FUN_1013b5b0 resolves it) */
};

/* the rule state (eng + ENG_RS, 0x11b2 bytes, malloc'd by FUN_10131c10) */
enum {
    RS_NVARS       = 0x000,  /* i  number of registered variables (the GC roots of the delta) */
    RS_VARS        = 0x004,  /* p[999] the registered variables (addresses of values in rule frames) */
    RS_FRAME       = 0xfa4,  /* i  RS_NVARS when the current rule was entered (a link to the previous) */
    RS_ABORT       = 0xfa8,  /* b  abort: every rule returns at once (longjmp to RS_JMPBUF) */
    RS_JMPBUF      = 0xfa9,  /* p  the current rule's jmp_buf */
    RS_STATUS      = 0xfad,  /* b */
    RS_FAIL        = 0xfba,  /* i  the rule's failure label */
    RS_NEXT        = 0xfbe,  /* i  the rule's success label */
    RS_POS         = 0xfc6,  /* the cursor, 7 bytes: */
    RS_POS_MARK    = 0xfc6,  /*   p  current sync mark */
    RS_POS_STREAM  = 0xfca,  /*   b  current stream */
    RS_POS_BACK    = 0xfcb,  /*   b  direction: 0 forward, 1 backward */
    RS_POS_FRESH   = 0xfcc,  /*   b  the cursor was just placed (not moved yet) */
    RS_TRAIL       = 0xfcd,  /* b  variable writes are trailed (undone on backtracking) */
    RS_SAVED       = 0xfce,  /* i  saved with the rule frame */
    RS_RULE_TOP    = 0xfd2,  /* p  control stack top at the current rule's frame */
    RS_CMP         = 0xfd6,  /* b  result of the last comparison: -1, 0, 1 */
    RS_NSCOPE      = 0xfd7,  /* b  number of streams in ENG_SCOPE */
    RS_TMP_SYNC    = 0xff0,  /* scratch values for conversions (-1/-6: +0xff0, -3: +0xff5, -4/-2: +0x100a, */
    RS_TMP_INT     = 0xff5,  /*   -5: +0x1015) */
    RS_TMP_SHORT   = 0x100a,
    RS_TMP_DOUBLE  = 0x1015,
    RS_BACK        = 0x114a, /* i  index of the backward links in a sync mark (see above) */
    RS_11AC        = 0x11ac,
    RS_MATCH_COUNT = 0x11b0  /* s */
};

/* the workspace (eng + ENG_WS, 0x597 bytes, FUN_10135900) */
enum {
    WS_END         = 0x000,  /* p  the utterance's last sync mark (the delta's right end) */
    WS_START       = 0x004,  /* p  its first sync mark (the left end) */
    WS_EVAL        = 0x093,  /* p  value stack: cells of 10 bytes (8 data, short type at +8) */
    WS_EVAL_TOP    = 0x097,  /* sb index of the top cell */
    WS_SZ_FRAME    = 0x098,  /* i  sizes of the control stack entries: rule frame (type 7) */
    WS_SZ_UNDO     = 0x09c,  /* i  undo entry header (type 2) */
    WS_SZ_POS      = 0x0a0,  /* i  saved cursor (type 1) */
    WS_SZ_LABEL    = 0x0a4,  /* i  choice point (types 0, 3) */
    WS_SZ_CUTPT    = 0x0a8,  /* i  saved cut point (type 5) */
    WS_SZ_MARK     = 0x0ac,  /* i  depth marks (types 4, 6) */
    WS_TOP         = 0x44b,  /* p  control stack top (the stack grows down) */
    WS_TOP_OFF     = 0x44f,  /* i  the same as an offset in the stack's chunk */
    WS_CHUNK       = 0x457,  /* p  the stack's chunk (+0x10: its base); 0 when no rule runs */
    WS_CUT         = 0x45f,  /* p  cut point: the control stack entry a cut returns to */
    WS_CHUNK_OFF   = 0x46b   /* i  offset of the chunk */
};

/* control stack entries (type byte, then:) */
enum {
    CS_RETRY       = 0,      /* +1 label: a choice point, resumed at the label */
    CS_POS         = 1,      /* +1 the cursor (7 bytes), restored on backtracking */
    CS_UNDO        = 2,      /* +1 short type, +3 address, +7 size, data at WS_SZ_UNDO: undone */
    CS_NEXT        = 3,      /* +1 label: a choice point resumed at the label if the cursor can move on */
    CS_DOWN        = 4,      /* depth marks: skipped with the depth argument of rl_backtrack */
    CS_CUTPT       = 5,      /* +1 saved WS_CUT */
    CS_UP          = 6,
    CS_FRAME       = 7,      /* +1 the rule's frame record (rl_enter) */
    CS_BARRIER     = 8
};

/* a rule's frame record (the rule function's local, filled by rl_enter, read back by rl_leave) */
enum {
    FR_SAVED = 0x00, FR_FAIL = 0x0d, FR_NEXT = 0x11, FR_TRAIL = 0x15, FR_RULE_TOP = 0x16, FR_TOP = 0x1a,
    FR_CUT = 0x1e, FR_NSCOPE = 0x22, FR_JMPBUF = 0x23, FR_POS = 0x27, FR_SYNC_A = 0x2e, FR_SYNC_B = 0x38,
    FR_CMP = 0x42, FR_EVAL_TOP = 0x43
};

/* the stream descriptor table */
#define STREAM_TABLE   0x10150be8u
#define STREAM_SIZE    0x39u
enum { SD_NAME = 0, SD_FIELDS = 4, SD_GETTERS = 8, SD_SETTERS = 0xc, SD_SYMBOLS = 0x10, SD_DEFAULT = 0x14,
       SD_HAS_DEFAULT = 0x18, SD_TOKEN_SIZE = 0x21, SD_SYMBOL_SIZE = 0x25, SD_SYMBOL_COPY = 0x29,
       SD_SINGLE = 0x2d, SD_QUOTE_OPEN = 0x31, SD_QUOTE_CLOSE = 0x32 };
#define FIELD_SIZE     0x15u
enum { FD_NAMES = 0x08, FD_NSYMS = 0x10, FD_TYPE = 0x12, FD_FLAG = 0x14 };

/* value types */
enum { T_SYM8 = -1, T_SYM16 = -2, T_INT = -3, T_SHORT = -4, T_DOUBLE = -5, T_SYNC = -6 };

/* ---------------------------------------------------------------------------------------------------
 * The library. `eng` is the engine instance; values and refs are guest addresses. Functions returning
 * int return what the original returns in eax. */

/* rule frames and the control stack */
int      rl_enter(cpu *c, uint32_t eng, uint32_t frame, uint32_t slot, uint32_t scope, uint32_t seen,
                  uint32_t jmpbuf);                                          /* FUN_101315e0 */
uint32_t rl_leave(cpu *c, uint32_t eng);                                     /* FUN_10131790 */
void     rl_throw(cpu *c, uint32_t eng);                                     /* FUN_10130e80 */
void     rl_set_scope(cpu *c, uint32_t eng, uint8_t n, uint32_t streams);    /* FUN_1004bdb2 */
void     rl_drop_top(cpu *c, uint32_t eng);                                  /* FUN_1013a040 */
int32_t  rl_backtrack(cpu *c, uint32_t eng, int32_t depth);                  /* FUN_101311a0 */
void     rl_cut(cpu *c, uint32_t eng);                                       /* FUN_10131f90 */
void     rl_push_retry(cpu *c, uint32_t eng, uint32_t label);                /* FUN_10131ff0 */
void     rl_push_pos(cpu *c, uint32_t eng);
void     rl_push_mark(cpu *c, uint32_t eng, int type);                       /* FUN_101320e0 / 10132120 */
void     rl_push_next(cpu *c, uint32_t eng, uint32_t label);

/* variables and values */
int      rl_var_init(cpu *c, uint32_t eng, uint32_t var, uint32_t src, int16_t type);   /* FUN_10130ea0 */
void     rl_ref(cpu *c, uint32_t eng, uint32_t ref, uint32_t val);           /* FUN_10131520 */

/* the cursor */
int      rl_step(cpu *c, uint32_t eng, int to_token, int check_scope);       /* FUN_10135d00 */
int      rl_step_to(cpu *c, uint32_t eng, uint32_t stop, int check_scope);   /* FUN_10135e40 */
int      rl_skip_marks(cpu *c, uint32_t eng, int check_scope);               /* FUN_10135f70 */
int      rl_next_token(cpu *c, uint32_t eng, int check_scope);               /* FUN_101360a0 */

#endif
