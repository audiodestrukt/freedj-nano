#!/bin/sh
# The whole deck firmware for RV32EC (the ESP-IDF image's GCC, as bench/), run under
# qemu-riscv32 on the emulated parts with a scripted session; count.py counts every
# instruction the firmware executes, by part, and per audio block.
set -e
cd "$(dirname "$0")/../.."
[ -f build/simtest/sd.img ] || { echo "build/simtest/sd.img missing: make simtest"; exit 1; }
mkdir -p build/rv
EXTRA="$*"                     # e.g. -DSD_STREAM=0
ARCH="-march=rv32ec_zicsr -mabi=ilp32e -Os -ffunction-sections -fdata-sections -ffreestanding -nostdlib -Ifw -Ilite -Isim/emu"
SRC="fw/app.c fw/ui.c fw/sd.c fw/tracks.c fw/font.c fw/pff/pff.c lite/lite.c lite/ulaw.c bench/rt.c sim/emu/sd_emu.c sim/emu/oled_emu.c sim/qemu/spi_bus.c sim/qemu/hal_qemu.c"
docker run --rm -v "$PWD":/w -w /w espressif/idf:release-v5.5 bash -c "
  set -e
  CC=riscv32-esp-elf-gcc
  objs=''
  for f in $SRC; do o=build/rv/\$(basename \$f .c).o; \$CC $ARCH $EXTRA -c \$f -o \$o; objs=\"\$objs \$o\"; done
  \$CC $ARCH \$objs -static -Wl,--gc-sections -Wl,-Map=build/rv/deck.map -e _start -o build/rv/deck.elf
  chown -R $(id -u):$(id -g) build
"
python3 sim/qemu/count.py
