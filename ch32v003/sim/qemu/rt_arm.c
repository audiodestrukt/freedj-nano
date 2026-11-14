// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// memcpy and memset for the freestanding PY32F030 build (libgcc brings the division helpers;
// the M0+ multiplies in hardware). Byte loops, as bench/rt.c, so the two chips count alike.
#include <stddef.h>
#include <stdint.h>
void* memcpy(void* d, const void* s, size_t n) { uint8_t* o = d; const uint8_t* i = s; while (n--) *o++ = *i++; return d; }
void* memset(void* d, int c, size_t n) { uint8_t* o = d; while (n--) *o++ = (uint8_t)c; return d; }
