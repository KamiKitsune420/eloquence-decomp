/* framer - Eloquence 6.1's frame builder (ENU.SYN FUN_101306b0 and the functions it calls, the
 * synthesizer excluded), hand-ported to C99 and bit-exact with the original.
 *
 * The engine's rules put each synthesis parameter (F0, AV, formants, bandwidths, ...) on a track: a queue of
 * breakpoints (time in ms, integer value). The frame builder walks time in steps of one frame (5 ms),
 * interpolates every track linearly between the breakpoints around the current time, writes the values into
 * the frame slots the tracks are mapped to and hands each frame to the synthesizer (FUN_1013caf0).
 *
 * Unlike the synthesizer (klatt.c), everything the frame builder touches lives in the engine's memory and it
 * calls engine code (the synthesizer, the C runtime's allocator), so it works directly on the machine the
 * recompiled engine runs on (x86rt): guest addresses in, guest memory read and written in place.
 * framer_guest.c puts these functions where the recompiled ones were.
 */
#ifndef FRAMER_H
#define FRAMER_H

#include <stdint.h>

#include "x86rt.h"

#define FRAMER_SLOTS 62            /* floats in a frame built here (the synthesizer reads 64) */

/* The frame builder, FUN_101306b0. `args` is the guest address of its ten cdecl arguments (the original
 * writes two of them back: see framer.c), `frame` the guest address of the 62-float buffer the frames are
 * built in; c->esp must be below it. Returns what the original returns in eax. */
uint32_t framer_build(cpu *c, uint32_t args, uint32_t frame);

/* The parameter tracks' queues. `eng` is the engine instance (the frame builder's first argument), `q` the
 * guest address of one track's queue. The ones that call others (next_point, pop, shrink) run as the
 * original at its entry (c->esp holds the return address) and make their calls on the machine. */
int32_t framer_track_count(cpu *c, uint32_t eng);                    /* FUN_1012f980 */
uint32_t framer_track_end(cpu *c, uint32_t eng, int16_t track);      /* FUN_1012f960 */
int framer_next_point(cpu *c, uint32_t eng, int16_t track, uint32_t time_p, uint32_t value_p, uint32_t *time);
                                                                     /* FUN_1012f8a0: 1, or 0 if none */
int framer_queue_empty(cpu *c, uint32_t q);                          /* FUN_10130bd0 */
uint32_t framer_queue_pop(cpu *c, uint32_t q, uint32_t out);         /* FUN_10130c60: eax, al 0 if empty */
uint32_t framer_queue_shrink(cpu *c, uint32_t q);                    /* FUN_10130d40: eax, al 0 if realloc failed */
int framer_stop_requested(cpu *c, uint32_t eng);                     /* FUN_10142350 */

#endif
