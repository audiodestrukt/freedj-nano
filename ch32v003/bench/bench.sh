#!/bin/sh
# Cross-compile nano-lite for the CH32V003 (RV32EC) in the ESP-IDF image's GCC, report size
# and RAM, then count instructions per sample in qemu-riscv32 (cpu rv32e).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/rv
ARCH="-march=rv32ec_zicsr -mabi=ilp32e -Os -ffunction-sections -fdata-sections -ffreestanding -nostdlib -Ilite"
docker run --rm -v "$PWD":/w -w /w espressif/idf:release-v5.5 bash -c "
  set -e
  CC=riscv32-esp-elf-gcc
  \$CC $ARCH -c lite/lite.c -o build/rv/lite.o
  \$CC $ARCH -c lite/ulaw.c -o build/rv/ulaw.o
  for m in 0 1 2 3; do
    \$CC $ARCH -DMODE=\$m bench/bench.c bench/rt.c lite/lite.c lite/ulaw.c -static -Wl,--gc-sections -e _start -o build/rv/bench\$m.elf
  done
  echo '== engine size (text data bss, bytes)'; riscv32-esp-elf-size build/rv/lite.o build/rv/ulaw.o
  echo '== multiply instructions in the engine:' \$(riscv32-esp-elf-objdump -d build/rv/lite.o | grep -c -E '\smul|\sdiv' || true)
  riscv32-esp-elf-nm -S build/rv/bench3.elf > build/rv/nm.txt
  chown -R $(id -u):$(id -g) build
"
python3 bench/count.py
