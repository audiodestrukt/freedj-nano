# The FM-1's hardware: what we found

What this project learned about the M-VAVE FM-1 by running on one (fm1-dock 0.9.0 Standard, build 5d16166),
with fm1-dock's sources (`~/sandbox/thirdparty/fm1-dock`: `firmware/hal/*.h`, `docs/RULES.md`) and JieLi's
AC79 SDK (github.com/jeffreywugz/fw-AC79_AIoT_SDK) to go on. Each fact says where it comes from:
**measured** (on our FM-1, with the probe or build named), **fm1-dock** (its sources or docs), **SDK**, or
**inferred** (fits everything seen, not proven). Dates are 2026-11.

## The chip

- **JieLi AC791N** (the package marking reads AC7911B8), two **pi32v2** cores, JieLi's own clang
  (`jieli-linux-toolchains-20260730.1`, `pi32v2/bin/clang -target pi32v2`).
- **A single-precision FPU.** *Measured* (`probes/tcbench.c`): it needs `-mcpu=r3 -mfprev1 -ffp-contract=off`
  (fm1-dock's `docs/RULES.md`; the SDK builds with them). Without them a `float` is a call into soft-float
  routines (objdump shows `call`s); with them it's inline (objdump can't decode the FPU opcodes:
  `3f e5 <unknown instruction>`). Floats live in the general registers, so the audio interrupt (which saves
  those) and the main loop may both use them. All three FM-1 builds here pass the flags.
  - The timecode decoder, 48 kHz stereo: **47 ns a frame (2.3 % of a core) in float, 79 ns (3.8 %) as a Q16
    integer port** (the Q16 one's 64-bit multiplies cost more than the FPU's instructions).
- **No `double`** (the link fails), **no 64-bit division** (`__udivdi3` isn't there: the link fails; 64-bit
  multiplication is fine), **a division by zero doesn't trap** (a wrong value). *fm1-dock RULES.md*; the 64-bit
  division one bit us (`fw/timecode.c`, caught before it ran).
- **The second core** is started by fm1-dock (the log: `cpu1: 2 cores, screened in us ...`) and idles unless a
  module hands it work: `c1_run(fn, arg)` / `c1_wait()` in `hb_host_t`, **from render() only**, one job at a
  time, which must end before render() returns, on a 2.8 KiB stack, no host calls but `ticks()`. Good for
  chunks of work alongside the audio interrupt (an MP3 frame), not for a long-running task. *fm1-dock.*
  Not used by the deck yet.
- **Hardware FFT**: the SDK has `vc_hw_fft` (`apps/common/jl_math/jl_fft.c`). *SDK*, not tried. The SDK's
  `mp3_decoder` is a closed library (as far as we can tell a software decoder).
- **Time**: TIMER4 counts at **24 MHz** (`hb_host_t.ticks()`, `p_ticks()` in probes). *fm1-dock, measured.*

## Memory and flash

- Module RAM **0x01C30000, 272 KiB** (`.data` + `.bss`); `.ram_text` **0x01C04000, 16 KiB** (code that must run
  while the flash is busy, called through a pointer); a probe's RAM at 0x01C60000. *fm1-dock hb_module.h.*
- Flash 1 MB in all. With CS-1, SCINTILLA, OBERVAVE, FREEDJ, FREEDJ USB and FREEDJ STICK installed: **56 KiB
  free, the largest gap 20 KiB**. *Measured* (deploy output).
- The log ring, the crash record and a probe's result are in **NOINIT: they survive a watchdog or crash reset,
  not a power-off.** *Measured* (a power cycle lost a probe's result). So: let the watchdog restart it.
- **Redeploying a module that is running can crash the FM-1 (a red screen),** probably its instruction cache
  holding the old code; it recovers by itself (the watchdog), the new module in flash. *Measured, cause inferred.*
  Stopping the host's streaming first helped; deploying the same module while it runs still sometimes does it.

## Interrupts, the watchdog

- Vectors in RAM at **0x01C7FE00** (128 of them); `FM1_ICFG(n)` at 0x1EEF100 + 4 (n >> 3), 4 bits a source
  (enable, priority 0..7); an ISR needs an asm entry (save psr / rets / reti and r0-r3, `call`, restore,
  `csync`, `rti`: clang won't emit the `csync`). *fm1-dock fm1_irq.h, fm1_isr.S.*
- **The top of RAM is write-protected**: fm1-dock's guards (`hal/fm1_guard.h`) make any core-0 write into
  **0x01C7FD50..0x01C7FFFF** (boot info, the mailbox, **the vector table**) a CPU exception (vector 1, DBG_MSG
  bit 13; the fault is reported a few instructions after the write). To attach a vector: `DBG_WR_EN`
  (0x1EEE240) = 0xE7 if its bit 0 is clear (unlock), clear bit 2 of `C0_WR_LIMIT_EN` (0x1EEE348), write the
  vector, set the bit back, `DBG_WR_EN` = 0xE7 again (lock), as fm1-dock's own BLE start does. *Measured*:
  `probes/usbirq.c` crashed this way until it did. Other guards: stack guard bands, PC limits (code may run only
  from the host's code, `.ram_text` and the deployed modules; a probe's RAM while it runs). *fm1-dock.*
- **fm1-dock's crash record** (`fm1_crash`, NOINIT, 0x01C7C000 in build 5d16166): magic "CRSH", count, vector,
  pc, rets, emu, dbg, sp, psr, icfg, uptime ms, the last four branch targets (ETM), early. The log shows only the
  pc; a probe can read the rest (the ETM trail showed the handler had never run).
- **TIMER5 (IRQ 63), 10 kHz**: the key scan, the millisecond count, and fm1-dock's USB (`usb_poll()` every
  0.5 ms; `uac_service()` instead when it interrupts the audio). *fm1-dock platform.c.*
- **The audio interrupt (ALNK0, IRQ 11): 256 frames, 5.8 ms a block** at 44.1 kHz; render() runs in it.
  *fm1-dock.* Too slow a clock for USB audio (1 ms packets).
- **The USB controller has two interrupts** (*SDK* wl82 `hwi.h` / `usb.h`), both unused by fm1-dock (it polls):
  **IRQ 8, start of frame**, enabled by `CON0` bit 10 (SOFIE), its pending flag `CON0` bit 13, cleared by writing
  `CON0` bit 12 (CLR_SOF_PND); **IRQ 9, the SIE's** (`CON0` bit 11, SIEIE; the SIE's INTRUSBE / INTRTXE / INTRRXE
  choose its sources). *Measured* (`probes/usbirq.c`): IRQ 9 with only the SIE's SOF source on never fired
  (SIEIE off); **IRQ 8 fired every 1000 us exactly** (7127 in a run, none over 1.2 ms apart, at priority 5 over the
  audio interrupt), the handler (an isochronous packet a frame) at most 8 us. A module's handler must be in
  `.ram_text` (no flash while it's written).
- **The watchdog bites after ~8 s** (`fm1_wdt_arm(0x0D)`); fm1-dock's main loop feeds it. A module that
  blocks longer needs to feed it itself: modules get no `wdt_feed` (probes do), so FREEDJ STICK writes P33
  directly, as `fm1_wdt_feed()` does: interrupts off, P33 `WDT_CON` (0x80) |= 0x40 through
  `P33_CON` 0x13E08 / `P33_DAT` 0x13E0C (three bytes: `0x20` = op "or", `0x80`, `0x40`). *Measured*: opening
  a stick through two hubs took long enough to trip it before this.

## The screen

- **240 x 240 colour panel on a 12 MHz SPI, no vsync** (fm1-dock doesn't turn its TE on). fm1-dock's `blit()`
  is **synchronous**: it waits until every byte has gone. A 240 x 30 band (14 KB) takes ~10 ms, the 44-row
  overview ~14 ms. *Measured.* So a big blit stalls the main loop: send it in small strips (2 rows, ~0.6 ms a
  pass) and send a waveform's two halves together (mirrored row pairs from the middle out), or it tears.
- The top line (`HB_TOP` = 24 rows) is the host's unless the module takes the screen (`HB_F_SCREEN`). Drawing
  text straight onto the panel flickers (cleared, then written, visibly); a canvas (`cv_begin` / `cv_text` /
  `cv_blit`) drawn off screen and sent at once doesn't. *Measured.*

## Buttons, knobs, LEDs

- HOME opens fm1-dock's picker, but **the module keeps running under it** (and keeps playing); `release()` comes
  only when another module starts. *fm1-dock, measured.* fm1-dock's picker loads with HOME / SEL / PLAY.
- LEDs: on / off read fine; fm1-dock's "glow" (2) is a fast pulse fm1-emu shows as a flicker.

## Audio

- Out: the codec behind fm1-dock's render() (stereo, 44.1 kHz, Q15 into the mix); USB audio to a computer
  (fm1-dock's UAC, device mode). *fm1-dock.*
- **In: no audio input** that we know of (the jacks: audio out and TRS MIDI). For timecode, a USB audio
  interface on the FM-1's own port (below). *fm1-dock*, the hardware not opened.

## USB: device mode (fm1-dock's)

- The FM-1 appears as **"Felucca"** (MIDI port `Felucca:Felucca MIDI 1`), not "FM-1". *Measured.*
- fm1-dock collects SysEx in the 2 kHz poll into **one frame**: a frame that starts arriving before the main
  loop has taken the last is dropped. That set the pace of streaming tracks from a computer (FREEDJ USB:
  `tools/fm1host.py`'s window of 4 frames on the FM-1). *Measured.*

## USB: host mode (ours)

**Read first** (JieLi's AC79 SDK): the register bits in `include_lib/driver/cpu/wl82/asm/usb.h` (CON0's PHY_ON ..
SOFIE, SIEIE, the pending / clear bits) and the IRQ numbers in `.../wl82/asm/hwi.h`; how its host drivers use the
controller, in `apps/common/usb/host/`: `audio.c` (iso in / out from the endpoint interrupt, re-armed there:
`usb_h_ep_read_async(..., 1)`), `usb_bulk_transfer.c` (bulk, async), `hid.c` / `usbnet.c` (interrupt endpoints),
`usb_ctrl_transfer.c` (control, `usb_write_faddr`), `usb_host.c` (`usb_sie_enable`, `usb_h_isr_reg`). The
register-level functions they call (`usb_h_ep_read_async`, `usb_h_ep_config`, ...) are in a closed library: only
their headers (`include_lib/driver/device/usb/usb_phy.h`). What's below is what we found where those stop.

The controller is a Mentor MUSB-style SIE behind JieLi's own registers. **Everything below is measured** with
`probes/usbhost.c`, `usbstick.c`, `usbaudio.c` and FREEDJ STICK, unless it says otherwise.

### Power and cables

- **The FM-1 can't supply VBUS.** The device needs 5 V from elsewhere.
- **Works:** a **passive powered OTG adapter** (USB-C plug, USB-A socket, a 5 V input; a plain 5 V wall
  supply is fine). Behind it, a **plain USB-A hub** (one bought hub turned out to be two Terminus
  1a40:0101 4-port chips, the second on the first's port 1).
- **Doesn't work:** USB-C docks / multiport adapters (Apple's, and a dock with HDMI and a card reader), with or
  without a charger. They negotiate roles over USB-C PD, which the FM-1 can't, make themselves the host, and
  their hub never sees the FM-1. Their own ports still get power (a stick lights up), which is misleading.

### Registers

| | |
|---|---|
| `CON0` 0x11800 | bit 0 PHY_ON, 1 LOW_SPEED, 2 USB_NRST, **3 TM1 (must be set or SOF never starts)**, 4 CID (1 = device), 5 VBUS, 16 DM_SE, 17 DP_SE (the lines' state) |
| `CON1` 0x11804 | the SIE mailbox: write `reg << 8 \| value`, read with bit 14, done = bit 15 |
| `EP0_CNT` 0x11808, `EP_CNT(n)` 0x11808 + 4 n | writing a count starts the TX DMA |
| `EP0_ADR` 0x11818 | EP0's DMA buffer (**write-only**: reads back 0) |
| `EP_TADR(n)` 0x1181C + 8 (n-1), `EP_RADR(n)` 0x11820 + 8 (n-1) | EP1..3 TX / RX DMA buffers (EP2 RX 0x11828, EP3 RX 0x11830); EP4 at 0x11834.. (*fm1-dock fm1_usb.h*) |
| `IO_CON0` 0x51000 | 4/5 D+/D- pull-downs (host), 6/7 pull-ups, 9/10 input enables; 0x630 in host mode |

The SIE's registers through the mailbox (indexed MUSB): FADDR 0, POWER 1 (bit 3 reset), INTRUSB 6,
INTRUSBE 11, FRAME 12/13, INDEX 14, DEVCTL 15 (0x03 session + host request; 0x40 FSDev, 0x20 LSDev),
TXMAXP 16, CSR0 / TXCSR1 17, TXCSR2 18, RXMAXP 19, RXCSR1 20, RXCSR2 21, COUNT 22/23, TXTYPE 24, RXTYPE 26
(protocol << 4 | endpoint: 0x10 iso, 0x20 bulk, 0x30 interrupt), RXINTERVAL 27 (bulk: the NAK limit in
frames). There seems to be no per-endpoint function address: writing mailbox 0x80 / 0x84 (MUSB's TXFUNCADDR /
RXFUNCADDR for EP0) acted like writing FADDR (*measured*, `stick: hub trace 9f...`; *inferred*: the numbers wrap).

### What it takes

- **Host mode**: as the SDK's `usb_h_sie_init` (`CON0 = 0`, `IO_CON0` pull-downs on), then `CON0 |= 0x2D`
  (PHY_ON | USB_NRST | **TM1** | VBUS). Without bit 3 the bus never starts (no SOF).
- **A device**: DEVCTL = 0x03, wait for FSDev / LSDev; then **restart the session with it there** (DEVCTL 0,
  100 ms; 0x03, 200 ms), a **50 ms reset** (POWER bit 3), 50 ms. Only then does it answer at address 0.
- **The DMA needs time**: wait **50 us after `EP_CNT` before TxPktRdy** (else the setup packet goes unanswered)
  and **50 us after RxPktRdy before reading the buffer**.
- **Flush the RX FIFO (RXCSR1 = 0x10) after each packet**, or the next one lands after it (a stick's CSW
  arrived glued to its data).
- **One address at a time**: FADDR is global (no per-endpoint function address), so the target of every
  transfer is set before it and only one device is talked to at once. Hubs, a stick, a DJ2Go and a UCA202
  share the bus this way.
- **Speeds**: everything is full speed (12 Mbit/s). Hubs then just repeat (no split transactions):
  **reads through two hubs are as fast as straight in.** Low-speed devices behind a hub would need PRE
  packets (not done).
- **A pulled device** shows as SE0 (CON0 bits 16 / 17 both 0) for 100 ms. **Behind a hub the root lines say
  nothing**: ask the hub's port (GET_PORT_STATUS). Checking that every 500 ms costs ~3 ms.
- **Configuration descriptors can pass 255 bytes** (a UCA202's: playback, record and HID interfaces, their
  alternate settings). Read the first 9 bytes, then `wTotalLength`.
- **Isochronous IN from interrupts** (*measured*, `probes/usbirq.c`, a UCA202 behind two hubs): **5024 packets in
  5024 frames, 0 errors, the handler at most 9 us**, the main loop free. What it took:
  - **A request goes out at the next frame's start.** Asking from the SOF interrupt (IRQ 8) gave one packet every
    2 ms: the packet lands ~0.15 ms into the frame after the request, after that SOF's look. Re-ask **when the
    packet arrives**: IRQ 9 with `CON0` bit 11 (SIEIE) and INTRRX1E bit 3 (EP3), INTRRX1 (SIE register 4,
    read-cleared) saying which. IRQ 8 stays as a backstop (no packet for 3 frames: ask again).
  - **The DMA is still writing when the packet interrupt comes** (the blocking path waited 50 us): copying at once
    gave corrupt audio. Two buffers: point the next request at the other one, take the previous frame's packet
    (written a millisecond ago). One frame of latency, no waiting.
  - **Clear RxPktRdy with FlushFIFO (0x10).** Writing 0 instead left packets piling up (counts of ~680 bytes).
  - IRQ 8 and 9 share one ICFG word (bits 3:0 and 7:4 of 0x1EEF104): set each with a fresh read-modify-write.
- **With the stream on interrupts, the main loop's SIE accesses must be atomic** (*measured*, FREEDJ STICK's DVS,
  2026-11-26). Every SIE register goes through CON1: the register written, then the answer read. The handler's
  own access landing between gave the main loop the handler's answer: a DEVCTL read without the session bit (the
  deck called the interface "pulled out" within a second, and restarted the session). `usbmsc.c` holds the
  interrupts off around the pair while the audio streams (`SIE_HOLD`).
- **CON0's line bits (16-17) read SE0 with a streaming device still there** (*measured*): fine as a pulled-out
  check for a polled stick, not with the SOF / SIE interrupts on. The stream's own packets are the check instead
  (none for 200 ms: gone; `umsc_audio_present`).
- **Isochronous IN works** (blocking): a UCA202 on EP3 (RXTYPE 0x10 | ep, RXINTERVAL 1), ReqPkt each frame: **1000 packets
  of 192 bytes in 1000 frames (1.000 s), 0 errors, 0 timeouts** (48 kHz stereo 16-bit), through two hubs.
- Bulk with a NAK limit of 2 frames (a DJ2Go's MIDI IN, polled): a poll with nothing to say costs ~2 ms.

### Devices seen

| Device | VID:PID | Notes |
|---|---|---|
| USB stick (30 GB) | 346d:5678 | bulk-only mass storage, 512-byte blocks; reads fine straight in and through two hubs |
| plain 4-port hub | 1a40:0101 | Terminus FE 1.1; the bought "hub" was two of them in series |
| Numark DJ2Go | 15e4:0027 | USB-MIDI, a bulk IN endpoint |
| Behringer UCA202 | 08bb:2902 | TI PCM2902, UAC1: record interface 2 alt 1, iso IN EP 4 (0x84), max packet 196, 2 ch 16-bit, 48 kHz (its default: our SET_CUR of the rate was refused, error -13, and wasn't needed) |
| (DVS, *measured* `probes/usbirq.c`) | | Serato Control Vinyl through a UCA202's line in, **the UCA202 straight on the FM-1's port: locks** - side B flags 0 locked forwards 3818 ms of 4 s, runs of 3836 bits, speed 0.985-1.018, the side scan (`vinyl_scan`) picking side B; the ping-pong and the wait-60-us packet handling alike, the sine's residual flat across the 48-frame packet (no packet fault). **Behind the two cheap hubs (and an extension with its own supply) the UCA202's power LED stays off and its input is ~3x noisier** (bit levels 2.8 sd apart, against 8 on the PC): no lock. Its USB side keeps working (0 misses), so it looks fine until you decode: put an audio interface on the port itself, or on a hub known to give it power. Also: the record's **side** matters (B is A's code backwards; read as A it "locks" backwards only), and a first UCA202 had a dead left input |

### fm1-dock's own USB, while a module uses the port

FREEDJ STICK takes the port by writing fm1-dock 0.9.0's `usb` struct (0x01C10F68: `up` +0, `config` +1,
`detached` +64), which gates its poll, `uac_service`, replies and the 1 s `usb_start` retry, and checks 16 bytes
of fm1-dock's code at 0x0200134C first (the build it was found in). The computer link is gone while the module
has the port. **This ties it to that fm1-dock build**; the right fix is an upstream API (see README's to-do).

## Measuring

- The deck's CPU meter counts its main-loop time, **waits included** (USB transfers, the panel's SPI): 97 %
  there isn't 97 % of compute. The stick build logs the split every 5 s (`cpu: busy / usb / lcd %, 5 s`,
  three bytes) to fm1-dock's log (`tools/dock.py logs`).
- **Measured** (FREEDJ STICK playing from a stick behind two hubs, a DJ2Go and a UCA202 on them too):

  | | of the time |
  |---|---|
  | busy (the meter) | 96 % |
  | waiting on USB (stick reads, the DJ2Go poll, the hub check) | 39 % |
  | waiting on the panel's SPI (`blit()`) | 48 % |
  | **the deck's own work** (decoding, keylock, drawing) | **~9 %** |
  | idle (not playing) | 7 % |

  The DJ2Go poll alone (up to ~2 ms of NAKs every 8 ms) is ~25 %. Opening the stick through the hubs (all
  three devices found) took 1.6 s.
