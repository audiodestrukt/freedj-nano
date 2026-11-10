// SPDX-FileCopyrightText: 2026 Dan Newcome
// SPDX-License-Identifier: GPL-3.0-or-later
// What a polled SPI byte costs the CPU on the chip: 8 clocks at 24 MHz = 16 CPU cycles of
// waiting on the shift register, plus writing DATAR and polling the flags. Modelled as 11
// counted instructions (~16 cycles at 1.5 cycles each), so count.py charges it per byte.
void spi_bus_byte(void)
{
    __asm__ volatile("li t1, 5\n1: addi t1, t1, -1\n bnez t1, 1b\n" ::: "t1");
}
