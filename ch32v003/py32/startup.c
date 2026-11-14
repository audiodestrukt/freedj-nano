// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// PY32F030 startup: the vector table (the core's 16; no peripheral interrupts used yet), .data
// copied from flash, .bss zeroed, then main().
#include <stdint.h>

extern uint32_t _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int main(void);
void SysTick_Handler(void);

void Reset_Handler(void)
{
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) *d++ = *s++;
    for (uint32_t* d = &_sbss; d < &_ebss;) *d++ = 0;
    main();
    for (;;) {}
}
static void Default_Handler(void) { for (;;) {} }

__attribute__((section(".vectors"), used)) void (*const g_vectors[16])(void) = {
    (void (*)(void))&_estack, Reset_Handler,
    Default_Handler, Default_Handler,          // NMI, HardFault
    0, 0, 0, 0, 0, 0, 0,
    Default_Handler, 0, 0,                     // SVC
    Default_Handler, SysTick_Handler,          // PendSV, SysTick
};

// The compiler may call these for struct copies and array clears.
void* memcpy(void* d, const void* s, unsigned n) { uint8_t* o = d; const uint8_t* i = s; while (n--) *o++ = *i++; return d; }
void* memset(void* d, int c, unsigned n) { uint8_t* o = d; while (n--) *o++ = (uint8_t)c; return d; }
