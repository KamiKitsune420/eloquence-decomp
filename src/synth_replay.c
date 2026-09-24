/* synth_replay - run the recompiled synthesizer (FUN_1013caf0) on every frame recorded from the real engine
 * (ecisay --dump-frames) and compare what it leaves behind: the whole state block, the return value and
 * the samples.
 *
 *   synth_replay pkg/ENU.SYN build/hello.frm [-v]
 *
 * The recording was made with ENU.SYN relocated; pointers into the image are moved back to the preferred
 * base, since the recompiled code uses the file's addresses. Which offsets hold such pointers is decided
 * over all records (an offset whose value points into the image in every one), so that neighbouring
 * small numbers are never mistaken for one.
 */
#include "x86rt.h"
#include "image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_TOP 0x00f00000u
#define FRAME_AT  0x00800000u
#define RETURN_AT 0xdead0000u

typedef struct {
    uint32_t addr, rbase, sz, ret, n;
    float frame[64];
    uint8_t *before, *after;
    int32_t *out;
} rec;

static void output_sink(cpu *c)        /* the frame builder's callback: accept, return 1 */
{
    c->eax = 1;
    c->esp += 4;
}

static guest_fn resolve(cpu *c, uint32_t target)
{
    (void)c;
    (void)target;
    return output_sink;
}

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: synth_replay ENU.SYN frames.frm [-v]\n"); return 2; }
    int verbose = argc > 3;
    cpu *c = x86_new();
    image_info img;
    if (image_load(c, argv[1], &img)) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }
    c->resolve = resolve;
    guest_fn synth = x86_lookup(0x1013caf0u);
    FILE *f = fopen(argv[2], "rb");
    if (!f || !synth) return 2;

    rec *r = NULL;
    long nrec = 0, cap = 0;
    for (;;) {
        char tag[4];
        if (fread(tag, 1, 4, f) != 4) break;
        if (memcmp(tag, "FRM2", 4)) { fprintf(stderr, "bad record\n"); return 2; }
        if (nrec == cap) { cap = cap ? cap * 2 : 256; r = (rec *)realloc(r, (size_t)cap * sizeof *r); }
        rec *x = &r[nrec++];
        fread(&x->addr, 4, 1, f);
        fread(&x->rbase, 4, 1, f);
        fread(x->frame, 4, 64, f);
        fread(&x->sz, 4, 1, f);
        x->before = (uint8_t *)malloc(x->sz);
        x->after = (uint8_t *)malloc(x->sz);
        fread(x->before, 1, x->sz, f);
        fread(x->after, 1, x->sz, f);
        fread(&x->ret, 4, 1, f);
        fread(&x->n, 4, 1, f);
        x->out = (int32_t *)malloc(4 * (x->n + 1));
        fread(x->out, 4, x->n, f);
    }
    fclose(f);
    if (!nrec) return 2;

    /* offsets that point into the image in every record */
    uint32_t sz = r[0].sz, rb = r[0].rbase;
    uint8_t *ptr = (uint8_t *)calloc(sz, 1);
    for (uint32_t i = 0; i + 4 <= sz; i++) {
        int all = 1;
        for (long k = 0; k < nrec && all; k++) {
            uint32_t v = get32(r[k].before + i);
            all = v >= rb && v < rb + img.size;
        }
        ptr[i] = (uint8_t)all;
    }
    for (long k = 0; k < nrec; k++)
        for (uint32_t i = 0; i + 4 <= sz; i++)
            if (ptr[i]) {
                uint32_t v;
                v = get32(r[k].before + i) - rb + img.base;
                memcpy(r[k].before + i, &v, 4);
                v = get32(r[k].after + i) - rb + img.base;
                memcpy(r[k].after + i, &v, 4);
            }

    long bad = 0, samples = 0, badsamples = 0;
    uint8_t *got = (uint8_t *)malloc(sz);
    for (long k = 0; k < nrec; k++) {
        rec *x = &r[k];
        x86_write(c, x->addr, x->before, sz);
        x86_write(c, FRAME_AT, x->frame, sizeof x->frame);
        c->esp = STACK_TOP;
        push32(c, FRAME_AT);
        push32(c, x->addr);
        push32(c, RETURN_AT);
        c->ftop = 0;
        synth(c);
        x86_read(c, x->addr, got, sz);
        int diff = -1;
        for (uint32_t i = 0; i < sz; i++)
            if (got[i] != x->after[i]) { diff = (int)i; break; }
        int sdiff = 0;
        for (uint32_t i = 0; i < x->n; i++) {
            samples++;
            if ((int32_t)rd32(c, x->addr + 0x70a + 4 * i) != x->out[i]) { sdiff++; badsamples++; }
        }
        int rdiff = (c->eax & 0xff) != (x->ret & 0xff);
        if (diff >= 0 || sdiff || rdiff) {
            bad++;
            if (bad <= 10 || verbose)
                printf("frame %ld: state differs first at +0x%x (got %02x want %02x), %d samples differ%s\n", k, diff,
                       diff >= 0 ? got[diff] : 0, diff >= 0 ? x->after[diff] : 0, sdiff, rdiff ? ", return differs" : "");
        }
    }
    printf("%ld frames, %ld differ; %ld samples, %ld differ\n", nrec, bad, samples, badsamples);
    return bad != 0;
}
