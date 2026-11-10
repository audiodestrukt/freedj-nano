# nano-lite for the CH32V003.
#   make test    host tests (lite against the C6 engine), listening WAVs in build/out/
#   make bench   cross-compile for RV32EC in Docker, count instructions in qemu: size, RAM, cost
#   make pwm     the PWM audio output modelled: measurements, and WAVs of IN= (default: the keylock test output)
#   make sim     the whole deck on the computer (fw/ on emulated parts): build/sim, card image from MUSIC=
#   make simtest scripted sessions through the sim, checked
#   make deckbench  the whole firmware for RV32EC in qemu: flash, RAM, stack, CPU by part, worst main-loop step
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

build/sd.img: tools/pack_lite.py tools/sdimage.py $(MUSIC)
	python3 tools/pack_lite.py $(MUSIC) -o build/TRACKS.BIN
	python3 tools/sdimage.py build/TRACKS.BIN -o $@

sd: build/sd.img
sim: build/sim build/sd.img
	cd . && ./build/sim $(SIMARGS)

simtest: build/sim build/sim_cmd17
	python3 sim/test_sim.py

deckbench:
	@test -f build/simtest/sd.img || $(MAKE) simtest
	sh sim/qemu/run.sh $(DECKFLAGS)

.PHONY: test bench pwm sd sim simtest deckbench
