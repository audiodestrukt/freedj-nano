#!/usr/bin/env python3
# Instruction counts from a qemu exec trace (one instruction per translation block).
import subprocess, sys
syms = {}
for l in open("build/rv/nm.txt"):
    f = l.split()
    syms[f[-1]] = f
mark = int(syms["bench_mark"][0], 16)
print(f"== lite_deck: {int(syms['deck'][1], 16)} bytes of RAM")
def trace(mode):
    p = subprocess.Popen(["qemu-riscv32", "-cpu", "rv32e,c=true", "-one-insn-per-tb", "-d", "nochain,exec",
                          "-D", "/dev/stdout", f"build/rv/bench{mode}.elf"], stdout=subprocess.PIPE, text=True)
    blocks, n, started = [], 0, False
    for line in p.stdout:
        if not line.startswith("Trace"): continue
        pc = int(line.split("/")[1].split()[0], 16)
        n += 1
        if pc == mark:
            if started: blocks.append(n)
            started, n = True, 0
    p.wait()
    return blocks, p.returncode
names = {1: "varispeed 1.00", 2: "varispeed 1.08", 3: "keylock 1.08"}
budget = 48e6 / 22050
print(f"== instructions per output sample (64-sample blocks; budget {budget:.0f} cycles per sample at 48 MHz)")
for m in (1, 2, 3):
    b, misses = trace(m)
    per = sum(b) / (len(b) * 64)
    worst = max(b) / 64
    print(f"{names[m]:>15}: {per:6.1f} per sample, worst block {worst:6.1f} per sample; "
          f"at 1.5 cycles/instruction {100 * per * 1.5 / budget:4.1f} % of the CPU, worst block "
          f"{100 * worst * 1.5 / budget:4.1f} % (ring misses {misses})")
