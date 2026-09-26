/* calls - ENU.SYN's first rule module (10001000-10004100): procedure rules.
 *
 * Most of these rules are wrappers: they take sync marks (and shorts) from their caller, keep copies in
 * their own registered variables, run one other rule on those - either as a goal the runtime may
 * backtrack into, or as a plain call - and hand the results back. They only do so while global d5e holds
 * its first value (d62: the rule language keeps its enumeration constants in the instance, set once at
 * start-up); otherwise they hold without doing anything. The rest are two-way switches on the same
 * global.
 *
 * Hand-written from the compiled rules; checked against the original with difftest
 * (DIFFTEST_UNINIT=1 DIFFTEST_RTRECOMP=1 --only <addresses>).
 */
#include "rule.h"

static const uint32_t SCOPE_NONE = 0x10193530u;    /* no streams */

/* the enumeration constants these rules compare global d5e with */
#define K_D62 0xd62u
#define K_D6E 0xd6eu

static int d5e_is(const rule *r, uint32_t k) { return rule_global16(r, 0xd5e) == rule_global16(r, k); }

/* Run rule f (n arguments, the instance first) as a goal, called with esp at fp - at: labels 1 and 2 mean
 * it held; if backtracking returns to label 3 its depth mark is dropped and it failed. */
static int goal(rule *r, uint32_t at, guest_fn f, int n, const uint32_t *a)
{
    int32_t depth = 0;
    rule_succeed(r, 2);
    rule_push_down(r);
    rule_push_retry(r, 3);
    if (rule_subn(r, at, f, n, a) == RULE_DONE) depth = 1;
    switch (rule_next(r, depth)) {
    case 1:
    case 2:
        return 1;
    case 3:
        rule_drop_top(r);
        return 0;
    default:
        return 0;
    }
}

/* a sync-mark variable of the rule: the copy of parameter i */
static void param(rule *r, uint32_t var, int i) { rule_var(r, var, rule_arg(r, i), T_SYNC); }
/* a short one */
static void param16(rule *r, uint32_t var, int i) { rule_var(r, var, rule_arg(r, i), T_SHORT); }
/* hand a short variable back to parameter i */
static void out16(rule *r, int i, uint32_t var) { wr16(r->c, rule_arg(r, i) + 2, rd16(r->c, var + 2)); }

/* ---------------------------------------------------------------------------------- the wrappers */

void f_10060ac8(cpu *c); void f_100ca510(cpu *c); void f_100ca726(cpu *c); void f_1005ea1f(cpu *c);
void f_1005848d(cpu *c); void f_100c6358(cpu *c); void f_100cfdae(cpu *c); void f_100cb610(cpu *c);
void f_100c67ab(cpu *c); void f_100cb31c(cpu *c); void f_100d0cb8(cpu *c); void f_100d1bda(cpu *c);
void f_100d1109(cpu *c); void f_100d0ed6(cpu *c); void f_100cc239(cpu *c); void f_100d21d8(cpu *c);
void f_100cac59(cpu *c); void f_1010cf80(cpu *c); void f_1010df1d(cpu *c); void f_10104dc1(cpu *c);
void f_1003ac8e(cpu *c); void f_100879b4(cpu *c); void f_100886f7(cpu *c); void f_1009326d(cpu *c);
void f_1008b068(cpu *c); void f_1009a516(cpu *c); void f_100f70f7(cpu *c); void f_100f8d62(cpu *c);
void f_100f91aa(cpu *c); void f_100fed83(cpu *c); void f_100ffc39(cpu *c); void f_100fff4d(cpu *c);
void f_1009eab2(cpu *c); void f_10119d9c(cpu *c); void f_100d89a3(cpu *c); void f_1011f5eb(cpu *c);
void f_10075f87(cpu *c); void f_1011b21a(cpu *c); void f_10081968(cpu *c); void f_10072df4(cpu *c);
void f_1006215d(cpu *c); void f_10072ff8(cpu *c); void f_1006db67(cpu *c);

/* 10001000(a, b): rule 10060ac8(a, b) as a goal */
RULE(10001000, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe4, f_10060ac8, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    return RULE_DONE;
}

/* 100010f2(a, b, c, d): rule 100ca510(a, b, c, d) as a goal; b, c, d come back */
RULE(100010f2, 0xdc, 0xd0, 0x88, 0x48, 0x30, 0x3c)
{
    const uint32_t a = r->fp - 0x24, b = r->fp - 0xc, cc = r->fp - 0x14, d = r->fp - 0x1c;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0x100, f_100ca510, 5, (const uint32_t[]){ r->eng, a, b, cc, d }))
        return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    rule_out(r, rule_arg(r, 2), cc);
    rule_out(r, rule_arg(r, 3), d);
    return RULE_DONE;
}

/* 1000122f(a, b): rule 100ca726(a, b) as a goal; b comes back */
RULE(1000122f, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe8, f_100ca726, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 1000132c(a, b): rule 1005ea1f(a, b) as a goal */
RULE(1000132c, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe4, f_1005ea1f, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    return RULE_DONE;
}

/* 1000141e(a, b, c): rule 1005848d(a, b, c) as a goal; b comes back */
RULE(1000141e, 0xd4, 0xc8, 0x80, 0x40, 0x28, 0x34)
{
    const uint32_t a = r->fp - 0x1c, b = r->fp - 0xc, cc = r->fp - 0x14;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xf4, f_1005848d, 4, (const uint32_t[]){ r->eng, a, b, cc })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 1000152e(a, b): rule 100c6358(a, b) */
RULE(1000152e, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c)
{
    const uint32_t a = r->fp - 0x10, b = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd0, f_100c6358, 3, (const uint32_t[]){ r->eng, a, b });
    return RULE_DONE;
}

/* 100015d2(a, b): rule 100cfdae(a, b) as a goal; b comes back */
RULE(100015d2, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe8, f_100cfdae, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 100016cf(a, b): rule 100cb610(a, b) as a goal; both come back */
RULE(100016cf, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0xc, b = r->fp - 0x14;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe8, f_100cb610, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 0), a);
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 100017d5(a, b): rule 100c67ab(a, b) as a goal */
RULE(100017d5, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe4, f_100c67ab, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    return RULE_DONE;
}

/* 100018c7(a, b): rule 100cb31c(a, b); b comes back */
RULE(100018c7, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c)
{
    const uint32_t a = r->fp - 0x10, b = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd0, f_100cb31c, 3, (const uint32_t[]){ r->eng, a, b });
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 10001974(a, b): rule 100d0cb8(a, b); a comes back */
RULE(10001974, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c)
{
    const uint32_t a = r->fp - 0x8, b = r->fp - 0x10;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd0, f_100d0cb8, 3, (const uint32_t[]){ r->eng, a, b });
    rule_out(r, rule_arg(r, 0), a);
    return RULE_DONE;
}

/* 10001a21, 10001ace, 10001b7b, 10001d4e (a, b): rules 100d1bda, 100d1109, 100d0ed6, 100d21d8 (a, b);
 * b comes back */
static uint32_t call_ab_out_b(rule *r, guest_fn f)
{
    const uint32_t a = r->fp - 0x10, b = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd0, f, 3, (const uint32_t[]){ r->eng, a, b });
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}
RULE(10001a21, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab_out_b(r, f_100d1bda); }
RULE(10001ace, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab_out_b(r, f_100d1109); }
RULE(10001b7b, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab_out_b(r, f_100d0ed6); }
RULE(10001d4e, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab_out_b(r, f_100d21d8); }

/* 10001c28(a, b, short m, short n): rule 100cc239(a, b, m, n) as a goal; b comes back */
RULE(10001c28, 0xd4, 0xc8, 0x80, 0x40, 0x28, 0x34)
{
    const uint32_t a = r->fp - 0x1c, b = r->fp - 0x14, m = r->fp - 0xc, n = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    param16(r, m, 2);
    param16(r, n, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xf8, f_100cc239, 5, (const uint32_t[]){ r->eng, a, b, m, n }))
        return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 10001dfb(a, b, c, d): rule 100cac59(a, b, c, d) */
RULE(10001dfb, 0xd4, 0xcc, 0x84, 0x44, 0x2c, 0x38)
{
    const uint32_t a = r->fp - 0x20, b = r->fp - 0x10, cc = r->fp - 0x8, d = r->fp - 0x18;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xe8, f_100cac59, 5, (const uint32_t[]){ r->eng, a, b, cc, d });
    return RULE_DONE;
}

/* 10001ecf(a, b): rule 1010cf80(a, b) as a goal */
RULE(10001ecf, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe4, f_1010cf80, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    return RULE_DONE;
}

/* 10001fc1(a, b): rule 1010df1d(a, b) as a goal; b comes back */
RULE(10001fc1, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe8, f_1010df1d, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 100020be(a, b, c, d): rule 10104dc1(a, c, d); b and c come back */
RULE(100020be, 0xd8, 0xcc, 0x84, 0x44, 0x2c, 0x38)
{
    const uint32_t a = r->fp - 0x18, b = r->fp - 0x20, cc = r->fp - 0x8, d = r->fp - 0x10;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xe8, f_10104dc1, 4, (const uint32_t[]){ r->eng, a, cc, d });
    rule_out(r, rule_arg(r, 1), b);
    rule_out(r, rule_arg(r, 2), cc);
    return RULE_DONE;
}

/* 100021a2(a, b, c, d): rule 1003ac8e(a, c, d) as a goal */
RULE(100021a2, 0xd8, 0xd0, 0x88, 0x48, 0x30, 0x3c)
{
    const uint32_t a = r->fp - 0x1c, b = r->fp - 0x24, cc = r->fp - 0x14, d = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xf8, f_1003ac8e, 4, (const uint32_t[]){ r->eng, a, cc, d })) return RULE_FAILED;
    return RULE_DONE;
}

/* 100022bf(a, b, c, d): rule 100879b4(a, b, c, d) as a goal */
RULE(100022bf, 0xd8, 0xd0, 0x88, 0x48, 0x30, 0x3c)
{
    const uint32_t a = r->fp - 0xc, b = r->fp - 0x14, cc = r->fp - 0x1c, d = r->fp - 0x24;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xfc, f_100879b4, 5, (const uint32_t[]){ r->eng, a, b, cc, d }))
        return RULE_FAILED;
    return RULE_DONE;
}

/* 100023e0(a, b, c, d): rule 100886f7(a, b, c, d) */
RULE(100023e0, 0xd4, 0xcc, 0x84, 0x44, 0x2c, 0x38)
{
    const uint32_t a = r->fp - 0x20, b = r->fp - 0x10, cc = r->fp - 0x8, d = r->fp - 0x18;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xe8, f_100886f7, 5, (const uint32_t[]){ r->eng, a, b, cc, d });
    return RULE_DONE;
}

/* 100024b4, 10002dce, 10003112 (a, b): rules 1009326d, 100d89a3, 1006215d (a, b) */
static uint32_t call_ab(rule *r, guest_fn f)
{
    const uint32_t a = r->fp - 0x10, b = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd0, f, 3, (const uint32_t[]){ r->eng, a, b });
    return RULE_DONE;
}
RULE(100024b4, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab(r, f_1009326d); }
RULE(10002dce, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab(r, f_100d89a3); }
RULE(10003112, 0xc4, 0xbc, 0x74, 0x34, 0x28, 0x1c) { return call_ab(r, f_1006215d); }

/* 10002558, 1000260b (a, b, c): rules 1008b068, 1009a516 (a, c) */
static uint32_t call_ac(rule *r, guest_fn f)
{
    const uint32_t a = r->fp - 0x8, b = r->fp - 0x18, cc = r->fp - 0x10;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd8, f, 3, (const uint32_t[]){ r->eng, a, cc });
    return RULE_DONE;
}
RULE(10002558, 0xcc, 0xc4, 0x7c, 0x3c, 0x24, 0x30) { return call_ac(r, f_1008b068); }
RULE(1000260b, 0xcc, 0xc4, 0x7c, 0x3c, 0x24, 0x30) { return call_ac(r, f_1009a516); }

/* 100026be, 10003006 (a): rules 100f70f7, 10081968 (a) */
static uint32_t call_a(rule *r, guest_fn f)
{
    const uint32_t a = r->fp - 0x8;
    param(r, a, 0);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xc4, f, 2, (const uint32_t[]){ r->eng, a });
    return RULE_DONE;
}
RULE(100026be, 0xbc, 0xb4, 0x6c, 0x2c, 0x20, 0x14) { return call_a(r, f_100f70f7); }
RULE(10003006, 0xbc, 0xb4, 0x6c, 0x2c, 0x20, 0x14) { return call_a(r, f_10081968); }

/* 1000274e(a, b, short n): rule 100f8d62(a, b, n) as a goal; n comes back. NOT IN USE: 192 of its 7074
 * calls differ from the original in one heap byte (not found yet); the lifted rule runs instead. */
#if 0
RULE(1000274e, 0xd0, 0xc4, 0x7c, 0x3c, 0x24, 0x30)
{
    const uint32_t a = r->fp - 0x18, b = r->fp - 0x10, n = r->fp - 0x8;
    param(r, a, 0);
    param(r, b, 1);
    param16(r, n, 2);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xf0, f_100f8d62, 4, (const uint32_t[]){ r->eng, a, b, n })) return RULE_FAILED;
    out16(r, 2, n);
    return RULE_DONE;
}
#endif

/* 10002860(a, b, short n): rule 100f91aa, which takes its arguments in globals 293, 299 (sync marks) and
 * f80 (a short) and leaves its results there; all three come back */
RULE(10002860, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    cpu *c = r->c;
    const uint32_t a = r->fp - 0xc, b = r->fp - 0x14, n = r->fp - 0x4;
    const uint32_t ga = r->eng + 0x293, gb = r->eng + 0x299, gn = r->eng + 0xf80;
    param(r, a, 0);
    param(r, b, 1);
    param16(r, n, 2);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) {
        rule_out(r, ga, a);
        rule_out(r, gb, b);
        wr16(c, gn + 2, rd16(c, n + 2));
        rule_sub(r, 0xd0, f_100f91aa);
        rule_out(r, a, ga);
        rule_out(r, b, gb);
        wr16(c, n + 2, rd16(c, gn + 2));
    }
    rule_out(r, rule_arg(r, 0), a);
    rule_out(r, rule_arg(r, 1), b);
    out16(r, 2, n);
    return RULE_DONE;
}

/* 10002963(a, b): rule 100fed83(a, b) as a goal; b comes back */
RULE(10002963, 0xcc, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe8, f_100fed83, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    rule_out(r, rule_arg(r, 1), b);
    return RULE_DONE;
}

/* 10002a60(a, b): rule 100ffc39(a) as a goal */
RULE(10002a60, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0xc, b = r->fp - 0x14;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe0, f_100ffc39, 2, (const uint32_t[]){ r->eng, a })) return RULE_FAILED;
    return RULE_DONE;
}

/* 10002b4e(a, b): rule 100fff4d(a, b) as a goal */
RULE(10002b4e, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t a = r->fp - 0x14, b = r->fp - 0xc;
    param(r, a, 0);
    param(r, b, 1);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && !goal(r, 0xe4, f_100fff4d, 3, (const uint32_t[]){ r->eng, a, b })) return RULE_FAILED;
    return RULE_DONE;
}

/* 10002c40(a, b, c, d): rule 1009eab2(a, b) */
RULE(10002c40, 0xd4, 0xcc, 0x84, 0x44, 0x2c, 0x38)
{
    const uint32_t a = r->fp - 0x8, b = r->fp - 0x10, cc = r->fp - 0x18, d = r->fp - 0x20;
    param(r, a, 0);
    param(r, b, 1);
    param(r, cc, 2);
    param(r, d, 3);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xe0, f_1009eab2, 3, (const uint32_t[]){ r->eng, a, b });
    return RULE_DONE;
}

/* 10002d0c(short n, a, b): rule 10119d9c(n, a, b); n comes back */
RULE(10002d0c, 0xc8, 0xc0, 0x78, 0x38, 0x20, 0x2c)
{
    const uint32_t n = r->fp - 0x4, a = r->fp - 0x14, b = r->fp - 0xc;
    param16(r, n, 0);
    param(r, a, 1);
    param(r, b, 2);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xd8, f_10119d9c, 4, (const uint32_t[]){ r->eng, n, a, b });
    out16(r, 0, n);
    return RULE_DONE;
}

/* 10003232(short n): rule 1006db67(n); n comes back */
RULE(10003232, 0xb8, 0xb0, 0x68, 0x28, 0x1c, 0x10)
{
    const uint32_t n = r->fp - 0x4;
    param16(r, n, 0);
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_subn(r, 0xc0, f_1006db67, 2, (const uint32_t[]){ r->eng, n });
    out16(r, 0, n);
    return RULE_DONE;
}

/* 10002efe, 10003096, 100031b6: rules 10075f87, 10072df4, 10072ff8 */
static uint32_t call_only(rule *r, guest_fn f)
{
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62)) rule_sub(r, 0xb8, f);
    return RULE_DONE;
}
RULE(10002efe, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18) { return call_only(r, f_10075f87); }
RULE(10003096, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18) { return call_only(r, f_10072df4); }
RULE(100031b6, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18) { return call_only(r, f_10072ff8); }

/* 10002e72, 10002f7a: rules 1011f5eb, 1011b21a, when also global 8eb holds 8ef */
static uint32_t call_if_8eb(rule *r, guest_fn f)
{
    rule_scope(r, 0, SCOPE_NONE);
    if (d5e_is(r, K_D62) && rule_global16(r, 0x8eb) == rule_global16(r, 0x8ef)) rule_sub(r, 0xb8, f);
    return RULE_DONE;
}
RULE(10002e72, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18) { return call_if_8eb(r, f_1011f5eb); }
RULE(10002f7a, 0xb4, 0xac, 0x64, 0x24, 0xc, 0x18) { return call_if_8eb(r, f_1011b21a); }

/* ---------------------------------------------------------------------------------- the switches */

/* Run rule f when global d5e is d62 or d6e: the two are alternatives (a choice point for the second).
 * Labels: 1 and 4 hold without running f, 2 tries d6e, 3 runs it. */
static uint32_t switch_d62_d6e(rule *r, guest_fn f)
{
    rule_scope(r, 0, SCOPE_NONE);
    rule_succeed(r, 1);
    rule_push_retry(r, 2);
    if (d5e_is(r, K_D62)) goto run;
    for (;;) {
        switch (rule_next(r, 0)) {
        case 1:
        case 4:
            return RULE_DONE;
        case 2:
            if (d5e_is(r, K_D6E)) goto run;
            continue;
        case 3:
            goto run;
        default:
            return RULE_FAILED;
        }
    }
run:
    rule_sub(r, 0xbc, f);
    return RULE_DONE;
}

void f_100699b5(cpu *c); void f_10069c1f(cpu *c); void f_1006a87c(cpu *c); void f_1006abf7(cpu *c);
void f_1006ad40(cpu *c); void f_1006b491(cpu *c); void f_1006b0bb(cpu *c); void f_10069725(cpu *c);
void f_1006d0d9(cpu *c); void f_1006b60f(cpu *c); void f_1006b940(cpu *c); void f_1006ba3b(cpu *c);
void f_1006bed3(cpu *c); void f_1006bdb5(cpu *c); void f_1006c151(cpu *c); void f_1006c78f(cpu *c);
void f_1006c3d5(cpu *c); void f_10069a3d(cpu *c);

#define SWITCH_RULE(a, f) RULE(a, 0xb8, 0xb0, 0x68, 0x28, 0x1c, 0x10) { return switch_d62_d6e(r, f); }
SWITCH_RULE(100032cd, f_100699b5)
SWITCH_RULE(1000338f, f_10069c1f)
SWITCH_RULE(10003451, f_1006a87c)
SWITCH_RULE(10003513, f_1006abf7)
SWITCH_RULE(100035d5, f_1006ad40)
SWITCH_RULE(10003697, f_1006b491)
SWITCH_RULE(10003759, f_1006b0bb)
SWITCH_RULE(10003907, f_10069725)
SWITCH_RULE(100039c9, f_1006d0d9)
SWITCH_RULE(10003a8b, f_1006b60f)
SWITCH_RULE(10003b4d, f_1006b940)
SWITCH_RULE(10003c0f, f_1006ba3b)
SWITCH_RULE(10003cd1, f_1006bed3)
SWITCH_RULE(10003d93, f_1006bdb5)
SWITCH_RULE(10003e55, f_1006c151)
SWITCH_RULE(10003f17, f_1006c78f)
SWITCH_RULE(10003fd9, f_1006c3d5)

/* 1000381b: the same switch around rule 10069a3d, entered only when global 1b5 is not 1c5 or global 815
 * is 577; its labels one higher, and label 1 starts it over. */
RULE(1000381b, 0xb8, 0xb0, 0x68, 0x28, 0x1c, 0x10)
{
    rule_scope(r, 0, SCOPE_NONE);
    if (rule_global16(r, 0x1b5) == rule_global16(r, 0x1c5) && rule_global16(r, 0x815) != rule_global16(r, 0x577))
        return RULE_FAILED;
start:
    rule_succeed(r, 2);
    rule_push_retry(r, 3);
    if (d5e_is(r, K_D62)) goto run;
    for (;;) {
        switch (rule_next(r, 0)) {
        case 1:
            goto start;
        case 2:
        case 5:
            return RULE_DONE;
        case 3:
            if (d5e_is(r, K_D6E)) goto run;
            continue;
        case 4:
            goto run;
        default:
            return RULE_FAILED;
        }
    }
run:
    rule_sub(r, 0xbc, f_10069a3d);
    return RULE_DONE;
}
