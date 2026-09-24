/* ecisay - the reference renderer: drive the real Eloquence (ECI.DLL + ENU.SYN) and write every
 * sample it hands over to a wave file. MUST be built 32-bit: the engine is a 32-bit DLL.
 *
 *   ecisay [--dll path\ECI.DLL] [--voice 1-8] [--param N=V ...] [--annot] "text"|@file out.wav
 *   ecisay [...] --lines file.txt outdir      one WAV per non-empty line, in one process
 *
 * The samples are taken from the output buffer the engine fills (eciWaveformBuffer), not from a sound
 * device, so nothing is lost at the end of an utterance.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *ECIHand;
enum { eciWaveformBuffer, eciPhonemeBuffer, eciIndexReply, eciPhonemeIndexReply, eciWordIndexReply };
enum { eciDataNotProcessed, eciDataProcessed, eciDataAbort };
typedef int(__stdcall *ECICallback)(ECIHand, int, long, void *);

static ECIHand(__stdcall *p_eciNew)(void);
static ECIHand(__stdcall *p_eciDelete)(ECIHand);
static int(__stdcall *p_eciAddText)(ECIHand, const char *);
static int(__stdcall *p_eciSynthesize)(ECIHand);
static int(__stdcall *p_eciSynchronize)(ECIHand);
static int(__stdcall *p_eciSetOutputBuffer)(ECIHand, int, short *);
static void(__stdcall *p_eciRegisterCallback)(ECIHand, ECICallback, void *);
static int(__stdcall *p_eciSetParam)(ECIHand, int, int);
static int(__stdcall *p_eciGetParam)(ECIHand, int);
static int(__stdcall *p_eciCopyVoice)(ECIHand, int, int);
static void(__stdcall *p_eciVersion)(char *);

#define FRAME 4096
static short frame[FRAME];
static short *samples;
static size_t nsamples, cap;
static long nwords;

static void keep(const short *p, size_t n)
{
    if (nsamples + n > cap) {
        cap = (nsamples + n) * 2 + FRAME;
        samples = (short *)realloc(samples, cap * sizeof *samples);
        if (!samples) { fprintf(stderr, "ecisay: out of memory\n"); exit(1); }
    }
    memcpy(samples + nsamples, p, n * sizeof *p);
    nsamples += n;
}

static int __stdcall on_message(ECIHand h, int msg, long param, void *data)
{
    (void)h; (void)data;
    if (msg == eciWaveformBuffer) keep(frame, (size_t)param);
    else if (msg == eciWordIndexReply) nwords++;
    return eciDataProcessed;
}

static void put(FILE *f, unsigned v, int n) { for (int i = 0; i < n; i++) fputc((v >> (8 * i)) & 0xff, f); }

static int write_wav(const char *path, unsigned rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "ecisay: cannot write %s\n", path); return 1; }
    unsigned bytes = (unsigned)nsamples * 2;
    fwrite("RIFF", 1, 4, f); put(f, 36 + bytes, 4); fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4); put(f, 1, 2); put(f, 1, 2); put(f, rate, 4); put(f, rate * 2, 4); put(f, 2, 2); put(f, 16, 2);
    fwrite("data", 1, 4, f); put(f, bytes, 4);
    fwrite(samples, 2, nsamples, f);
    fclose(f);
    return 0;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "ecisay: cannot read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc(n + 1);
    if (!b || fread(b, 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    b[n] = 0;
    return b;
}

/* ---- hooks inside ENU.SYN (call-site patches: the rel32 of a `call` is pointed at a stub) ----
 *
 * --dump-frames FILE: every call of the synthesizer FUN_1013caf0(state, float *frame), from its one call
 * site in FUN_101306b0 (0x10130a86), wrapped so the call happens between two snapshots. Record:
 *   "FRM2", uint32 state address, uint32 ENU.SYN base, 64 floats of the frame,
 *   uint32 S, S bytes of state before, S bytes of state after, uint32 return value,
 *   uint32 n, n int32 output samples (state+0x70a, n = state+0x14f3; 0 if output was off) */
#define SYN_PREF 0x10000000u
#define STATE_BYTES 0x1c00u
static FILE *g_frames;
static unsigned char *g_syn_base;
typedef unsigned(__cdecl *synth_fn)(void *state, float *frame);
static synth_fn g_synth_fn;

static unsigned __cdecl synth_wrap(void *state, float *frame)
{
    static unsigned char before[STATE_BYTES];
    unsigned char *s = (unsigned char *)state;
    static int cw_reported;
    if (!cw_reported) {
        unsigned short cw;
        __asm fnstcw cw
        fprintf(stderr, "x87 control word in the synthesizer: 0x%04x\n", cw);
        cw_reported = 1;
    }
    memcpy(before, s, STATE_BYTES);
    unsigned r = g_synth_fn(state, frame);
    unsigned addr = (unsigned)(size_t)s, base = (unsigned)(size_t)g_syn_base, sz = STATE_BYTES;
    fwrite("FRM2", 1, 4, g_frames);
    fwrite(&addr, 4, 1, g_frames);
    fwrite(&base, 4, 1, g_frames);
    fwrite(frame, 4, 64, g_frames);
    fwrite(&sz, 4, 1, g_frames);
    fwrite(before, 1, sz, g_frames);
    fwrite(s, 1, sz, g_frames);
    fwrite(&r, 4, 1, g_frames);
    unsigned n = s[0x1add] ? *(unsigned *)(s + 0x14f3) : 0;
    if (n > 4096) n = 0;
    fwrite(&n, 4, 1, g_frames);
    fwrite(s + 0x70a, 4, n, g_frames);
    return r;
}

static int patch_call(unsigned char *site, void *expect, void *stub)
{
    if (site[0] != 0xE8 || site + 5 + *(int *)(site + 1) != (unsigned char *)expect) return 1;
    DWORD old;
    VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int *)(site + 1) = (int)((unsigned char *)stub - (site + 5));
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return 0;
}

static int install_hooks(const char *frames)
{
    unsigned char *base = (unsigned char *)GetModuleHandleA("ENU.SYN");
    if (!base) { fprintf(stderr, "ecisay: ENU.SYN is not loaded\n"); return 1; }
    g_syn_base = base;
    if (frames) {
        g_synth_fn = (synth_fn)(base + (0x1013caf0u - SYN_PREF));
        if (patch_call(base + (0x10130a86u - SYN_PREF), (void *)g_synth_fn, (void *)synth_wrap)) {
            fprintf(stderr, "ecisay: synthesizer call site does not match this ENU.SYN\n");
            return 1;
        }
        g_frames = fopen(frames, "wb");
        if (!g_frames) return 1;
    }
    return 0;
}

/* --trace-engine FILE: every call ECI.DLL makes through ENU.SYN's engine interface (the 39-slot vtable at
 * 0x1014478c, method names from openevv's EngineWrapper). ENU.SYN is loaded before eciNew so the setup
 * calls are seen too. Each slot gets a 10-byte thunk `push slot; jmp trace_common`. */
static const char *const slot_names[39] = {
    "QueryInterface", "AddRef", "Release", "start", "end", "processSentences", "processRemaining",
    "getLastError", "restart", "readPhonemes", "readErrorMessage", "flush", "clearInput", "setAbort",
    "outputPlaying", "pause", "setSynthToNamedFile", "setSynthToCallback", "setDurationCallback",
    "registerWordCallback", "registerIndexCallback", "registerPhonemeCallback", "registerAnnoCallback",
    "insertSynthesisIndex", "insertDelayedSynthesisIndex", "wantPhonemeIndices", "close", "newDict",
    "getDict", "setDict", "deleteDict", "loadDict", "saveDict", "updateDict", "dictFindFirst",
    "dictFindNext", "dictLookup", "registerWordIndexCallback", "registerUserIndexCallback" };
static FILE *g_trace;
static unsigned g_slot_orig[39];

static void __stdcall log_vcall(const unsigned *p)
{
    unsigned slot = p[0];
    if (!g_trace || slot >= 39) return;
    fprintf(g_trace, "%-28s this=%08x", slot_names[slot], p[2]);
    for (int i = 0; i < 5; i++) fprintf(g_trace, " %08x", p[3 + i]);
    if (slot == 5 || slot == 6 || slot == 16 || slot == 31 || slot == 36) {
        const char *s = (const char *)(size_t)p[slot >= 31 ? 5 : 3];
        if (slot == 36) s = (const char *)(size_t)p[5];
        fprintf(g_trace, "  \"");
        for (int i = 0; s && s[i] && i < 400; i++) {
            unsigned char ch = (unsigned char)s[i];
            if (ch >= 32 && ch < 127 && ch != '"') fputc(ch, g_trace);
            else fprintf(g_trace, "\\x%02x", ch);
        }
        fprintf(g_trace, "\"");
    }
    fputc('\n', g_trace);
    fflush(g_trace);
}

static __declspec(naked) void trace_common(void)
{
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        call log_vcall
        popad
        xchg eax, [esp]
        mov eax, dword ptr [g_slot_orig + eax * 4]
        xchg eax, [esp]
        ret
    }
}

static int install_trace(const char *path, const char *dllpath)
{
    char syn[MAX_PATH];
    strcpy(syn, dllpath);
    char *s = strrchr(syn, '\\');
    strcpy(s ? s + 1 : syn, "ENU.SYN");
    unsigned char *base = (unsigned char *)LoadLibraryA(syn);
    if (!base) { fprintf(stderr, "ecisay: cannot preload %s\n", syn); return 1; }
    unsigned *vt = (unsigned *)(base + (0x1014478cu - SYN_PREF));
    unsigned char *thunks = (unsigned char *)VirtualAlloc(NULL, 39 * 10, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    DWORD old;
    VirtualProtect(vt, 39 * 4, PAGE_READWRITE, &old);
    for (unsigned i = 0; i < 39; i++) {
        unsigned char *t = thunks + 10 * i;
        g_slot_orig[i] = vt[i];
        t[0] = 0x68;
        memcpy(t + 1, &i, 4);
        t[5] = 0xe9;
        int rel = (int)((unsigned char *)trace_common - (t + 10));
        memcpy(t + 6, &rel, 4);
        vt[i] = (unsigned)(size_t)t;
    }
    VirtualProtect(vt, 39 * 4, old, &old);
    g_trace = fopen(path, "w");
    return g_trace == NULL;
}

#define GET(name) (*(FARPROC *)&p_##name = GetProcAddress(dll, #name))

static unsigned rate_of(ECIHand h)
{
    int r = p_eciGetParam(h, 5);            /* eciSampleRate: 0 8000, 1 11025, 2 22050 */
    return r == 0 ? 8000 : r == 2 ? 22050 : 11025;
}

static int speak(ECIHand h, const char *text, const char *out)
{
    nsamples = 0;
    nwords = 0;
    if (!p_eciAddText(h, text)) { fprintf(stderr, "ecisay: eciAddText refused\n"); return 1; }
    if (!p_eciSynthesize(h)) { fprintf(stderr, "ecisay: eciSynthesize refused\n"); return 1; }
    p_eciSynchronize(h);
    unsigned rate = rate_of(h);
    printf("%zu samples at %u Hz (%.2f s)  %s\n", nsamples, rate, nsamples / (double)rate, out);
    return write_wav(out, rate);
}

int main(int argc, char **argv)
{
    const char *dllpath = NULL, *text = NULL, *out = NULL, *lines = NULL, *frames = NULL, *trace = NULL;
    int voice = 0, annot = 0, np = 0, pn[16], pv[16];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) dllpath = argv[++i];
        else if (!strcmp(argv[i], "--voice") && i + 1 < argc) voice = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--annot")) annot = 1;
        else if (!strcmp(argv[i], "--dump-frames") && i + 1 < argc) frames = argv[++i];
        else if (!strcmp(argv[i], "--trace-engine") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--lines") && i + 1 < argc) lines = argv[++i];
        else if (!strcmp(argv[i], "--param") && i + 1 < argc && np < 16) {
            const char *s = argv[++i];
            pn[np] = atoi(s);
            const char *eq = strchr(s, '=');
            pv[np++] = eq ? atoi(eq + 1) : 0;
        } else if (!text && !lines) text = argv[i];
        else if (!out) out = argv[i];
    }
    if ((!text && !lines) || !out) {
        fprintf(stderr, "usage: ecisay [--dll ECI.DLL] [--voice 1-8] [--param N=V] [--annot] \"text\"|@file out.wav\n"
                        "       ecisay [...] --lines file.txt outdir\n");
        return 2;
    }
    char path[MAX_PATH];
    if (!dllpath) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *s = strrchr(path, '\\');
        strcpy(s ? s + 1 : path, "..\\pkg\\ECI.DLL");
        dllpath = path;
    }
    HMODULE dll = LoadLibraryA(dllpath);
    if (!dll) { fprintf(stderr, "ecisay: cannot load %s (%lu)\n", dllpath, GetLastError()); return 1; }
    GET(eciNew); GET(eciDelete); GET(eciAddText); GET(eciSynthesize); GET(eciSynchronize);
    GET(eciSetOutputBuffer); GET(eciRegisterCallback); GET(eciSetParam); GET(eciGetParam);
    GET(eciCopyVoice); GET(eciVersion);
    if (!p_eciNew || !p_eciAddText || !p_eciSynthesize || !p_eciSynchronize || !p_eciSetOutputBuffer ||
        !p_eciRegisterCallback || !p_eciGetParam) {
        fprintf(stderr, "ecisay: missing exports\n");
        return 1;
    }
    if (p_eciVersion) {
        char v[64] = "";
        p_eciVersion(v);
        fprintf(stderr, "ECI version %s\n", v);
    }
    if (trace && install_trace(trace, dllpath)) return 1;
    ECIHand h = p_eciNew();
    if (!h) { fprintf(stderr, "ecisay: eciNew failed\n"); return 1; }
    if (frames && install_hooks(frames)) return 1;
    p_eciRegisterCallback(h, on_message, NULL);
    p_eciSetOutputBuffer(h, FRAME, frame);
    if (voice > 0 && p_eciCopyVoice) p_eciCopyVoice(h, voice, 0);
    if (annot) p_eciSetParam(h, 1, 1);      /* eciInputType: annotations */
    for (int i = 0; i < np; i++) p_eciSetParam(h, pn[i], pv[i]);

    int r = 0;
    if (lines) {
        char *all = slurp(lines);
        int k = 0;
        for (char *line = strtok(all, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            if (!*line) continue;
            char o[MAX_PATH];
            snprintf(o, sizeof o, "%s\\%03d.wav", out, k++);
            r |= speak(h, line, o);
        }
    } else {
        r = speak(h, text[0] == '@' ? slurp(text + 1) : text, out);
    }
    p_eciDelete(h);
    if (g_frames) fclose(g_frames);
    return r;
}
