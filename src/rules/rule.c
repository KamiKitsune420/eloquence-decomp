/* rule - the frame every hand-written rule runs in (see rule.h) */
#include "rule.h"

void rule_run(cpu *c, const rule_layout *L, uint32_t (*body)(rule *r))
{
    rule r = rule_open(c, L->bytes);
    const uint32_t jb = r.fp - L->jb;
    volatile uint32_t result = RULE_FAILED;
    if (!RULE_CATCH(r, jb)) {
        if (!rule_enter(&r, r.fp - L->record, r.fp - L->slots, r.fp - L->scope, r.fp - L->seen, jb))
            result = body(&r);
    }
    rule_leave(&r);
    rule_return(&r, result);
}
