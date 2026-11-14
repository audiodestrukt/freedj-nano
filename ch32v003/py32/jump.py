#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""jump.py: start the firmware past Puya's boot ROM (needed while BOOT0 / PF4 floats).
python3 py32/jump.py"""
# Start the flash firmware from the debugger, past the boot ROM: map flash at 0, point VTOR,
# SP and PC at its vector table, run.
from pyocd.core.helpers import ConnectHelper
with ConnectHelper.session_with_chosen_probe(target_override="py32f030x8") as s:
    t = s.target
    t.reset_and_halt()
    t.write32(0x40021040, t.read32(0x40021040) | 1)     # SYSCFG clock (APBENR2 bit 0)
    t.write32(0x40010000, 0)                            # MEM_MODE: main flash at 0
    t.write32(0xE000ED08, 0x08000000)                   # VTOR
    sp, pc = t.read32(0x08000000), t.read32(0x08000004)
    t.write_core_register("sp", sp); t.write_core_register("pc", pc & ~1)
    t.write_core_register("xpsr", 1 << 24)
    t.resume()
    print(f"started at {pc:#x}, sp {sp:#x}")
