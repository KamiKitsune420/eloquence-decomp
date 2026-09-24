/* engine - one instance of the recompiled ENU.SYN: its own machine (memory, stack, heap), started the way
 * ECI.DLL starts it, and its interface's methods callable from the host.
 *
 * The engine calls back into "ECI" through function pointers it was given. Those are the addresses
 * ENG_CB_BASE + n; a call to one lands in the host's handler with the machine as the guest's `call` left
 * it (return address at [esp], arguments above), and the handler says how many argument bytes to remove
 * (0 for cdecl, 4 * nargs for stdcall).
 *
 * Not thread-safe: one thread at a time per engine (the ECI layer's worker).
 */
#ifndef ENGINE_H
#define ENGINE_H

#include "x86rt.h"

/* the interface's 39 stdcall methods (vtable 0x1014478c; names from openevv's EngineWrapper) */
enum {
    ENG_QUERYINTERFACE, ENG_ADDREF, ENG_RELEASE, ENG_START, ENG_END, ENG_PROCESS_SENTENCES,
    ENG_PROCESS_REMAINING, ENG_GET_LAST_ERROR, ENG_RESTART, ENG_READ_PHONEMES, ENG_READ_ERROR_MESSAGE,
    ENG_FLUSH, ENG_CLEAR_INPUT, ENG_SET_ABORT, ENG_OUTPUT_PLAYING, ENG_PAUSE, ENG_SET_SYNTH_TO_NAMED_FILE,
    ENG_SET_SYNTH_TO_CALLBACK, ENG_SET_DURATION_CALLBACK, ENG_REGISTER_WORD_CALLBACK,
    ENG_REGISTER_INDEX_CALLBACK, ENG_REGISTER_PHONEME_CALLBACK, ENG_REGISTER_ANNO_CALLBACK,
    ENG_INSERT_SYNTHESIS_INDEX, ENG_INSERT_DELAYED_SYNTHESIS_INDEX, ENG_WANT_PHONEME_INDICES, ENG_CLOSE,
    ENG_NEW_DICT, ENG_GET_DICT, ENG_SET_DICT, ENG_DELETE_DICT, ENG_LOAD_DICT, ENG_SAVE_DICT, ENG_UPDATE_DICT,
    ENG_DICT_FIND_FIRST, ENG_DICT_FIND_NEXT, ENG_DICT_LOOKUP, ENG_REGISTER_WORD_INDEX_CALLBACK,
    ENG_REGISTER_USER_INDEX_CALLBACK, ENG_NSLOTS
};

#define ENG_CB_BASE 0xe0000000u      /* guest addresses standing for host callbacks: ENG_CB_BASE + n, n < 16 */

typedef struct eng eng;

/* a callback from the engine: n = which (address - ENG_CB_BASE); returns the bytes of arguments to pop */
typedef uint32_t (*eng_handler)(eng *e, cpu *c, unsigned n);

/* a new engine with the data compiled in (or from `syn_path` when not NULL): DllMain, getObject(1) */
eng *eng_new(const char *syn_path, eng_handler handler, void *user);
void eng_free(eng *e);

/* an extension: run the synthesizer at `hz` (0: the rate the engine chose, 8000 or 11025). Takes effect
 * at the next synthesizer init, which a "`esr" annotation causes. */
void eng_set_rate(eng *e, float hz);
/* an extension: see (and change) every synthesizer frame (64 floats, 5 ms) before it is used */
void eng_set_frame_hook(eng *e, void (*hook)(void *user, float frame[64]), void *user);

cpu *eng_cpu(eng *e);
void *eng_user(eng *e);

/* a method with up to 4 dword arguments; returns eax */
uint32_t eng_call(eng *e, int slot, int nargs, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
/* a method whose one argument is a string (copied into the machine for the call) */
uint32_t eng_call_text(eng *e, int slot, const char *s);

/* the n-th dword argument of a callback being handled (0 = first) */
static inline uint32_t eng_arg(cpu *c, int n) { return rd32(c, c->esp + 4 + 4 * (uint32_t)n); }

#endif
