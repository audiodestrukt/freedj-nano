#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts the deck firmware's instructions in a qemu-riscv32 run (sim/qemu/run.sh): every
translated block's size (in_asm) times its executions (exec), attributed to source files
through the linker map. The emulated parts and the HAL are left out. Also: flash and RAM
per part, the stack depth reached, and the worst main-loop steps against the audio ring."""
import bisect, collections, re, subprocess, sys

CPI = 1.5                       # cycles per instruction, until phase 1 measures it
CLK = 48e6
RATE = 22059
BLOCK = 32
RING = 4
PARTS = {"lite.o": "engine", "ulaw.o": "engine", "app.o": "app", "ui.o": "screen", "font.o": "screen",
         "sd.o": "SD driver", "tracks.o": "tracks", "pff.o": "FAT (Petit FatFs)", "rt.o": "mul/div runtime",
         "spi_bus.o": "SPI bus wait"}
EMU = {"hal_qemu.o", "sd_emu.o", "oled_emu.o"}

# ---- linker map: (start, size, object, section kind)
ranges, sizes = [], collections.defaultdict(lambda: [0, 0])     # part -> [flash, ram]
pending = None
for line in open("build/rv/deck.map"):
    m = re.match(r"\s*(\.(text|rodata|srodata|data|sdata|bss|sbss)[^\s]*)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o))?\s*$", line)
    if m and not m.group(3):
        pending = m.group(1); continue
    m2 = re.match(r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)\s*$", line) if pending else None
    if m and m.group(3):
        sec, addr, size, obj = m.group(1), int(m.group(3), 16), int(m.group(4), 16), m.group(5)
    elif m2:
        sec, addr, size, obj = pending, int(m2.group(1), 16), int(m2.group(2), 16), m2.group(3)
    else:
        pending = None; continue
    pending = None
    if not size or addr == 0: continue
    obj = obj.split("/")[-1]
    kind = sec.split(".")[1]
    if kind in ("text",): ranges.append((addr, size, obj))
    part = "engine" if sec.endswith(".g_deck") else PARTS.get(obj)    # the deck's state lives in app.c
    if part:
        if kind in ("text", "rodata", "srodata"): sizes[part][0] += size
        elif kind in ("data", "sdata"): sizes[part][0] += size; sizes[part][1] += size
        else: sizes[part][1] += size
ranges.sort()
starts = [r[0] for r in ranges]

def obj_of(pc):
    i = bisect.bisect_right(starts, pc) - 1
    if i >= 0 and pc < ranges[i][0] + ranges[i][1]: return ranges[i][2]
    return "?"

syms = {}
for line in subprocess.run(["nm", "build/rv/deck.elf"], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3: syms[f[2]] = int(f[0], 16)
MARK, EVENT = syms["sim_block_mark"], syms["sim_event_mark"]

# ---- trace: "IN:" listings give each translated block's size, "Trace" lines its executions
p = subprocess.Popen(["qemu-riscv32", "-cpu", "rv32e,c=true", "-d", "in_asm,exec,nochain", "-D", "/dev/stdout",
                      "build/rv/deck.elf"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1 << 20)
tbsize, part_of = {}, {}
total = collections.Counter()
steps, n, started, events = [], 0, False, []
listing, start, cnt, said = False, None, 0, []
for line in p.stdout:
    if line.startswith("Trace"):
        pc = int(line.split("/")[1], 16)
        if pc == MARK:
            if started: steps.append(n)
            started, n = True, 0
        elif pc == EVENT:
            events.append(len(steps))
        else:
            part = part_of.get(pc, 0)
            if part == 0:
                obj = obj_of(pc)
                part = part_of[pc] = None if obj in EMU else PARTS.get(obj, "other")
            if part:
                k = tbsize[pc]; total[part] += k; n += k
    elif line.startswith("IN:"):
        listing, start, cnt = True, None, 0
    elif listing and line.startswith("0x"):
        if start is None: start = int(line[2:line.index(":")], 16)
        cnt += 1
    elif listing and not line.strip():
        tbsize[start] = cnt; listing = False
p.wait()
said = p.stderr.read().strip().splitlines()
if p.returncode: sys.exit(f"qemu exited {p.returncode}: " + "\n".join(said))

# ---- report
secs = len(steps) * BLOCK / RATE
budget = CLK / CPI                                           # instructions per second
fw = sum(total.values())
print("== the deck firmware in qemu-riscv32 (rv32e), scripted session (sim/qemu/hal_qemu.c)")
for l in said: print("   " + l)
print(f"== size by part (flash = code + constants + initial data; RAM = data + bss)")
ft = rt = 0
for part in ["engine", "app", "screen", "SD driver", "tracks", "FAT (Petit FatFs)", "mul/div runtime"]:
    f, r = sizes[part]; ft += f; rt += r
    print(f"   {part:20s} {f:6d} B flash {r:6d} B RAM")
print(f"   {'total':20s} {ft:6d} B flash {rt:6d} B RAM")
stack = int(next((re.search(r"stack: (\d+)", l).group(1) for l in said if l.startswith("stack")), "0"))
dma = RING * BLOCK * 2
print(f"   + the PWM DMA ring {dma} B, + stack {stack} B (deepest seen) = {rt + dma + stack} of 2048 B RAM; "
      f"+ chip drivers and startup (~1.5 KB flash, a few bytes RAM) = ~{(ft + 1536) / 1024:.1f} of 16 KB flash")
print(f"== instructions over {secs:.1f} s of audio ({len(steps)} main-loop steps, one 32-sample block each)")
for part, k in total.most_common():
    print(f"   {part:20s} {k / secs / 1e3:8.1f} k/s  {k / fw * 100:5.1f} %   {k * CPI / secs / CLK * 100:5.1f} % of the CPU")
print(f"   {'total':20s} {fw / secs / 1e3:8.1f} k/s           {fw * CPI / secs / CLK * 100:5.1f} % of the CPU at {CPI} cycles/instruction")
limit = (RING - 1) * BLOCK / RATE * budget
print(f"== slowest main-loop steps (the ring holds {RING - 1} blocks ahead: {limit / 1e3:.0f} k instructions before audio drops)")
order = sorted(range(len(steps)), key=lambda i: -steps[i])[:6]
for i in order:
    print(f"   {steps[i] / 1e3:7.1f} k at {i * BLOCK / RATE:6.3f} s  ({steps[i] / limit * 100:4.0f} % of the slack)")
worst = steps[order[0]] if order else 0
print(f"== verdict: {fw * CPI / secs / CLK * 100:.0f} % of the CPU on average, worst step {worst / limit * 100:.0f} % of the ring's slack"
      + (" -> AUDIO WOULD DROP" if worst > limit else ""))
