/* image - map an engine binary into the recompiled machine's memory at its preferred base.
 *
 * Only the data matters (the code is recompiled), but mapping every section keeps addresses honest.
 */
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int image_load(cpu *c, const char *path, image_info *info)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -1; }
    fclose(f);
    uint32_t pe;
    memcpy(&pe, d + 0x3c, 4);
    uint16_t nsec, opt;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&opt, d + pe + 20, 2);
    uint32_t base, size;
    memcpy(&base, d + pe + 24 + 28, 4);
    memcpy(&size, d + pe + 24 + 56, 4);
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = d + pe + 24 + opt + 40 * i;
        uint32_t vsz, va, rsz, ra;
        memcpy(&vsz, s + 8, 4);
        memcpy(&va, s + 12, 4);
        memcpy(&rsz, s + 16, 4);
        memcpy(&ra, s + 20, 4);
        uint32_t cp = rsz < vsz ? rsz : vsz;
        if (ra + cp <= (uint32_t)n) x86_write(c, base + va, d + ra, cp);
    }
    if (info) { info->base = base; info->size = size; }
    free(d);
    return 0;
}

void image_load_sections(cpu *c, const image_section *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++) x86_write(c, s[i].addr, s[i].bytes, s[i].len);
}
