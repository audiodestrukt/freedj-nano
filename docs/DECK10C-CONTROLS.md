# The 10-cent deck: controls (undecided)

Written 2026-11-10 to come back to. Nothing here is built yet; today's firmware and sim still
use the plan in [DECK10C.md](DECK10C.md): one EC11 for jog and browse. The pins are draft:
check them against the CH32V003 datasheet before wiring.

## What the deck needs

- **Jog**: nudge while playing, scrub (audible, both ways) while paused. Wants resolution and
  smoothness; turned fast when scratching.
- **Browse**: scroll the track list, push to load. Wants detents (one row per click).
- **PLAY, CUE, SHIFT** buttons. CUE held + PLAY pressed must work (a CDJ move: keep playing
  from a cue preview), so two buttons at once must read correctly.
- **Pitch**: a pot, on an ADC pin.

## The pin problem

The CH32V003F4P6 has 18 I/O pins; PD1 is the programming pin and PD7 is reset, so 16. The
current plan uses all 16:

| Use | Pins | |
|---|---|---|
| SD card (SPI) | PC5, PC6, PC7, PC0 | 4 |
| OLED (I2C) | PC1, PC2 | 2 |
| PWM audio (TIM1 CH1, CH2) | PD2, PA1 | 2 |
| EC11: A, B, push | PD3, PD4, PC3 | 3 |
| PLAY, CUE, SHIFT | PC4, PA2, PD0 | 3 |
| Pitch pot | **PD5** (ADC A5; it's USART TX, which the deck doesn't use: it only receives) | 1 |
| MIDI in (the bridge) | PD6 (USART RX) | 1 |
| spare | | 0 |

Almost every ADC-capable pin is taken by something else, so the pot goes on PD5 (DECK10C's
draft said only "an ADC pin"). Any change that adds a pin has to free one.

## Jog options

### A. EC11 (the current plan)

A detented rotary knob with push, ~$0.30. 20-30 clicks and 80-120 counts a turn. Fine for
browsing, coarse and clicky for a jog. 3 pins. Read by polling in the main loop (an EC11's
edges are slow enough even in the worst 5 ms step).

### B. AS5600 magnetic encoder (what the other decks use)

A magnet on the platter's axle over the AS5600: 4096 positions a turn, absolute, smooth.
~$1-2 for a board plus the magnet.

- **No pins**: it's I2C at 0x36, sharing the OLED's bus (0x3C). The screen is already sent in
  8 separate strips a frame, so the angle (2 bytes, ~0.1 ms) is read between strips: ~160
  readings a second, enough for any scratch (it would take 80 turns a second to lose track).
- The jog delta is the angle difference, wrapped at 4096, scaled like the P4's feel table.
- Its analog out (into an ADC) or PWM out would cost a pin and lose resolution: not worth it.
- Costs: a few bytes of RAM, a little flash, an I2C read in the HAL.

### C. Optical: a slotted 3D-printed wheel and two slot sensors (cheapest)

How cheap controllers do it (probably the DJ2Go too: see the test below). One ring of N slots
in the printed jog wheel; two slot-type opto interrupters, offset so their signals are a
quarter cycle apart (quadrature); which one changes first gives the direction.

- **Counted in hardware**: TIM2 has an encoder mode that counts every edge of both channels
  with no CPU at all. Its inputs are **PD3 and PD4**, the pins the EC11 has now. The firmware
  just reads the counter: fast scratches can't lose counts.
- **Resolution**: 4N counts a turn (every edge of both channels). Printed slots and bars need
  ~1-1.5 mm each: a ~70 mm wheel fits ~70-100 slots = ~300-400 counts. Less than the AS5600,
  plenty for a jog (CONFIGS.md considered 100-600 slots for an optical jog).
- **Cost**: two slot sensors + resistors, ~$0.20-0.60.
- **2 pins**, so something has to give (see "Freeing pins").

Designing the wheel:

- N slots, slots and bars the **same width** (50/50) for clean edges. Slot pitch = 360 / N
  degrees.
- The sensors needn't sit side by side: put them **(k + 1/4) slot pitches apart**, any whole
  k. With N = 72 (5 degree pitch), 2 1/4 pitches = 11.25 degrees: room for two separate
  sensors in one printed bracket, with the offset designed in.
- A dual-channel sensor (both channels in one package) has a fixed spacing: pick it first and
  make the slot pitch suit it.
- Print the slotted ring **black and >= 1 mm thick** (thin or light plastic passes infrared).
  Keep the slots aligned with the sensor's beam. Uneven slot widths are tolerated: quadrature
  only needs the order of edges.
- No touch sensing either way (B or C): "hand on the platter" would need capacitive sensing
  on the wheel, later.

### Comparison

| | EC11 | AS5600 | Optical, printed |
|---|---|---|---|
| Counts a turn | 80-120 | 4096 | 4N: ~300-400 |
| Feel | clicks | smooth | smooth |
| Cost | ~$0.30 | ~$1-2 | ~$0.20-0.60 |
| Pins | 3 | 0 (shared I2C) | 2 (TIM2) |
| CPU | polling | an I2C read between screen strips | none (hardware counter) |
| Mechanics | off the shelf | magnet centred on the axle | the wheel *is* the encoder; sensor bracket |

## Browse: a thumbwheel on the side

A side-mounted thumbwheel encoder (like a mouse wheel; Alps, Panasonic and many clones) is
electrically the same as an EC11: two contacts + usually a push switch, 3 pins, the same
firmware. Look for detents (one row per click) and a push (to load). Many mouse-wheel encoders
have no push: then a small button beside it does the same job.

With a separate jog (B or C), browse no longer needs SHIFT + turn: the thumbwheel scrolls,
its push loads. SHIFT is then free for other things (SHIFT + push = keylock, SHIFT + wheel =
tempo range, ...).

## Freeing pins: buttons on a resistor ladder

All the buttons on one ADC pin: each button switches its own resistor to ground against a
pull-up, the voltage says which are pressed.

- **Binary-weighted** (1 : 2 : 4 : 8) so every **combination** reads differently, which CUE +
  PLAY needs. 1 % resistors; the 10-bit ADC has room for 4 buttons. Needs a check of the actual
  levels (the sim can model it).
- Read every main-loop pass (an ADC conversion is a few microseconds), debounced as now.
- 4 buttons (PLAY, CUE, SHIFT, the thumbwheel's push) on PC4 frees PA2 and PD0, which then
  take the thumbwheel's two contacts.

## Pin plans

| | EC11 (now) | AS5600 jog + thumbwheel | Optical jog + thumbwheel |
|---|---|---|---|
| SD, OLED, PWM, pot, MIDI | as above (10) | as above (10) | as above (10) |
| Jog | EC11 A, B: PD3, PD4 | on the I2C bus | TIM2: PD3, PD4 |
| Browse | EC11 (SHIFT + turn) | thumbwheel A, B: PD3, PD4 | thumbwheel A, B: PA2, PD0 |
| Buttons | PLAY, CUE, SHIFT, push: PC4, PA2, PD0, PC3 | PLAY, CUE, SHIFT, push: PC4, PA2, PD0, PC3 | ladder on PC4: PLAY, CUE, SHIFT, push |
| Spare | 0 | 0 | PC3 (1) |

## Things to find out

- **The DJ2Go's jog**: with the bridge running, mark the wheel and turn it one full turn,
  slowly; count the steps in the bridge log (each jog message is +1 or -1). Steps / 4 = slots,
  if it reports every edge. That gives a target that's known to feel right. Or open it (a few
  screws underneath): an optical jog shows a thin slotted disc with a small sensor block
  straddling its edge.
- **Slot sensors**: pick a part (single or dual channel), then its spacing sets the wheel.
- **Ladder levels**: model the four-button ladder in the sim before choosing resistors.

## When it's decided

Firmware: a jog source (EC11 polling / AS5600 over I2C / TIM2 counter) chosen at build time,
browse on its own encoder, the ladder pad. Sim: an emulated jog for each (a slotted wheel
with a chosen N, an AS5600 angle), the thumbwheel on the mouse wheel. Then the pin table in
DECK10C.
