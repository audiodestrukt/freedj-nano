# nano-lite for the CH32V003.
#   make test    host tests (lite against the C6 engine), listening WAVs in build/out/
#   make bench   cross-compile for RV32EC in Docker, count instructions in qemu: size, RAM, cost
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

.PHONY: test bench
