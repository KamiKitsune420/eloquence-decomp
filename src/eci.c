/* eci - the ECI API (Eloquence 6.1's ECI.DLL) over the recompiled engine. notes/eci_api.md is the
 * specification this follows; section numbers below refer to it.
 *
 * Threads, as in the original: the thread that calls eciNew owns a message window; a worker per handle
 * makes every engine call (commands from a queue); engine events become dispatcher items that run the
 * user's callback when that window gets message 0x500, or inside eciSpeaking / eciSynchronize.
 * Waveform buffers are send-and-wait: the worker waits inside the engine's audio callback.
 *
 * Differences, by necessity: one engine can't be entered from two threads, so eciStop's flush(1) is made
 * by the worker from inside its next engine callback (where the original's worker nearly always is:
 * blocked delivering a buffer), and only US English (1.0) exists.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "eci_text.h"
#include "voicefx.h"

#define ECIFN __stdcall
typedef struct eci eci;
typedef int(ECIFN *ECICallback)(void *hEngine, int msg, LONG_PTR lParam, void *pData);   /* long on x86; pointer-sized on x64 (eciPhonemeIndexReply passes a pointer) */

enum { MSG_WAVE, MSG_PHONEME_BUFFER, MSG_INDEX, MSG_PHONEME_INDEX, MSG_WORD_INDEX };
enum { IT_INDEX, IT_WAVE, IT_PHONEMES, IT_MOUTH, IT_ENGINE_ERR, IT_AUDIO_ERR, IT_MEMORY, IT_END, IT_WORD };
enum { OUT_DEVICE, OUT_BUFFER, OUT_PHONEMES, OUT_PINYIN, OUT_NONE };
enum { CB_SYNTH = 1, CB_WORD, CB_INDEX, CB_PHONEME, CB_ANNO };
#define WM_ECI 0x500
#define LANG_ENU 0x10000

/* ---------------------------------------------------------------- voices (ECI.INI [1.0], 1.4) */
static const char *const preset_names[8] = { "Adult Male 1", "Adult Female 1", "Child 1", "Adult Male 2",
                                             "Adult Male 3", "Adult Female 2", "Elderly Female 1",
                                             "Elderly Male 1" };
static const int preset_values[8][8] = {
    { 0, 50, 65, 30, 0, 0, 50, 92 }, { 1, 50, 81, 30, 0, 50, 50, 100 }, { 1, 22, 93, 35, 0, 0, 50, 90 },
    { 0, 86, 56, 47, 0, 0, 50, 93 }, { 0, 50, 69, 34, 0, 0, 70, 92 },  { 1, 56, 89, 35, 0, 40, 70, 95 },
    { 1, 45, 68, 30, 3, 40, 50, 90 }, { 0, 30, 61, 44, 18, 20, 50, 90 } };
static eci_voice g_presets[8];

/* this port's voices (an extension): eciCopyVoice(h, 17..21, 0) takes one, like a preset. Their voice
 * parameters are ordinary; the effects (voicefx) work on the synthesizer's frames and output. */
static const struct { const char *name; int p[8]; const char *fx; } ext_voices[] = {
    { "Robo", { 0, 50, 40, 0, 0, 0, 50, 92 }, "mono=105,oq=20,bw=0.6" },
    { "Newsreader", { 0, 65, 45, 45, 5, 0, 50, 92 }, "formant=0.93,oq=48,bw=0.9" },
    { "Vintage Reed", { 0, 50, 65, 5, 70, 0, 50, 92 }, "growl=6:32,oq=45,crush=6,hold=2,hp=170" },
    { "Robo Vibrato", { 0, 50, 40, 0, 0, 0, 50, 92 }, "mono=105,oq=20,bw=0.6,vib=35:6" },
    { "Newsreader Vibrato", { 0, 65, 45, 45, 5, 0, 50, 92 }, "formant=0.93,oq=48,bw=0.9,vib=22:5.5" },
};
#define N_EXT ((int)(sizeof ext_voices / sizeof ext_voices[0]))
#define EXT_FIRST 17
static eci_voice g_ext[N_EXT];

/* mouth shapes for eciPhonemeIndexReply (ECI.INI [1.0] PhonemeN: 4 phoneme bytes, 3 shorts, 8 longs) */
static const unsigned char mouth_table[][15] = {
    { 65, 0, 0, 0, 0, 0, 0, 75, 150, 128, 100, 130, 175, 200, 125 }, { 68, 0, 0, 0, 0, 0, 0, 50, 100, 128, 25, 130, 130, 180, 125 },
    { 69, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 175, 0, 125 },    { 70, 0, 0, 0, 0, 0, 0, 75, 150, 128, 50, 130, 130, 255, 0 },
    { 71, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 130, 0, 0 },      { 72, 0, 0, 0, 0, 0, 0, 50, 75, 128, 100, 150, 130, 0, 100 },
    { 73, 0, 0, 0, 0, 0, 0, 50, 100, 128, 50, 130, 175, 0, 125 },    { 82, 0, 0, 0, 0, 0, 0, 75, 100, 128, 75, 130, 0, 0, 150 },
    { 83, 0, 0, 0, 0, 0, 0, 75, 125, 128, 100, 130, 130, 0, 200 },   { 84, 0, 0, 0, 0, 0, 0, 50, 100, 128, 25, 130, 130, 180, 125 },
    { 85, 0, 0, 0, 0, 0, 0, 25, 0, 130, 90, 60, 60, 0, 255 },        { 88, 0, 0, 0, 0, 0, 0, 10, 50, 128, 50, 130, 0, 0, 0 },
    { 90, 0, 0, 0, 0, 0, 0, 75, 125, 128, 100, 130, 130, 0, 200 },   { 97, 0, 0, 0, 0, 0, 0, 100, 125, 128, 200, 150, 130, 150, 150 },
    { 98, 0, 0, 0, 0, 0, 0, 0, 100, 128, 0, 0, 0, 0, 200 },          { 99, 0, 0, 0, 0, 0, 0, 25, 0, 130, 150, 60, 60, 0, 255 },
    { 100, 0, 0, 0, 0, 0, 0, 75, 150, 128, 50, 130, 130, 255, 0 },   { 101, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 175, 0, 255 },
    { 102, 0, 0, 0, 0, 0, 0, 10, 100, 128, 25, 130, 0, 0, 200 },     { 103, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 130, 0, 0 },
    { 105, 0, 0, 0, 0, 0, 0, 25, 150, 128, 50, 130, 130, 0, 255 },   { 107, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 130, 0, 0 },
    { 108, 0, 0, 0, 0, 0, 0, 75, 150, 128, 50, 130, 130, 255, 0 },   { 109, 0, 0, 0, 0, 0, 0, 0, 100, 128, 0, 0, 0, 0, 200 },
    { 110, 0, 0, 0, 0, 0, 0, 75, 150, 128, 50, 130, 130, 255, 0 },   { 111, 0, 0, 0, 0, 0, 0, 25, 0, 130, 130, 60, 60, 0, 255 },
    { 112, 0, 0, 0, 0, 0, 0, 0, 100, 128, 0, 0, 0, 0, 200 },         { 114, 0, 0, 0, 0, 0, 0, 75, 50, 128, 75, 130, 0, 0, 175 },
    { 115, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 130, 150, 0 },   { 116, 0, 0, 0, 0, 0, 0, 75, 150, 128, 50, 130, 130, 255, 0 },
    { 117, 0, 0, 0, 0, 0, 0, 25, 0, 130, 90, 60, 60, 0, 255 },       { 118, 0, 0, 0, 0, 0, 0, 10, 100, 128, 25, 130, 0, 0, 200 },
    { 119, 0, 0, 0, 0, 0, 0, 25, 0, 130, 90, 60, 60, 0, 255 },       { 120, 0, 0, 0, 0, 0, 0, 10, 50, 128, 75, 130, 0, 0, 0 },
    { 121, 0, 0, 0, 0, 0, 0, 25, 150, 128, 50, 130, 130, 0, 255 },   { 122, 0, 0, 0, 0, 0, 0, 50, 100, 128, 75, 130, 130, 150, 0 },
    { 164, 0, 0, 0, 0, 0, 0, 0, 170, 128, 0, 0, 0, 0, 0 } };

#pragma pack(push, 1)
typedef struct {                 /* ECIMouthData: what eciPhonemeIndexReply's lParam points at (5.3) */
    unsigned char phoneme[4];
    unsigned char wide[6];           /* UCS dialects: the phoneme as 4 wide characters */
    int dialect;
    unsigned char mouth[8];
} eci_mouth;
#pragma pack(pop)

static void presets_init(void)
{
    static LONG done;
    if (InterlockedCompareExchange(&done, 1, 0)) return;
    for (int n = 0; n < 8; n++) {
        eci_voice *v = &g_presets[n];
        strcpy(v->name, preset_names[n]);
        memcpy(v->p, preset_values[n], sizeof v->p);
        v->defined = 1;
        eci_voice_realworld(v, -1);
    }
    for (int n = 0; n < N_EXT; n++) {
        eci_voice *v = &g_ext[n];
        strcpy(v->name, ext_voices[n].name);
        memcpy(v->p, ext_voices[n].p, sizeof v->p);
        v->defined = 1;
        v->fx = n + 1;
        eci_voice_realworld(v, -1);
    }
}

/* ---------------------------------------------------------------- the callback dispatcher (2.2) */
typedef struct item {
    int type;
    long data;
    int waiting, aborted;
    HANDLE done;
    struct item *next;
} item;

typedef struct {
    CRITICAL_SECTION lock;
    item *head, *tail, *retained;
    HANDLE nonempty;                 /* manual reset: set while the queue has items */
    volatile LONG posted, completed;
    volatile LONG paused, blocked;
} dispatcher;

/* ---------------------------------------------------------------- the worker (2.1) */
enum { C_TEXT, C_ANNOT, C_INDEX, C_SYNTH, C_TS, C_WANTPH, C_LANG, C_WORDIDX, C_BLOCK, C_FX };

typedef struct cmd {
    int type;
    char *text;
    int annot, value;
    struct cmd *next;
} cmd;

typedef struct { unsigned slot; int delta; } idxpos;
typedef struct { int kind, value, used; } slotent;

typedef struct wavedev wavedev;

typedef struct {
    eci *h;
    eng *e;
    HANDLE thread;
    CRITICAL_SECTION qlock, englock, slotlock;
    CONDITION_VARIABLE qcond, idlecond;
    cmd *qhead, *qtail;
    volatile LONG running, quit, abort, flushed;
    HANDLE unblock;                  /* manual mode: the worker waits on it (C_BLOCK) */
    int mirror[20];
    /* counters (3.4, 3.7) */
    volatile LONG pending, busy;
    int chars, chars_last;
    idxpos *idx;
    int nidx, capidx;
    slotent *slots;
    unsigned nslots;
    unsigned *freelist;
    unsigned nfree;
    /* output */
    short *obuf;
    int ocap, on;
    wavedev *dev;
    voicefx vfx;                     /* this port's voice effects (C_FX) */
    int engine_err_reported, audio_error;
    /* phoneme buffer (3.8) */
    char *pbuf;
    int pcap, pn;
} ctl;

struct eci {
    HWND hwnd;
    ECICallback cb;
    void *data;
    int param[17], sent[17];
    eci_voice active, sent_voice, user[8];
    int bufsize;
    short *buf;
    int last_index;
    int mode;
    char devname[256];
    char *phon_user;
    unsigned status, lasterr;
    volatile char reenter;
    char force;
    char newex2;
    eci_mouth mouth;
    struct mitem *mhead, *mtail;      /* manual mode queue (items carry params and voice) */
    ctl *c;
    dispatcher d;
    void *dicts[16];
    char *dict_key, *dict_value;
};

typedef struct mitem {               /* manual mode item */
    int type;                        /* 0 text, 1 index */
    char *text;
    int index;
    int params[17];
    eci_voice voice;
    struct mitem *next;
} mitem;

static void set_error(eci *h, unsigned e) { h->lasterr = e; h->status |= e; }

/* the user's callback, called the way ECI.DLL calls it (5.1) */
static __declspec(noinline) int call_user(eci *h, int msg, LONG_PTR lparam)
{
    return h->cb(h, msg, lparam, h->data);
}

/* FUN_100069ee: an item as the user sees it; -1 = the user said NotProcessed */
static int exec_item(eci *h, item *it)
{
    int r = 1;
    switch (it->type) {
    case IT_INDEX:
        h->last_index = it->data;
        if (h->cb) r = call_user(h, MSG_INDEX, it->data);
        break;
    case IT_WAVE:
        if (h->cb) r = call_user(h, MSG_WAVE, it->data);
        break;
    case IT_PHONEMES:
        if (h->cb) r = call_user(h, MSG_PHONEME_BUFFER, it->data);
        break;
    case IT_MOUTH: {
        const unsigned char *m = NULL;
        for (size_t i = 0; i < sizeof mouth_table / sizeof mouth_table[0]; i++)
            if (mouth_table[i][0] == (unsigned)(it->data & 0xff) && !(it->data >> 8)) m = mouth_table[i];
        if (!m) { set_error(h, 0x10); break; }
        memset(&h->mouth, 0, sizeof h->mouth);
        memcpy(h->mouth.phoneme, m, 4);
        h->mouth.dialect = LANG_ENU;      /* the dialect of the INI section the entry came from */
        for (int k = 0; k < 8; k++) h->mouth.mouth[k] = m[7 + k];
        if (h->cb) r = call_user(h, MSG_PHONEME_INDEX, (LONG_PTR)&h->mouth);
        break;
    }
    case IT_ENGINE_ERR: set_error(h, 0x10); break;
    case IT_AUDIO_ERR: set_error(h, 0x20); break;
    case IT_MEMORY: set_error(h, 0x2); break;
    case IT_WORD:
        h->last_index = it->data;
        if (h->cb) r = call_user(h, MSG_WORD_INDEX, it->data);
        break;
    }
    return r == 0 ? -1 : 0;
}

static void item_release(item *it)
{
    if (it->waiting) SetEvent(it->done);
    else free(it);
}

/* post an item; with wait, return only when it has been executed (0) or discarded by eciStop (-1) */
static int disp_post(eci *h, int type, long data, int wait)
{
    dispatcher *d = &h->d;
    item *it = (item *)calloc(1, sizeof *it);
    if (!it) return -1;
    it->type = type;
    it->data = data;
    it->waiting = wait;
    if (wait) it->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    EnterCriticalSection(&d->lock);
    if (d->blocked) {
        LeaveCriticalSection(&d->lock);
        if (wait) CloseHandle(it->done);
        free(it);
        return -1;
    }
    if (d->tail) d->tail->next = it; else d->head = it;
    d->tail = it;
    SetEvent(d->nonempty);
    LeaveCriticalSection(&d->lock);
    PostMessageA(h->hwnd, WM_ECI, (WPARAM)h, 0);
    if (!wait) return 0;
    WaitForSingleObject(it->done, INFINITE);
    int r = it->aborted ? -1 : 0;
    CloseHandle(it->done);
    free(it);
    return r;
}

static item *disp_pop(dispatcher *d)
{
    EnterCriticalSection(&d->lock);
    item *it = d->head;
    if (it) {
        d->head = it->next;
        if (!d->head) d->tail = NULL;
        it->next = NULL;
    }
    if (!d->head) ResetEvent(d->nonempty);
    LeaveCriticalSection(&d->lock);
    return it;
}

/* eciStop's part: drop every item, waking the worker if it waits on one */
static void disp_discard(eci *h)
{
    dispatcher *d = &h->d;
    EnterCriticalSection(&d->lock);
    d->blocked = 1;
    item *it = d->head;
    d->head = d->tail = NULL;
    ResetEvent(d->nonempty);
    if (d->retained) { d->retained->next = it; it = d->retained; d->retained = NULL; }
    LeaveCriticalSection(&d->lock);
    while (it) {
        item *n = it->next;
        it->aborted = 1;
        item_release(it);
        it = n;
    }
    KillTimer(h->hwnd, (UINT_PTR)h);
    MSG m;
    while (PeekMessageA(&m, h->hwnd, WM_ECI, WM_ECI, PM_REMOVE)) {}
}

/* eciPoll2 (FUN_100013e1) */
static int disp_poll(eci *h)
{
    dispatcher *d = &h->d;
    if (d->paused) return d->posted == d->completed ? 6 : 1;
    if (d->posted == d->completed) return 2;
    if (d->retained) {
        if (exec_item(h, d->retained) != 0) return 3;
        item_release(d->retained);
        d->retained = NULL;
        KillTimer(h->hwnd, (UINT_PTR)h);
    }
    for (;;) {
        item *it = disp_pop(d);
        if (!it) return 3;
        MSG m;
        PeekMessageA(&m, h->hwnd, WM_ECI, WM_ECI, PM_REMOVE);
        if (it->type == IT_END) {
            d->completed = it->data;
            item_release(it);
            if (d->completed == d->posted) {
                while (PeekMessageA(&m, h->hwnd, WM_ECI, WM_ECI, PM_REMOVE)) {}
                h->c->busy = 0;
                return 4;
            }
            continue;
        }
        if (exec_item(h, it) != 0) {
            d->retained = it;
            SetTimer(h->hwnd, (UINT_PTR)h, 30, NULL);
            return 3;
        }
        item_release(it);
    }
}

/* eciSynchronize2 (FUN_1000128f) */
static int disp_sync(eci *h)
{
    dispatcher *d = &h->d;
    if (d->paused) return 1;
    if (d->posted == d->completed) return 2;
    if (d->retained) {                        /* the original leaves it; its poster would wait forever */
        while (exec_item(h, d->retained) != 0) Sleep(30);
        item_release(d->retained);
        d->retained = NULL;
        KillTimer(h->hwnd, (UINT_PTR)h);
    }
    for (;;) {
        WaitForSingleObject(d->nonempty, 30);
        if (d->posted == d->completed) return 2;        /* eciStop from a callback */
        item *it = disp_pop(d);
        if (!it) continue;
        if (it->type == IT_END) {
            d->completed = it->data;
            item_release(it);
            if (d->completed == d->posted) { h->c->busy = 0; return 4; }
            continue;
        }
        while (exec_item(h, it) != 0) {
            if (d->paused) { d->retained = it; return 1; }
            Sleep(30);
        }
        item_release(it);
    }
}

/* ---------------------------------------------------------------- audio devices and files (6.1) */
#define DEV_BLOCKS 10
#define DEV_BLOCK_BYTES 2000
#define DEV_PREROLL 8

typedef struct marker { unsigned slot; long long pos; struct marker *next; } marker;

struct wavedev {
    ctl *c;
    int file;                        /* 0 wave device, 1 wav file */
    int devid;
    char path[256];
    int rate;
    HWAVEOUT wo;
    FILE *f;
    long long written;               /* samples handed over */
    WAVEHDR hdr[DEV_BLOCKS];
    char data[DEV_BLOCKS][DEV_BLOCK_BYTES];
    int cur, fill, queued, started;
    int held[DEV_BLOCKS], nheld;
    CRITICAL_SECTION lock;
    marker *markers;
    HANDLE ticker, stop_ticker, end_event;
    int open;
};

static void handle_slot(ctl *c, unsigned slot);

static long long dev_played(wavedev *w)
{
    if (w->file) return w->written;
    MMTIME t;
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(w->wo, &t, sizeof t) != MMSYSERR_NOERROR) return 0;
    return t.wType == TIME_SAMPLES ? (long long)t.u.sample : (long long)t.u.cb / 2;
}

static void dev_fire(wavedev *w, int all)
{
    for (;;) {
        EnterCriticalSection(&w->lock);
        marker *m = w->markers;
        if (!m || (!all && dev_played(w) < m->pos)) { LeaveCriticalSection(&w->lock); return; }
        w->markers = m->next;
        LeaveCriticalSection(&w->lock);
        handle_slot(w->c, m->slot);
        free(m);
    }
}

static DWORD WINAPI dev_ticker(void *p)
{
    wavedev *w = (wavedev *)p;
    while (WaitForSingleObject(w->stop_ticker, 30) == WAIT_TIMEOUT) dev_fire(w, 0);
    return 0;
}

static void dev_write_block(wavedev *w, int b, int bytes)
{
    WAVEHDR *hd = &w->hdr[b];
    hd->lpData = w->data[b];
    hd->dwBufferLength = (DWORD)bytes;
    hd->dwFlags = 0;
    waveOutPrepareHeader(w->wo, hd, sizeof *hd);
    waveOutWrite(w->wo, hd, sizeof *hd);
}

static void dev_release_done(wavedev *w)
{
    for (int b = 0; b < DEV_BLOCKS; b++)
        if ((w->hdr[b].dwFlags & WHDR_DONE) && (w->hdr[b].dwFlags & WHDR_PREPARED)) {
            waveOutUnprepareHeader(w->wo, &w->hdr[b], sizeof w->hdr[b]);
            w->hdr[b].dwFlags = 0;
            w->queued--;
        }
}

static int dev_open(wavedev *w)
{
    if (w->open) return 0;
    w->written = 0;
    w->cur = w->fill = w->queued = w->started = w->nheld = 0;
    memset(w->hdr, 0, sizeof w->hdr);
    if (w->file) {
        w->f = fopen(w->path, "wb");
        if (!w->f) return -1;
        static const unsigned char hdr[44] = { 0 };
        fwrite(hdr, 1, 44, w->f);
    } else {
        WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 1, (DWORD)w->rate, (DWORD)w->rate * 2, 2, 16, 0 };
        if (waveOutOpen(&w->wo, (UINT)w->devid, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return -1;
        w->stop_ticker = CreateEventA(NULL, TRUE, FALSE, NULL);
        w->ticker = CreateThread(NULL, 0, dev_ticker, w, 0, NULL);
    }
    w->open = 1;
    return 0;
}

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static void wav_header(wavedev *w)
{
    unsigned char h[44];
    unsigned bytes = (unsigned)(w->written * 2);
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0; put32(h + 24, (unsigned)w->rate); put32(h + 28, (unsigned)w->rate * 2);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0; memcpy(h + 36, "data", 4); put32(h + 40, bytes);
    long pos = ftell(w->f);
    fseek(w->f, 0, SEEK_SET);
    fwrite(h, 1, 44, w->f);
    fseek(w->f, pos, SEEK_SET);
    fflush(w->f);
}

static void dev_close(wavedev *w, int reset)
{
    if (!w->open) return;
    if (w->file) {
        wav_header(w);
        fclose(w->f);
        w->f = NULL;
    } else {
        SetEvent(w->stop_ticker);
        WaitForSingleObject(w->ticker, INFINITE);
        CloseHandle(w->ticker);
        CloseHandle(w->stop_ticker);
        if (reset) waveOutReset(w->wo);
        dev_release_done(w);
        waveOutClose(w->wo);
    }
    EnterCriticalSection(&w->lock);
    marker *m = w->markers;
    w->markers = NULL;
    LeaveCriticalSection(&w->lock);
    while (m) { marker *n = m->next; free(m); m = n; }
    w->open = 0;
}

static void dev_flush_block(wavedev *w)
{
    if (w->file || w->fill == 0) return;
    w->held[w->nheld++] = w->cur;
    w->queued++;
    int b = w->cur, bytes = w->fill;
    w->hdr[b].dwBufferLength = (DWORD)bytes;
    w->cur = (w->cur + 1) % DEV_BLOCKS;
    w->fill = 0;
    if (w->started || w->nheld >= DEV_PREROLL) {
        for (int i = 0; i < w->nheld; i++) dev_write_block(w, w->held[i], (int)w->hdr[w->held[i]].dwBufferLength);
        w->nheld = 0;
        w->started = 1;
    }
}

static void dev_samples(wavedev *w, const short *s, int n)
{
    if (w->file) { fwrite(s, 2, (size_t)n, w->f); w->written += n; return; }
    for (int i = 0; i < n; i++) {
        while (w->fill == 0 && w->queued >= DEV_BLOCKS) {        /* all blocks in use: wait for one */
            dev_release_done(w);
            if (w->queued >= DEV_BLOCKS) Sleep(10);
        }
        memcpy(w->data[w->cur] + w->fill, &s[i], 2);
        w->fill += 2;
        w->written++;
        if (w->fill == DEV_BLOCK_BYTES) dev_flush_block(w);
    }
}

static void dev_marker(wavedev *w, unsigned slot)
{
    marker *m = (marker *)calloc(1, sizeof *m);
    if (!m) return;
    m->slot = slot;
    m->pos = w->written;
    EnterCriticalSection(&w->lock);
    marker **p = &w->markers;
    while (*p) p = &(*p)->next;
    *p = m;
    LeaveCriticalSection(&w->lock);
    if (w->file) dev_fire(w, 0);
}

/* end of an utterance (FUN_10013a5d): push out everything, wait until it has played, close */
static void dev_finish(wavedev *w)
{
    if (!w->open) return;
    short z = 0;
    dev_samples(w, &z, 1);                   /* FUN_10013a5d ends every utterance with one zero sample */
    if (!w->file) {
        dev_flush_block(w);
        if (w->nheld) {
            for (int i = 0; i < w->nheld; i++) dev_write_block(w, w->held[i], (int)w->hdr[w->held[i]].dwBufferLength);
            w->nheld = 0;
            w->started = 1;
        }
        while (dev_played(w) < w->written && !w->c->abort) { dev_release_done(w); Sleep(10); }
    }
    dev_fire(w, 1);
    if (w->file) wav_header(w);          /* a file stays open: the next utterance goes after this one */
    else dev_close(w, 0);
}

static wavedev *dev_new(ctl *c, const char *name, int rate)
{
    wavedev *w = (wavedev *)calloc(1, sizeof *w);
    if (!w) return NULL;
    w->c = c;
    w->rate = rate;
    InitializeCriticalSection(&w->lock);
    int id = atoi(name);
    if (id != 0 || !strcmp(name, "0")) {
        w->file = 0;
        w->devid = id == -1 ? (int)WAVE_MAPPER : id;
    } else {
        w->file = 1;
        strncpy(w->path, name, sizeof w->path - 1);
    }
    return w;
}

static void dev_free(wavedev *w)
{
    if (!w) return;
    dev_close(w, 1);
    DeleteCriticalSection(&w->lock);
    free(w);
}

/* ---------------------------------------------------------------- index slots (3.7) */
static unsigned slot_alloc(ctl *c, int kind, int value)
{
    EnterCriticalSection(&c->slotlock);
    if (!c->nfree) {
        unsigned grow = c->nslots ? c->nslots : 60;
        slotent *s = (slotent *)realloc(c->slots, (c->nslots + grow + 1) * sizeof *s);
        unsigned *f = (unsigned *)realloc(c->freelist, (c->nslots + grow + 1) * sizeof *f);
        if (!s || !f) { LeaveCriticalSection(&c->slotlock); return 0; }
        c->slots = s;
        c->freelist = f;
        /* free list kept as a stack whose top is the end: push in reverse so the lowest comes out first */
        for (unsigned k = 0; k < grow; k++) c->freelist[k] = c->nslots + grow - k;
        c->nfree = grow;
        c->nslots += grow;
    }
    unsigned n = c->freelist[--c->nfree];
    c->slots[n].kind = kind;
    c->slots[n].value = value;
    c->slots[n].used = 1;
    LeaveCriticalSection(&c->slotlock);
    return n;
}

static int slot_take(ctl *c, unsigned n, int *kind, int *value)
{
    EnterCriticalSection(&c->slotlock);
    int ok = n > 0 && n <= c->nslots && c->slots[n].used;
    if (ok) {
        *kind = c->slots[n].kind;
        *value = c->slots[n].value;
        c->slots[n].used = 0;
        c->freelist[c->nfree++] = n;          /* freed slots come back first */
    }
    LeaveCriticalSection(&c->slotlock);
    return ok;
}

/* FUN_10012848 */
static void handle_slot(ctl *c, unsigned slot)
{
    int kind, value;
    if (!slot_take(c, slot, &kind, &value)) return;
    eci *h = c->h;
    if (kind == 0) disp_post(h, IT_INDEX, value, 0);
    else if (kind == 1) disp_post(h, IT_MOUTH, value, 0);
    else if (kind == 3) disp_post(h, IT_WORD, value, 0);
}

/* ---------------------------------------------------------------- engine side (on the worker) */
static void engine_error(ctl *c)
{
    if (c->engine_err_reported) return;
    c->engine_err_reported = 1;
    if (c->h->cb) disp_post(c->h, IT_ENGINE_ERR, 0, 0);
}

/* eciStop reached the worker inside an engine callback: stop the engine there */
static int aborting(ctl *c)
{
    if (!c->abort) return 0;
    if (!InterlockedExchange(&c->flushed, 1)) eng_call(c->e, ENG_FLUSH, 1, 1, 0, 0, 0);
    return 1;
}

/* FUN_10013fdf: the partly filled buffer */
static void deliver_partial(ctl *c)
{
    if (c->obuf && c->on > 0 && !c->abort) {
        disp_post(c->h, IT_WAVE, c->on, 1);
    }
    c->on = 0;
}

static void read_phonemes(ctl *c);

/* FUN_10013e03: read what the engine has, then deliver the rest */
static void deliver_phonemes(ctl *c)
{
    read_phonemes(c);
    if (c->pbuf && c->pn > 0 && !c->abort) {
        memcpy(c->h->phon_user, c->pbuf, (size_t)c->pn);
        c->h->phon_user[c->pn] = 0;
        disp_post(c->h, IT_PHONEMES, c->pn, 1);
    }
    c->pn = 0;
}

static void cb_synth(ctl *c, cpu *m)
{
    uint32_t count = eng_arg(m, 0), p = eng_arg(m, 1);
    if (aborting(c)) return;
    if (c->dev && !c->audio_error) {
        short tmp[512];
        for (uint32_t i = 0; i < count; ) {
            int k = 0;
            for (; k < 512 && i < count; k++, i++) {
                int32_t v = (int32_t)rd32(m, p + 4 * i);
                if (c->vfx.on) voicefx_audio(&c->vfx, &v, 1);
                tmp[k] = (short)v;                                    /* low 16 bits */
            }
            if (dev_open(c->dev)) { c->audio_error = 1; if (c->h->cb) disp_post(c->h, IT_AUDIO_ERR, 0, 0); break; }
            dev_samples(c->dev, tmp, k);
        }
    }
    if (c->obuf) {
        for (uint32_t i = 0; i < count; i++) {
            if (c->abort) { aborting(c); return; }
            int32_t v = (int32_t)rd32(m, p + 4 * i);
            if (c->vfx.on) voicefx_audio(&c->vfx, &v, 1);
            c->obuf[c->on++] = (short)v;                       /* FUN_10013f27: truncated, not clipped */
            if (c->on == c->ocap) {
                disp_post(c->h, IT_WAVE, c->on, 1);
                c->on = 0;
            }
        }
    }
}

/* FUN_10012a3b */
static void cb_word(ctl *c, cpu *m)
{
    int count = (int)eng_arg(m, 0);
    if (aborting(c)) return;
    if (c->chars < count) count = c->chars;
    c->chars -= count;
    c->chars_last -= count;
    if (c->chars_last < 0) c->chars_last = 0;
    int stop = 0;
    while (c->nidx > 0 && !stop) {
        idxpos *hd = &c->idx[0];
        int take = count < hd->delta ? count : hd->delta;
        hd->delta -= take;
        count -= take;
        if (hd->delta == 0) {
            unsigned slot = hd->slot;
            memmove(c->idx, c->idx + 1, (size_t)(c->nidx - 1) * sizeof *c->idx);
            c->nidx--;
            if (eng_call(c->e, ENG_INSERT_SYNTHESIS_INDEX, 1, slot, 0, 0, 0)) engine_error(c);
        } else
            stop = count == 0;
    }
}

/* FUN_100127ce */
static void cb_index(ctl *c, cpu *m)
{
    unsigned slot = eng_arg(m, 0);
    if (aborting(c)) return;
    if (c->dev && !c->audio_error) {
        if (c->dev->open) { dev_marker(c->dev, slot); return; }
    }
    if (c->obuf) deliver_partial(c);
    if (c->pbuf) deliver_phonemes(c);
    handle_slot(c, slot);
}

/* FUN_100129a0 */
static void cb_phoneme(ctl *c, cpu *m)
{
    uint32_t p1 = eng_arg(m, 0), p2 = eng_arg(m, 1);
    if (aborting(c)) return;
    unsigned slot = slot_alloc(c, 1, (int)p1);
    if (!slot) { if (c->h->cb) disp_post(c->h, IT_MEMORY, 0, 0); return; }
    if (eng_call(c->e, ENG_INSERT_DELAYED_SYNTHESIS_INDEX, 2, slot, p2, 0, 0)) engine_error(c);
}

static uint32_t engine_callback(eng *e, cpu *m, unsigned n)
{
    ctl *c = (ctl *)eng_user(e);
    switch (n) {
    case CB_SYNTH: cb_synth(c, m); break;
    case CB_WORD: cb_word(c, m); break;
    case CB_INDEX: cb_index(c, m); break;
    case CB_PHONEME: cb_phoneme(c, m); break;
    case CB_ANNO: {
        int id = (int)eng_arg(m, 0);
        if (id >= 0 && id < 20) c->mirror[id] = (int)eng_arg(m, 1);
        break;
    }
    }
    return 0;                        /* all cdecl */
}

#define CBADDR(n) (ENG_CB_BASE + (n))

static uint32_t call_text(ctl *c, int slot, const char *s)
{
    if (!s) return eng_call(c->e, slot, 1, 0, 0, 0, 0);
    return eng_call_text(c->e, slot, s);
}

/* FUN_10013080: every string given to processSentences counts for index placement */
static void process_sentences(ctl *c, const char *s)
{
    c->chars += eci_char_count(s);
    if (call_text(c, ENG_PROCESS_SENTENCES, s)) engine_error(c);
    read_phonemes(c);
}

/* FUN_10013c0e: take what the engine has transcribed; a full buffer goes to the user at once */
static void read_phonemes(ctl *c)
{
    if (!c->pbuf) return;
    cpu *m = eng_cpu(c->e);
    uint32_t tmp = x86_alloc(m, (uint32_t)c->pcap + 4), np = x86_alloc(m, 4);
    for (;;) {
        wr32(m, np, 0);
        if (eng_call(c->e, ENG_READ_PHONEMES, 3, tmp, (uint32_t)(c->pcap - c->pn), np, 0)) { engine_error(c); break; }
        uint32_t n = rd32(m, np);
        for (uint32_t i = 0; i < n && c->pn < c->pcap; i++) c->pbuf[c->pn++] = (char)rd8(m, tmp + i);
        if (c->pn < c->pcap) break;
        if (!c->abort) {
            memcpy(c->h->phon_user, c->pbuf, (size_t)c->pn);
            c->h->phon_user[c->pn] = 0;
            disp_post(c->h, IT_PHONEMES, c->pn, 1);
        }
        c->pn = 0;
    }
    x86_dealloc(m, tmp);
    x86_dealloc(m, np);
}

static void process_remaining(ctl *c) { if (call_text(c, ENG_PROCESS_REMAINING, NULL)) engine_error(c); }

/* FUN_100131b2 */
static int register_callbacks(ctl *c)
{
    eng *e = c->e;
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, CBADDR(CB_WORD), 0, 0, 0);
    eng_call(e, ENG_REGISTER_INDEX_CALLBACK, 2, CBADDR(CB_INDEX), 0, 0, 0);
    eng_call(e, ENG_REGISTER_PHONEME_CALLBACK, 2, CBADDR(CB_PHONEME), 0, 0, 0);
    eng_call(e, ENG_REGISTER_ANNO_CALLBACK, 2, CBADDR(CB_ANNO), 0, 0, 0);
    if (eng_call(e, ENG_SET_SYNTH_TO_CALLBACK, 2, CBADDR(CB_SYNTH), 0, 0, 0)) return -1;
    if (eng_call(e, ENG_CLEAR_INPUT, 0, 0, 0, 0, 0)) return -1;
    if (eng_call(e, ENG_FLUSH, 1, 0, 0, 0, 0)) return -1;
    return 0;
}

/* FUN_10010e96: the engine setup of eciNew / eciReset */
static int engine_setup(ctl *c)
{
    eng *e = c->e;
    eng_call(e, ENG_WANT_PHONEME_INDICES, 1, 0, 0, 0, 0);
    c->mirror[4] = 0;
    eng_call(e, ENG_REGISTER_ANNO_CALLBACK, 2, CBADDR(CB_ANNO), 0, 0, 0);
    eng_call(e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    if (eng_call_text(e, ENG_PROCESS_REMAINING, "`v1 `ts0 `da1 `ty1 `pp1")) return -1;
    if (register_callbacks(c)) return -1;
    c->mirror[2] = LANG_ENU;
    process_remaining(c);
    return 0;
}

/* eciSampleRate -> Hz: 0 and 1 as the original, 2..4 this port's extension */
static int rate_hz(int p) { return p == 0 ? 8000 : p == 2 ? 22050 : p == 3 ? 44100 : p == 4 ? 48000 : 11025; }

/* the esr group (FUN_10012538 / FUN_100110a4); above 11025 the engine is told 11025 and the synthesizer
 * is run at the real rate (eng_set_rate) */
static int send_rate(ctl *c, int param5)
{
    eng_set_rate(c->e, param5 >= 2 ? (float)rate_hz(param5) : 0.0f);
    int rate8000 = param5 == 0;
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    int r = (int)eng_call_text(c->e, ENG_PROCESS_REMAINING, rate8000 ? "`esr0" : "`esr1");
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, CBADDR(CB_WORD), 0, 0, 0);
    return r ? -0xf : 0;
}

static void worker_run(ctl *c, cmd *k)
{
    switch (k->type) {
    case C_TEXT: {
        if (c->dev && !c->audio_error && dev_open(c->dev)) {
            c->audio_error = 1;
            if (c->h->cb) disp_post(c->h, IT_AUDIO_ERR, 0, 0);
        }
        char *t = eci_prepare(k->text, k->annot);
        if (t) { if (!c->abort) process_sentences(c, t); free(t); }
        InterlockedDecrement(&c->pending);
        break;
    }
    case C_ANNOT:
        process_sentences(c, k->text);
        break;
    case C_INDEX: {
        unsigned slot = slot_alloc(c, 0, k->value);
        if (!slot) { if (c->h->cb) disp_post(c->h, IT_MEMORY, 0, 0); }
        else if (c->chars == 0) {
            if (eng_call(c->e, ENG_INSERT_SYNTHESIS_INDEX, 1, slot, 0, 0, 0)) engine_error(c);
        } else {
            if (c->nidx == c->capidx) {
                c->capidx = c->capidx ? 2 * c->capidx : 16;
                c->idx = (idxpos *)realloc(c->idx, (size_t)c->capidx * sizeof *c->idx);
            }
            c->idx[c->nidx].slot = slot;
            c->idx[c->nidx].delta = c->chars - c->chars_last;
            c->nidx++;
        }
        c->chars_last = c->chars;
        InterlockedDecrement(&c->pending);
        break;
    }
    case C_SYNTH:
        if (InterlockedDecrement(&c->pending) == 0) {
            process_remaining(c);
            if (c->pending == 0) {
                c->chars = c->chars_last = 0;
                if (c->dev && !c->audio_error) dev_finish(c->dev);
                if (c->obuf) deliver_partial(c);
                if (c->pbuf) deliver_phonemes(c);
            }
            if (c->pending == 0 && !c->abort) disp_post(c->h, IT_END, k->value, 0);
        }
        break;
    case C_TS:
        if (k->value) process_remaining(c);
        break;
    case C_WANTPH:
        process_remaining(c);
        eng_call(c->e, ENG_WANT_PHONEME_INDICES, 1, (uint32_t)k->value, 0, 0, 0);
        c->mirror[4] = k->value;
        break;
    case C_LANG:
        process_remaining(c);
        break;
    case C_WORDIDX:
        if (k->value) process_remaining(c);
        break;
    case C_FX:                           /* this port's voice effects, in step with the text */
        voicefx_init(&c->vfx, rate_hz(c->h->param[5]));
        if (k->value > 0 && k->value <= N_EXT) voicefx_parse(&c->vfx, ext_voices[k->value - 1].fx);
        break;
    case C_BLOCK:
        WaitForSingleObject(c->unblock, INFINITE);
        break;
    }
}

static void cmd_free(cmd *k) { free(k->text); free(k); }

static DWORD WINAPI worker_main(void *p)
{
    ctl *c = (ctl *)p;
    for (;;) {
        EnterCriticalSection(&c->qlock);
        while (!c->qhead && !c->quit) SleepConditionVariableCS(&c->qcond, &c->qlock, INFINITE);
        if (c->quit) { LeaveCriticalSection(&c->qlock); break; }
        cmd *k = c->qhead;
        c->qhead = k->next;
        if (!c->qhead) c->qtail = NULL;
        c->running = 1;
        LeaveCriticalSection(&c->qlock);
        EnterCriticalSection(&c->englock);
        if (!c->abort) worker_run(c, k);
        LeaveCriticalSection(&c->englock);
        cmd_free(k);
        EnterCriticalSection(&c->qlock);
        c->running = 0;
        WakeAllConditionVariable(&c->idlecond);
        LeaveCriticalSection(&c->qlock);
    }
    return 0;
}

static int post_cmd(ctl *c, int type, const char *text, size_t len, int annot, int value)
{
    cmd *k = (cmd *)calloc(1, sizeof *k);
    if (!k) return -2;
    k->type = type;
    if (text) {
        k->text = (char *)malloc(len + 1);
        if (!k->text) { free(k); return -2; }
        memcpy(k->text, text, len);
        k->text[len] = 0;
    }
    k->annot = annot;
    k->value = value;
    if (type == C_TEXT || type == C_INDEX || type == C_SYNTH) {
        InterlockedIncrement(&c->pending);
        c->busy = 1;
    }
    EnterCriticalSection(&c->qlock);
    if (type == C_SYNTH) k->value = (int)InterlockedIncrement(&c->h->d.posted);
    else if (type == C_TEXT || type == C_INDEX) InterlockedIncrement(&c->h->d.posted);
    if (c->qtail) c->qtail->next = k; else c->qhead = k;
    c->qtail = k;
    WakeAllConditionVariable(&c->qcond);
    LeaveCriticalSection(&c->qlock);
    return 0;
}

static void post_annot(ctl *c, const char *name, int v)
{
    char s[32];
    sprintf(s, "`%s%u", name, (unsigned)v);
    post_cmd(c, C_ANNOT, s, strlen(s), 0, 0);
}

/* wait until the worker has nothing to do */
static void worker_wait_idle(ctl *c)
{
    EnterCriticalSection(&c->qlock);
    while (c->running || c->qhead) SleepConditionVariableCS(&c->idlecond, &c->qlock, 50);
    LeaveCriticalSection(&c->qlock);
}

/* every synthesizer frame, on the worker: this port's voice effects (nothing when none are chosen) */
static void fx_frame_hook(void *user, float f[64])
{
    ctl *c = (ctl *)user;
    if (c->vfx.on) voicefx_frame(&c->vfx, f);
}

static ctl *ctl_new(eci *h)
{
    ctl *c = (ctl *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->h = h;
    InitializeCriticalSection(&c->qlock);
    InitializeCriticalSection(&c->englock);
    InitializeCriticalSection(&c->slotlock);
    InitializeConditionVariable(&c->qcond);
    InitializeConditionVariable(&c->idlecond);
    c->unblock = CreateEventA(NULL, FALSE, FALSE, NULL);
    c->e = eng_new(NULL, engine_callback, c);
    if (!c->e || engine_setup(c)) {
        if (c->e) eng_free(c->e);
        free(c);
        return NULL;
    }
    voicefx_init(&c->vfx, 11025);
    eng_set_frame_hook(c->e, fx_frame_hook, c);
    c->thread = CreateThread(NULL, 0, worker_main, c, 0, NULL);
    return c;
}

static void ctl_free(ctl *c)
{
    if (!c) return;
    EnterCriticalSection(&c->qlock);
    c->quit = 1;
    WakeAllConditionVariable(&c->qcond);
    LeaveCriticalSection(&c->qlock);
    SetEvent(c->unblock);
    WaitForSingleObject(c->thread, INFINITE);
    CloseHandle(c->thread);
    while (c->qhead) { cmd *k = c->qhead; c->qhead = k->next; cmd_free(k); }
    dev_free(c->dev);
    eng_call(c->e, ENG_CLOSE, 0, 0, 0, 0, 0);
    eng_call(c->e, ENG_RELEASE, 0, 0, 0, 0, 0);
    eng_free(c->e);
    free(c->idx);
    free(c->slots);
    free(c->freelist);
    free(c->pbuf);
    CloseHandle(c->unblock);
    DeleteCriticalSection(&c->qlock);
    DeleteCriticalSection(&c->englock);
    DeleteCriticalSection(&c->slotlock);
    free(c);
}

/* FUN_10012c0c (3.5) */
static int do_stop(eci *h)
{
    ctl *c = h->c;
    c->abort = 1;
    disp_discard(h);
    EnterCriticalSection(&c->qlock);
    while (c->qhead) { cmd *k = c->qhead; c->qhead = k->next; cmd_free(k); }
    c->qtail = NULL;
    LeaveCriticalSection(&c->qlock);
    SetEvent(c->unblock);
    worker_wait_idle(c);
    EnterCriticalSection(&c->englock);
    if (c->dev) dev_close(c->dev, 1);
    if (!c->flushed) eng_call(c->e, ENG_FLUSH, 1, 1, 0, 0, 0);
    int r = eng_call(c->e, ENG_FLUSH, 1, 0, 0, 0, 0) ? -0xf : 0;
    c->nidx = 0;
    c->on = 0;
    c->pn = 0;
    c->chars = c->chars_last = 0;
    c->pending = 0;
    c->busy = 0;
    c->abort = 0;
    c->flushed = 0;
    LeaveCriticalSection(&c->englock);
    h->d.posted = h->d.completed = 0;
    h->d.paused = 0;
    h->d.blocked = 0;
    PostMessageA(h->hwnd, WM_ECI, (WPARAM)h, 0);
    return r;
}

/* ---------------------------------------------------------------- the handle */
static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_ECI || msg == WM_TIMER) && wp) {
        eci *h = (eci *)wp;
        if (h->hwnd == w && !h->reenter) {
            h->reenter = 1;
            disp_poll(h);
            h->reenter = 0;
        }
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static int class_registered;
static CRITICAL_SECTION g_lock;
static HMODULE g_module;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = inst;
        InitializeCriticalSection(&g_lock);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

static int make_window(eci *h)
{
    EnterCriticalSection(&g_lock);
    if (!class_registered) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = g_module;
        wc.lpszClassName = "eciWindowETI";
        if (RegisterClassA(&wc)) class_registered = 1;
    }
    LeaveCriticalSection(&g_lock);
    h->hwnd = CreateWindowExA(0, "eciWindowETI", "", 0, 0, 0, 50, 50, HWND_MESSAGE, NULL, g_module, NULL);
    return h->hwnd ? 0 : -1;
}

static void set_defaults(eci *h)
{
    ctl *c = h->c;
    memset(h->param, 0, sizeof h->param);
    h->param[2] = c->mirror[0];
    h->param[3] = c->mirror[3];
    h->param[7] = c->mirror[4];
    h->param[9] = c->mirror[2];
    h->param[11] = c->mirror[13];
    h->param[10] = c->mirror[1];
    h->param[5] = 1;
    h->param[13] = 10;
    h->param[14] = 2200;
    h->param[16] = 2200;
    memcpy(h->sent, h->param, sizeof h->sent);
    memset(&h->active, 0, sizeof h->active);
    for (int k = 0; k < 8; k++) h->active.p[k] = c->mirror[5 + k];
    eci_voice_realworld(&h->active, -1);
    strcpy(h->active.name, "Adult Male 1");
    h->sent_voice = h->active;
    for (int n = 0; n < 8; n++) {
        h->user[n] = g_presets[n];
        strcpy(h->user[n].name, "User-Defined");
    }
}

/* output: FUN_10006dd4 (device or file), FUN_10006fd5 (buffer), FUN_100070fc (none) */
static int output_device(eci *h, const char *name)
{
    ctl *c = h->c;
    if (c->busy) return -9;
    EnterCriticalSection(&c->englock);
    int rate = rate_hz(h->param[5]);
    int same = c->dev && h->mode == OUT_DEVICE && !strcmp(h->devname, name) && c->dev->rate == rate;
    int r = 0;
    if (!same) {
        dev_free(c->dev);
        c->dev = NULL;
        c->obuf = NULL;
        c->pbuf = NULL;
        c->dev = dev_new(c, name, rate);
        if (!c->dev) r = -4;
        else r = send_rate(c, h->param[5]);
    }
    LeaveCriticalSection(&c->englock);
    if (r == 0) {
        h->mode = OUT_DEVICE;
        if (name != h->devname) strncpy(h->devname, name, sizeof h->devname - 1);
        h->sent[5] = h->param[5];
    }
    return r;
}

static int output_buffer(eci *h)
{
    ctl *c = h->c;
    if (c->busy) return -9;
    EnterCriticalSection(&c->englock);
    dev_free(c->dev);
    c->dev = NULL;
    c->pbuf = NULL;
    c->obuf = h->buf;
    c->ocap = h->bufsize;
    c->on = 0;
    int r = send_rate(c, h->param[5]);
    LeaveCriticalSection(&c->englock);
    if (r == 0) { h->mode = OUT_BUFFER; h->sent[5] = h->param[5]; }
    return r;
}

static void output_none(eci *h)
{
    ctl *c = h->c;
    EnterCriticalSection(&c->englock);
    dev_free(c->dev);
    c->dev = NULL;
    c->obuf = NULL;
    c->pbuf = NULL;
    LeaveCriticalSection(&c->englock);
    h->mode = OUT_NONE;
}

static void map_error(eci *h, int r)
{
    switch (r) {
    case -1: set_error(h, 0x1); break;
    case -2: set_error(h, 0x2); break;
    case -3: case -6: case -7: case -8: set_error(h, 0x80); break;
    case -4: case -5: case -0x10: set_error(h, 0x20); break;
    case -9: set_error(h, 0x100); break;
    case -0xd: set_error(h, 0x200); break;
    case -0xe: case -0x15: set_error(h, 0x4000); break;
    case -0xf: set_error(h, 0x10); break;
    }
}

/* sendParameters (FUN_1000905e -> FUN_10007f6a, FUN_100082ec) */
static int send_params(eci *h, const int *params, const eci_voice *voice)
{
    ctl *c = h->c;
    int force = h->force;
    if (force || params[5] != h->sent[5] || params[13] != h->sent[13] || params[14] != h->sent[14] ||
        params[15] != h->sent[15] || params[16] != h->sent[16]) {
        int save = h->param[5], r = 0;
        h->param[5] = params[5];
        if (h->mode == OUT_DEVICE) r = output_device(h, h->devname);
        else if (h->mode == OUT_BUFFER) r = output_buffer(h);
        else h->sent[5] = params[5];
        h->param[5] = save;
        if (r) { map_error(h, r); return r; }
        for (int k = 13; k <= 16; k++) h->sent[k] = params[k];
    }
    int lang_changed = 0;
    if (force || params[9] != h->sent[9]) {
        lang_changed = (params[9] >> 16) != (h->sent[9] >> 16);
        post_cmd(c, C_LANG, NULL, 0, 0, params[9]);
        h->sent[9] = params[9];
    }
    if (force || params[2] != h->sent[2]) {
        post_annot(c, "ts", params[2]);
        post_cmd(c, C_TS, NULL, 0, 0, params[2]);
        h->sent[2] = params[2];
    }
    if (force || params[3] != h->sent[3]) { post_annot(c, "da", params[3]); h->sent[3] = params[3]; }
    if (force || params[11] != h->sent[11]) { post_annot(c, "pp", params[11]); h->sent[11] = params[11]; }
    if (force || params[7] != h->sent[7]) { post_cmd(c, C_WANTPH, NULL, 0, 0, params[7]); h->sent[7] = params[7]; }
    if (force) post_cmd(c, C_LANG, NULL, 0, 0, params[9]);
    if (force || params[10] != h->sent[10]) { post_annot(c, "ty", params[10]); h->sent[10] = params[10]; }
    if (force || lang_changed || params[12] != h->sent[12]) {
        post_cmd(c, C_WORDIDX, NULL, 0, 0, params[12]);
        h->sent[12] = params[12];
    }
    static const char names[8][3] = { "vg", "vh", "vb", "vf", "vr", "vy", "vs", "vv" };
    for (int k = 0; k < 8; k++)
        if (force || voice->p[k] != h->sent_voice.p[k]) {
            post_annot(c, names[k], voice->p[k]);
            h->sent_voice.p[k] = voice->p[k];
        }
    if (voice->fx != h->sent_voice.fx) {    /* this port's voice effects */
        post_cmd(c, C_FX, NULL, 0, 0, voice->fx);
        h->sent_voice.fx = voice->fx;
    }
    for (int k = 0; k < 17; k++)
        if (k != 5 && (k < 13 || k > 16)) h->sent[k] = params[k];
    h->force = 0;
    return 0;
}

/* eciAddText2 (FUN_10015ced): split at `l annotations (annotations on) */
static int add_text2(eci *h, char *text, int annot)
{
    ctl *c = h->c;
    size_t n = strlen(text), start = 0;
    if (annot) {
        for (size_t i = 0; i < n; i++) {
            if (text[i] == '`' && text[i + 1] == 'l' && text[i + 2] >= '0' && text[i + 2] <= '9' &&
                (i == 0 || text[i - 1] != '\\')) {
                if (i > start) post_cmd(c, C_TEXT, text + start, i - start, annot, 0);
                size_t j = i + 2;
                while (j < n && ((text[j] >= '0' && text[j] <= '9') || text[j] == '.')) j++;
                post_cmd(c, C_LANG, NULL, 0, 0, LANG_ENU);
                memset(text + i, ' ', j - i);
                start = i;
                i = j - 1;
            }
        }
    }
    return post_cmd(c, C_TEXT, text + start, n - start, annot, 0);
}

static void manual_free(eci *h)
{
    struct mitem *m = h->mhead;
    while (m) { struct mitem *n = m->next; free(m->text); free(m); m = n; }
    h->mhead = h->mtail = NULL;
}

static int manual_push(eci *h, int type, const char *text, int index)
{
    mitem *m = (mitem *)calloc(1, sizeof *m);
    if (!m) return -2;
    m->type = type;
    m->index = index;
    memcpy(m->params, h->param, sizeof m->params);
    m->voice = h->active;
    if (text) {
        m->text = _strdup(text);
        if (!m->text) { free(m); return -2; }
        if (h->param[1] == 1) eci_parse_annotations(&h->active, h->param, NULL, NULL, m->params[8], m->text, g_presets);
    }
    if (h->mtail) h->mtail->next = m; else h->mhead = m;
    h->mtail = m;
    return 0;
}

/* FUN_10007309: send the manual-mode queue */
static int manual_send(eci *h)
{
    mitem *m = h->mhead;
    if (!m) return 0;
    ctl *c = h->c;
    post_cmd(c, C_BLOCK, NULL, 0, 0, 0);
    int r = 0;
    for (; m && !r; m = m->next) {
        r = send_params(h, m->params, &m->voice);
        if (r) break;
        if (m->type == 0) {
            if (m->params[1] == 1)
                eci_parse_annotations(&h->sent_voice, h->sent, NULL, NULL, 0, m->text, g_presets);
            r = add_text2(h, m->text, m->params[1]);
        } else
            r = post_cmd(c, C_INDEX, NULL, 0, 0, m->index);
    }
    manual_free(h);
    SetEvent(c->unblock);
    return r;
}

#define GUARD(h, fail) do { if ((h) && (h)->reenter) { set_error((h), 0x800); return fail; } } while (0)

static eci *new_handle(int dialect, int newex2)
{
    presets_init();
    if (dialect != LANG_ENU) return NULL;
    eci *h = (eci *)calloc(1, sizeof *h);
    if (!h) return NULL;
    InitializeCriticalSection(&h->d.lock);
    h->d.nonempty = CreateEventA(NULL, TRUE, FALSE, NULL);
    h->newex2 = (char)newex2;
    if (make_window(h)) goto fail;
    h->c = ctl_new(h);
    if (!h->c) goto fail;
    set_defaults(h);
    if (newex2) h->mode = OUT_NONE;
    else if (output_device(h, "-1")) goto fail;
    return h;
fail:
    if (h->c) ctl_free(h->c);
    if (h->hwnd) DestroyWindow(h->hwnd);
    CloseHandle(h->d.nonempty);
    DeleteCriticalSection(&h->d.lock);
    free(h);
    return NULL;
}

/* ================================================================= exports */
void *ECIFN eciNew(void) { return new_handle(LANG_ENU, 0); }
void *ECIFN eciNewEx(int dialect) { return new_handle(dialect, 0); }
void *ECIFN eciNewEx2(void) { return new_handle(LANG_ENU, 1); }

void *ECIFN eciDelete(void *hh)
{
    eci *h = (eci *)hh;
    if (!h) return NULL;
    GUARD(h, NULL);
    h->reenter = 1;
    do_stop(h);
    ctl_free(h->c);
    DestroyWindow(h->hwnd);
    manual_free(h);
    CloseHandle(h->d.nonempty);
    DeleteCriticalSection(&h->d.lock);
    free(h->dict_key);
    free(h->dict_value);
    free(h);
    return NULL;
}

int ECIFN eciReset(void *hh)
{
    eci *h = (eci *)hh;
    if (!h) return 0;
    GUARD(h, 0);
    h->reenter = 1;
    do_stop(h);
    EnterCriticalSection(&h->c->englock);
    int r = engine_setup(h->c);
    LeaveCriticalSection(&h->c->englock);
    manual_free(h);
    set_defaults(h);
    h->force = 0;
    if (!r) {
        if (h->newex2) output_none(h);
        else { h->mode = OUT_DEVICE; h->buf = NULL; r = output_device(h, "-1"); }
    }
    h->reenter = 0;
    if (r || h->newex2) return 0;
    h->status = h->lasterr = 0;
    return 1;
}

void ECIFN eciVersion(char *buf) { if (buf) strcpy(buf, "6.1.0.2"); }
int ECIFN eciProgStatus(void *hh) { return hh ? (int)((eci *)hh)->status : 0x80; }

void ECIFN eciErrorMessage(void *hh, void *buf)
{
    eci *h = (eci *)hh;
    const char *s;
    if (!h) s = "System resources are low.";
    else switch (h->lasterr) {
    case 0: s = ""; break;
    case 1: s = "System error."; break;
    case 2: s = "System resources are low."; break;
    case 0x10: s = "Synthesis engine error."; break;
    case 0x20: s = "Audio device error."; break;
    case 0x80: s = "Invalid or out of range parameter."; break;
    case 0x100: case 0x2000: s = "Synthesis engine is busy."; break;
    case 0x200: s = "Audio device busy."; break;
    case 0x400: s = "Synthesis engine is paused."; break;
    case 0x800: s = "Cannot reenter ECI on the same thread."; break;
    case 0x1000: s = "No Romanizer Error"; break;
    case 0x4000: s = "Cannot load module."; break;
    default: s = ""; break;
    }
    if (buf) strcpy((char *)buf, s);
}

void ECIFN eciClearErrors(void *hh)
{
    eci *h = (eci *)hh;
    if (!h || h->reenter) { if (h) set_error(h, 0x800); return; }
    h->c->engine_err_reported = 0;
    h->c->audio_error = 0;
    h->lasterr = h->status = 0;
}

int ECIFN eciGetIndex(void *hh) { return hh ? ((eci *)hh)->last_index : 0; }

int ECIFN eciGetAvailableLanguages(int *langs, int *n)
{
    if (!n || *n < 0 || (!langs && *n != 0)) return 0x80;
    if (*n == 0) { *n = 1; return 0; }
    langs[0] = LANG_ENU;
    *n = 1;
    return 0;
}

int ECIFN eciIsBeingReentered(void *hh) { return hh ? ((eci *)hh)->reenter : 0; }
int ECIFN eciDialogBox(void) { return 1; }
void ECIFN eciRequestLicense(int code) { (void)code; }
void ECIFN eciStartLogging(void) {}
void ECIFN eciStopLogging(void) {}
int ECIFN eciGetLog(void) { return 0; }
int ECIFN eciGetIntLog(void) { return 0; }
int ECIFN eciSynchronizeSynth(void *hh) { (void)hh; return 0; }

/* ---------------------------------------------------------------- parameters and voices (3.3) */
int ECIFN eciGetParam(void *hh, int p)
{
    eci *h = (eci *)hh;
    if (!h || p < 0 || p > 16) return -1;
    if (p == 3) return h->param[3] == 0 ? 1 : 0;
    return h->param[p];
}

int ECIFN eciSetParam(void *hh, int p, int v)
{
    eci *h = (eci *)hh;
    GUARD(h, -1);
    if (!h || p < 0 || p > 16 || v < eci_param_range[p][0] || v > eci_param_range[p][1]) return -1;
    if (p == 9 && v != LANG_ENU) return -1;
    if (p >= 13) return -1;
    int old = eciGetParam(h, p);
    h->param[p] = p == 3 ? 1 - v : v;
    return old;
}

int ECIFN eciGetDefaultParam(int p) { (void)p; return -1; }
int ECIFN eciSetDefaultParam(int p, int v) { (void)p; (void)v; return -1; }

static eci_voice *voice_of(eci *h, int voice)
{
    if (voice == 0) return &h->active;
    if (voice >= 1 && voice <= 8) return &g_presets[voice - 1];
    if (voice >= 9 && voice <= 16) return &h->user[voice - 9];
    if (voice >= EXT_FIRST && voice < EXT_FIRST + N_EXT) return &g_ext[voice - EXT_FIRST];
    return NULL;
}

static int voice_value(eci *h, const eci_voice *v, int p)
{
    if (h->param[8] == 1) {
        if (p == 2) return v->rw[0];
        if (p == 6) return v->rw[1];
        if (p == 7) return v->rw[2];
    }
    return v->p[p];
}

int ECIFN eciGetVoiceParam(void *hh, int voice, int p)
{
    eci *h = (eci *)hh;
    if (!h || p < 0 || p > 7 || voice < 0 || voice >= EXT_FIRST + N_EXT || !voice_of(h, voice)) return -1;
    return voice_value(h, voice_of(h, voice), p);
}

int ECIFN eciSetVoiceParam(void *hh, int voice, int p, int v)
{
    eci *h = (eci *)hh;
    GUARD(h, -1);
    if (!h || !(voice == 0 || (voice >= 9 && voice <= 16)) || p < 0 || p > 7) return -1;
    int units = h->param[8] == 1;
    int lo = units ? eci_to_real(p, eci_voice_range[p][0]) : eci_voice_range[p][0];
    int hi = units ? eci_to_real(p, eci_voice_range[p][1]) : eci_voice_range[p][1];
    if (v < lo || v > hi) return -1;
    eci_voice *vc = voice_of(h, voice);
    int old = voice_value(h, vc, p);
    if (units && (p == 2 || p == 6 || p == 7)) {
        vc->rw[p == 2 ? 0 : p == 6 ? 1 : 2] = v;
        vc->p[p] = eci_from_real(p, v, 0, 250);
    } else {
        vc->p[p] = v;
        eci_voice_realworld(vc, p);
    }
    return old;
}

int ECIFN eciCopyVoice(void *hh, int from, int to)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    int to_ok = to == 0 || (to >= 9 && to <= 16);
    int ok = (to_ok && from == 0) || (from >= 9 && from <= 16) || (from >= 1 && from <= 8 && g_presets[from - 1].defined) ||
             (from >= EXT_FIRST && from < EXT_FIRST + N_EXT);   /* this port's voices */
    if (!ok || !to_ok) return 0;      /* the original writes out of bounds for other `to`; refuse instead */
    *voice_of(h, to) = *voice_of(h, from);
    return 1;
}

int ECIFN eciGetVoiceName(void *hh, int voice, void *buf)
{
    eci *h = (eci *)hh;
    if (!h || voice < 0 || voice >= EXT_FIRST + N_EXT || !voice_of(h, voice)) return 0;
    if (!buf) { set_error(h, 0x80); return 0; }
    strcpy((char *)buf, voice_of(h, voice)->name);
    return 1;
}

int ECIFN eciSetVoiceName(void *hh, int voice, const void *name)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    if ((voice == 0 || (voice >= 9 && voice <= 16)) && name) {
        eci_voice *v = voice_of(h, voice);
        strncpy(v->name, (const char *)name, 30);
        v->name[30] = 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- text (3.4, 3.7) */
int ECIFN eciAddText(void *hh, const char *text)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h || !text) { if (h) set_error(h, 0x80); return 0; }
    if (!*text) return 1;
    if (h->param[0] == 1) {
        if (manual_push(h, 0, text, 0)) { set_error(h, 0x2); return 0; }
        return 1;
    }
    manual_send(h);
    if (send_params(h, h->param, &h->active)) return 0;
    char *copy = _strdup(text);
    if (!copy) { set_error(h, 0x2); return 0; }
    if (h->param[1] == 1)
        eci_parse_annotations(&h->active, h->param, &h->sent_voice, h->sent, h->param[8], copy, g_presets);
    int r = add_text2(h, copy, h->param[1]);
    free(copy);
    if (r) { map_error(h, r); return 0; }
    return 1;
}

int ECIFN eciInsertIndex(void *hh, int index)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    if (h->param[0] == 1) return manual_push(h, 1, NULL, index) == 0;
    if (send_params(h, h->param, &h->active)) return 0;
    return post_cmd(h->c, C_INDEX, NULL, 0, 0, index) == 0;
}

int ECIFN eciSynthesize(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    manual_send(h);
    return post_cmd(h->c, C_SYNTH, NULL, 0, 0, 0) == 0;
}

int ECIFN eciClearInput(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    manual_free(h);
    return 1;
}

int ECIFN eciStop(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    h->reenter = 1;
    manual_free(h);
    int r = do_stop(h);
    h->reenter = 0;
    if (r) { map_error(h, r); return 0; }
    h->force = 1;
    return 1;
}

int ECIFN eciSpeaking(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    h->reenter = 1;
    int r = disp_poll(h);
    h->reenter = 0;
    return r == 3 || r == 1;
}

int ECIFN eciSynchronize(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    h->reenter = 1;
    int r = disp_sync(h);
    h->reenter = 0;
    if (r == 1) { set_error(h, 0x400); return 0; }
    return r != -2;
}

int ECIFN eciSyncWait(void *hh, int ms)
{
    eci *h = (eci *)hh;
    if (ms < 0) return eciSynchronize(h);
    if (ms == 0) return disp_poll(h) == 3;
    DWORD end = GetTickCount() + (DWORD)ms;
    int r;
    while ((r = disp_poll(h)) == 3)
        if ((int)(GetTickCount() - end) >= 0) return 1;
    return 0;
}

int ECIFN eciPause(void *hh, int on)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    h->d.paused = on != 0;
    if (!on || !h->d.retained) {
        if (h->d.retained) SetTimer(h->hwnd, (UINT_PTR)h, 30, NULL);
        PostMessageA(h->hwnd, WM_ECI, (WPARAM)h, 0);
    } else
        KillTimer(h->hwnd, (UINT_PTR)h);
    wavedev *w = h->c->dev;
    if (w && w->open && !w->file) {
        if (on) waveOutPause(w->wo); else waveOutRestart(w->wo);
    }
    return 1;
}

void ECIFN eciRegisterCallback(void *hh, ECICallback cb, void *data)
{
    eci *h = (eci *)hh;
    if (!h || h->reenter) { if (h) set_error(h, 0x800); return; }
    h->cb = cb;
    h->data = data;
}

/* ---------------------------------------------------------------- output (6) */
int ECIFN eciSetOutputBuffer(void *hh, int size, short *buf)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h || !h->cb) return 0;
    if (size == 0 || !buf) {
        h->param[5] = 1;
        if (h->newex2) { output_none(h); return 0; }
        int r = output_device(h, "-1");
        if (r) { map_error(h, r); return 0; }
        return 1;
    }
    h->bufsize = size;
    h->buf = buf;
    int r = output_buffer(h);
    if (r) { map_error(h, r); return 0; }
    return 1;
}

int ECIFN eciSetOutputDevice(void *hh, int dev)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    char name[32];
    sprintf(name, "%d", dev);
    int r = output_device(h, name);
    if (r) { map_error(h, r); return 0; }
    return 1;
}

int ECIFN eciSetOutputFilename(void *hh, const void *name)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    if (!name) return eciSetOutputBuffer(h, 0, NULL);
    char n[256];
    strncpy(n, (const char *)name, sizeof n - 1);
    n[sizeof n - 1] = 0;
    int r = output_device(h, n);
    if (r) { map_error(h, r); return 0; }
    return 1;
}

/* ---------------------------------------------------------------- phonemes (3.8) */
int ECIFN eciGeneratePhonemes(void *hh, int size, void *buf)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h || !h->cb || h->param[0] != 1 || size <= 1 || !buf) return 0;
    ctl *c = h->c;
    if (c->busy) { set_error(h, 0x100); return 0; }
    h->reenter = 1;
    int mode = h->mode;
    EnterCriticalSection(&c->englock);
    dev_free(c->dev);
    c->dev = NULL;
    c->obuf = NULL;
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    eng_call_text(c->e, ENG_PROCESS_REMAINING, "`espr1");
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, CBADDR(CB_WORD), 0, 0, 0);
    c->pbuf = (char *)malloc((size_t)size);
    c->pcap = size - 1;
    c->pn = 0;
    h->phon_user = (char *)buf;
    LeaveCriticalSection(&c->englock);
    h->mode = OUT_PHONEMES;
    manual_send(h);
    post_cmd(c, C_SYNTH, NULL, 0, 0, 0);
    disp_sync(h);
    EnterCriticalSection(&c->englock);
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, 0, 0, 0, 0);
    eng_call_text(c->e, ENG_PROCESS_REMAINING, "`espr0");
    eng_call(c->e, ENG_REGISTER_WORD_CALLBACK, 2, CBADDR(CB_WORD), 0, 0, 0);
    free(c->pbuf);
    c->pbuf = NULL;
    LeaveCriticalSection(&c->englock);
    if (mode == OUT_DEVICE) output_device(h, h->devname);
    else if (mode == OUT_BUFFER) output_buffer(h);
    else h->mode = mode;
    h->reenter = 0;
    return 1;
}

int ECIFN eciGeneratePinyins(void *hh, int size, void *buf) { (void)size; (void)buf; if (hh) ((eci *)hh)->reenter = 1; return 0; }

/* ---------------------------------------------------------------- whole-utterance helpers */
int ECIFN eciSpeakTextEx(const char *text, int annot, int dialect)
{
    eci *h = (eci *)eciNewEx(dialect);
    if (!h) return 0;
    if (annot) eciSetParam(h, 1, 1);
    int ok = eciAddText(h, text) && eciSynthesize(h) && eciSynchronize(h);
    eciDelete(h);
    return ok;
}

int ECIFN eciSpeakText(const char *text, int annot)
{
    eci *h = new_handle(LANG_ENU, 1);
    if (!h) return 0;
    int r = output_device(h, "0");
    if (r == 0) {
        h->param[1] = annot ? 1 : 0;
        char *copy = _strdup(text ? text : "");
        r = copy ? add_text2(h, copy, annot ? 1 : 0) : -2;
        free(copy);
        if (r == 0) { post_cmd(h->c, C_SYNTH, NULL, 0, 0, 0); disp_sync(h); }
    }
    eciDelete(h);
    return r == 0;
}

int ECIFN eciTestPhrase(void *hh)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h) return 0;
    eciStop(h);
    eciCopyVoice(h, 1, 0);
    return eciAddText(h, "1 2 3.") && eciSynthesize(h);
}

int ECIFN eciSynthesizeFile(void *hh, const void *filename)
{
    eci *h = (eci *)hh;
    GUARD(h, 0);
    if (!h || !filename) return 0;
    FILE *f = fopen((const char *)filename, "rb");
    if (!f) return 0;
    manual_send(h);
    char line[512];
    int any = 0, ok = 1;
    while (ok && fgets(line, sizeof line, f)) { ok = eciAddText(h, line); any = 1; }
    fclose(f);
    if (!any) return 1;
    manual_send(h);
    return ok && post_cmd(h->c, C_SYNTH, NULL, 0, 0, 0) == 0;
}

/* ---------------------------------------------------------------- dictionaries (7) */
typedef struct { eci *h; uint32_t engdict; } dict;

static int dict_code(int r) { return r == -2 ? 2 : r < 0 ? 6 : 0; }

void *ECIFN eciNewDict(void *hh)
{
    eci *h = (eci *)hh;
    if (!h) return NULL;
    if (h->c->busy && h->d.posted != h->d.completed) { set_error(h, 0x2000); return NULL; }
    send_params(h, h->param, &h->active);
    post_cmd(h->c, C_SYNTH, NULL, 0, 0, 0);
    disp_sync(h);
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t d = eng_call(h->c->e, ENG_NEW_DICT, 0, 0, 0, 0, 0);
    LeaveCriticalSection(&h->c->englock);
    if (!d) return NULL;
    dict *x = (dict *)calloc(1, sizeof *x);
    if (!x) return NULL;
    x->h = h;
    x->engdict = d;
    return x;
}

void *ECIFN eciGetDict(void *hh) { return hh ? ((eci *)hh)->dicts[0] : NULL; }

static uint32_t dict_eng_call(eci *h, int slot, int nargs, uint32_t a, uint32_t b, uint32_t c3, uint32_t d)
{
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t r = eng_call(h->c->e, slot, nargs, a, b, c3, d);
    LeaveCriticalSection(&h->c->englock);
    return r;
}

int ECIFN eciSetDict(void *hh, void *dd)
{
    eci *h = (eci *)hh;
    dict *d = (dict *)dd;
    if (!h) return 6;
    if (!d) {
        if (h->dicts[0]) dict_eng_call(h, ENG_SET_DICT, 1, 0, 0, 0, 0);
        h->dicts[0] = NULL;
        return 0;
    }
    if (dict_eng_call(h, ENG_SET_DICT, 1, d->engdict, 0, 0, 0)) return 6;
    h->dicts[0] = d;
    return 0;
}

void *ECIFN eciDeleteDict(void *hh, void *dd)
{
    eci *h = (eci *)hh;
    dict *d = (dict *)dd;
    if (!h || !d) return NULL;
    if (h->dicts[0] == d) h->dicts[0] = NULL;
    dict_eng_call(h, ENG_DELETE_DICT, 1, d->engdict, 0, 0, 0);
    free(d);
    return NULL;
}

static int dict_file(eci *h, dict *d, int vol, const char *file, int slot)
{
    if (!h || !d || !file) return 6;
    if (vol < 0 || vol > 2) return 6;
    cpu *m = eng_cpu(h->c->e);
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t s = x86_strdup(m, file);
    uint32_t r = eng_call(h->c->e, slot, 3, d->engdict, (uint32_t)vol, s, 0);
    x86_dealloc(m, s);
    LeaveCriticalSection(&h->c->englock);
    return dict_code(r ? -0xf : 0);
}

int ECIFN eciLoadDict(void *hh, void *d, int vol, const char *file) { return dict_file((eci *)hh, (dict *)d, vol, file, ENG_LOAD_DICT); }
int ECIFN eciSaveDict(void *hh, void *d, int vol, const char *file) { return dict_file((eci *)hh, (dict *)d, vol, file, ENG_SAVE_DICT); }

int ECIFN eciUpdateDict(void *hh, void *dd, int vol, const char *key, const char *value)
{
    eci *h = (eci *)hh;
    dict *d = (dict *)dd;
    if (!h || !d || !key || !*key || vol < 0 || vol > 2) return 6;
    if (value) {
        while (*value == ' ') value++;
        if (!*value) return 6;
    }
    cpu *m = eng_cpu(h->c->e);
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t k = x86_strdup(m, key), v = value ? x86_strdup(m, value) : 0;
    uint32_t r = eng_call(h->c->e, ENG_UPDATE_DICT, 4, d->engdict, (uint32_t)vol, k, v);
    x86_dealloc(m, k);
    if (v) x86_dealloc(m, v);
    LeaveCriticalSection(&h->c->englock);
    return dict_code(r ? -0xf : 0);
}

static char *guest_copy(cpu *m, uint32_t p)
{
    if (!p) return NULL;
    size_t n = 0;
    while (rd8(m, p + (uint32_t)n)) n++;
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    for (size_t i = 0; i <= n; i++) s[i] = (char)rd8(m, p + (uint32_t)i);
    return s;
}

const char *ECIFN eciDictLookup(void *hh, void *dd, int vol, const char *key)
{
    eci *h = (eci *)hh;
    dict *d = (dict *)dd;
    if (!h || !d || !key || !*key || vol < 0 || vol > 2) return NULL;
    cpu *m = eng_cpu(h->c->e);
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t k = x86_strdup(m, key);
    uint32_t r = eng_call(h->c->e, ENG_DICT_LOOKUP, 3, d->engdict, (uint32_t)vol, k, 0);
    x86_dealloc(m, k);
    free(h->dict_value);
    h->dict_value = guest_copy(m, r);
    LeaveCriticalSection(&h->c->englock);
    return h->dict_value;
}

static int dict_find(eci *h, dict *d, int vol, const char **key, const char **value, int slot)
{
    if (key) *key = NULL;
    if (value) *value = NULL;
    if (!h || !d || !key || !value || vol < 0 || vol > 2) return 6;
    cpu *m = eng_cpu(h->c->e);
    worker_wait_idle(h->c);
    EnterCriticalSection(&h->c->englock);
    uint32_t pk = x86_alloc(m, 8);
    wr32(m, pk, 0);
    wr32(m, pk + 4, 0);
    eng_call(h->c->e, slot, 4, d->engdict, (uint32_t)vol, pk, pk + 4);
    uint32_t k = rd32(m, pk), v = rd32(m, pk + 4);
    x86_dealloc(m, pk);
    free(h->dict_key);
    free(h->dict_value);
    h->dict_key = guest_copy(m, k);
    h->dict_value = guest_copy(m, v);
    LeaveCriticalSection(&h->c->englock);
    if (!h->dict_key || !h->dict_value) return 4;
    *key = h->dict_key;
    *value = h->dict_value;
    return 0;
}

int ECIFN eciDictFindFirst(void *hh, void *d, int vol, const char **key, const char **value)
{
    return dict_find((eci *)hh, (dict *)d, vol, key, value, ENG_DICT_FIND_FIRST);
}

int ECIFN eciDictFindNext(void *hh, void *d, int vol, const char **key, const char **value)
{
    return dict_find((eci *)hh, (dict *)d, vol, key, value, ENG_DICT_FIND_NEXT);
}

/* the romanizer (Chinese/Japanese/Korean) variants: not for English */
int ECIFN eciUpdateDictEx(void *h, void *d, int vol, const void *k, const void *v, int pos) { (void)h; (void)d; (void)vol; (void)k; (void)v; (void)pos; return 6; }
int ECIFN eciDictFindFirstEx(void *h, void *d, int vol, void *k, void *v, void *pos) { (void)h; (void)d; (void)vol; (void)k; (void)v; (void)pos; return 4; }
int ECIFN eciDictFindNextEx(void *h, void *d, int vol, void *k, void *v, void *pos) { (void)h; (void)d; (void)vol; (void)k; (void)v; (void)pos; return 4; }
const void *ECIFN eciDictLookupEx(void *h, void *d, int vol, const void *k, void *pos) { (void)h; (void)d; (void)vol; (void)k; (void)pos; return NULL; }
