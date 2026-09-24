/* ecitrace - drive an ECI DLL (the real ECI.DLL, or the port) through a script of API calls and log what
 * comes back: every call's return value and every callback, with the number of samples delivered before
 * it. The logs of two DLLs given the same script must be identical, and so must the samples.
 *
 *   ecitrace [--dll path\ECI.DLL] script.txt log.txt out.wav
 *
 * Script: one call per line, # comments.
 *   new | newex LANG | delete | reset
 *   buffer N                   eciSetOutputBuffer(N samples)
 *   param P V | getparam P     eciSetParam / eciGetParam
 *   vparam VOICE P V | getvparam VOICE P
 *   copy FROM TO               eciCopyVoice
 *   add TEXT                   eciAddText (the rest of the line; \n for a newline)
 *   index N                    eciInsertIndex
 *   synth | sync | speaking | stop | clear | getindex | pause 0|1
 *   cbret wave|index|any processed|notprocessed|abort [N]    what the callback answers (N times, else always)
 *   speaktext 0|1 TEXT         eciSpeakText
 *   version
 * Build for the DLL's architecture (the real one is 32-bit).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *ECIHand;
typedef int(__stdcall *ECICallback)(ECIHand, int, LONG_PTR, void *);

#define API(ret, name, args) static ret(__stdcall *p_##name) args
API(ECIHand, eciNew, (void));
API(ECIHand, eciNewEx, (int));
API(ECIHand, eciDelete, (ECIHand));
API(int, eciReset, (ECIHand));
API(int, eciAddText, (ECIHand, const char *));
API(int, eciInsertIndex, (ECIHand, int));
API(int, eciSynthesize, (ECIHand));
API(int, eciSynchronize, (ECIHand));
API(int, eciSpeaking, (ECIHand));
API(int, eciStop, (ECIHand));
API(int, eciClearInput, (ECIHand));
API(int, eciGetIndex, (ECIHand));
API(int, eciPause, (ECIHand, int));
API(int, eciSetOutputBuffer, (ECIHand, int, short *));
API(void, eciRegisterCallback, (ECIHand, ECICallback, void *));
API(int, eciSetParam, (ECIHand, int, int));
API(int, eciGetParam, (ECIHand, int));
API(int, eciSetVoiceParam, (ECIHand, int, int, int));
API(int, eciGetVoiceParam, (ECIHand, int, int));
API(int, eciCopyVoice, (ECIHand, int, int));
API(int, eciSpeakText, (const char *, int));
API(void, eciVersion, (char *));
API(void *, eciNewDict, (ECIHand));
API(int, eciSetDict, (ECIHand, void *));
API(void *, eciDeleteDict, (ECIHand, void *));
API(int, eciLoadDict, (ECIHand, void *, int, const char *));
API(int, eciSaveDict, (ECIHand, void *, int, const char *));
API(int, eciUpdateDict, (ECIHand, void *, int, const char *, const char *));
API(const char *, eciDictLookup, (ECIHand, void *, int, const char *));
API(int, eciDictFindFirst, (ECIHand, void *, int, const char **, const char **));
API(int, eciDictFindNext, (ECIHand, void *, int, const char **, const char **));

static FILE *g_log;
static CRITICAL_SECTION g_lock;
static short *g_buf;
static int g_bufsize;
static short *g_pcm;
static size_t g_n, g_cap;

/* callback answers: per kind (0 wave, 1 other), a value and how many times it applies (-1 forever) */
static int g_ret[2] = { 1, 1 }, g_ret_times[2] = { -1, -1 };

static unsigned crc32(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    unsigned c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

static int answer(int kind)
{
    int r = g_ret[kind];
    if (g_ret_times[kind] > 0 && --g_ret_times[kind] == 0) { g_ret[kind] = 1; g_ret_times[kind] = -1; }
    return r;
}

static int __stdcall on_message(ECIHand h, int msg, LONG_PTR param, void *data)
{
    (void)h; (void)data;
    EnterCriticalSection(&g_lock);
    int r;
    if (msg == 0) {
        size_t n = (size_t)param;
        r = answer(0);
        fprintf(g_log, "cb wave %ld @%zu crc=%08x -> %d\n", param, g_n, n <= (size_t)g_bufsize ? crc32(g_buf, 2 * n) : 0, r);
        if (r == 1 && n <= (size_t)g_bufsize) {
            if (g_n + n > g_cap) { g_cap = (g_n + n) * 2 + 4096; g_pcm = (short *)realloc(g_pcm, g_cap * 2); }
            memcpy(g_pcm + g_n, g_buf, 2 * n);
            g_n += n;
        }
    } else {
        r = answer(1);
        if (msg == 3 && param) {           /* eciPhonemeIndexReply: lParam points at an ECIMouthData */
            const unsigned char *m = (const unsigned char *)(size_t)param;
            fprintf(g_log, "cb msg3 mouth=");
            for (int i = 0; i < 22; i++) fprintf(g_log, "%02x", m[i]);
            fprintf(g_log, " @%zu -> %d\n", g_n, r);
        } else
            fprintf(g_log, "cb msg%d %ld @%zu -> %d\n", msg, param, g_n, r);
    }
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
    return r;
}

static void put(FILE *f, unsigned v, int n) { for (int i = 0; i < n; i++) fputc((v >> (8 * i)) & 0xff, f); }

static void write_wav(const char *path, unsigned rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    unsigned bytes = (unsigned)g_n * 2;
    fwrite("RIFF", 1, 4, f); put(f, 36 + bytes, 4); fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4); put(f, 1, 2); put(f, 1, 2); put(f, rate, 4); put(f, rate * 2, 4); put(f, 2, 2); put(f, 16, 2);
    fwrite("data", 1, 4, f); put(f, bytes, 4);
    fwrite(g_pcm, 2, g_n, f);
    fclose(f);
}

static void unescape(char *s)
{
    char *w = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') { *w++ = '\n'; s++; }
        else if (s[0] == '\\' && s[1] == 't') { *w++ = '\t'; s++; }
        else *w++ = *s;
    }
    *w = 0;
}

#define LOGCALL(fmt, ...) do { EnterCriticalSection(&g_lock); fprintf(g_log, "> " fmt "\n", __VA_ARGS__); fflush(g_log); LeaveCriticalSection(&g_lock); } while (0)

int main(int argc, char **argv)
{
    const char *dllpath = NULL;
    int a = 1;
    if (argc > 2 && !strcmp(argv[1], "--dll")) { dllpath = argv[2]; a = 3; }
    if (argc - a < 3) { fprintf(stderr, "usage: ecitrace [--dll ECI.DLL] script.txt log.txt out.wav\n"); return 2; }
    char path[MAX_PATH];
    if (!dllpath) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *s = strrchr(path, '\\');
        strcpy(s ? s + 1 : path, "..\\pkg\\ECI.DLL");
        dllpath = path;
    }
    HMODULE dll = LoadLibraryA(dllpath);
    if (!dll) { fprintf(stderr, "ecitrace: cannot load %s (%lu)\n", dllpath, GetLastError()); return 1; }
#define GET(name) (*(FARPROC *)&p_##name = GetProcAddress(dll, #name))
    GET(eciNew); GET(eciNewEx); GET(eciDelete); GET(eciReset); GET(eciAddText); GET(eciInsertIndex);
    GET(eciSynthesize); GET(eciSynchronize); GET(eciSpeaking); GET(eciStop); GET(eciClearInput);
    GET(eciGetIndex); GET(eciPause); GET(eciSetOutputBuffer); GET(eciRegisterCallback); GET(eciSetParam);
    GET(eciGetParam); GET(eciSetVoiceParam); GET(eciGetVoiceParam); GET(eciCopyVoice); GET(eciSpeakText);
    GET(eciVersion); GET(eciNewDict); GET(eciSetDict); GET(eciDeleteDict); GET(eciLoadDict); GET(eciSaveDict);
    GET(eciUpdateDict); GET(eciDictLookup); GET(eciDictFindFirst); GET(eciDictFindNext);
    void *dict = NULL;
    FILE *sc = fopen(argv[a], "r");
    g_log = fopen(argv[a + 1], "w");
    if (!sc || !g_log) { fprintf(stderr, "ecitrace: cannot open the script or the log\n"); return 1; }
    InitializeCriticalSection(&g_lock);
    ECIHand h = NULL;
    char line[8192];
    unsigned rate = 11025;
    while (fgets(line, sizeof line, sc)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        char cmd[32] = "";
        int n = 0;
        sscanf(s, "%31s%n", cmd, &n);
        char *rest = s + n;
        if (*rest == ' ') rest++;
        int x = 0, y = 0, z = 0;
        int nargs = sscanf(rest, "%d %d %d", &x, &y, &z);
        (void)nargs;
        if (!strcmp(cmd, "new") || !strcmp(cmd, "newex")) {
            h = !strcmp(cmd, "new") ? p_eciNew() : p_eciNewEx(x);
            LOGCALL("%s -> %s", cmd, h ? "handle" : "NULL");
            if (h) p_eciRegisterCallback(h, on_message, NULL);
        } else if (!strcmp(cmd, "delete")) {
            ECIHand r = p_eciDelete(h);
            LOGCALL("delete -> %s", r ? "handle" : "NULL");
            h = NULL;
        } else if (!strcmp(cmd, "reset")) LOGCALL("reset -> %d", p_eciReset(h) & 0xff);
        else if (!strcmp(cmd, "buffer")) {
            free(g_buf);
            g_buf = (short *)calloc((size_t)x, 2);
            g_bufsize = x;
            LOGCALL("buffer %d -> %d", x, p_eciSetOutputBuffer(h, x, g_buf));
        } else if (!strcmp(cmd, "param")) LOGCALL("param %d %d -> %d", x, y, p_eciSetParam(h, x, y));
        else if (!strcmp(cmd, "getparam")) LOGCALL("getparam %d -> %d", x, p_eciGetParam(h, x));
        else if (!strcmp(cmd, "vparam")) LOGCALL("vparam %d %d %d -> %d", x, y, z, p_eciSetVoiceParam(h, x, y, z));
        else if (!strcmp(cmd, "getvparam")) LOGCALL("getvparam %d %d -> %d", x, y, p_eciGetVoiceParam(h, x, y));
        else if (!strcmp(cmd, "copy")) LOGCALL("copy %d %d -> %d", x, y, p_eciCopyVoice(h, x, y));
        else if (!strcmp(cmd, "add")) {
            unescape(rest);
            LOGCALL("add \"%s\" -> %d", rest, p_eciAddText(h, rest));
        } else if (!strcmp(cmd, "index")) LOGCALL("index %d -> %d", x, p_eciInsertIndex(h, x));
        else if (!strcmp(cmd, "synth")) LOGCALL("synth -> %d", p_eciSynthesize(h));
        else if (!strcmp(cmd, "sync")) LOGCALL("sync -> %d", p_eciSynchronize(h));
        else if (!strcmp(cmd, "speaking")) LOGCALL("speaking -> %d", p_eciSpeaking(h) & 0xff);   /* a byte; the rest is junk */
        else if (!strcmp(cmd, "stop")) LOGCALL("stop -> %d", p_eciStop(h) & 0xff);
        else if (!strcmp(cmd, "clear")) LOGCALL("clear -> %d", p_eciClearInput(h) & 0xff);
        else if (!strcmp(cmd, "getindex")) LOGCALL("getindex -> %d", p_eciGetIndex(h));
        else if (!strcmp(cmd, "pause")) LOGCALL("pause %d -> %d", x, p_eciPause(h, x) & 0xff);
        else if (!strcmp(cmd, "speaktext")) {
            char *t = rest;
            while (*t && *t != ' ') t++;
            if (*t) t++;
            unescape(t);
            LOGCALL("speaktext %d \"%s\" -> %d", x, t, p_eciSpeakText(t, x));
        } else if (!strcmp(cmd, "pump")) {         /* callbacks through the window, as a GUI thread gets them */
            DWORD end = GetTickCount() + (DWORD)x;
            MSG m;
            while ((int)(GetTickCount() - end) < 0) {
                while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
                Sleep(1);
            }
            LOGCALL("pump %d", x);
        } else if (!strcmp(cmd, "outfile")) {
            int (__stdcall *f)(ECIHand, const char *) = (int(__stdcall *)(ECIHand, const char *))GetProcAddress(dll, "eciSetOutputFilename");
            LOGCALL("outfile %s -> %d", rest, f(h, rest) & 0xff);
        } else if (!strcmp(cmd, "outdev")) {
            int (__stdcall *f)(ECIHand, int) = (int(__stdcall *)(ECIHand, int))GetProcAddress(dll, "eciSetOutputDevice");
            LOGCALL("outdev %d -> %d", x, f(h, x) & 0xff);
        } else if (!strcmp(cmd, "newdict")) {
            dict = p_eciNewDict(h);
            LOGCALL("newdict -> %s", dict ? "dict" : "NULL");
        } else if (!strcmp(cmd, "setdict")) LOGCALL("setdict %d -> %d", x, p_eciSetDict(h, x ? dict : NULL));
        else if (!strcmp(cmd, "deletedict")) { p_eciDeleteDict(h, dict); dict = NULL; LOGCALL("%s", "deletedict"); }
        else if (!strcmp(cmd, "loaddict") || !strcmp(cmd, "savedict")) {
            char *f = rest;
            while (*f && *f != ' ') f++;
            if (*f) f++;
            int r = cmd[0] == 'l' ? p_eciLoadDict(h, dict, x, f) : p_eciSaveDict(h, dict, x, f);
            LOGCALL("%s %d %s -> %d", cmd, x, f, r);
        } else if (!strcmp(cmd, "updatedict")) {    /* updatedict VOL key=value (no =: delete) */
            char *k = rest;
            while (*k && *k != ' ') k++;
            if (*k) k++;
            char *v = strchr(k, '=');
            if (v) *v++ = 0;
            LOGCALL("updatedict %d %s -> %d", x, k, p_eciUpdateDict(h, dict, x, k, v));
        } else if (!strcmp(cmd, "lookup")) {
            char *k = rest;
            while (*k && *k != ' ') k++;
            if (*k) k++;
            const char *v = p_eciDictLookup(h, dict, x, k);
            LOGCALL("lookup %d %s -> %s", x, k, v ? v : "(null)");
        } else if (!strcmp(cmd, "listdict")) {
            const char *k, *v;
            int r = p_eciDictFindFirst(h, dict, x, &k, &v);
            while (r == 0) {
                LOGCALL("  entry %s = %s", k, v);
                r = p_eciDictFindNext(h, dict, x, &k, &v);
            }
            LOGCALL("listdict %d -> %d", x, r);
        } else if (!strcmp(cmd, "version")) {
            char v[128] = "";
            p_eciVersion(v);
            LOGCALL("version -> %s", v);
        } else if (!strcmp(cmd, "cbret")) {
            char kind[16] = "", what[32] = "";
            int times = -1;
            sscanf(rest, "%15s %31s %d", kind, what, &times);
            int v = !strcmp(what, "notprocessed") ? 0 : !strcmp(what, "abort") ? 2 : 1;
            for (int k = 0; k < 2; k++)
                if (!strcmp(kind, "any") || (k == 0) == !strcmp(kind, "wave")) { g_ret[k] = v; g_ret_times[k] = times; }
            LOGCALL("cbret %s %s %d", kind, what, times);
        } else {
            fprintf(stderr, "ecitrace: unknown command %s\n", cmd);
            return 2;
        }
        if (h && p_eciGetParam && strcmp(cmd, "delete")) {
            int r = p_eciGetParam(h, 5);
            rate = r == 0 ? 8000 : r == 2 ? 22050 : r == 3 ? 44100 : r == 4 ? 48000 : 11025;   /* 2..4: the port's extension */
        }
    }
    if (h) p_eciDelete(h);
    fclose(g_log);
    write_wav(argv[a + 2], rate);
    return 0;
}
