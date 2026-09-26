/* letters - letter-to-sound rules: the phones for the letters of a word (stream inp to stream phone).
 *
 * Hand-written from ENU.SYN's compiled rules; each is checked against the original with difftest
 * (DIFFTEST_UNINIT=1 DIFFTEST_RTRECOMP=1 --only <address>).
 */
#include "rule.h"

/* streams */
enum { INP = 1, PHONE = 2, MORPH = 3 };

/* The rules' shared variables (sync-mark values in the engine instance: short type, then the mark). The
 * names are from how these rules use them; provisional until the rules that set them are written. */
#define VAR_LETTER     0x2f3u       /* where the letters being converted start */
#define VAR_LETTER_END 0x2f9u       /* where they end */
#define VAR_WORD_END   0x311u       /* the end of the word */

/* Rule 100dbdd1: the letter b. "bt" together gives /t/ (debt, doubt); b alone gives /b/.
 *
 * Looks at stream morph. First try: back to where the letters start; if the input reads "bt", the end is
 * after the t (a choice point, label 2: try again from there); if the word's end can be reached from
 * there, phone t goes between start and end. If "bt" does not match, or the runtime backtracks to label 1:
 * phone b between the letter's start and end. */
static const uint32_t SCOPE_100dbdd1 = 0x1014e328u;    /* { morph } */
static const uint32_t STR_BT = 0x1014e428u;            /* inp: b t */
static const uint32_t PHONES_T = 0x1014e33cu;          /* phone: t */
static const uint32_t PHONES_B = 0x1014e338u;          /* phone: b */

RULE(100dbdd1, 0xbc, 0xb0, 0x68, 0x28, 0x1c, 0x10)
{
    const uint32_t letter = r->eng + VAR_LETTER, letter_end = r->eng + VAR_LETTER_END;
    uint32_t phones;

    rule_scope(r, 1, SCOPE_100dbdd1);
    rule_succeed(r, 1);
    if (rule_back_to(r, letter, INP) || rule_match(r, INP, 2, STR_BT)) goto plain_b;
retry:
    rule_mark_here(r, 2, letter_end);
    rule_set_a(r, r->eng + VAR_WORD_END);
    if (rule_advance_to_a(r)) goto next;
    rule_set_ab(r, letter, letter_end);
    phones = PHONES_T;
emit:
    if (!rule_insert(r, PHONE, 1, phones)) return RULE_DONE;
next:
    switch (rule_next(r, 0)) {
    case 1: goto plain_b;
    case 2: goto retry;
    case 3: return RULE_DONE;
    default: return RULE_FAILED;
    }
plain_b:
    rule_set_ab(r, letter, letter_end);
    phones = PHONES_B;
    goto emit;
}
