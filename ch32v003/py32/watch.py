#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""watch.py: the AS5600 jog as the running firmware sees it, over SWD (any CMSIS-DAP probe).

    python3 py32/watch.py [samples]      (from ch32v003/, after make py32)

Prints whether the AS5600 answers, its STATUS (magnet detected / too weak / too strong) and
the raw angle. Finds g_jog in build/py32fw/deck.elf.
"""
import os, subprocess, sys, time
from pyocd.core.helpers import ConnectHelper

here = os.path.dirname(os.path.abspath(__file__))
elf = os.path.join(here, "..", "build", "py32fw", "deck.elf")
nm = next((p for p in ("arm-none-eabi-nm",
           os.path.expanduser("~/.local/opt/arm-gnu-toolchain-14.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-nm"))
           if subprocess.call(["sh", "-c", f"command -v {p} >/dev/null"]) == 0), "arm-none-eabi-nm")
addr = next(int(l.split()[0], 16) for l in subprocess.check_output([nm, elf], text=True).splitlines()
            if l.endswith(" g_jog"))
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
with ConnectHelper.session_with_chosen_probe(target_override="py32f030x8", connect_mode="attach") as s:
    t = s.target
    for _ in range(n):
        b = t.read_memory_block8(addr, 4)
        raw, st, ok = b[0] | b[1] << 8, b[2], b[3]
        field = "ok" if st & 0x20 else "none"
        if st & 0x10: field += ", too weak"
        if st & 0x08: field += ", too strong"
        print(f"answers={'yes' if ok else 'no '} magnet={field:16s} angle={raw:4d} ({raw * 360 / 4096:5.1f} deg)")
        time.sleep(0.3)
