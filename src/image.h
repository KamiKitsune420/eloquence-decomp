/* image - put an engine binary's data into the recompiled machine at its virtual addresses */
#ifndef IMAGE_H
#define IMAGE_H

#include "x86rt.h"

typedef struct { uint32_t base, size; } image_info;

/* from the file: every section at its preferred base */
int image_load(cpu *c, const char *path, image_info *info);

/* compiled in (tools/embed_image.py, src/gen/enu_data.c): the sections the recompiled code reads -
 * .rdata and .data; nothing touches .text - each with its trailing zeros cut */
typedef struct { uint32_t addr, len; const uint8_t *bytes; } image_section;

extern const image_section enu_sections[];
extern const unsigned enu_nsections;
extern const image_info enu_info;

void image_load_sections(cpu *c, const image_section *s, unsigned n);

#endif
