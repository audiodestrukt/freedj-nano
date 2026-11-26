# The CH32V003 deck: nano-lite and its firmware

freedj-nano's deck engine in plain C for the CH32V003 (RV32EC, 48 MHz, no multiply, 16 KB flash,
2 KB RAM): varispeed and keylock on a mono 22.05 kHz mu-law track streamed through two small
rings, and the deck firmware around it, which runs today on the computer against emulated
parts. The plan, the budgets and the results: [../docs/DECK10C.md](../docs/DECK10C.md).

| | |
|---|---|
| `lite/` | the engine (`lite.c`, `lite.h`) and the mu-law table and encoder (`ulaw.c`) |
| `tests/test_lite.cpp` | host tests, some against the C6 engine (`components/nano`) |
| `bench/` | RV32EC bench: a freestanding program, the runtime bits the toolchain lacks (`rt.c`), and the instruction counter (`count.py`) |
| `fw/` | the deck firmware: `app.c` (transport, controls, DJ2Go map), `ui.c` (SSD1306 128x64, a page at a time), `sd.c` (SD over SPI, streaming reads), `tracks.c` (TRACKS.BIN), Petit FatFs (`pff/`, ChaN, 1-clause BSD), all behind `hal.h` |
| `sim/host/` | the firmware on the computer: an SDL window (the 128x64 OLED), sound, keyboard and mouse wheel as the panel, a pty for `midi_bridge.py`, or the DJ2Go read directly; headless with a script |
| `sim/emu/` | the emulated parts, shared with the qemu run: an SDHC card on SPI (`sd_emu.c`), an SSD1306 on I2C (`oled_emu.c`) |
| `sim/qemu/` | the same firmware for RV32EC under qemu-riscv32 with a scripted session: every instruction counted, by part; `run_py32.sh` does the same for the PY32F030 (Cortex-M0+ under qemu-arm) |
| `sim/test_sim.py` | scripted sessions through the sim, checked (36 checks) |
| `tools/` | `pack_lite.py` (audio files -> TRACKS.BIN), `sdimage.py` (a FAT32 card image) |
| `sim/pwm_sim.py` | the PWM audio output modelled: TIM1's two pins, the resistor sum and the filter, solved exactly per pulse; measurements and listening WAVs |

```
make test     # host tests; listening WAVs in build/out/
make bench    # Docker (espressif/idf:release-v5.5 for its RISC-V GCC) + qemu-riscv32 on the host
make pwm      # numpy, scipy (and ngspice for the self-test); IN=song.wav to hear a track
make sim      # the deck in a window (SDL2); MUSIC="a.mp3 b.wav" picks the tracks (ffmpeg)
make simtest  # scripted sessions, checked
make deckbench  # whole firmware in qemu: flash, RAM, stack, CPU by part, worst step

make sim CHIP=py32        # the same, as the PY32F030: an 8-block audio ring, its cycle costs
make simtest CHIP=py32
make deckbench CHIP=py32  # Thumb under qemu-arm -cpu cortex-m0 (the Arm GNU toolchain, no Docker)
```

### The PY32F030 instead

## A new chip: `tools/target.py`

A target is a capability file, `targets/<name>.toml`. It gives the compiler and its flags, the flash and RAM
budget, the audio out and in, the storage, the screen, the controls and the network. The tool measures the
shared C modules with that chip's own compiler and picks what the chip can run:

```
python3 tools/target.py new mychip --cc arm-none-eabi-gcc --cflags='-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16'
python3 tools/target.py plan mychip     # after filling in the board's part of targets/mychip.toml
make targets                            # every target: probe and plan
make targetcheck                        # every plan against the [expect] in its file
```

- `new` reads the compiler's macros for the ISA. It also compiles a few test functions to see which
  operations are hardware and which call the runtime (32- and 64-bit multiply and divide, float, double).
- `probe` compiles every module (and the port's HAL, under `[hal]`) and reports code, data, bss, and each
  module's state as `sizeof` on the target's ABI. It also lists the runtime helpers each module calls and
  flags any the target's runtime lacks (`missing`: the FM-1 has no 64-bit division and no `double`).
- `plan` chooses the engine (lite, or hifi with stereo 16-bit out and memory-mapped tracks) and the
  features (DVS needs stereo audio in, Pro DJ Link a network). It drops features until the budget holds
  and gives the reason for each one left out. It lists the `hal.h` functions still to write and writes
  `build/targets/<name>/config.h` (`FEAT_*`) and `target.mk` (the sources).

Sizes are per object before linking, an upper bound. The CPU isn't checked: `make deckbench` measures that.

The same firmware builds for Puya's PY32F030x8 (Cortex-M0+, 48 MHz, 64 KB flash, 8 KB RAM).
On deckbench it's 35 % of the CPU against the CH32V003's 39 %, and with the RAM for an 8-block
audio ring its worst step is 51 % of the cushion instead of 135 %. The comparison:
[../docs/DECK10C-PERF.md](../docs/DECK10C-PERF.md#on-the-py32f030-instead-2026-11-13-make-deckbench-chippy32).
It runs on a real PY32 board (all but the audio out so far): wiring, programmer, build and
flash in [py32/README.md](py32/README.md).

### On an M-VAVE FM-1

`make fm1` builds the deck as a module for fm1-dock on the FM-1 pocket synth, and `make fm1-emu`
/ `make fm1-live` run it in fm1-emu: [fm1/README.md](fm1/README.md).

### The deck on the computer (`make sim`)

The real firmware, unchanged, on emulated parts: the SD card answers SPI byte by byte from a
FAT32 image, the SSD1306 is drawn from the I2C bytes the firmware sends, and the pins are the
keyboard. Keys: space PLAY, c CUE, shift SHIFT, enter the encoder's push, left / right or the
mouse wheel turn it (ctrl: x4), up / down the pitch pot, home centres it.

The DJ2Go: `./build/sim --midi auto` reads it directly; or run freedj-esp's bridge against the
sim's USART, exactly as against the chip (`midi_bridge.py --port ch32v003/build/tty`). Only one
program can have the DJ2Go open, so stop any other bridge first.

`--sd-access-us`, `--sd-stall-ms` and `--sd-stall-every` set the card's timing; the window shows
the SD bus load, commands per second, an estimated CPU load and worst main-loop step (per-unit
costs from `make deckbench`'s cycle model; deckbench is the measurement), the frame rate, ring
misses and audio underruns.

`make pwm` checks the model (period tables against tick-by-tick stepping, the filter against
ngspice), prints THD+N, images and ripple for each variant (8-bit, dual PWM, resistor choices,
edge- and centre-aligned, RC-RC and Sallen-Key filters, interpolation), and writes
`build/out/pwm_*.wav` with `pwm_reference.wav` (the same track through a perfect DAC) to A/B.

`make bench` prints the engine's size, its RAM (`lite_deck`), and instructions per sample for
varispeed and keylock, with the worst 64-sample block. CPU percentages assume 1.5 cycles per
instruction until phase 1 measures it on the chip.

What's left for the chip: `hal.h` on ch32fun (clock, GPIO, TIM1 + DMA, SPI, I2C + DMA, ADC,
USART), phase 1.
