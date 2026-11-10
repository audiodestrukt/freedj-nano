// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// The bits of libgcc/libc that a freestanding RV32EC build needs (the toolchain has no
// ilp32e multilib). Same as ch32fun ships.
#include <stddef.h>
#include <stdint.h>

uint32_t __mulsi3(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    while (b) { if (b & 1) r += a; a <<= 1; b >>= 1; }
    return r;
}
void* memcpy(void* d, const void* s, size_t n) { uint8_t* o = d; const uint8_t* i = s; while (n--) *o++ = *i++; return d; }
void* memset(void* d, int c, size_t n) { uint8_t* o = d; while (n--) *o++ = (uint8_t)c; return d; }
// Used by the bench's track generator only (the engine never divides).
static uint32_t udiv(uint32_t n, uint32_t d, uint32_t* rem)
{
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) { r = (r << 1) | ((n >> i) & 1); if (r >= d) { r -= d; q |= 1u << i; } }
    if (rem) *rem = r;
    return q;
}
int32_t __divsi3(int32_t a, int32_t b)
{
    const int neg = (a < 0) != (b < 0);
    const uint32_t q = udiv(a < 0 ? -(uint32_t)a : (uint32_t)a, b < 0 ? -(uint32_t)b : (uint32_t)b, 0);
    return neg ? -(int32_t)q : (int32_t)q;
}
// The deck firmware's few divisions (the screen's numbers, the tempo maths, FAT mount).
uint32_t __udivsi3(uint32_t a, uint32_t b) { return udiv(a, b, 0); }
uint32_t __umodsi3(uint32_t a, uint32_t b) { uint32_t r; udiv(a, b, &r); return r; }
int32_t __modsi3(int32_t a, int32_t b)
{
    uint32_t r;
    udiv(a < 0 ? -(uint32_t)a : (uint32_t)a, b < 0 ? -(uint32_t)b : (uint32_t)b, &r);
    return a < 0 ? -(int32_t)r : (int32_t)r;
}
