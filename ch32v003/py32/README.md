# The deck on a PY32F030

The same firmware as the CH32V003 (`../fw/`, behind `../fw/hal.h`), on Puya's PY32F030x8:
Cortex-M0+, 48 MHz, 64 KB flash, 8 KB RAM, about the same price. `hal_py32.c` is the chip's
half: registers through Puya's CMSIS header, no vendor HAL.

Status (2026-11-14), on an EmbedFire PY32F030K28U6 board:

| | |
|---|---|
| Clock 48 MHz (HSI 24 MHz trimmed, PLL x2) | works |
| PLAY / CUE / SHIFT keys, EC11 encoder | works |
| SSD1306 OLED on I2C1 | works (blocking, not DMA yet) |
| AS5600 magnetic jog, same I2C bus | works |
| SD card on SPI1, cheap blue module | works |
| Clip in flash instead of a card (`MEMCARD=1`) | works |
| Audio out (TIM1 PWM + DMA on PA8) | **not yet**: blocks are consumed on a timer, silently |
| Pitch pot (ADC), USART bridge, OLED by DMA | not yet: pitch is centred |

## PY32 or CH32V003?

| | CH32V003 | PY32F030 |
|---|---|---|
| Core | RV32EC, 48 MHz | Cortex-M0+, 48 MHz, hardware multiply |
| Flash / RAM | 16 KB / **2 KB** | 64 KB / **8 KB** |
| deckbench: average CPU | 39 % | 35 % |
| deckbench: worst step (100 % = audio drops) | **135 %: drops** | **51 %** (8-block ring) |
| RAM used | 2007 of 2048 B | 2331 of 8192 B |
| Firmware on a real chip | not yet (sim and qemu only) | running, all but audio |
| Programmer | WCH-LinkE or PicoRVD | any CMSIS-DAP (debugprobe on a Pico) |

The PY32's lead is the RAM: it holds an 8-block audio ring, which rides over the SD card's
slow reads that make the CH32 drop audio. It also leaves room for the to-do list in
[../../docs/DECK10C-PERF.md](../../docs/DECK10C-PERF.md). The CH32 needs those fixes to stop
dropping; the PY32 doesn't. Both run the same `fw/`, so the choice can wait for the audio
output: that's the part still untried on either chip.

## Wiring

Only header pins (PA9, PA10, PF0 and PF1 aren't brought out on this board).

| Part | Its pin | PY32 board |
|---|---|---|
| PLAY | | PA5 (KEY1 on the board) |
| CUE | | PA6 (KEY2) |
| SHIFT | to GND | PA7 |
| EC11 encoder | A / C / B | PA11 / GND / PA12 |
| | push | PA15, other side to GND |
| SSD1306 OLED | SCL / SDA | PB6 / PB7 |
| | VCC / GND | 3V3 / GND |
| AS5600 breakout | SCL / SDA | PB6 / PB7 (shared with the OLED) |
| | VCC / GND / DIR | 3V3 / GND / GND |
| | OUT, GPO | unconnected |
| SD card module | SCK / MISO / MOSI / CS | PB3 / PB4 / PB5 / PB0 |
| | VCC / GND | 5 V (see below) / GND |
| Audio, later | | PA8 (TIM1 CH1) |
| LED2 | | PA2: on once the firmware runs |

All inputs are to ground, with the chip's pull-ups: no resistors. 10 nF from PA11 and PA12 to
GND helps if the EC11 bounces.

**The SD module.** The cheap blue one (AMS1117 regulator + 74LVC125) works as is with **5 V
on VCC**: its regulator makes the card's 3.3 V and the buffer's outputs are 3.3 V. With the
board powered from the Pico, take 5 V from the Pico's VBUS (pin 40, live while it's on USB).
On 3V3 alone the regulator starves the card (~2.2 V) and reads fail at random; a bare 3.3 V
socket breakout goes straight on 3V3. Its 74LVC125 never releases MISO, which doesn't matter
while the card is alone on the SPI bus. The card: SDHC 4-32 GB, FAT32, with a `TRACKS.BIN`
(`python3 tools/pack_lite.py a.wav b.mp3 -o TRACKS.BIN`).

**The AS5600.** A *diametric* magnet (magnetised across its face: 4 x 2 mm), centred over the
chip, 0.5-2 mm away. `watch.py` shows "too weak" until it's close enough, and the jog does
nothing until the chip reports a magnet. 128 counts a turn (`JOG_DIV`, default 32; an EC11
gives 80); backwards: DIR to 3V3, or `-DJOG_DIR=-1`. The EC11 keeps working alongside it.

## Programmer: debugprobe on a Pico 2

Raspberry Pi's debugprobe firmware, rebuilt with the SWD pins moved to the header:
`~/sandbox/debugprobe`, `include/board_pico_config.h` (PROBE_PIN_OFFSET 27, reset 26), the
uf2 at `~/.local/opt/debugprobe/debugprobe_on_pico2_gp27.uf2`.

| Pico 2 | PY32 board |
|---|---|
| GP27 (SWCLK) | PA14 |
| GP28 (SWDIO) | PA13 |
| GP26 (reset) | RST (NRST) |
| GND | GND |
| 3V3 out, pin 36 | 3V3 (powers the board) |
| VBUS, pin 40 | the SD module's VCC |

Keep PA13 / PA14 for the probe: nothing else goes on them.

## Build and flash

Needs the Arm GNU toolchain, pyOCD (target `py32f030x8`, built in) and Puya's CMSIS headers:

```
git clone https://github.com/IOsetting/py32f0-template ~/sandbox/thirdparty/py32f0-template
make py32                  # build/py32fw/deck.hex, the SD card build
make flash-py32            # build, flash with pyOCD, reset
make flash-py32 MEMCARD=1  # no card: a 1.8 s clip in flash (DEMO=file.wav DEMO_S=seconds)
make flash-py32 UI_FLIP=0  # the OLED the other way up (1, the default, is as it sat on the bench)
python3 py32/jump.py       # start it (below)
python3 py32/watch.py      # the AS5600 as the firmware sees it
```

`MEMCARD=1` packs the clip into a FAT32 image and keeps only its non-zero sectors in flash
(`tools/memcard.py`), served by the simulator's emulated card (`../sim/emu/sd_emu.c`): the
same firmware path as a real card, ~44 KB of flash.

### It boots into Puya's ROM, not the firmware

This board's option bytes (OPTR 0xBEAA, nBOOT1 = 1) start the ROM's serial bootloader when
BOOT0 is high, and BOOT0 (PF4) floats here. Until it's tied down, `jump.py` starts the
firmware from the probe: maps flash at 0, points VTOR, SP and PC at it, runs. It lasts until
the next reset or power cycle.

The fix: **PF4 to GND** (right header, between PB7 and PB8). Then it boots the firmware on its
own.

## In the simulators

`make sim CHIP=py32`, `make simtest CHIP=py32` and `make deckbench CHIP=py32` run the same
firmware as a PY32 (an 8-block ring, its cycle costs) on the computer: see `../README.md`.

## Still to do

1. **Audio**: TIM1 PWM on PA8 (CH1) fed by circular DMA, as on the CH32; CH2 (the low byte)
   needs a header pin: PA9 isn't brought out, so remap it or go 1-channel.
2. The pitch pot on the ADC (PA0).
3. I2C by DMA, so the OLED doesn't hold up the jog and the audio fill.
4. The USART bridge (DJ2Go over the computer) on header pins.
