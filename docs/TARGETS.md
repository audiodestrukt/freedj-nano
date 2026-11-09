# Targets

What freedj-nano runs on, and the chips it could run on next. Plan only for the last two
(2026-11-09): nothing is ported yet.

The engine is what makes small chips plausible: the keylock reads the track directly
instead of buffering it, so it needs a few dozen bytes of state plus a window table of at most
1 KB. What a deck needs from a chip, then:
- **Storage for the tracks:** memory-mapped flash ideally, or external SPI flash.
- **An audio output:** I2S to a DAC, or PWM with a filter.
- **CPU for ~20-40 operations per sample:** about 600 cycles per stereo frame at 44.1 kHz
  with keylock on the C6.
- **A screen:** an SPI TFT drawn in bands, or a 128x32 / 128x64 OLED.

| | ESP32-C6 (now) | **RP2350** (next) | **CH32V003** (experiment) |
|---|---|---|---|
| Core | 1 x RISC-V 160 MHz, no FPU | 2 x Cortex-M33 150 MHz with FPU (or 2 x Hazard3 RISC-V) | 1 x RISC-V RV32EC 48 MHz, no multiply |
| RAM | 512 KB | 520 KB, PSRAM optional | 2 KB |
| Flash | 4 MB, memory-mapped | external QSPI, memory-mapped (XIP), up to 16 MB | 16 KB internal; tracks on external SPI flash |
| Audio out | I2S | I2S from PIO | PWM + RC filter (8-10 bit) |
| USB host | no | yes, full speed | no |
| Wireless | Wi-Fi 6, BLE | none (Pico 2 W adds it) | none |
| Price | ~$2-5 (module) | under $1 | ~10 cents |

## RP2350: the second real target

Arguably a better nano chip than the C6 for a deck without wireless.

**Why:**
- **Two cores:** audio on one, UI and USB on the other, the same split freedj-esp makes,
  without needing freedj-esp's PSRAM and decoders.
- **An FPU,** though the engine doesn't need one.
- **Memory-mapped flash (XIP):** "tracks are just memory" carries over. 16 MB holds about
  6 min of 22.05 kHz mono, or 1.5 min of 44.1 kHz stereo.
- **USB host (full speed):** with TinyUSB's mass-storage host it can read a rekordbox stick,
  which the C6 can't. Two caveats: the tracks on a stick are MP3/AAC/WAV, so playing them
  directly needs a decoder (MP3 fits; the M33 has DSP instructions) and a RAM window for keylock
  and scratching (520 KB holds ~3 s of 44.1 kHz stereo). Copying a stick's tracks into the
  flash pack on the device is the simpler first step.
- **PIO:** I2S for the PCM5102A, and quadrature decoding for the jog, both without CPU.
- **Price:** under $1; Pico 2 boards are cheap and easy to get.

**Port plan.** The core library (`components/nano`) is portable C++ and moves as it is. The
firmware layer gets Pico SDK equivalents:

| ESP-IDF piece | RP2350 |
|---|---|
| `audio.cpp` (I2S + DMA, underrun count) | PIO I2S program + DMA ring; underrun from the DMA IRQ |
| `tracks.cpp` (`esp_partition_mmap`) | a fixed XIP address range at the end of flash; flashed with picotool |
| `display_tft.cpp` (esp_lcd, bands) | SPI + DMA, the same banded renderer and ILI9341 init (IPS values) |
| `display_u8g2.cpp` | u8g2 with a Pico I2C byte callback |
| `inputs.cpp` (PCNT, ADC ladder) | PIO quadrature, ADC |
| `midi_uart.cpp`, the USB console | UART; TinyUSB CDC |
| FreeRTOS tasks | audio on core 1 (a tight loop or an IRQ), UI on core 0; or FreeRTOS SMP |

**Open questions:** Pico SDK alone or FreeRTOS? Arm or RISC-V cores (both work: the
measurements decide)? Which board (Pico 2, or a smaller RP2350 board like the XIAO RP2350)?

## CH32V003: how small can a deck go

The 10-cent chip that's already on the freedj control board (the CTRL PCB's I2C target). Not a
practical deck, an experiment in how far the no-buffers design stretches. It overlaps with what
was once going to be freedj-nano's "bit-banged" tier.

**What it would take:**
- **Audio:** PWM at ~22 kHz from a timer, 8-10 bits, an RC filter, and a small amp or line
  driver. No I2S.
- **Tracks:** external SPI flash (W25Q, 16 MB). No memory mapping, so the engine reads
  through a tiny cache: playback reads sequentially, and the keylock's search reads ~9 short
  runs per hop.
- **CPU:** RV32EC at 48 MHz with no hardware multiply. Varispeed needs one multiply per
  sample: easy. Keylock at 22 kHz mono needs ~20 abs-diffs and 2 multiplies per sample plus the
  search, so it's borderline. Measure before believing it.
- **RAM:** 2 KB. The window table (256 entries, 512 bytes) and the deck state fit. The OLED
  can't have a 1 KB frame buffer: draw a 128x32 screen one 128-byte page at a time.
- **Controls:** an EC11 on GPIO interrupts (no PCNT), a few buttons, MIDI in on the UART.

**Open questions:** Does keylock fit at all, or is this a varispeed-only deck? Is 8-10 bit PWM
acceptable, or is an I2S DAC (bit-banged I2S: probably too costly) needed? Is a 16 KB program
enough for the engine, the OLED page renderer and the inputs?

## First step for both: measure the engine on the computer

Before buying anything, cross-compile `stretch.cpp` and `deck.cpp` for each core
(`arm-none-eabi-gcc -mcpu=cortex-m33`, `riscv-none-elf-gcc -march=rv32ec`) and count cycles per
output sample in an instruction-set simulator, or estimate from the generated code. The C6's
measured 600 cycles per stereo frame at 44.1 kHz is the reference. This answers the
CH32V003's keylock question cheaply, and picks Arm or RISC-V on the RP2350.
