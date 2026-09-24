/* x87math - 32-bit msvcrt.dll's log, exp and pow as ENU.SYN gets them: msvcrt's x87 code (its SSE2 code
 * is off unless a program calls _set_SSE2_enable), with the result left in st0 at extended precision.
 * Built on fx80, so as exact as fx80's f2xm1 and fyl2x (notes/port.md). */
#ifndef X87MATH_H
#define X87MATH_H

#include "fx80.h"

fx80 x87m_exp(double x);
fx80 x87m_log(double x);
fx80 x87m_pow(double x, double y, uint16_t caller_cw);

#endif
