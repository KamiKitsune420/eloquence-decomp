/* rules_io - the rule machine's text input (the original's "DELTIO"), its set-up, and splitting a token at
 * a new sync mark: FUN_10138080, FUN_10137620, FUN_10136320, FUN_10138430.
 *
 * The text input reads the value of a stream's token from an input channel of the engine (the rule code's
 * `read` statement; in ENU.SYN it is how the input text gets into the delta): a word of text is read with
 * the stream's quoting and escapes, then looked up in the stream's symbol names or converted to a number.
 * Channels are the engine's (FUN_10141e00 getc, FUN_10141e80 ungetc); the error path (a word that is no
 * value of the stream) reports through the engine's windows and asks again, as the original does.
 *
 * Everything works in guest memory at the original's stack addresses: the word is read into the
 * function's own frame, which the callees see, and the error path's calls are made with esp where the
 * original has it. Calls go through the machine (f_XXXXXXXX: a hand port or the recompiled function).
 */
#include "rules_int.h"

/* engine functions (hand-ported or recompiled) */
void f_10135b20(cpu *c);   /* the type of a stream's field 0 */
void f_101364c0(cpu *c);   /* a token's data from the stream's default and a field 0 value */
void f_10136570(cpu *c);   /* insert a token between two sync marks */
void f_10136720(cpu *c);   /* insert a sync mark */
void f_10137460(cpu *c);   /* printable copy of a string (escapes) */
void f_10137620(cpu *c);
void f_10138020(cpu *c);   /* error message: "Error: <module>: <printf message>" */
void f_10138080(cpu *c);
void f_10138370(cpu *c);   /* after an input error: 1 to give up */
void f_10139b20(cpu *c);   /* the element pool */
void f_10139f90(cpu *c);   /* the control stack */
void f_10140220(cpu *c);
void f_10140270(cpu *c);
void f_10140280(cpu *c);
void f_101402c0(cpu *c);
void f_10140350(cpu *c);
void f_10141bb0(cpu *c);
void f_10141e00(cpu *c);   /* channel getc (eng, channel) */
void f_10141e80(cpu *c);   /* channel ungetc (eng, channel) */
void f_10141ed0(cpu *c);
void f_10141f00(cpu *c);
void f_10142210(cpu *c);   /* a window by name */
void f_10142350(cpu *c);   /* stop requested */

/* msvcrt through the image's import table */
#define IAT_ATOF        0x101440a4u
#define IAT_ATOI        0x101440a8u
#define IAT_ATOL        0x101440acu
#define IAT_PCTYPE      0x101440b0u   /* data: address of _pctype (the ctype table pointer) */
#define IAT_MB_CUR_MAX  0x101440b4u   /* data: address of __mb_cur_max */
#define IAT_ISCTYPE     0x101440b8u
#define IAT_MALLOC      0x10144110u

/* the original's strings */
#define STR_MODULE      0x10192ef4u   /* "DELTIO" */
#define STR_REENTER     0x10192efcu   /* "Error in <%s>, re-enter:\n" */
#define STR_PGMWIN      0x10192f18u   /* "pgmwin" */
#define STR_CMDWIN      0x10192f20u   /* "cmdwin" */
#define STR_NOT_A_NAME  0x10192f38u   /* "\"%s\" is not a token name in stream \"%s\"" */

static uint32_t call1(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a)
{
    return call_at(c, sp, f, ret, 1, &a);
}
static uint32_t call4(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e)
{
    uint32_t args[4] = { a, b, d, e };
    return call_at(c, sp, f, ret, 4, args);
}
static uint32_t call5(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e,
                      uint32_t g)
{
    uint32_t args[5] = { a, b, d, e, g };
    return call_at(c, sp, f, ret, 5, args);
}
static uint32_t call6(cpu *c, uint32_t sp, guest_fn f, uint32_t ret, uint32_t a, uint32_t b, uint32_t d, uint32_t e,
                      uint32_t g, uint32_t h)
{
    uint32_t args[6] = { a, b, d, e, g, h };
    return call_at(c, sp, f, ret, 6, args);
}

static int streq(cpu *c, uint32_t a, uint32_t b)
{
    for (;; a++, b++) {
        uint8_t x = rd8(c, a);
        if (x != rd8(c, b)) return 0;
        if (!x) return 1;
    }
}

static uint32_t guest_strlen(cpu *c, uint32_t a)
{
    uint32_t n = 0;
    while (rd8(c, a + n)) n++;
    return n;
}

/* ------------------------------------------------------------------------------------ reading a word */

/* FUN_10138080: read a word of stream s's text from channel `ch` into buf (NUL-terminated). Leading
 * spaces are skipped; a space ends the word (put back), unless the stream reads single characters
 * (descriptor SD_SINGLE == 1: then one character is the word) or the word is quoted (SD_QUOTE_OPEN ...
 * SD_QUOTE_CLOSE, the quotes not stored). Escapes: \a \b \f \n \r \t \v, \ooo octal, \xhh hex, \c the
 * character itself. Streams whose field 0 is a number (types -3, -4, -5) take everything up to the end
 * of the line. al: the word's last character; '\n' for an empty line, 0 at the end of the input or for
 * an unmatched quote (then buf may be unterminated, as in the original). The flag is left in the
 * argument slot of s (the original keeps it there). */
static uint32_t read_word(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t ch, uint32_t buf)
{
    const uint32_t at = sp - 24;        /* esp at the calls */
    uint32_t desc = stream_desc(s & 0xff);
    uint8_t single = rd32(c, desc + SD_SINGLE) == 1;
    wr8(c, sp + 8, single);
    uint8_t quoted = 0, got = 0;
    int16_t t = (int16_t)call1(c, at, f_10135b20, 0x101380b9u, s);
    uint8_t numeric = t == T_SHORT;
    if (!numeric) numeric = (int16_t)call1(c, at, f_10135b20, 0x101380c8u, s) == T_INT;
    if (!numeric) numeric = (int16_t)call1(c, at, f_10135b20, 0x101380d7u, s) == T_DOUBLE;
    int32_t qopen = (int8_t)rd8(c, desc + SD_QUOTE_OPEN), qclose = (int8_t)rd8(c, desc + SD_QUOTE_CLOSE);
    uint32_t p = buf;
    for (;;) {
        int32_t k = (int32_t)call2(c, at, f_10141e00, 0x101380fcu, eng, ch);
        uint32_t v;
        if (k == '\\') {
            k = (int32_t)call2(c, at, f_10141e00, 0x1013810fu, eng, ch);
            if (k >= '0' && k <= '7') {
                v = 0;
                while (k >= '0' && k <= '7') {
                    v = v * 8 + (uint32_t)k - '0';
                    k = (int32_t)call2(c, at, f_10141e00, 0x1013813bu, eng, ch);
                }
                call2(c, at, f_10141e80, 0x101381f7u, eng, ch);
            } else if (k == 'x' || k == 'X') {
                v = 0;
                for (;;) {
                    k = (int32_t)call2(c, at, f_10141e00, 0x101381a5u, eng, ch);
                    if (k >= '0' && k <= '9') v = v * 16 + (uint32_t)k - 0x30;
                    else if (k >= 'a' && k <= 'f') v = v * 16 + (uint32_t)k - 0x57;
                    else if (k >= 'A' && k <= 'F') v = v * 16 + (uint32_t)k - 0x37;
                    else break;
                }
                call2(c, at, f_10141e80, 0x101381f7u, eng, ch);
            } else {
                switch (k) {
                case 'a': v = 7; break;
                case 'b': v = 8; break;
                case 'f': v = 0xc; break;
                case 'n': v = 0xa; break;
                case 'r': v = 0xd; break;
                case 't': v = 9; break;
                case 'v': v = 0xb; break;
                default: v = (uint32_t)k; break;
                }
            }
        } else if (k == '\n') {
            if (quoted) {
                if (!single) return 0;
                wr8(c, p++, (uint8_t)qopen);
                wr8(c, p, 0);
                return rd8(c, p - 1);
            }
            wr8(c, p, 0);
            if (!got) return '\n';
            call2(c, at, f_10141e80, 0x101382dfu, eng, ch);
            return rd8(c, p - 1);
        } else if (k == -1 || k == 0) {
            wr8(c, p, 0);
            return (uint32_t)k & ~0xffu;
        } else if (k == qopen) {
            if (!quoted) { quoted = 1; continue; }
            if (k != qclose) return 0;
            wr8(c, p, 0);
            return rd8(c, p - 1);
        } else if (k == qclose) {
            if (!quoted) return 0;
            wr8(c, p, 0);
            return rd8(c, p - 1);
        } else {
            if (k == ' ' && !single && !quoted) {
                if (!got) continue;
                wr8(c, p, 0);
                return 0x20;
            }
            v = (uint32_t)k;
        }
        wr8(c, p++, (uint8_t)v);
        if (numeric || !single) got = 1;
        else if (!quoted) {
            wr8(c, p, 0);
            return rd8(c, p - 1);
        }
    }
}

/* ------------------------------------------------------------------------------------- reading a value */

/* is character x a digit (msvcrt's isdigit as compiled: _isctype when multibyte, else the table) */
static int is_digit(cpu *c, uint32_t sp, uint8_t x, uint32_t ret)
{
    if ((int32_t)rd32(c, rd32(c, IAT_MB_CUR_MAX)) > 1) {
        uint32_t args[2] = { (uint32_t)(int32_t)(int8_t)x, 4 };
        return (import_call(c, sp, IAT_ISCTYPE, ret, 2, args)) != 0;
    }
    return (rd8(c, rd32(c, rd32(c, IAT_PCTYPE)) + 2u * (uint32_t)(int32_t)(int8_t)x) & 4) != 0;
}

/* the word is not a value of the stream: "Error: DELTIO: "<word>" is not a token name in stream "<s>""
 * with the word's printable copy at esc; returns what FUN_10138370 says (nonzero: give up) */
static uint32_t word_error(cpu *c, uint32_t sp0, uint32_t eng, uint32_t ch, uint32_t esc, const uint32_t *ret)
{
    uint32_t name = rd32(c, stream_desc(rd32(c, sp0 + 0x14) & 0xff) + SD_NAME);
    call3(c, sp0, f_10137460, ret[0], sp0 + 0x2c, esc, 0x4b);
    call6(c, sp0 - 12, f_10138020, ret[1], eng, ch, STR_MODULE, STR_NOT_A_NAME, esc, name);
    return call3(c, sp0 - 36, f_10138370, 0x10137c30u, eng, ch, esc);
}

/* the same for a number: the message, then "Error in <cmdwin>, re-enter:" (or pgmwin) with the input
 * position moved back over the word; 1 if neither window exists (give up), else 0 (read again) */
static int number_error(cpu *c, uint32_t sp0, uint32_t eng, uint32_t ch, uint32_t esc, const uint32_t *ret)
{
    uint32_t name = rd32(c, stream_desc(rd32(c, sp0 + 0x14) & 0xff) + SD_NAME);
    call3(c, sp0, f_10137460, ret[0], sp0 + 0x2c, esc, 0x4b);
    call6(c, sp0 - 12, f_10138020, ret[1], eng, ch, STR_MODULE, STR_NOT_A_NAME, esc, name);
    if (!(call5(c, sp0 - 36, f_10142210, ret[2], eng, ch, STR_CMDWIN, 1, 1) & 0xff)
        && !(call5(c, sp0, f_10142210, ret[3], eng, ch, STR_PGMWIN, 1, 1) & 0xff))
        return 1;
    uint32_t w = call2(c, sp0, f_10141f00, ret[4], eng, ch);
    uint32_t pos = call2(c, sp0 - 8, f_10140280, ret[5], w, 0);
    uint32_t n = call1(c, sp0 - 16, f_10140270, ret[6], w);
    call2(c, sp0 - 20, f_101402c0, ret[7], w, n - 1);
    call2(c, sp0 - 28, f_10140220, ret[8], w, 1);
    call2(c, sp0 - 36, f_101402c0, ret[9], w, pos);
    uint32_t base = call1(c, sp0 - 44, f_10140350, ret[10], w);
    uint32_t len = guest_strlen(c, esc);
    call5(c, sp0 - 48, f_10141bb0, ret[11], eng, ch, 1, STR_REENTER, base + pos - len);
    call2(c, sp0, f_10141ed0, ret[12], eng, ch);
    return 0;
}

static const uint32_t RET_SYM8[2] = { 0x10137735u, 0x1013775au };
static const uint32_t RET_SYM16[2] = { 0x101377f4u, 0x1013781cu };
static const uint32_t RET_DOUBLE[2] = { 0x10137beau, 0x10137c21u };
static const uint32_t RET_INT[13] = { 0x101378c4u, 0x101378fbu, 0x1013790bu, 0x10137922u, 0x1013793bu, 0x10137945u,
                                      0x1013794du, 0x10137955u, 0x1013795du, 0x10137964u, 0x1013796au, 0x10137997u,
                                      0x101379a1u };
static const uint32_t RET_SHORT[13] = { 0x10137a45u, 0x10137a7cu, 0x10137a8cu, 0x10137aa3u, 0x10137abcu,
                                        0x10137ac6u, 0x10137aceu, 0x10137ad6u, 0x10137adeu, 0x10137ae5u,
                                        0x10137aebu, 0x10137b18u, 0x10137b22u };

/* the word at p is an integer: an optional sign, then digits only (a lone sign passes) */
static int int_word(cpu *c, uint32_t sp0, uint32_t p, uint32_t ret_first, uint32_t ret_loop)
{
    uint8_t x = rd8(c, p);
    if (x != '-' && x != '+' && !is_digit(c, sp0, x, ret_first)) return 0;
    for (;;) {
        x = rd8(c, ++p);
        if (!x) return 1;
        if (!is_digit(c, sp0, x, ret_loop)) return 0;
    }
}

/* the word at p is a decimal number: a sign or '.' or digit first, then digits and one '.' that is not
 * last */
static int double_word(cpu *c, uint32_t sp0, uint32_t p)
{
    uint8_t x = rd8(c, p);
    if (x != '-' && x != '+' && x != '.' && !is_digit(c, sp0, x, 0x10137b62u)) return 0;
    int dot = 0;
    for (;;) {
        x = rd8(c, p++);
        if (x == '.') {
            if (dot || !rd8(c, p)) return 0;
            dot = 1;
        }
        x = rd8(c, p);
        if (!x) return 1;
        if (x != '.' && !is_digit(c, sp0, x, 0x10137bbcu)) return 0;
    }
}

/* FUN_10137620: read the value of stream s's token from channel `ch` (the ref's type byte is s) and make
 * it the token data of the ref's token (FUN_101364c0): a symbol name (types -1/-2) or a number (-3, -4,
 * -5); other types take the value slot as it is. Empty lines are skipped; a word that is not a value is
 * reported and read again. After the word the rest of the line's newline is consumed. 0 when read; 1 at
 * the end of the input or on a stop request (the output's state cleared) or when the error handling
 * gives up. The frame (0x1c8 bytes + 4 registers) holds the word at +0x2c and the values. */
static uint32_t read_value(cpu *c, uint32_t sp, uint32_t eng, uint32_t ch, uint32_t ref)
{
    const uint32_t sp0 = sp - 0x1d8;    /* the original's esp in the body */
    const uint32_t word = sp0 + 0x2c, vptr = sp0 + 0x18;
    wr8(c, sp0 + 0x14, rd8(c, ref + 4));
    for (;;) {
        uint32_t s = rd32(c, sp0 + 0x14);
        uint8_t last = (uint8_t)call4(c, sp0, f_10138080, 0x10137657u, eng, s, ch, word);
        wr8(c, sp0 + 0x13, last);
        if (!last || (call1(c, sp0, f_10142350, 0x1013766cu, eng) & 0xff)) {
            uint32_t out = rd32(c, eng + ENG_OUT);
            wr8(c, out + 0x14, 0);
            wr32(c, rd32(c, eng + ENG_OUT) + 0x1a8, 0);
            c->edx = out;
            return 1;
        }
        if (last == '\n') continue;
        int16_t t = (int16_t)call1(c, sp0, f_10135b20, 0x10137684u, s);
        uint32_t fd = rd32(c, stream_desc(s & 0xff) + SD_FIELDS);
        int bad = 0;
        switch (t) {
        case T_SYM8: {
            uint8_t i = 0;
            if ((int16_t)rd16(c, fd + FD_NSYMS) > 0) {
                while (!streq(c, word, rd32(c, rd32(c, fd + FD_NAMES) + 4u * i))) {
                    i++;
                    if (!((int16_t)i < (int16_t)rd16(c, fd + FD_NSYMS))) break;
                }
            }
            wr8(c, sp0 + 0x1c, i);
            if ((int16_t)i == (int16_t)rd16(c, fd + FD_NSYMS)) {
                if (word_error(c, sp0, eng, ch, sp0 + 0x5c, RET_SYM8)) return 1;
                continue;
            }
            wr32(c, vptr, sp0 + 0x1c);
            break;
        }
        case T_SYM16: {
            int32_t i = 0;
            wr32(c, sp0 + 0x20, 0);
            if (0 < (int16_t)rd16(c, fd + FD_NSYMS)) {
                while (!streq(c, word, rd32(c, rd32(c, fd + FD_NAMES) + 4u * (uint32_t)i))) {
                    wr32(c, sp0 + 0x20, (uint32_t)++i);
                    if (!(i < (int16_t)rd16(c, fd + FD_NSYMS))) break;
                }
            }
            if (i == (int16_t)rd16(c, fd + FD_NSYMS)) {
                if (word_error(c, sp0, eng, ch, sp0 + 0x140, RET_SYM16)) return 1;
                continue;
            }
            wr32(c, vptr, sp0 + 0x20);
            break;
        }
        case T_INT:
            wr32(c, vptr, sp0 + 0x28);
            if (!int_word(c, sp0, word, 0x10137857u, 0x10137896u)) bad = 1;
            else wr32(c, sp0 + 0x28, import_call(c, sp0, IAT_ATOL, 0x10137cafu, 1, &word));
            if (bad) {
                if (number_error(c, sp0, eng, ch, sp0 + 0xa8, RET_INT)) return 1;
                continue;
            }
            break;
        case T_SHORT:
            wr32(c, vptr, sp0 + 0x24);
            if (!int_word(c, sp0, word, 0x101379d8u, 0x10137a17u)) bad = 1;
            else wr32(c, sp0 + 0x24, import_call(c, sp0, IAT_ATOI, 0x10137cc3u, 1, &word));
            if (bad) {
                if (number_error(c, sp0, eng, ch, sp0 + 0xf4, RET_SHORT)) return 1;
                continue;
            }
            break;
        case T_DOUBLE:
            wr32(c, vptr, sp0 + 0x54);
            if (!double_word(c, sp0, word)) {
                if (word_error(c, sp0, eng, ch, sp0 + 0x18c, RET_DOUBLE)) return 1;
                continue;
            }
            import_call(c, sp0, IAT_ATOF, 0x10137cdau, 1, &word);
            wr64(c, sp0 + 0x54, fx_to_f64(ST(0), FENV(c)));
            fpop(c);
            break;
        default:
            break;
        }
        call4(c, sp0, f_101364c0, 0x10137c62u, eng, rd32(c, sp0 + 0x14), rd32(c, ref), rd32(c, vptr));
        if (rd8(c, sp0 + 0x13) != '\n'
            && call2(c, sp0, f_10141e00, 0x10137c7bu, eng, ch) != '\n')
            call2(c, sp0, f_10141e80, 0x10137c8au, eng, ch);
        return 0;
    }
}

/* ------------------------------------------------------------------------------ splitting a token */

/* FUN_10136320: split off n (a duration: field 0 of stream s, an int or a short) at sync mark m: a new
 * sync mark next to m (after it for n < 0, before it otherwise), the token beyond it shortened by |n|, and
 * a new token of |n| inserted between m and the new mark. The new mark (0 if none could be made; also in
 * ecx, and left in m's argument slot as the original does); 0 when the token could not be inserted. The
 * argument slots of eng and s are the original's scratch for the shortened value. */
static uint32_t split_at(cpu *c, uint32_t sp, uint32_t eng, uint32_t s, uint32_t m, int32_t n)
{
    const uint32_t sp0 = sp - 0x24;
    const uint32_t si = s & 0xff;
    uint32_t e, tok, left, right, nm;
    if (n < 0) {
        e = fwd_link(c, m, si) & ~3u;
        tok = (e && is_mark(c, e)) ? 0 : e;
        nm = call5(c, sp0, f_10136720, 0x10136366u, eng, s, e, m, 0);
        left = m;
        right = nm;
    } else {
        e = back_link(c, RS(eng), m, si) & ~3u;
        tok = (e && is_mark(c, e)) ? 0 : e;
        nm = call5(c, sp0, f_10136720, 0x101363aau, eng, s, m, e, 0);
        left = nm;
        right = m;
    }
    wr32(c, sp + 0xc, nm);
    if (!nm) return 0;
    uint32_t an = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t desc = stream_desc(si);
    if (tok) {
        int16_t t = (int16_t)rd16(c, rd32(c, desc + SD_FIELDS) + FD_TYPE);
        uint32_t get = rd32(c, rd32(c, desc + SD_GETTERS));
        uint32_t src = 0;
        if (t == T_SHORT) {
            uint32_t f = icall1(c, sp0, get, 0x10136416u, tok + 8);
            uint32_t v = (uint32_t)(int32_t)(int16_t)rd16(c, f) - an;
            wr32(c, sp + 4, v);
            wr32(c, sp + 8, v);
            src = sp + 8;
        } else if (t == T_INT) {
            uint32_t f = icall1(c, sp0, get, 0x101363efu, tok + 8);
            wr32(c, sp + 4, rd32(c, f) - an);
            src = sp + 4;
        }
        if (src) call4(c, sp0 - 4, f_101364c0, 0x10136439u, eng, s, tok + 8, src);
    }
    /* the ref of |n| for the new token */
    wr32(c, sp0 + 0x18, an);
    int16_t t = (int16_t)rd16(c, rd32(c, desc + SD_FIELDS) + FD_TYPE);
    if (t == T_SHORT) {
        wr16(c, sp0 + 0x20, 0xfffcu);
        wr32(c, sp + 8, an);
        wr32(c, sp0 + 0x1c, sp + 8);
        wr8(c, sp0 + 0x22, 0);
    } else if (t == T_INT) {
        wr16(c, sp0 + 0x20, 0xfffdu);
        wr32(c, sp0 + 0x1c, sp0 + 0x18);
        wr8(c, sp0 + 0x22, 0);
    }
    uint32_t ok = call5(c, sp0, f_10136570, 0x101364a1u, eng, s, right, left, sp0 + 0x1c) & 0xff;
    nm = rd32(c, sp + 0xc);
    c->ecx = nm;
    return ok ? nm : 0;
}

/* ------------------------------------------------------------------------------------------ set-up */

/* FUN_10138430: the rule machine's set-up: the control stack entry sizes (a frame entry of the engine's
 * byte +0x70 rounded up to even), the control stack (64000 bytes, FUN_10139f90), the element pool (4096,
 * FUN_10139b20), the value stack (malloc 0x32, empty), and the symbol entry sizes of streams 1 and 2
 * (FUN_10128fe4). al 1, or 0 when out of memory. */
static uint32_t machine_init(cpu *c, uint32_t sp, uint32_t eng)
{
    const uint32_t at = sp - 4;
    wr32(c, WS(eng) + WS_SZ_FRAME, (((uint32_t)rd8(c, eng + 0x70) - 1u) | 1u) + 1u);
    wr32(c, WS(eng) + WS_SZ_UNDO, 0xc);
    wr32(c, WS(eng) + WS_SZ_POS, 8);
    wr32(c, WS(eng) + WS_SZ_LABEL, 6);
    wr32(c, WS(eng) + WS_SZ_CUTPT, 6);
    wr32(c, WS(eng) + WS_SZ_MARK, 2);
    uint32_t r = call2(c, at, f_10139f90, 0x10138490u, eng, 0xfa00);
    if (!(r & 0xff)) return r;
    r = call2(c, at, f_10139b20, 0x101384a4u, eng, 0x1000);
    if (!(r & 0xff)) return r;
    wr32(c, RS(eng) + 0xfd2, rd32(c, WS(eng) + WS_TOP));
    wr32(c, WS(eng) + 0x8f, 0);
    wr32(c, WS(eng) + 0x8b, 0);
    uint32_t size = 0x32;
    wr32(c, WS(eng) + WS_EVAL, import_call(c, at, IAT_MALLOC, 0x101384e1u, 1, &size));
    uint32_t ws = WS(eng);
    if (!rd32(c, ws + WS_EVAL)) return 0;
    wr8(c, ws + WS_EVAL_TOP, 0xff);
    /* FUN_10128fe4 */
    wr32(c, stream_desc(1) + SD_SYMBOL_SIZE, 3);
    wr32(c, stream_desc(1) + SD_SYMBOL_COPY, 3);
    wr32(c, stream_desc(2) + SD_SYMBOL_SIZE, 8);
    wr32(c, stream_desc(2) + SD_SYMBOL_COPY, 8);
    return 1;
}

/* ------------------------------------------------------------------------------------------ adapters */

PORT_FN(10138080) { port_ret(c, read_word(c, c->esp, ARG(0), ARG(1), ARG(2), ARG(3)), 0); }
PORT_FN(10137620) { port_ret(c, read_value(c, c->esp, ARG(0), ARG(1), ARG(2)), 0); }
PORT_FN(10136320) { port_ret(c, split_at(c, c->esp, ARG(0), ARG(1), ARG(2), (int32_t)ARG(3)), 0); }
PORT_FN(10138430) { port_ret(c, machine_init(c, c->esp, ARG(0)), 0); }
