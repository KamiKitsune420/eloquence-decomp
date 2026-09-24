/* klatt_check - run the hand-written synthesizer (klatt.c) on every frame recorded from the real engine
 * (ecisay --dump-frames, the FRM2 files synth_replay reads) and compare with the recording: the whole
 * state block after the call, the output samples of the last block and the return value.
 *
 *   klatt_check pkg/ENU.SYN build/hello.frm [more.frm ...] [-v]
 *   klatt_check pkg/ENU.SYN -diff build/hello.frm [more.frm ...]
 *
 * -diff: a differential test against the recompiled original (gen_synth.c on the soft x86) for the paths
 * the recordings do not reach. Starting from each file's first recorded state, both run the file's frame
 * sequence, mutated per scenario (flutter, the other sample-rate modes, 8 formants, source-only output,
 * unit gain, odd frame lengths / F0 = 0 / OQ = 0 / diplophonia / tilt), and are compared after every
 * frame: the state, the return value and every sample block passed to the output callback.
 *
 * As in synth_replay, pointers into the image (offsets that hold one in every record) are moved from the
 * recording's base to the preferred base 0x10000000. The image data (tables, the handle's ID string)
 * comes from ENU.SYN, mapped section by section at its preferred base.
 */
#include "klatt.h"
#include "image.h"
#include "x86rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr, rbase, sz, ret, n;
    float frame[64];
    uint8_t *before, *after;
    int32_t *out;
} rec;

typedef struct { uint8_t *flat; uint32_t base, size; long calls; } image;

static const void *img(void *user, uint32_t addr, size_t n)
{
    image *im = (image *)user;
    if (addr < im->base || addr - im->base + n > im->size) return NULL;
    return im->flat + (addr - im->base);
}

static uint8_t output(void *user, uint32_t cookie, void *samples, int32_t count)
{
    (void)cookie; (void)samples; (void)count;
    ((image *)user)->calls++;
    return 1;                   /* what the frame builder's callback returns */
}

static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* ENU.SYN's sections at their virtual addresses, in one flat buffer */
static int map_image(const char *path, image *im)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -1; }
    fclose(f);
    uint32_t pe = get32(d + 0x3c);
    uint16_t nsec, opt;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&opt, d + pe + 20, 2);
    im->base = get32(d + pe + 24 + 28);
    im->size = get32(d + pe + 24 + 56);
    im->flat = (uint8_t *)calloc(im->size, 1);
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = d + pe + 24 + opt + 40 * i;
        uint32_t vsz = get32(s + 8), va = get32(s + 12), rsz = get32(s + 16), ra = get32(s + 20);
        uint32_t cp = rsz < vsz ? rsz : vsz;
        if (ra + cp <= (uint32_t)n && va + cp <= im->size) memcpy(im->flat + va, d + ra, cp);
    }
    free(d);
    return 0;
}

static long load(const char *path, rec **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    rec *r = NULL;
    long nrec = 0, cap = 0;
    for (;;) {
        char tag[4];
        if (fread(tag, 1, 4, f) != 4) break;
        if (memcmp(tag, "FRM2", 4)) { fprintf(stderr, "%s: bad record\n", path); fclose(f); return -1; }
        if (nrec == cap) { cap = cap ? cap * 2 : 256; r = (rec *)realloc(r, (size_t)cap * sizeof *r); }
        rec *x = &r[nrec++];
        int ok = fread(&x->addr, 4, 1, f) == 1 && fread(&x->rbase, 4, 1, f) == 1 &&
                 fread(x->frame, 4, 64, f) == 64 && fread(&x->sz, 4, 1, f) == 1;
        if (!ok || x->sz != KLATT_STATE_SIZE) { fprintf(stderr, "%s: truncated record\n", path); fclose(f); return -1; }
        x->before = (uint8_t *)malloc(x->sz);
        x->after = (uint8_t *)malloc(x->sz);
        ok = fread(x->before, 1, x->sz, f) == x->sz && fread(x->after, 1, x->sz, f) == x->sz &&
             fread(&x->ret, 4, 1, f) == 1 && fread(&x->n, 4, 1, f) == 1;
        x->out = ok ? (int32_t *)malloc(4 * ((size_t)x->n + 1)) : NULL;
        if (!ok || fread(x->out, 4, x->n, f) != x->n) { fprintf(stderr, "%s: truncated record\n", path); fclose(f); return -1; }
    }
    fclose(f);
    *out = r;
    return nrec;
}

/* offsets that point into the image in every record: rebase them to the preferred base */
static void relocate(rec *r, long nrec, const image *im)
{
    uint32_t sz = r[0].sz, rb = r[0].rbase;
    for (uint32_t i = 0; i + 4 <= sz; i++) {
        int all = 1;
        for (long k = 0; k < nrec && all; k++) {
            uint32_t v = get32(r[k].before + i);
            all = v >= rb && v < rb + im->size;
        }
        if (!all) continue;
        for (long k = 0; k < nrec; k++) {
            uint32_t v = get32(r[k].before + i) - rb + im->base;
            memcpy(r[k].before + i, &v, 4);
            v = get32(r[k].after + i) - rb + im->base;
            memcpy(r[k].after + i, &v, 4);
        }
    }
}

/* ------------------------------------------------------------------------ differential mode */

#define STACK_TOP 0x00f00000u
#define FRAME_AT  0x00800000u
#define RETURN_AT 0xdead0000u
#define MAXCAP    8192

typedef struct { int n; int32_t v[MAXCAP]; } capture;
static capture cap_guest, cap_host;

static void guest_sink(cpu *c)          /* the frame builder's callback: record the block, return 1 */
{
    uint32_t desc = rd32(c, c->esp + 8);
    int32_t count = (int32_t)rd32(c, desc);
    uint32_t p = rd32(c, desc + 4);
    for (int32_t i = 0; i < count && cap_guest.n < MAXCAP; i++) cap_guest.v[cap_guest.n++] = (int32_t)rd32(c, p + 4 * i);
    c->eax = 1;
    c->esp += 4;
}

static guest_fn resolve(cpu *c, uint32_t target) { (void)c; (void)target; return guest_sink; }

static uint8_t host_sink(void *user, uint32_t cookie, void *samples, int32_t count)
{
    (void)user; (void)cookie;
    for (int32_t i = 0; i < count && cap_host.n < MAXCAP; i++)
        memcpy(&cap_host.v[cap_host.n++], (const uint8_t *)samples + 4 * i, 4);
    return 1;
}

static uint32_t rng_state;
static uint32_t rnd(uint32_t n) { rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 8) % n; }

enum { SC_FLUTTER, SC_RATE8K, SC_RATE_OTHER, SC_EIGHT_FORMANTS, SC_SOURCE_ONLY, SC_UNIT_GAIN, SC_ODD, NSCENARIOS };
static const char *SC_NAME[] = { "flutter", "rate mode 0 (8k tables)", "rate mode 2 (tilt computed)",
                                 "8 formants", "source only", "unit gain", "odd frames" };

static void mutate_state(uint8_t *s, int sc)
{
    int32_t v;
    float f;
    switch (sc) {
    case SC_RATE8K: v = 0; memcpy(s + 0x1ade, &v, 4); break;
    case SC_RATE_OTHER: v = 2; memcpy(s + 0x1ade, &v, 4); break;
    case SC_EIGHT_FORMANTS: v = 8; memcpy(s + 0x184b, &v, 4); break;
    case SC_SOURCE_ONLY: v = 1; memcpy(s + 0xa46, &v, 4); break;
    case SC_UNIT_GAIN: f = 1.0f; memcpy(s + 0x4a, &f, 4); break;
    }
}

static void mutate_frame(float *fr, int sc)
{
    switch (sc) {
    case SC_FLUTTER:
        fr[5] = (float)rnd(60);
        if (rnd(4) == 0) fr[6] = (float)rnd(60);
        break;
    case SC_RATE8K:
    case SC_RATE_OTHER:
        if (fr[4] == 0 && rnd(2)) fr[4] = (float)rnd(40);
        break;
    case SC_EIGHT_FORMANTS:
        fr[21] = 4500.0f + rnd(300); fr[22] = 200.0f + rnd(100);
        fr[23] = 5500.0f + rnd(300); fr[24] = 250.0f + rnd(100);
        fr[25] = 6500.0f + rnd(300); fr[26] = 300.0f + rnd(100);
        for (int i = 5; i < 8; i++) {
            fr[35 + i] = fr[8] != 0 ? (float)rnd(60) : 0;
            fr[44 + i] = 300.0f + rnd(200);
        }
        break;
    case SC_ODD:
        if (rnd(8) == 0) fr[0] = (float)(1 + rnd(90));      /* 1..90 ms: several 200-sample blocks */
        if (rnd(10) == 0) fr[1] = 0;                        /* F0 = 0 while voiced */
        if (rnd(10) == 0) fr[3] = 0;                        /* no open phase */
        if (rnd(6) == 0) fr[6] = (float)rnd(100);           /* diplophonia */
        if (rnd(10) == 0) fr[5] = (float)rnd(40);
        if (rnd(12) == 0) fr[4] = (float)rnd(50);           /* tilt, sometimes beyond 35 */
        break;
    }
}

static long run_diff(const char *path, int sc, cpu *c, guest_fn synth, image *im, long *frames_out, long *samples_out)
{
    rec *r;
    long nrec = load(path, &r);
    if (nrec <= 0) return -1;
    relocate(r, nrec, im);
    uint32_t addr = r[0].addr, sz = r[0].sz;
    uint8_t *host = (uint8_t *)malloc(sz), *guest = (uint8_t *)malloc(sz);
    memcpy(host, r[0].before, sz);
    mutate_state(host, sc);
    x86_write(c, addr, host, sz);
    klatt_ctx ctx = { img, host_sink, im, addr };
    rng_state = 12345u + 977u * (uint32_t)sc;
    long bad = 0, samples = 0;
    for (long k = 0; k < nrec; k++) {
        float fr[64];
        memcpy(fr, r[k].frame, sizeof fr);
        mutate_frame(fr, sc);
        cap_guest.n = cap_host.n = 0;
        x86_write(c, FRAME_AT, fr, sizeof fr);
        c->esp = STACK_TOP;
        push32(c, FRAME_AT);
        push32(c, addr);
        push32(c, RETURN_AT);
        c->ftop = 0;
        synth(c);
        x86_read(c, addr, guest, sz);
        int ret = klatt_synth(&ctx, host, fr);
        int diff = -1;
        for (uint32_t i = 0; i < sz; i++)
            if (host[i] != guest[i]) { diff = (int)i; break; }
        int sdiff = cap_guest.n != cap_host.n || memcmp(cap_guest.v, cap_host.v, 4 * (size_t)cap_host.n) != 0;
        int rdiff = (ret & 0xff) != (int)(c->eax & 0xff);
        samples += cap_host.n;
        if (diff >= 0 || sdiff || rdiff) {
            if (++bad <= 5)
                printf("  %s: %s frame %ld: state differs first at +0x%x, samples %s%s\n", SC_NAME[sc], path, k, diff,
                       sdiff ? "differ" : "same", rdiff ? ", return differs" : "");
            memcpy(host, guest, sz);            /* resynchronize */
        }
        free(r[k].before);
        free(r[k].after);
        free(r[k].out);
    }
    free(r);
    free(host);
    free(guest);
    *frames_out += nrec;
    *samples_out += samples;
    return bad;
}

static int diff_main(const char *syn, const char **files, int nfiles)
{
    cpu *c = x86_new();
    image_info info;
    if (image_load(c, syn, &info)) { fprintf(stderr, "cannot load %s\n", syn); return 2; }
    c->resolve = resolve;
    guest_fn synth = x86_lookup(0x1013caf0u);
    image im = { 0 };
    if (!synth || map_image(syn, &im)) return 2;
    long allbad = 0;
    for (int sc = 0; sc < NSCENARIOS; sc++) {
        long frames = 0, samples = 0, bad = 0;
        for (int fi = 0; fi < nfiles; fi++) {
            long b = run_diff(files[fi], sc, c, synth, &im, &frames, &samples);
            if (b < 0) { fprintf(stderr, "cannot read %s\n", files[fi]); return 2; }
            bad += b;
        }
        printf("%-28s %ld frames, %ld differ; %ld samples compared\n", SC_NAME[sc], frames, bad, samples);
        allbad += bad;
    }
    return allbad != 0;
}

int main(int argc, char **argv)
{
    int verbose = 0, diff = 0, nfiles = 0;
    const char *files[64];
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-diff")) diff = 1;
        else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    if (argc < 3 || !nfiles) { fprintf(stderr, "usage: klatt_check ENU.SYN [-diff] frames.frm [...] [-v]\n"); return 2; }
    if (diff) return diff_main(argv[1], files, nfiles);

    /* the image, flat at its preferred base */
    image im = { 0 };
    if (map_image(argv[1], &im)) { fprintf(stderr, "cannot load %s\n", argv[1]); return 2; }

    klatt_ctx ctx = { img, output, &im, 0 };
    char msg[128];
    if (klatt_check_constants(&ctx, msg, sizeof msg)) { printf("constants: %s\n", msg); return 1; }

    long allbad = 0, allsamples = 0, allbadsamples = 0;
    uint8_t *got = (uint8_t *)malloc(KLATT_STATE_SIZE);
    for (int fi = 0; fi < nfiles; fi++) {
        rec *r;
        long nrec = load(files[fi], &r);
        if (nrec <= 0) { fprintf(stderr, "cannot read %s\n", files[fi]); return 2; }
        relocate(r, nrec, &im);
        long bad = 0, samples = 0, badsamples = 0;
        for (long k = 0; k < nrec; k++) {
            rec *x = &r[k];
            memcpy(got, x->before, x->sz);
            ctx.state_addr = x->addr;
            int ret = klatt_synth(&ctx, got, x->frame);
            int diff = -1;
            for (uint32_t i = 0; i < x->sz; i++)
                if (got[i] != x->after[i]) { diff = (int)i; break; }
            int sdiff = 0;
            for (uint32_t i = 0; i < x->n; i++) {
                int32_t v;
                memcpy(&v, got + 0x70a + 4 * i, 4);
                samples++;
                if (v != x->out[i]) { sdiff++; badsamples++; }
            }
            int rdiff = (ret & 0xff) != (int)(x->ret & 0xff);
            if (diff >= 0 || sdiff || rdiff) {
                bad++;
                if (bad <= 10 || verbose)
                    printf("%s frame %ld: state differs first at +0x%x (got %02x want %02x), %d samples differ%s\n",
                           files[fi], k, diff, diff >= 0 ? got[diff] : 0, diff >= 0 ? x->after[diff] : 0, sdiff,
                           rdiff ? ", return differs" : "");
            }
            free(x->before);
            free(x->after);
            free(x->out);
        }
        free(r);
        printf("%s: %ld frames, %ld differ; %ld samples, %ld differ\n", files[fi], nrec, bad, samples, badsamples);
        allbad += bad;
        allsamples += samples;
        allbadsamples += badsamples;
    }
    if (nfiles > 1)
        printf("total: %ld frames differ; %ld samples, %ld differ; %ld output calls\n", allbad, allsamples,
               allbadsamples, im.calls);
    return allbad != 0;
}
