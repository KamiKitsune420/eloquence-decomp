/* misc - small rules not placed with the others yet (they test the rule language's global variables, or
 * do nothing but enter and leave).
 *
 * Hand-written from ENU.SYN's compiled rules; each is checked against the original with difftest
 * (DIFFTEST_UNINIT=1 DIFFTEST_RTRECOMP=1 --only <address>).
 */
#include "rule.h"

static const uint32_t SCOPE_NONE_10193530 = 0x10193530u;   /* no streams */
static const uint32_t SCOPE_NONE_10193554 = 0x10193554u;   /* no streams */

/* Rule 10004106: nothing to do (enters, looks at no stream, done). */
RULE(10004106, 0xb0, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193530);
    return RULE_DONE;
}

/* Rule 1000d377: holds when global 1b1 is 1, unless global 1cd equals global 141 but not global 1b9. */
RULE(1000d377, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193554);
    if (rule_global16(r, 0x1b1) != 1) return RULE_FAILED;
    if (rule_global16(r, 0x1cd) != rule_global16(r, 0x141)) return RULE_DONE;
    return rule_global16(r, 0x1cd) == rule_global16(r, 0x1b9) ? RULE_DONE : RULE_FAILED;
}

/* ------------------------------------------------------------------ rules that run others in order */

void f_10035aec(cpu *c);
void f_10035b73(cpu *c);
void f_1012b210(cpu *c);
void f_10004483(cpu *c);
void f_1007afeb(cpu *c);
void f_10006636(cpu *c);
void f_100066b9(cpu *c);

static const uint32_t SCOPE_NONE_10193538 = 0x10193538u;   /* no streams */
static const uint32_t SCOPE_NONE_1019353c = 0x1019353cu;
static const uint32_t SCOPE_NONE_1019355c = 0x1019355cu;
static const uint32_t SCOPE_NONE_1019358c = 0x1019358cu;
static const uint32_t SCOPE_INTON_PHR = 0x1014f804u;       /* { inton_phr } */

/* Rule 100044ee: nothing to do. */
RULE(100044ee, 0xb0, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193538);
    return RULE_DONE;
}

/* Rule 1000409b: runs 10035b73. */
RULE(1000409b, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193530);
    rule_sub(r, 0xc4, f_10035b73);
    return RULE_DONE;
}

/* Rule 1000416c: runs 10035aec. */
RULE(1000416c, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193530);
    rule_sub(r, 0xc4, f_10035aec);
    return RULE_DONE;
}

/* Rule 10004483: runs the engine's 1012b210. */
RULE(10004483, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_10193538);
    rule_sub(r, 0xc4, f_1012b210);
    return RULE_DONE;
}

/* Rule 10021189: runs rule 10004483. */
RULE(10021189, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_1019358c);
    rule_sub(r, 0xc4, f_10004483);
    return RULE_DONE;
}

/* Rule 1011f0e7: runs 1007afeb, looking at inton_phr. */
RULE(1011f0e7, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 1, SCOPE_INTON_PHR);
    rule_sub(r, 0xc4, f_1007afeb);
    return RULE_DONE;
}

/* Rule 100065c5: runs 10006636, then 100066b9. */
RULE(100065c5, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_1019353c);
    rule_sub(r, 0xc4, f_10006636);
    rule_sub(r, 0xc8, f_100066b9);
    return RULE_DONE;
}

/* Rule 10011bb8: global 1c1 := global 1d9. */
RULE(10011bb8, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18)
{
    rule_scope(r, 0, SCOPE_NONE_1019355c);
    wr16(r->c, r->eng + 0x1c1, (uint16_t)rule_global16(r, 0x1d9));
    return RULE_DONE;
}
