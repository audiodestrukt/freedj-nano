# nano-lite for the CH32V003.
#   make test    host tests (lite against the C6 engine), listening WAVs in build/out/
#   make bench   cross-compile for RV32EC in Docker, count instructions in qemu: size, RAM, cost
#   make pwm     the PWM audio output modelled: measurements, and WAVs of IN= (default: the keylock test output)
#   make sim     the whole deck on the computer (fw/ on emulated parts): build/sim, card image from MUSIC=
#   make simtest scripted sessions through the sim, checked
#   make deckbench  the whole firmware for RV32EC in qemu: flash, RAM, stack, CPU by part, worst main-loop step
#   CHIP=py32       sim and deckbench for the PY32F030 instead (Cortex-M0+, 8 KB RAM: an 8-block audio ring)
#   make py32       the firmware for the PY32F030 board (py32/), build/py32fw/deck.hex
#   make flash-py32 build it and flash it with pyOCD (any CMSIS-DAP probe, e.g. debugprobe on a Pico)
NANO := ../components/nano
CFLAGS := -O2 -Wall -Wextra -Ilite

test: build/test_lite
	cd build && ./test_lite

build/test_lite: lite/lite.c lite/ulaw.c lite/lite.h tests/test_lite.cpp $(NANO)/src/stretch.cpp
	@mkdir -p build/out
	gcc $(CFLAGS) -c lite/lite.c -o build/lite.o
	gcc $(CFLAGS) -c lite/ulaw.c -o build/ulaw.o
	g++ $(CFLAGS) -std=c++17 -I$(NANO)/include tests/test_lite.cpp $(NANO)/src/stretch.cpp build/lite.o build/ulaw.o -o $@

bench:
	sh bench/bench.sh

IN ?= build/out/lite_keylock_108.wav
RAW ?= $(if $(filter build/out/lite_%,$(IN)),--raw,)
pwm:
	python3 sim/pwm_sim.py --selftest
	python3 sim/pwm_sim.py --in $(IN) $(RAW)

FW := fw/app.c fw/ui.c fw/sd.c fw/tracks.c fw/font.c fw/pff/pff.c lite/lite.c lite/ulaw.c
EMU := sim/emu/sd_emu.c sim/emu/oled_emu.c
MUSIC ?= $(wildcard ../../freedj-esp/native/media/beat.wav ../../freedj-esp/native/media/bass.wav)

build/sim: $(FW) $(EMU) sim/host/main.c fw/*.h lite/lite.h sim/emu/*.h
	@mkdir -p build/out
	gcc -O2 -g -Wall -Wextra -Ifw -Ilite -Isim/emu $(FW) $(EMU) sim/host/main.c $$(sdl2-config --cflags --libs) -o $@

build/sim_cmd17: $(FW) $(EMU) sim/host/main.c fw/*.h lite/lite.h sim/emu/*.h
	gcc -O2 -g -Wall -Wextra -DSD_STREAM=0 -Ifw -Ilite -Isim/emu $(FW) $(EMU) sim/host/main.c $$(sdl2-config --cflags --libs) -o $@

build/sim_py32: $(FW) $(EMU) sim/host/main.c fw/*.h lite/lite.h sim/emu/*.h
	@mkdir -p build/out
	gcc -O2 -g -Wall -Wextra -DCHIP_PY32 -DHAL_AUDIO_BLOCKS=8 -Ifw -Ilite -Isim/emu $(FW) $(EMU) sim/host/main.c $$(sdl2-config --cflags --libs) -o $@

CHIP ?= ch32
SIMBIN := build/$(if $(filter py32,$(CHIP)),sim_py32,sim)

build/sd.img: tools/pack_lite.py tools/sdimage.py $(MUSIC)
	python3 tools/pack_lite.py $(MUSIC) -o build/TRACKS.BIN
	python3 tools/sdimage.py build/TRACKS.BIN -o $@

sd: build/sd.img
sim: $(SIMBIN) build/sd.img
	./$(SIMBIN) $(SIMARGS)

simtest: $(SIMBIN) build/sim_cmd17
	SIM=$(SIMBIN) python3 sim/test_sim.py

deckbench:
	@test -f build/simtest/sd.img || $(MAKE) simtest
	sh sim/qemu/$(if $(filter py32,$(CHIP)),run_py32.sh,run.sh) $(DECKFLAGS)

# ---- the PY32F030 firmware. Puya's CMSIS headers (BSD-3) from github.com/IOsetting/py32f0-template
# UI_FLIP=1: the OLED as it sits on the bring-up bench; 0 the other way up
UI_FLIP ?= 1
PY32_SDK ?= $(HOME)/sandbox/thirdparty/py32f0-template
ARM_GCC ?= $(firstword $(shell command -v arm-none-eabi-gcc) $(wildcard $(HOME)/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc))
PY32_CFLAGS := -mcpu=cortex-m0plus -mthumb -Os -g -Wall -Wextra -ffunction-sections -fdata-sections -ffreestanding \
	-DPY32F030x8 -DHAL_AUDIO_BLOCKS=8 -DUI_FLIP=$(UI_FLIP) -Ifw -Ilite -I$(PY32_SDK)/Libraries/CMSIS/Core/Include \
	-I$(PY32_SDK)/Libraries/CMSIS/Device/PY32F0xx/Include
PY32_SRC := $(FW) py32/hal_py32.c py32/startup.c
# MEMCARD=1: no SD card, a short clip (DEMO=, DEMO_S= seconds) in flash on an emulated card
MEMCARD ?= 0
DEMO ?= ../../freedj-esp/native/media/beat.wav
DEMO_S ?= 1.8
ifeq ($(MEMCARD),1)
PY32_SRC += sim/emu/sd_emu.c build/py32fw/memcard.c
PY32_CFLAGS += -DMEMCARD=1 -Isim/emu
endif
build/py32fw/memcard.c: tools/pack_lite.py tools/sdimage.py tools/memcard.py $(DEMO)
	@mkdir -p build/py32fw
	python3 tools/pack_lite.py $(DEMO) --length $(DEMO_S) -o build/py32fw/TRACKS.BIN
	python3 tools/sdimage.py build/py32fw/TRACKS.BIN -o build/py32fw/sd.img
	python3 tools/memcard.py build/py32fw/sd.img -o $@
build/py32fw/deck.elf: $(PY32_SRC) fw/*.h lite/lite.h py32/py32f030x8.ld FORCE
	@test -d $(PY32_SDK) || { echo "PY32_SDK: git clone https://github.com/IOsetting/py32f0-template $(PY32_SDK)"; exit 1; }
	@mkdir -p build/py32fw
	$(ARM_GCC) $(PY32_CFLAGS) $(PY32_SRC) -nostdlib -Wl,--gc-sections -Wl,-Map=build/py32fw/deck.map -T py32/py32f030x8.ld -lgcc -o $@
	$(dir $(ARM_GCC))arm-none-eabi-size $@
build/py32fw/deck.hex: build/py32fw/deck.elf
	$(dir $(ARM_GCC))arm-none-eabi-objcopy -O ihex $< $@
py32: build/py32fw/deck.hex
flash-py32: build/py32fw/deck.hex
	pyocd load -t py32f030x8 $<
	pyocd reset -t py32f030x8

FORCE:
.PHONY: test bench pwm sd sim simtest deckbench py32 flash-py32 FORCE
