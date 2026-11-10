# The 10-cent DJ deck (plan)

Status: plan, 2026-11-09; phase 0 done 2026-11-10 (the engine runs in the emulator, see
[Phase 0 results](#phase-0-results-2026-11-10)). No hardware yet. A DJ deck around the **CH32V003**, a 10-cent
RISC-V microcontroller with 2 KB of RAM. The point is to find out how far freedj-nano's
no-buffers engine stretches; the board around it costs a few dollars. Context: why this chip and
not the others in the cheap-MCU roundup is in [TARGETS.md](TARGETS.md).

**Goal:** play a track from flash with varispeed and a jog; CUE and PLAY as on a CDJ; a 128x32
OLED; DJ2Go control. **Stretch goal:** keylock.

## The chip

CH32V003F4P6 (TSSOP-20), the part already on the freedj control board:

| | |
|---|---|
| Core | QingKe V2A, RV32EC, 48 MHz. **No hardware multiply** |
| Memory | 16 KB flash, 2 KB SRAM |
| Peripherals | TIM1 (advanced, 4-channel PWM), TIM2, SPI, I2C, USART, 10-bit ADC, 7-channel DMA |
| Programming | SWIO, one wire on PD1, from a **WCH-LinkE** (~$5); no USB, no factory bootloader |

## Hardware

| Part | Choice | Notes |
|---|---|---|
| MCU | CH32V003F4P6 dev board | buy the listing that includes the WCH-LinkE |
| Tracks | **microSD in SPI mode** (default) | a whole library; files written on the computer. Read with ChaN's **Petit FatFs** (read-only FAT, ~2-4 KB code, ~40 B RAM, partial-sector reads: no 512-byte buffer) |
| Tracks, fallback | W25Q128 SPI NOR flash, 16 MB | if SD latency spikes turn out to matter: NOR reads start in microseconds. Written by a CH341A programmer with `flashrom` |
| Audio out | **PWM** from TIM1 + RC filter | see below; mono |
| Amp / line out | small op-amp line driver, or a PAM8302 for a speaker | |
| Screen | **SSD1306 128x32** (0.91") on I2C | 512 bytes per frame, drawn a page at a time |
| Controls | EC11 encoder with push (jog / browse), PLAY, CUE, SHIFT buttons, a 10k pitch pot | |
| MIDI | the USART | DJ2Go through freedj-esp's `midi_bridge.py`, over the WCH-LinkE's own serial port (no adapter needed); DIN MIDI with an optocoupler later |

### Audio: PWM

No I2S, so audio is PWM from TIM1: **two 8-bit channels summed through resistors weighted
1 : 256** (CH1 the high byte through R1, CH2 the low byte through R2), then a low-pass filter.
The sample rate is 22.05 kHz; DMA from a small buffer updates the duty cycles, so the CPU
doesn't take an interrupt per sample.

Modelled before there's hardware: `ch32v003/sim/pwm_sim.py` (`make pwm`) solves the real
network exactly, pulse by pulse (checked tick by tick and against ngspice), and renders WAVs.
THD+N in 20 Hz-20 kHz, tones at -6 dBFS; 1 % parts the worst way round, 40 ohm pins:

| | 1 kHz | 5 kHz | 1 kHz at -40 dBFS | 12.05 kHz image of 10 kHz | carrier ripple |
|---|---|---|---|---|---|
| mu-law track, perfect 22.05 kHz DAC | -37.6 dB | -37.5 | -35.5 | -19.9 | |
| 8-bit, one pin, edge-aligned, RC-RC | -43.8 | -20.2 | **-14.1** | -3.3 | 14 mVpp |
| dual, R2 255k, edge-aligned, RC-RC | -47.8 | -20.2 | -36.3 | -3.3 | 14 mVpp |
| dual, R2 267k, edge-aligned, RC-RC | -47.8 | -20.2 | -43.6 | -3.3 | 14 mVpp |
| dual, R2 267k, **centre-aligned**, RC-RC | **-76.2** | -20.6 | -48.4 | -3.4 | 55 mVpp |
| dual, R2 267k, centre-aligned, **Sallen-Key** | -75.1 | -25.9 | -46.9 | -4.9 | 6 mVpp |
| the same with 2x interpolation on the chip | -75.4 | -34.5 | -47.2 | -6.2 | 6 mVpp |

- **Centre-aligned, not edge-aligned.** An edge-aligned pulse's centre moves with its width: a
  2nd harmonic at -48 dB for 1 kHz, rising with frequency. Centre-aligned pulses don't move:
  -76 dB. It halves the carrier to 88.2 kHz, so the filter has to work harder.
- **R2 = 267k**: 256 x (R1 + the pin's output resistance), the nearest E96. 255k is 7 dB worse
  at low level. The 40 ohms is a guess: measure it in phase 1 and pick R2 from it.
- **Filter: a pole (R1 + 15 nF) and a unity-gain Q = 1 Sallen-Key** (10k, 10k, 3.3 nF, 820 pF)
  on the MCP6002 that's there anyway, about 3rd-order Butterworth at 10 kHz: ripple 55 -> 6 mVpp,
  -19 dB at 20 kHz.
- **The PWM isn't the limit, mu-law is.** Every dual-PWM row beats the track format; one 8-bit
  pin doesn't at low level (-14 dB at -40 dBFS: quiet passages and fades turn to grit).
- **What's left to hear: the 22.05 kHz images.** Content at f also comes out at 22.05 - f: 5 kHz
  puts 17.05 kHz in band (the 5 kHz column), and no cheap filter separates 10 kHz from 12 kHz.
  2x linear interpolation on the chip pushes the mid images down 9 dB, but doubles the DMA ring
  (512 B, against ~180 B of RAM margin) unless the blocks get smaller. Packing tracks band-limited
  to ~9 kHz is free.
- **Supply noise goes straight to the output** (the pins swing rail to rail): 1 mV rms on VDD is
  hiss at -74 dB on silence and caps THD+N at -67 dB. A clean 3.3 V, an LDO and an RC on VDD.

### Draft pins (check against the datasheet before wiring)

| Function | Pin |
|---|---|
| SD card (or SPI flash) SCK / MOSI / MISO / CS | PC5 / PC6 / PC7 / PC0 |
| OLED I2C SDA / SCL | PC1 / PC2 |
| PWM audio high / low byte | PD2 (TIM1 CH1) / PA1 (TIM1 CH2) |
| EC11 A / B / push | PD3 / PD4 / PC3 |
| PLAY / CUE / SHIFT | PC4 / PA2 / PD0 (or a resistor ladder on one ADC pin) |
| Pitch pot | an ADC pin (ladder and pot share the ADC) |
| USART RX (MIDI, bridge) / TX | PD6 / PD5 |
| SWIO | PD1 |

## Shrinking the engine

nano's engine (`components/nano`) is C++ with atomics, Q16 positions and a 1 KB window table.
On the CH32V003 it becomes a small C port, "nano-lite", that keeps the same ideas:

- **Tracks streamed forward through two small rings, not memory mapping.** Everything the
  engine reads stays near the playhead, so storage is read sequentially; only jumps (cue, load)
  seek, with a 1.5 ms fade out and in. A 768-byte ring of 8-bit mu-law at full rate (35 ms) is
  what's played; a 192-byte copy decimated 4:1 is what the keylock's coarse search reads. The
  search has to reach +-1 hop (11.6 ms) so bass can line up (see the C6's 2026-11-10 fix), and
  every read it and the coming hop make has to fit in the ring at once: it's clamped to make sure.
- **Linear crossfade instead of the sin^2 table.** WSOLA's two halves still sum to exactly 1, and
  it's one 8-bit multiply per sample.
- **Multiplies by shift and add.** RV32EC has no multiply instruction; the engine compiles to none.
  Crossfade and interpolation weights are 8-bit, so a multiply is 8 tests and adds.
- **Two-pass search:** coarse, every 8th offset over +-1 hop on the decimated ring (32 points =
  hop/2); fine, +-7 at full rate (32 points, every 2nd sample). Centre out, so among equally
  good matches (every period of a tone) the nearest wins: offsets stay small and the rings can.
- **Same track data, one file per track.** On SD, `pack_tracks.py` writes each track as its own
  file (the pack entry's PCM, peaks and grid), 22.05 kHz mono, 44 KB/s at 16 bits; on NOR flash it's
  the same pack image as the C6's. The peak table gives the OLED its waveform strip; rekordbox grids
  work as on the C6. MP3s from a rekordbox SD export can't play directly: no decoder fits.

### Budgets

Engine numbers are measured (phase 0); the rest are estimates until phase 1-3.

| | |
|---|---|
| **CPU**, varispeed | 87-144 instructions per sample (speed 1.00 / 1.08): **6-10 %** of 48 MHz at 1.5 cycles per instruction |
| **CPU**, keylock | 257 per sample on average: **18 %**. The block with the search: 911 per sample, 63 % of real time, absorbed by the DMA ring (4 x 32 samples, so a burst may take up to 3 blocks) |
| **RAM** | engine (`lite_deck`) **1032 B** (measured), PWM DMA ring 4 x 32 x 2 B = 256 B, stack ~350 B (the search's two reference arrays are 96 B of it), OLED page 128 B, Petit FatFs ~44 B, app ~60 B: **~1.87 of 2 KB**. Margin ~180 B; a half-page OLED buffer would give 64 B more |
| **Flash** | engine **3.7 KB** (measured: 3.1 KB code + 0.6 KB mu-law table), Petit FatFs ~2.5 KB, SSD1306 + 5x7 font ~1.5 KB, startup and drivers (clock, SPI, I2C, TIM1, DMA) ~1.5 KB, inputs + MIDI + DJ2Go map ~1.5 KB, app ~1.5 KB: **~12 of 16 KB** |
| **Screen** | 512 bytes per frame over I2C at 400 kHz (~13 ms) or 1 MHz (~5 ms) with DMA: 20-30 fps, in the gaps |

The cycles-per-instruction figure is the open one: QEMU counts instructions exactly, but the
QingKe V2A's flash wait states and branch costs are assumed (1.5). Phase 1 measures it with
SysTick; at 3 cycles per instruction keylock would still fit (36 % average).

## Phases

0. **Measure on the computer.** Done: see below.
1. **Bring-up.** WCH-LinkE + `minichlink` (ch32fun), blink, then a PWM test tone through the
   filter. TIM1 centre-aligned with ARR 272: 48 MHz / 544 = 88.2 kHz, and the repetition counter
   at 7 (it counts half periods in this mode) gives a sample every 4 periods: **22,059 Hz**,
   within 0.04 % of 22.05 kHz, so DMA can update the duty cycles once per sample. Make the update
   land where both pins are low, not mid-pulse. Measure the pins' output resistance (for R2),
   compare a tone with the model (`make pwm`), and measure cycles per instruction with SysTick
   around `lite_render`.
2. **Play a track.** Mount the SD card with Petit FatFs, stream a track file through the ring,
   play it at speed 1 and with varispeed. Log the longest read stall per card: that decides SD vs
   the NOR fallback.
3. **Screen.** SSD1306 page renderer: time remaining, tempo, a 16-pixel waveform strip.
4. **Controls.** EC11 (jog: nudge / scrub; with SHIFT: browse), PLAY, CUE, the pitch pot, and the
   DJ2Go over the bridge.
5. **Keylock,** if phase 0 says it fits.
6. **A board.** A small PCB in the style of `ctrl-pcb` (tscircuit, JLCPCB-stocked parts): CH32V003,
   a microSD socket, the PWM filter, a 3.5 mm jack, the OLED header, the encoder and buttons.

## Phase 0 results (2026-11-10)

`ch32v003/` builds nano-lite twice: on the computer against the C6 engine (`make test`), and
for RV32EC (`-march=rv32ec_zicsr -mabi=ilp32e -Os`) in the ESP-IDF image's GCC, run under
`qemu-riscv32 -cpu rv32e,c=true` with a per-instruction trace (`make bench`).

- **It fits.** Keylock 18 % of the CPU on average, varispeed 6-10 %; engine 3.7 KB of flash and
  1032 bytes of RAM, no multiply or divide instructions. So the deck isn't varispeed-only.
- **Quality:** pitch kept within 1 % for 50 Hz-1 kHz tones at -8 %, +8 % and +16 % (bass
  included); mu-law playback 37 dB SNR; keylock level matches the C6 engine's. Listening files:
  `ch32v003/build/out/` (source, lite and C6 keylock at +8 %).
- **Rings:** 0 reads outside them at 0.5x to 2x, keylock and varispeed, and across 200 random
  seeks, engine switches and tempo moves (no clicks above the music's own steps).
- **What it took** (first try to last): a 512-byte ring was too small for the +-1 hop search
  (the coarse pass reads ahead and evicted the frame still fading out), so 768; the search
  scanned from -hop up, so ties went to the most negative offset and stretched the span, so
  centre out; checked reads cost 200k instructions per search (222 % of real time in that block),
  so one fetch up front and direct ring reads; `-Os` didn't inline the per-sample reads, so
  forced; coarse every 8th offset instead of 4th. 613 -> 257 instructions per sample.
- **PWM output** modelled since (see Audio: PWM): dual PWM, centre-aligned, beats the mu-law
  track it plays.
- **Still unknown:** cycles per instruction on the real core, SD stalls, the pins' output
  resistance and the board's supply noise. Phase 1-2.

## Shopping list

- CH32V003F4P6 dev boards, **with the WCH-LinkE**
- microSD breakout for 3.3 V (SPI), and a small, fast card (any A1-rated card)
- (fallback) W25Q128 SPI flash breakout and a CH341A programmer with a SOIC-8 clip
- SSD1306 0.91" 128x32 I2C OLED
- EC11 encoder with push switch, 3 tact switches, a 10k pot
- 1 % resistors for the dual-PWM sum (1k, 267k; 255k and 270k to try), the filter (2 x 10k,
  15 nF, 3.3 nF, 820 pF, C0G), and an op-amp (MCP6002) for the Sallen-Key and as a line driver

## Where it lives

`ch32v003/` in freedj-nano: a ch32fun Makefile build, the nano-lite engine in C, and the drivers.
It shares the pack format, `pack_tracks.py`, the rekordbox reader, the DJ2Go map and the host
tests with the C6 build.

## Open questions

- SD read stalls: how long, how often, per card (phase 2 logs it)? The 768-byte ring holds 35 ms
  of mu-law at 22.05 kHz, but the keylock keeps most of it in use, against the occasional
  10 ms+ stall: reads have to run ahead of the playhead with some slack.
- Does Petit FatFs fit next to everything else in 16 KB? (estimated ~12 KB total)
- Cycles per instruction on the QingKe V2A (phase 1): keylock is 18 % at 1.5, 36 % at 3.
- Dual PWM is good enough on paper (it beats mu-law). Are the 22.05 kHz images? Listen to
  `ch32v003/build/out/pwm_*.wav` against `pwm_reference.wav`; if they're not, 2x interpolation
  (RAM) or tracks packed band-limited to ~9 kHz (free).
- Stereo: TIM1 has four channels, enough for two dual-PWM outputs; costs RAM and CPU.
