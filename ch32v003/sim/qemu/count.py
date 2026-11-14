#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts the deck firmware's instructions in a qemu run (sim/qemu/run.sh: CH32V003, RV32EC
under qemu-riscv32; run_py32.sh: PY32F030, Cortex-M0+ Thumb under qemu-arm): every
translated block's size (in_asm) times its executions (exec), attributed to source files
through the linker map. The emulated parts and the HAL are left out. Also: flash and RAM
per part, the stack depth reached, and the worst main-loop steps against the audio ring.
python3 count.py [--chip ch32|py32]"""
import bisect, collections, glob, os, re, shutil, subprocess, sys

CHIP = sys.argv[sys.argv.index("--chip") + 1] if "--chip" in sys.argv else "ch32"
CPI = 1.5                       # the flat assumption so far; the cycle model below replaces it
# Cycle model for the QingKe V2A at 48 MHz. Nothing official gives a CPI; what's known: flash
# has one wait state at 48 MHz (2 cycles per 32-bit fetch, with a prefetch buffer), most
# instructions take 1 cycle, a taken branch refills the 2-stage pipeline. So per translated
# block: max(instructions, 2 x 32-bit words fetched) + 1 per load or store + 2 if the block
# ends in a taken branch. A polled SPI byte: 20 cycles (16 on the wire + the register work).
#
# Cycle model for the PY32F030's Cortex-M0+ at 48 MHz, from Arm's M0+ timings (DDI 0484):
# ALU 1, MULS 1 (the fast multiplier: assumed, Puya doesn't say), load / store 2, PUSH / POP /
# LDM / STM 1 + N (+ 2 for POP {pc}), B 2, BL 3, BX / BLX 2, a conditional branch 1, or 2
# taken. Flash has one wait state above 24 MHz (2 cycles per 32-bit fetch) and the M0+ has one
# bus, shared by fetch and data: per block max(those cycles, 2 x words fetched + data
# accesses), + 1 wait state for a literal-pool load and for the fetch after a taken branch.
# Loads from tables in flash (the mu-law table, the font) also wait one, but the trace can't
# tell them apart: ~2 a sample, ~0.1 % of the CPU, left out.
if CHIP == "py32":
    NAME, QEMU, ELF, MAP = "PY32F030 (Cortex-M0+, Thumb) in qemu-arm -cpu cortex-m0", \
        ["qemu-arm", "-cpu", "cortex-m0"], "build/py32/deck.elf", "build/py32/deck.map"
    FLASH, RAM, DRV = 65536, 8192, 2048          # PY32F030x8; drivers + vector table + startup, guessed
    TAKEN, MEM, SPI_BYTE = 1, 0, 20
else:
    NAME, QEMU, ELF, MAP = "CH32V003 (rv32e) in qemu-riscv32", ["qemu-riscv32", "-cpu", "rv32e,c=true"], \
        "build/rv/deck.elf", "build/rv/deck.map"
    FLASH, RAM, DRV = 16384, 2048, 1536
    TAKEN, MEM, SPI_BYTE = 2, 1, 20
CLK = 48e6
RATE = 22059
BLOCK = 32
RING = 4
PARTS = {"lite.o": "engine", "ulaw.o": "engine", "app.o": "app", "ui.o": "screen", "font.o": "screen",
         "sd.o": "SD driver", "tracks.o": "tracks", "pff.o": "FAT (Petit FatFs)", "rt.o": "mul/div runtime",
         "rt_arm.o": "mul/div runtime", "spi_bus.o": "SPI bus wait"}
EMU = {"hal_qemu.o", "sd_emu.o", "oled_emu.o"}
ARM_OBJDUMP = shutil.which("arm-none-eabi-objdump") or next(iter(sorted(glob.glob(os.path.expanduser(
    "~/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-objdump")))), "arm-none-eabi-objdump")
def part_name(obj):
    if obj.startswith("libgcc.a("): return "mul/div runtime"
    return PARTS.get(obj)

# ---- linker map: (start, size, object, section kind)
ranges, sizes = [], collections.defaultdict(lambda: [0, 0])     # part -> [flash, ram]
pending = None
for line in open(MAP):
    m = re.match(r"\s*(\.(text|rodata|srodata|data|sdata|bss|sbss)[^\s]*)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o\)?))?\s*$", line)
    if m and not m.group(3):
        pending = m.group(1); continue
    m2 = re.match(r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o\)?)\s*$", line) if pending else None
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
    part = "engine" if sec.endswith(".g_deck") else part_name(obj)    # the deck's state lives in app.c
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
for line in subprocess.run(["nm", ELF], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3: syms[f[2]] = int(f[0], 16) & ~1          # Thumb functions carry bit 0
MARK, EVENT = syms["sim_block_mark"], syms["sim_event_mark"]
for line in subprocess.run(["nm", "-S", ELF], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 4 and f[3] == "s_ring": RING = int(f[1], 16) // (2 * BLOCK)   # HAL_AUDIO_BLOCKS as built

# ---- trace: "IN:" listings give each translated block's size, "Trace" lines its executions
# qemu has no Thumb disassembler (it prints the bytes): decode from objdump instead
COND = re.compile(r"b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(\.n|\.w)?$")
def m0_cost(mn, ops):
    """(cycles, data accesses, flash waits, conditional branch) for one Thumb instruction."""
    mn = mn.split(".")[0]
    if mn in ("push", "pop", "ldmia", "stmia", "ldm", "stm"):
        n = len(ops.split("}")[0].split(","))                # objdump lists every register
        return 1 + n + (2 if mn == "pop" and "pc" in ops else 0), n, 0, False
    if mn.startswith(("ldr", "str")):
        return 2, 1, 1 if "[pc" in ops else 0, False
    if COND.match(mn): return 1, 0, 0, True
    if mn == "b": return 2, 0, 0, False
    if mn == "bl": return 3, 0, 0, False
    if mn in ("bx", "blx"): return 2, 0, 0, False
    if mn in ("mov", "add") and ops.split(",")[0].strip() == "pc": return 2, 0, 0, False
    return 1, 0, 0, False
insn = {}
if CHIP == "py32":
    od = subprocess.run([ARM_OBJDUMP, "-d", ELF], capture_output=True, text=True).stdout
    for line in od.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\t([0-9a-f]{4}(?: [0-9a-f]{4})?)\s*\t(\S+)\s*([^;]*)", line)
        if m: insn[int(m.group(1), 16)] = (len(m.group(2).replace(" ", "")) // 2, m.group(3), m.group(4))
def arm_block(start, nbytes):
    """Instructions, modelled cycles, end address and 'ends in a conditional branch'."""
    a, cnt, base, data, waits, cond, words = start, 0, 0, 0, 0, False, set()
    while a < start + nbytes:
        size, mn, ops = insn.get(a, (2, "?", ""))
        c, d, w, cond = m0_cost(mn, ops)
        cnt += 1; base += c; data += d; waits += w
        words.add(a >> 2); words.add((a + size - 1) >> 2)
        a += size
    return cnt, max(base, 2 * len(words) + data) + waits, a, cond

p = subprocess.Popen(QEMU + ["-d", "in_asm,exec,nochain", "-D", "/dev/stdout", ELF], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1 << 20)
tbsize, part_of, tbcyc, tbend, tbcond = {}, {}, {}, {}, {}
prev_cond = False
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
            t = TAKEN + (1 if prev_cond else 0)         # M0+: a conditional branch taken costs one more
            cyc[prev_part] += t; cstep += t; cparts[prev_part] += t
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
                part = part_of[pc] = None if obj in EMU else (part_name(obj) or "other")
            if part:
                k = tbsize[pc]; total[part] += k; n += k
                c = SPI_BYTE if pc == SPI_FN else tbcyc[pc]
                cyc[part] += c; cstep += c; cparts[part] += c
                prev_end, prev_part, prev_cond = tbend[pc], (None if pc == SPI_FN else part), tbcond.get(pc, False)
    elif line.startswith("IN:"):
        listing, start, cnt, words, mem = True, None, 0, set(), 0
    elif listing and CHIP == "py32" and line.startswith("0x"):
        start = int(line.split(":")[0], 16)
    elif listing and CHIP == "py32" and line.startswith("OBJD-T:"):
        tbsize[start], tbcyc[start], tbend[start], tbcond[start] = arm_block(start, len(line.split()[1]) // 2)
        listing = False
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
print(f"== the deck firmware for the {NAME}, scripted session (sim/qemu/hal_qemu.c)")
for l in said: print("   " + l)
print(f"== size by part (flash = code + constants + initial data; RAM = data + bss)")
ft = rt = 0
for part in ["engine", "app", "screen", "SD driver", "tracks", "FAT (Petit FatFs)", "mul/div runtime"]:
    f, r = sizes[part]; ft += f; rt += r
    print(f"   {part:20s} {f:6d} B flash {r:6d} B RAM")
print(f"   {'total':20s} {ft:6d} B flash {rt:6d} B RAM")
stack = int(next((re.search(r"stack: (\d+)", l).group(1) for l in said if l.startswith("stack")), "0"))
dma = RING * BLOCK * 2
print(f"   + the PWM DMA ring {dma} B ({RING} blocks), + stack {stack} B (deepest seen) = {rt + dma + stack} of {RAM} B RAM; "
      f"+ chip drivers and startup (~{DRV / 1024:.1f} KB flash, a few bytes RAM) = ~{(ft + DRV) / 1024:.1f} of {FLASH // 1024} KB flash")
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
