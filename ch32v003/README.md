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
| `fw/` | the deck firmware: `app.c` (transport, controls, DJ2Go map), `ui.c` (SSD1306 pages), `sd.c` (SD over SPI, streaming reads), `tracks.c` (TRACKS.BIN), Petit FatFs (`pff/`, ChaN, 1-clause BSD), all behind `hal.h` |
| `sim/host/` | the firmware on the computer: an SDL window (the OLED), sound, keyboard and mouse wheel as the panel, a pty for `midi_bridge.py`, or the DJ2Go read directly; headless with a script |
| `sim/emu/` | the emulated parts, shared with the qemu run: an SDHC card on SPI (`sd_emu.c`), an SSD1306 on I2C (`oled_emu.c`) |
| `sim/qemu/` | the same firmware for RV32EC under qemu-riscv32 with a scripted session: every instruction counted, by part |
| `sim/test_sim.py` | scripted sessions through the sim, checked (31 checks) |
| `tools/` | `pack_lite.py` (audio files -> TRACKS.BIN), `sdimage.py` (a FAT32 card image) |
| `sim/pwm_sim.py` | the PWM audio output modelled: TIM1's two pins, the resistor sum and the filter, solved exactly per pulse; measurements and listening WAVs |

```
make test     # host tests; listening WAVs in build/out/
make bench    # Docker (espressif/idf:release-v5.5 for its RISC-V GCC) + qemu-riscv32 on the host
make pwm      # numpy, scipy (and ngspice for the self-test); IN=song.wav to hear a track
make sim      # the deck in a window (SDL2); MUSIC="a.mp3 b.wav" picks the tracks (ffmpeg)
make simtest  # scripted sessions, checked
make deckbench  # whole firmware in qemu: flash, RAM, stack, CPU by part, worst step
```

### The deck on the computer (`make sim`)

The real firmware, unchanged, on emulated parts: the SD card answers SPI byte by byte from a
FAT32 image, the SSD1306 is drawn from the I2C bytes the firmware sends, and the pins are the
keyboard. Keys: space PLAY, c CUE, shift SHIFT, enter the encoder's push, left / right or the
mouse wheel turn it (ctrl: x4), up / down the pitch pot, home centres it.

The DJ2Go: `./build/sim --midi auto` reads it directly; or run freedj-esp's bridge against the
sim's USART, exactly as against the chip (`midi_bridge.py --port ch32v003/build/tty`). Only one
program can have the DJ2Go open, so stop any other bridge first.

`--sd-access-us`, `--sd-stall-ms` and `--sd-stall-every` set the card's timing; the window shows
the SD bus load, commands per second, ring misses and audio underruns.

`make pwm` checks the model (period tables against tick-by-tick stepping, the filter against
ngspice), prints THD+N, images and ripple for each variant (8-bit, dual PWM, resistor choices,
edge- and centre-aligned, RC-RC and Sallen-Key filters, interpolation), and writes
`build/out/pwm_*.wav` with `pwm_reference.wav` (the same track through a perfect DAC) to A/B.

`make bench` prints the engine's size, its RAM (`lite_deck`), and instructions per sample for
varispeed and keylock, with the worst 64-sample block. CPU percentages assume 1.5 cycles per
instruction until phase 1 measures it on the chip.

What's left for the chip: `hal.h` on ch32fun (clock, GPIO, TIM1 + DMA, SPI, I2C + DMA, ADC,
USART), phase 1.
