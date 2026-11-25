# The deck on an M-VAVE FM-1

The same firmware as the CH32V003 and the PY32 (`../fw/`, unchanged), as a module for
[fm1-dock](https://github.com/joakimeriksson/fm1-dock) on M-VAVE's FM-1 pocket synth, and in
[fm1-emu](https://github.com/joakimeriksson/fm1-emu), the emulator of it. fm1-dock is a
community base firmware that keeps the FM-1's USB, installer and rescue, and runs one module
at a time; `hal_fm1.c` is this deck's `hal.h` on fm1-dock's module interface
(`firmware/dock/hb_module.h`).

Status (2026-11-25): runs in fm1-emu, plays, jogs, browses, loads. Two builds: `make fm1` with a short
clip in the module's flash, and `make fm1-usb` with the tracks streamed from the computer over USB
(`tools/fm1host.py`, below): four full tracks play with no missed blocks in fm1-emu. Not tried on a real
FM-1 yet.

![The deck in fm1-emu](../../docs/media/fm1-deck.png)

## Controls

| FM-1 | The deck |
|---|---|
| SELECT | the jog (nudge while playing, scrub while paused) |
| FX + SELECT | the track list |
| EDIT | the encoder's push: keylock; loads in the list; FX + EDIT: tempo range |
| KNOB1 | tempo (the pitch pot) |
| SEQ | CUE (left of PLAY, as on a CDJ) |
| PLAY | PLAY |
| MASTER | volume (fm1-dock's) |
| MIDI IN | the DJ2Go map, as over the CH32's bridge |
| HOME held 1.5 s | fm1-dock's picker (another module) |

PLAY lights while playing, SEQ while previewing the cue. In fm1-live's window: Space PLAY, `0` SEQ (CUE),
`1` FX (SHIFT), `5` EDIT, Left / Right SELECT; so `1` held + Left / Right browses, `5` loads.

## How it maps

- **Screen:** `fw/ui.c` still talks to an SSD1306. `hal_i2c_send` keeps its 128 x 64 picture and
  each page that changed is laid out on the colour panel: the text pages scaled to 240 x 15, the
  waveform's three at 30 px each coloured by loudness (deep blue to white) and dimmed behind the
  playhead, the overview drawn from the track's own 128 columns at 44 px (orange ahead, brown
  behind, a white playhead, a yellow cue), the track list at 25 px a row.
- **Top line:** the deck's own (`HB_F_SCREEN`): its name and a CPU meter, the share of the time its
  main-loop work takes (decoding, keylock, the screen) on the 24 MHz counter. fm1-dock's own "%"
  there counts only the audio callback, ~0.4 % here. About 20 % while playing in fm1-emu's default
  timing; `FM1_STRICT=1` is the chip's.
- **Audio:** nano-lite at 22050 Hz into a 64-block ring (93 ms); `render()`, in fm1-dock's
  audio interrupt, plays it at 44.1 kHz with linear interpolation, the same on both channels.
- **Main loop:** `draw()`, which fm1-dock calls often, runs `app_step()`.
- **Tracks (`make fm1`):** the FM-1 has 1 MB of flash in all and no card slot. A clip in the
  module's own flash, on the simulator's emulated SD card, as the PY32's `MEMCARD=1` build does:
  about 8 s in a module's 272 KiB. `FM1_DEMO=file.wav FM1_DEMO_S=seconds` picks it.

## Tracks over USB (`make fm1-usb`, `make fm1-usb-live`)

The computer is the card: `tools/fm1host.py IMAGE` serves a card image from `tools/sdimage.py` to
the FREEDJ USB module over fm1-dock's USB-MIDI (`make fm1-usb-live` builds the image from
`FM1_MUSIC=` files, installs the module, opens fm1-live with its USB cable in and runs the host).

- The module asks with CCs on channel 16 (102..105 a block number, 106 a run, 108 a one-off,
  107 hello); the computer answers with fm1-dock MODULE frames (SysEx, `data()`).
- Before the deck starts, the blocks that aren't track data (FAT, directory, TRACKS.BIN's
  entries) are pinned. The track data comes into a 128 KB cache, 120 blocks (2.8 s) ahead of the
  deck; the waveform's peaks into a 32 KB cache of their own. A paused deck's position is fetched
  too, so PLAY starts with audio. A block not there plays silence.
- Each frame is half a block with its block number, followed by 32 events of filler (fm1-dock holds
  one frame at a time and drops one that starts arriving too soon). Up to 16 frames are in flight;
  nothing is resent by the computer: the module sees gaps in what came and asks again.
- Rates: the deck plays 43 blocks a second (22 KB/s). fm1-dock's USB driver takes about one
  64-byte packet every half millisecond on the chip (one a millisecond in fm1-emu), ~2x what the
  deck needs with pipelining; more would need fm1-dock itself changed (its CDC channel, or polling
  faster). The module reports its CPU, main-loop passes and misses to the host each second.
- fm1-live's computer sends one USB packet a frame; `FM1_USB_OUT_PER_FRAME=8` (a local change to
  fm1-emu, crates/fm1-soc/src/usb.rs, not upstream) lets it send more, as a computer does.

## A USB stick (next)

The FM-1's USB controller (a Mentor MUSB core) can be a host, and JieLi's AC79 SDK has host and
mass-storage code. Untested: whether host mode works through the FM-1's USB-C port, and the stick's
5 V (the FM-1 almost certainly can't supply it: an OTG adapter with power pass-through, or a powered
hub, would). The plan: an fm1-dock probe (runs once from RAM, never touches the flash) that counts
down on the screen, switches the controller to host, and shows whether a stick appears and its
VID / PID. A stick would then stand in for the SD card's blocks, the deck above unchanged.

## Build and run

Needs fm1-dock, JieLi's Linux toolchain and fm1-emu:

```
git clone https://github.com/joakimeriksson/fm1-dock ~/sandbox/thirdparty/fm1-dock
(cd ~/sandbox/thirdparty/fm1-dock && tools/get_toolchain.sh)     # or set JIELI_TOOLCHAIN
git clone https://github.com/joakimeriksson/fm1-emu ~/sandbox/thirdparty/fm1-emu
(cd ~/sandbox/thirdparty/fm1-emu && cargo build --release -p fm1-live)   # Rust 1.95 or newer
mkdir -p ~/sandbox/thirdparty/fm1-dock/reference && cd ~/sandbox/thirdparty/fm1-dock/reference && \
  gh release download v0.9.0 -R joakimeriksson/fm1-dock -p fm1-dock-0.9.0-empty.fwsc

make fm1        # build/fm1/freedj.dock
make fm1-emu    # headless: build/fm1/emu/panel.png, audio.wav, log.txt (FM1_KEYS=, FM1_MS=)
make fm1-live   # a window: click the panel, Space is PLAY
```

The first `fm1-emu` or `fm1-live` installs the module through the emulated MIDI IN, which at
31250 baud takes ~80 s of emulated time (~35 s here), into `build/fm1/flash.bin`; later runs
boot from that with the module in place, until the module changes. `FM1_STRICT=1 make fm1-emu`
times it as the chip would.

The variables (`FM1_DOCK`, `FM1_LIVE`, `FM1_FWSC`, `JIELI_TOOLCHAIN`) default to the paths above.

`make fm1-emu` with a module this size needs fm1-dock's `tools/dock.py` to hand fm1-live the
deploy script as a file (`FM1_BENCH_KEYS=@file`) rather than in the environment, where 196 KB
doesn't fit: a five-line change in the local clone, not yet upstream.

## On a real FM-1 (untried)

Install fm1-dock (its web installer, or `tools/fm1_install.py` with its package), then over USB:

```
cd ~/sandbox/thirdparty/fm1-dock && uv run --with python-rtmidi tools/dock.py deploy \
  ~/sandbox/audiodestrukt/freedj-nano/ch32v003/build/fm1/freedj.dock
```

Over USB that takes seconds. Holding OCT- at power-on always reaches fm1-dock's rescue.

## Still to do

- The USB stick probe (above).
- Browsing on its own encoder (PRESETS), no FX to hold.
- The 27 keys: hot cues, loops.
- A layout of its own at 240 x 240 instead of the OLED's pages laid out.
