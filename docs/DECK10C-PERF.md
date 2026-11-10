# The 10-cent deck: where the time goes, and what to do about it

Written 2026-11-10, for coming back to later. The plan is [DECK10C.md](DECK10C.md); this is
the performance side of it, in plain terms first and numbers after.

## The one rule: never let the audio run dry

The CH32V003 plays audio from a tiny buffer the hardware drains by itself (DMA into the PWM
timer): **4 blocks of 32 samples = 5.8 ms**. While one block is playing, the other three are
the cushion: **4.35 ms**. The firmware is one loop (`app_step()`):

```
forever:
    refill the audio buffer (the engine: varispeed or keylock, reading the SD card)
    read the buttons, encoder, pot and MIDI
    draw one 128-byte strip of the screen (1/8 of a frame)
```

Each pass through that loop is a **step**. If any single step takes longer than the cushion
(4.35 ms, which is **209,000 CPU cycles** at 48 MHz), the buffer empties and you hear a click
or a gap. So two numbers matter, and they're different:

- **Average CPU**: how busy the chip is over a second. Must be under 100 %, and comfortably.
- **Worst step**: the slowest single pass through the loop, against the 209 k-cycle cushion.
  **This is the one that's in trouble.** A deck can be 30 % busy on average and still drop
  audio if one step in a thousand is too long.

The frame rate (20 fps) doesn't tell you either: a frame is 8 steps spread over 50 ms.

## Why the SD card is the slow part

The card is fast at reading **straight through** and slow at **jumping**:

| | |
|---|---|
| Reading straight on | the audio needs 22 KB/s; the card over SPI can do ~3 MB/s. About 1 % busy. |
| A jump to somewhere else | the card's controller has to find the block and load it: **~300 us** (a guess: 100 us to over 1 ms by card; phase 2 measures) |
| Where in the block | it only reads whole 512-byte blocks, from the start: getting to byte 400 means clocking out the 400 before it |
| While it's busy | SPI has no "ready" signal, so the CPU asks over and over: **the CPU waits** |

The audio itself reads straight through (one long "multi-block read" kept open). Anything
else that reads the card (the track list's names, the waveform's peaks) makes it **jump away
and jump back**: two waits, twice, inside one step.

## Cycles per instruction (CPI)

WCH doesn't publish one. What's known: the flash the program runs from needs **2 cycles per
4-byte fetch** at 48 MHz; most instructions take 1 cycle; a taken branch empties the 2-stage
pipeline. So it depends on the code: two 2-byte (compressed) instructions per fetch run at
~1 CPI, 4-byte ones at ~2.

- Earlier numbers assumed a flat **1.5**.
- `make deckbench` now also models it from the actual instructions run (sizes, loads and
  stores, taken branches): **2.57**. Probably a bit pessimistic (it ignores the flash
  prefetch buffer).
- The truth is in between, until phase 1 measures it on the chip (SysTick around the loop).

## Where it stands (2026-11-10, `make deckbench`, cycle model)

| | average CPU | worst step |
|---|---|---|
| without the waveform | 22 % | **91 %** of the cushion (the track list) |
| with the waveform | 37 % | **135 %: audio drops** (a waveform strip + a keylock search in one step) |
| scratching (jog back and forth, ~2.7x, paused) | | **129 %**: each backwards ring refill is a card jump, on top of the waveform's |

What a bad step is made of (with the waveform): ~110 k cycles waiting on SPI bytes, ~57 k in
the SD driver, ~60-95 k in the engine (a keylock search), ~30 k drawing the strip. Most of it
is **waiting on the card**, not computing.

RAM: 2007 of 2048 bytes used, **41 spare**. Flash: ~13.8 of 16 KB.

## Done already

| What | Why | Effect |
|---|---|---|
| **Stream the SD card** (CMD18 kept open) instead of a fresh one-block read (CMD17) per 32 bytes | every fresh read pays the card's wait and all 512 bytes | 66 % CPU / worst step 303 % -> 14 % / 68 % (1.5 CPI) |
| **TRACKS.BIN in one piece**, read by sector number | Petit FatFs walks the file's cluster chain from the start on every backwards seek: hundreds of reads for a cue | no FAT reads after the mount |
| **Draw the screen a strip at a time** (128 bytes, one per step) | a whole 128x64 frame is 1 KB: half the RAM | a 128x64 screen costs no more RAM than a 128x32 |
| **Lend the screen strip to the FAT code** while mounting | Petit FatFs needs ~40 B, only during the mount | 7 -> 50 B spare |
| **Keylock search in two passes**, coarse on a 4:1 copy (phase 0) | a full search is 4x the work | 613 -> 257 instructions per sample |

## To do, in the order I'd try them

### 1. Keep the waveform's window in RAM (fixes the waveform)

Now: each of the waveform's 3 strips reads its 128 peaks from the card, every frame: 6 jumps.
Instead: keep the 128 visible columns in RAM, 4 bits each (**64 B**), and per frame read only
the few columns that scrolled in (~4) with **one** short read. To find the RAM: store the
bottom overview as 64 columns drawn 2 px wide (frees 32 B), which with the 50 spare is enough.

Expect: the worst step back near the no-waveform 91 %. Effort: small (`ui.c`).

Scrubbing backwards adds jumps of its own (a refill of the ring every ~700 frames going back);
with no waveform reads beside them they'd be ~40 k cycles each, well inside the cushion.

### 2. Don't do two expensive things in one step

A keylock search (every 256 samples) and a card jump in the same step add up. The firmware
can tell when a search just ran (the engine knows) and skip that step's screen strip; the
strip goes next step instead. Expect: worst step = the bigger of the two, not their sum.
Effort: small (`lite.h` exposes a flag, `app.c` checks it).

### 3. The track list: fewer jumps

Each visible name is a jump (7 names = 7 jumps per frame). Options: draw the list at a lower
rate, read one name per frame and keep the rest on screen (needs pages that aren't redrawn:
the page addressing for that is already in), or keep the 7 names in RAM (7 x 21 B: too much).
Expect: list view from 91 % to well under. Effort: small.

### 4. Don't wait for the card: non-blocking reads

The ~110 k cycles of waiting are the CPU asking "ready yet?" byte after byte. A state machine
could send the read command, go do other work, and come back. The audio buffer has 35 ms of
track ahead (the engine's 768-byte ring), so reads can be started early. Expect: the waiting
mostly disappears from the worst step. Effort: large (the SD driver and the engine's fetch
both change); worth it only if 1-3 aren't enough or real cards are slow (phase 2).

### 5. Store tracks in SPI NOR flash instead (hardware)

A W25Q-series chip (8 pins, well under $1) on the same SPI pins: a jump costs **~1 us**
instead of ~300 us, any byte can be read directly, and there are no surprise stalls. Every
problem above shrinks to nothing. The catch: 16 MB holds about **12 minutes** of tracks
(1.4 MB a minute), 64 MB ~45 minutes; and loading tracks needs a programmer (CH341A) or a
"load over USB/serial" feature, instead of copying files to a card. The firmware change is
small: a different `disk_readp()` and no FAT. Could also be **both**: SD for the library,
NOR as a cache of the loaded track.

### 6. Small change

- The screen's number formatting divides (no hardware divide: ~200 cycles each): ~1 % CPU.
  Counting down by subtraction would halve it. Not urgent.
- The engine's inner loops would run faster from RAM than from flash (no fetch wait), but
  there's no RAM to spare. Not possible here.

## Measuring

| | |
|---|---|
| `make deckbench` | the whole firmware on the chip's instruction set in qemu: flash, RAM, stack, CPU by part, the worst steps and what's in them. **The measurement.** `DECKFLAGS=-DUI_NO_WAVE` without the waveform, `-DWAVE_DIV=2` waveform at 10 fps, `-DSD_STREAM=0` the old SD reads; `-DSD_ACCESS_US=...`, `-DSD_STALL_US=... -DSD_STALL_EVERY=...` for slower cards |
| `make sim` | the deck in a window, playable (keyboard or the DJ2Go). Its CPU figure is an **estimate** (close on average, low on the worst step); its underrun count stays 0 because the PC is fast |
| `make simtest` | 31 checks that the deck behaves |
| Phase 1 (chip) | real cycles per instruction (SysTick around `app_step()`) |
| Phase 2 (card) | real access times and stalls, per card |
