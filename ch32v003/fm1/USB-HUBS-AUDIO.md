# USB hubs and audio interfaces on the FM-1

Everything we learned (2026-11-24 .. 26) getting the FM-1's USB host port to run a USB audio interface (a
Behringer UCA202, for DVS), a USB stick and a DJ2Go, alone and through hubs. The register-level detail is in
[HARDWARE.md](HARDWARE.md) ("USB: host mode"); this is the story and the rules that came out of it. Marked
*measured* where a probe or the deck showed it, *not yet tested* where it's code only.

## The short version

1. **Power the bus yourself.** The FM-1 can't supply VBUS. Use a passive powered OTG adapter (USB-C plug, USB-A
   socket, 5 V input), not a USB-C dock.
2. **An audio interface needs a hub that really powers it.** The UCA202 works straight on the port and behind a
   **Genesys GL850G** hub (05e3:0610). Behind cheap Terminus FE1.1 hubs it enumerates and streams perfectly
   but its analog input is starved, and the record never locks. *Measured.*
3. **Its green LED is the test.** On a healthy supply it lights once the host has configured it. If the deck is
   streaming from it and the LED is still off, it's the power, not the decoder.
4. **Everything runs at full speed** (12 Mbit/s): any USB 2.0 / 3 hub works and just repeats the traffic. A newer
   hub doesn't make transfers faster. It's worth buying for its power delivery.
5. **One device at a time.** The controller has a single device address (FADDR), so the audio stream is paused
   around every stick read, hub check and DJ2Go poll.
6. **Interrupts change the rules.** Streaming audio on interrupts exposed two things that polling never did:
   the SIE mailbox isn't atomic, and CON0's line bits lie. Both are fixed in `usbmsc.c`.

## Power and cables

- **The FM-1 gives no 5 V on its port.** A stick, hub or interface needs it from elsewhere. *Measured.*
- **Works:** a passive powered OTG adapter. A plain 5 V wall supply on its input is enough.
- **Doesn't work:** USB-C docks and multiport adapters (Apple's, one with HDMI and a card reader). They
  negotiate roles over USB-C PD, make themselves the host, and their hub never sees the FM-1. Their ports still
  get power, so a stick lights up, which is misleading. *Measured.*
- **A powered extension doesn't fix a starving hub.** With a beefy supply on the extension the UCA202 behind the
  cheap hubs still had its LED off. Whatever is starving it is the hub's port, not the supply upstream. *Measured.*
- **Don't backfeed a computer.** A setup with its own 5 V (a powered extension, a powered OTG adapter) can push
  that 5 V back into a computer's port if you move it there. Plug devices into the computer on their own, not
  the powered chain.

## Hubs

| Hub | VID:PID | Stick | DJ2Go | UCA202 (DVS) |
|---|---|---|---|---|
| none (straight on the OTG adapter) | | works | works | **works**: side B locked ~95 % |
| cheap 4/7-port, bus-powered (Terminus FE1.1) | 1a40:0101 | works, through two in a chain | works | **no lock**: LED off, input ~3x noisier |
| Genesys GL850G | 05e3:0610 | works, with the UCA202 streaming | works, with the UCA202 streaming | **works**: LED on, locks |

- **The cheap 7-port hubs are two 4-port chips**, the second on the first's port 1. `usbmsc.c` walks hubs within
  hubs, powers each port, resets each occupied one, and gives every device its own address. *Measured.*
- **Full speed only, so a hub just repeats.** With every device at full speed there are no split transactions:
  reads through two hubs run as fast as straight in. A USB 2.0 / 3 hub falls back to a full-speed hub under the
  FM-1, so any modern hub works, and none is faster than another. *Measured* (stick reads, iso packets).
- **Low-speed devices behind a hub** would need PRE packets. Not done; nothing we use is low speed.
- **Behind a hub the root lines say nothing** about a device being pulled: ask the hub's port
  (GET_PORT_STATUS). `usbmsc.c` does that every 500 ms, ~3 ms each time. *Measured.*
- **Opening everything through two hubs takes 1.6 s or more** (several devices, reset recovery, descriptors).
  fm1-dock's 8 s watchdog has to be fed through it. *Measured.*

### Why the UCA202 failed behind the cheap hubs

The puzzling part: the USB side was perfect behind those hubs (every packet, every frame, 0 misses), so it
looked fine until the audio was decoded. The UCA202's analog input runs off the same 5 V as its USB side and
its LED. On the starved hub:

- its green power LED stayed off, though it was enumerated and streaming;
- its input was ~3x noisier than on a computer: the timecode's loud and quiet cycles 2.8 standard deviations
  apart, against ~8 on PC recordings of the same record;
- the decoder never held a lock.

Straight on the port and behind the GL850G, the LED came on and the record locked at once. On a computer the
LED lights straight away. On the FM-1 it lights only once our host code has configured the device, so "the LED
comes on a moment after plugging in" is normal there; "the LED never comes on" is the power problem. *Measured.*

## The audio interface (UCA202)

- **08bb:2902**, TI PCM2902, USB Audio Class 1. Recording is interface 2, alt setting 1, isochronous IN on
  endpoint 0x84, max packet 196 bytes: 48 frames of 16-bit stereo (192 bytes) per millisecond at 48 kHz.
  *Measured.*
- **The sample rate is already 48 kHz.** Our SET_CUR for the rate was refused (error -13) and isn't needed.
- **Its configuration descriptor is longer than 255 bytes** (playback, record and HID interfaces with their
  alternate settings). Read the first 9 bytes, then `wTotalLength`.
- **Not every UCA202 is good.** The first one had a dead left input. Record a few seconds into a computer when a
  lock looks wrong.
- **Straight on the port** (no hub) it's found by the root-port fallback: a device there that isn't a stick is
  tried as MIDI, then as audio. `umsc_open` returns 1 for "found something, no drive".

## Streaming audio on interrupts

A blocking read (ReqPkt each frame, wait) already got 1000 packets in 1000 frames through two hubs. To free the
main loop, the stream moved to interrupts: 5024 packets in 5024 frames, 0 errors, the handler at most 9 µs.
*Measured* (`probes/usbirq.c`). What it took:

- **Ask for the next packet when one arrives** (IRQ 9, the SIE interrupt, EP3's RX bit), not from SOF. A request
  goes out at the next frame's start, so asking from the SOF interrupt gave one packet every 2 ms. SOF (IRQ 8)
  stays as a backstop: no packet for 3 frames, ask again.
- **Ping-pong buffers.** When the packet interrupt fires, the DMA is still writing. Point the next request at the
  other buffer and take the previous frame's packet. One frame of latency, no waiting.
- **Clear RxPktRdy with FlushFIFO** (RXCSR1 = 0x10). Writing 0 left packets piling up.
- **IRQ 8 and 9 share one priority word**: read-modify-write each. Both run at priority 5. The vector table has
  a write guard to unlock first (`vec_set`).

## Sharing the bus

The stick and the DJ2Go worked together before the audio did because everything was **polled from the main
loop**: each transfer set FADDR to its device, ran, and finished before the next started. Nothing touched the
controller behind the main loop's back. Streaming on interrupts broke that in three ways.

- **The SIE mailbox isn't atomic.** Every SIE register goes through CON1: write the register number, then read
  the answer. The audio interrupt landing between gave the main loop the handler's answer, e.g. a DEVCTL read
  without the session bit. The deck called the interface pulled out and restarted the session ("kicked back to
  the plug-in screen", the first time). Fix: `SIE_HOLD` turns interrupts off around each main-loop access while
  the stream is on. *Measured.*
- **CON0's line bits read SE0 while the interrupts stream**, with the device still there. That was the second
  "kicked back to the plug-in screen". Fix: while streaming, presence is the stream's own packets: none for 200
  ms means gone (`umsc_audio_present`). The line bits stay the check for a polled stick. *Measured.*
- **One FADDR.** The audio interrupt addresses the UCA202 every millisecond, so any other transfer has to take
  the bus. `a_pause` holds the stream (it lets the packet in flight finish), the transfer sets its own address,
  and `a_resume` puts the UCA202's back and restarts from a fresh buffer. Pauses nest. `umsc_read` (stick), the
  hub check in `umsc_present` and `umsc_midi` (DJ2Go) are wrapped. *Works on the device* (2026-11-26): stick, DJ2Go
  and UCA202 on the GL850G, the record driving the deck with the stick's tracks. The hold counts are still to be
  read from the log.

What a pause costs: a 4 KB stick read is a few milliseconds of missing timecode, a few times a second. The vinyl
rules call the record stopped only after 40 ms without carrier, so relative DVS rides over it. The DJ2Go poll
moved from every 8 ms to every 16 ms inside the DVS loop (each poll holds the stream ~2-3 ms of NAKs). The log
counts the holds and the longest one (`umsc_aheld`, `umsc_aheldMax`, 24 MHz ticks).

**Later:** interleave the stick's packets between audio packets within a frame instead of pausing the stream.
Full speed leaves room: a 192-byte iso packet is a small part of a 1 ms frame.

## Faster transfers

The hub isn't the limit, full speed is: 12 Mbit/s raw, and bulk packets of 64 bytes. What sets our stick read
rate is how many packets we get per frame and the waits around each one (50 µs after EP_CNT, 50 µs after
RxPktRdy, see HARDWARE.md). Shorter or overlapped waits, and more packets per frame, are where speed would
come from. *Not yet tried.*

## Debugging order when the record won't lock

Most of the time lost was spent on the decoder when the fault was upstream. Check in this order:

1. **The interface's LED.** Off while streaming: a power problem. Plug it straight in or use a hub that powers
   it.
2. **The interface itself.** Record it on a computer: both channels, clean carrier (a first UCA202 had a dead
   left input).
3. **The turntable.** Low batteries gave a wobbling speed that looked like a decoder fault.
4. **The record's side.** Serato side B is side A's code backwards. Read as side A it "locks" only backwards
   with the phase switched. `vinyl_scan` picks the side and the flags from a forwards lock in the first ~300 ms.
5. **The decoder on the chip.** `probes/tcself.c` feeds it a synthetic record. It matched the computer bit for
   bit, so it hasn't been the problem yet.

Tools: `sim/tc_wav.c` decodes a WAV for every side, level mode and flag set (freedj-3000's recordings of side A
decode at 99 %); `probes/usbirq.c` captures on the FM-1 and reports locks, bit separation and per-position
residuals across the 48-frame packet (flat: no packet fault).
