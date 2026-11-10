# Chip options for the small decks

Written 2026-11-10, to come back to. What the CH32V003 deck ([DECK10C.md](DECK10C.md)) is held
back by, which chips lift which limits, and what each would sound like. Prices are rough and
from memory unless a source is given: check LCSC before deciding. Datasheet details marked
**(check)** weren't confirmed.

## What limits the CH32V003 deck

| Limit | Effect on the deck |
|---|---|
| **2 KB RAM** | tracks stored as 8-bit mu-law (a 16-bit buffer would need 1.5 KB of the 2); a 35 ms track buffer; a 4.35 ms audio cushion; 41 B spare. The waveform and backwards scrubbing are tight because there's no room to cache |
| **No I2S** | no DAC chip; audio is two PWM pins summed (`make pwm`: fine, cleaner than the tracks) |
| **48 MHz, no multiply, ~2.6 cycles per instruction (modelled)** | the engine is shift-and-add arithmetic; keylock fits, but not much more |
| **16 usable pins** | all taken; buttons may have to share a pin (DECK10C-CONTROLS.md) |
| **One SPI, one USART, no USB host** | SD card and MIDI each take one; the DJ2Go needs the computer bridge |

What you hear, worst first: mu-law noise (~37 dB below the music), the 22.05 kHz rate (nothing
above ~10 kHz: hi-hats), mono. The output stage isn't the limit.

## The chips

| | RAM | Flash | Core | Audio out | USB | Price |
|---|---|---|---|---|---|---|
| **CH32V003** (now) | 2 KB | 16 KB | 48 MHz RV32EC, no multiply | dual PWM | none | ~$0.15-0.30 ([LCSC](https://www.lcsc.com/product-detail/C5187096.html)) |
| CH32V002 | 4 KB | 16 KB | 48 MHz, multiply (reported) | PWM | none | ~$0.20 |
| **CH32V006** | 8 KB | 62 KB | 48 MHz (one more flash wait state than the V003, reported) | PWM | none | ~$0.20-0.40 |
| **CH32V203** (e.g. C8T6) | 20 KB (up to 64 KB by model) | 64 KB (up to 224 KB) | 144 MHz RV32IMAC, hardware multiply and divide | 2 SPI, no I2S **(check)**: an I2S DAC through SPI + a timer (below) | **full-speed host / device** | ~$0.40-0.80 |
| CH32V208 / V303 / V305 / V307 | 64-128 KB | up to 256 KB | 144 MHz, V30x with FPU | **I2S** (2 of 3 SPIs) | host / device | ~$1-2 |
| RP2350 (on hand) | 520 KB | external QSPI, 4-16 MB on boards | 150 MHz, 2 cores, FPU (M33) | I2S from PIO, no CPU | host / device | ~$1 |

All the CH32s build with the same open toolchain (ch32fun, the ESP-IDF RISC-V GCC we already use
for `make bench`), and `fw/` is written against `hal.h`, so the deck firmware, the sim and the
bench carry over to any of them.

## Scenarios

### 1. CH32V003, as it is (~$0.30 chip)

8-bit mu-law mono at 22.05 kHz, dual PWM. Fits, with the margins in
[DECK10C-PERF.md](DECK10C-PERF.md). The point is the cheapest possible deck.

### 2. CH32V006: the same deck with breathing room (~$0.30)

Four times the RAM, four times the flash, more pins, same family. What 8 KB buys:

- **Either** 16-bit mono tracks (the 768 B buffer becomes 1.5 KB): the mu-law noise goes, the
  rate and mono stay. 44 KB/s from the card is easy.
- **Or** keep mu-law and use the RAM for safety: a bigger audio cushion (SD stalls stop
  mattering), the waveform cache, backwards scrubbing without card jumps.
- Pins for buttons, the optical jog and a thumbwheel without sharing.

Cost: little; the firmware ports nearly as is. Risk: the extra flash wait state (reported)
makes it a bit slower per clock: re-run `make deckbench` with that in the cycle model.

### 3. CH32V203 + a PCM5102A DAC: CD quality (~$0.60 chip + ~$1-3 DAC)

The big step: **16-bit stereo at 44.1 kHz** into a real DAC, like the XIAO C6 build.

- **RAM** (20 KB on the C8T6): a 16-bit stereo buffer for keylock at 44.1 kHz is ~6 KB
  (35 ms x 4 bytes a frame), plus an 8 KB audio cushion (46 ms: SD stalls and the waveform
  stop being a problem), and room left.
- **CPU**: 144 MHz with a multiply instruction: the full engine (`components/nano`, the C6's
  keylock with the sin^2 window), not the cut-down nano-lite, reading from the stream instead of
  memory-mapped flash.
- **SD**: 176 KB/s for 16-bit stereo; the V203's SPI runs much faster than the V003's, and the
  same streaming reads apply.
- **The DAC without I2S**: the PCM5102A wants a bit clock, a data line and a left/right clock.
  SPI2 sends the bit clock and data continuously by DMA; a timer counts the bit clock (fed back
  into it on one pin) and toggles left/right every 16 or 32 bits. A known trick on chips
  without I2S: one extra pin, no CPU. Or skip the trick with a V208/V30x, which have I2S.
- **USB host**: the V203's full-speed USB host could talk to the DJ2Go directly (a USB-MIDI
  host driver: WCH has host examples; the DJ2Go is class-compliant). No computer, no bridge.
- **Pins**: 48-pin package, plenty.

What changes in sound: mu-law noise gone (a PCM5102A is ~-90 dB), hi-hats back (44.1 kHz),
stereo. From telephone-ish to CD. The deck becomes a C6 deck in features at a fraction of the
board cost.

### 4. CH32V30x: I2S built in, an FPU (~$1-2)

Everything in 3, with real I2S (no timer trick), 64 KB+ of RAM and an FPU (the float code from
freedj-esp could run). At this price it competes with the RP2350.

### 5. RP2350: you already have one (~$1)

520 KB of RAM, two cores, I2S from PIO with no CPU, USB host. With 4-16 MB of QSPI flash on
the board, tracks could even live in flash like the C6's (no SD jumps at all: see
DECK10C-PERF.md on NOR flash). The easiest path to a CD-quality small deck; the least "10 cent".

## Comparison

| | V003 | V006 | V203 + PCM5102A | V30x + DAC | RP2350 + DAC |
|---|---|---|---|---|---|
| Sound | 8-bit mu-law mono 22 kHz | 16-bit mono 22 kHz, or mu-law with margin | 16-bit stereo 44.1 kHz | 16-bit stereo 44.1 kHz | 16-bit stereo 44.1 kHz |
| Chip | ~$0.30 | ~$0.30 | ~$0.60 | ~$1-2 | ~$1 |
| + DAC | none | none | ~$1-3 | ~$1-3 | ~$1-3 |
| DJ2Go | computer bridge | computer bridge | **USB host** | USB host | USB host |
| Firmware | done (sim) | port, small | new engine port + DAC + USB host | as V203, simpler DAC | new board support |
| Margins | tight | comfortable | comfortable | comfortable | huge |

## To check before choosing

- **V203 I2S**: the peripheral table in the V203 datasheet (it looks like 2 SPIs, no I2S).
- **V006 flash wait states and multiply**: its datasheet; then `make deckbench` adjusted.
- **V203 USB host**: a class-compliant USB-MIDI device on WCH's USBFS host examples.
- **Prices**: LCSC, for the exact package.

Sources: [CH32V006 (CNX Software)](https://www.cnx-software.com/2024/05/09/wch-ch32v006-risc-v-microcontroller-adds-more-i-os-memory-and-storage-compared-to-ch32v003/),
[CH32V002 (CNX Software)](https://www.cnx-software.com/2024/05/29/wch-ch32v002-32-bit-risc-v-mcu-comes-with-4kb-sram-supports-2v-to-5v-dc-supply-voltage/?amp=1),
[openwch/ch32v20x](https://github.com/openwch/ch32v20x),
[CH32V203C8T6 (LCSC)](https://www.lcsc.com/product-detail/C3001172.html),
[CH32V203C8T6 datasheet (LCSC)](https://datasheet.lcsc.com/lcsc/2403131350_WCH-Jiangsu-Qin-Heng-CH32V203C8T6_C3001172.pdf),
[CH32V303 (mikrocontroller.net)](https://www.mikrocontroller.net/part/CH32V303).
