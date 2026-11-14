#!/bin/sh
# The whole deck firmware for the PY32F030 (Cortex-M0+, ARMv6-M Thumb), the same sources and
# session as run.sh, built with the Arm GNU toolchain and run under qemu-arm -cpu cortex-m0
# (which refuses anything the M0+ lacks); count.py --chip py32 counts it. The PY32 build has an
# 8-block audio ring (it has the RAM); DECKFLAGS=-DHAL_AUDIO_BLOCKS=4 to compare like for like.
set -e
cd "$(dirname "$0")/../.."
[ -f build/simtest/sd.img ] || { echo "build/simtest/sd.img missing: make simtest"; exit 1; }
CC=${ARM_CC:-$(command -v arm-none-eabi-gcc || ls ~/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | head -1)}
[ -x "$CC" ] || { echo "no arm-none-eabi-gcc (set ARM_CC)"; exit 1; }
mkdir -p build/py32
ARCH="-mcpu=cortex-m0plus -mthumb -Os -ffunction-sections -fdata-sections -ffreestanding -nostdlib -Ifw -Ilite -Isim/emu"
SRC="fw/app.c fw/ui.c fw/sd.c fw/tracks.c fw/font.c fw/pff/pff.c lite/lite.c lite/ulaw.c sim/emu/sd_emu.c sim/emu/oled_emu.c sim/qemu/spi_bus.c sim/qemu/hal_qemu.c sim/qemu/rt_arm.c"
case "$*" in *HAL_AUDIO_BLOCKS*) RINGF= ;; *) RINGF=-DHAL_AUDIO_BLOCKS=8 ;; esac
objs=""
for f in $SRC; do o=build/py32/$(basename $f .c).o; $CC $ARCH $RINGF $* -c $f -o $o; objs="$objs $o"; done
$CC $ARCH $objs -static -Wl,--gc-sections -Wl,-Map=build/py32/deck.map -e _start -o build/py32/deck.elf -lgcc
python3 sim/qemu/count.py --chip py32
