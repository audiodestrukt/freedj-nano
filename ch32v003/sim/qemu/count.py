#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts the deck firmware's instructions in a qemu-riscv32 run (sim/qemu/run.sh): every
translated block's size (in_asm) times its executions (exec), attributed to source files
through the linker map. The emulated parts and the HAL are left out. Also: flash and RAM
per part, the stack depth reached, and the worst main-loop steps against the audio ring."""
import bisect, collections, re, subprocess, sys

CPI = 1.5                       # the flat assumption so far; the cycle model below replaces it
# Cycle model for the QingKe V2A at 48 MHz. Nothing official gives a CPI; what's known: flash
# has one wait state at 48 MHz (2 cycles per 32-bit fetch, with a prefetch buffer), most
# instructions take 1 cycle, a taken branch refills the 2-stage pipeline. So per translated
# block: max(instructions, 2 x 32-bit words fetched) + 1 per load or store + 2 if the block
# ends in a taken branch. A polled SPI byte: 20 cycles (16 on the wire + the register work).
TAKEN, MEM, SPI_BYTE = 2, 1, 20
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
tbsize, part_of, tbcyc, tbend = {}, {}, {}, {}
total = collections.Counter()
cyc = collections.Counter()
SPI_FN = syms.get("spi_bus_byte")
stepc, cstep = [], 0
stepparts, cparts = [], collections.Counter()
prev_end, prev_part = None, None
steps, n, started, events = [], 0, False, []
listing, start, cnt, said = False, None, 0, []
words, mem, last = set(), 0, 0
for line in p.stdout:
    if line.startswith("Trace"):
        pc = int(line.split("/")[1], 16)
        if prev_part and pc != prev_end:                # the last block's branch was taken
            cyc[prev_part] += TAKEN; cstep += TAKEN; cparts[prev_part] += TAKEN
        prev_part = None
        if pc == MARK:
            if started: steps.append(n); stepc.append(cstep); stepparts.append(cparts)
            started, n, cstep, cparts = True, 0, 0, collections.Counter()
        elif pc == EVENT:
            events.append(len(steps))
        else:
            part = part_of.get(pc, 0)
            if part == 0:
                obj = obj_of(pc)
                part = part_of[pc] = None if obj in EMU else PARTS.get(obj, "other")
            if part:
                k = tbsize[pc]; total[part] += k; n += k
                c = SPI_BYTE if pc == SPI_FN else tbcyc[pc]
                cyc[part] += c; cstep += c; cparts[part] += c
                prev_end, prev_part = tbend[pc], (None if pc == SPI_FN else part)
    elif line.startswith("IN:"):
        listing, start, cnt, words, mem = True, None, 0, set(), 0
    elif listing and line.startswith("0x"):
        f = line.split()
        a = int(f[0][2:-1], 16)
        size = len(f[1]) // 2
        if start is None: start = a
        cnt += 1
        words.add(a >> 2); words.add((a + size - 1) >> 2)
        if f[2] in ("lw", "lh", "lhu", "lb", "lbu", "sw", "sh", "sb"): mem += 1
        last = a + size
    elif listing and not line.strip():
        tbsize[start] = cnt; listing = False
        tbcyc[start] = max(cnt, 2 * len(words)) + MEM * mem
        tbend[start] = last
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
ct = sum(cyc.values())
print(f"== cycles, fetch/branch model: {ct / fw:.2f} cycles per instruction on average")
for part, k in cyc.most_common():
    print(f"   {part:20s} {k / total[part] if total[part] else 0:5.2f} CPI  {k / secs / CLK * 100:5.1f} % of the CPU")
print(f"   {'total':20s} {ct / fw:5.2f} CPI  {ct / secs / CLK * 100:5.1f} % of the CPU")
limit = (RING - 1) * BLOCK / RATE * budget
climit = (RING - 1) * BLOCK / RATE * CLK
print(f"== slowest main-loop steps (the ring holds {RING - 1} blocks ahead: {limit / 1e3:.0f} k instructions before audio drops)")
order = sorted(range(len(steps)), key=lambda i: -steps[i])[:6]
for i in order:
    print(f"   {steps[i] / 1e3:7.1f} k at {i * BLOCK / RATE:6.3f} s  ({steps[i] / limit * 100:4.0f} % of the slack)")
worst = steps[order[0]] if order else 0
first = events[0] if events else 0                          # before it: the mount, nothing playing
live = range(first, len(stepc))
cw = max((stepc[i] for i in live), default=0)
print(f"   cycle model, from the first event on (the boot mount, silent, is left out):")
for i in sorted(live, key=lambda i: -stepc[i])[:5]:
    print(f"   {stepc[i] / 1e3:7.1f} k cycles at {i * BLOCK / RATE:6.3f} s  ({stepc[i] / climit * 100:4.0f} % of the ring's {climit / 1e3:.0f} k): "
          + ", ".join(f"{k} {v / 1e3:.0f}k" for k, v in stepparts[i].most_common(4)))
print(f"== verdict: {ct / secs / CLK * 100:.0f} % of the CPU on average (cycle model; {fw * CPI / secs / CLK * 100:.0f} % at a flat {CPI}), "
      f"worst step {cw / climit * 100:.0f} % of the ring's slack" + (" -> AUDIO WOULD DROP" if cw > climit else ""))
