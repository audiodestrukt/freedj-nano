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
#   make fm1        the deck as an fm1-dock module for the M-VAVE FM-1 (fm1/), build/fm1/freedj.dock
#   make fm1-emu    it in fm1-emu, headless (screen, audio, log in build/fm1/emu); make fm1-live: a window
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

# ---- the M-VAVE FM-1, as an fm1-dock module (fm1/): github.com/joakimeriksson/fm1-dock, JieLi's
# Linux toolchain (fm1-dock's tools/get_toolchain.sh), fm1-emu's fm1-live for make fm1-emu.
# The tracks: a clip in the module's flash (FM1_DEMO=, FM1_DEMO_S= seconds; a module is <= 272 KiB)
FM1_DOCK ?= $(HOME)/sandbox/thirdparty/fm1-dock
FM1_LIVE ?= $(HOME)/sandbox/thirdparty/fm1-emu/target/release/fm1-live
FM1_FWSC ?= $(FM1_DOCK)/reference/fm1-dock-0.9.0-empty.fwsc
JIELI_TOOLCHAIN ?= $(HOME)/.local/opt/jieli/jieli-linux-toolchains-20260730.1
FM1_DEMO ?= $(DEMO)
FM1_DEMO_S ?= 8
build/fm1/memcard.c: tools/pack_lite.py tools/sdimage.py tools/memcard.py $(FM1_DEMO)
	@mkdir -p build/fm1
	python3 tools/pack_lite.py $(FM1_DEMO) --length $(FM1_DEMO_S) -o build/fm1/TRACKS.BIN
	python3 tools/sdimage.py build/fm1/TRACKS.BIN -o build/fm1/sd.img
	python3 tools/memcard.py build/fm1/sd.img -o $@
build/fm1/freedj.dock: fm1/*.c fm1/module.cfg $(FW) fw/*.h lite/lite.h build/fm1/memcard.c
	@test -d $(FM1_DOCK) || { echo "FM1_DOCK: git clone https://github.com/joakimeriksson/fm1-dock $(FM1_DOCK)"; exit 1; }
	cd $(FM1_DOCK) && JIELI_TOOLCHAIN=$(JIELI_TOOLCHAIN) python3 tools/hbmod.py $(CURDIR)/fm1
	cp $(FM1_DOCK)/build/modules/fm1.dock $@
fm1: build/fm1/freedj.dock
# FM1_HOST: the tracks from the computer over USB instead (fm1/usb/, tools/fm1host.py)
build/fm1/freedj-usb.dock: fm1/*.c fm1/usb/module.cfg $(FW) fw/*.h lite/lite.h
	@mkdir -p build/fm1
	cd $(FM1_DOCK) && JIELI_TOOLCHAIN=$(JIELI_TOOLCHAIN) python3 tools/hbmod.py $(CURDIR)/fm1/usb
	cp $(FM1_DOCK)/build/modules/usb.dock $@
fm1-usb: build/fm1/freedj-usb.dock
# the card it streams (FM1_MUSIC= audio files, as many as you like), and the module installed in a flash of its own
FM1_MUSIC ?= $(DEMO) $(dir $(DEMO))bass.wav $(dir $(DEMO))sine.wav
build/fm1/usb.img: tools/pack_lite.py tools/sdimage.py
	@mkdir -p build/fm1/usbcard
	python3 tools/pack_lite.py $(FM1_MUSIC) -o build/fm1/usbcard/TRACKS.BIN
	python3 tools/sdimage.py build/fm1/usbcard/TRACKS.BIN -o $@
build/fm1/usbflash.bin: build/fm1/freedj-usb.dock
	rm -f $@
	$(FM1_EMU:flash.bin=usbflash.bin) --deploy $(CURDIR)/$< --ms 12000 --out $(CURDIR)/build/fm1/usbdeploy | tail -2
# a window with the USB cable in, and tools/fm1host.py serving the card to it (Esc in the window ends both).
# FM1_USB_OUT_PER_FRAME: the emulated computer sends up to 8 USB packets a millisecond, as a real one does
# (fm1-emu's default is 1; a local change to fm1-emu, crates/fm1-soc/src/usb.rs)
fm1-usb-live: build/fm1/usbflash.bin build/fm1/usb.img
	FM1_USB_OUT_PER_FRAME=8 $(FM1_LIVE) --fwsc $(FM1_FWSC) --flash build/fm1/usbflash.bin --usb & \
	python3 -u tools/fm1host.py build/fm1/usb.img; wait
# headless: fm1-dock in fm1-emu with the module (FM1_KEYS: fm1-live's FM1_BENCH_KEYS; buttons PLAY 12,
# SEQ 11 (CUE), FX 2 (SHIFT), EDIT 6 (push); e0:N turns SELECT (the jog), e2:N KNOB1 (tempo)).
# The deploy goes in through the emulated MIDI IN at 31250 baud (~80 s for the module with its clip): it
# happens once, into build/fm1/flash.bin, and later runs boot from that with the module in place.
FM1_MS ?= 8000
FM1_KEYS ?= 3000:12,3150:12^
FM1_EMU = cd $(FM1_DOCK) && FM1_LIVE=$(FM1_LIVE) python3 tools/dock.py emu --fwsc $(FM1_FWSC) --flash $(CURDIR)/build/fm1/flash.bin
build/fm1/flash.bin: build/fm1/freedj.dock
	rm -f $@
	$(FM1_EMU) --deploy $(CURDIR)/$< --ms 82000 --out $(CURDIR)/build/fm1/deploy | tail -4
# a window: the panel with the mouse, Space is PLAY, MIDI IN from any MIDI input (a DJ2Go: the bridge's map)
fm1-live: build/fm1/flash.bin
	$(FM1_LIVE) --fwsc $(FM1_FWSC) --flash build/fm1/flash.bin
fm1-emu: build/fm1/flash.bin
	$(FM1_EMU) --ms $(FM1_MS) --keys '$(FM1_KEYS)' --out $(CURDIR)/build/fm1/emu

FORCE:
.PHONY: test bench pwm sd sim simtest deckbench py32 flash-py32 fm1 fm1-usb fm1-usb-live fm1-emu fm1-live FORCE
