/**
 * freedj-nano XIAO mini player: a carrier board for a socketed Seeed XIAO ESP32-C6.
 * Matches firmware/main/board.h, CONFIG_NANO_BOARD_XIAO_C6, OLED layout:
 *   D0 button ladder (ADC)   D1 I2S BCK   D2 I2S WS    D10 I2S DOUT  -> PCM5102A -> 3.5 mm jack
 *   D4 SDA  D5 SCL -> SSD1306 OLED module (4-pin header)
 *   D8 ENC A  D9 ENC B  D3 ENC push -> EC11
 *   D7 MIDI RX (UART from the bridge, 3-pin header)       D6 spare
 * 72 x 46 mm, 2 layers, GND pour on the bottom. Board frame: origin at the centre, +Y up.
 * Edges: USB-C (XIAO) top-left, jack left, LiPo right, buttons along the bottom.
 */
import React from "react"
import { JLCPCB } from "./lib/fab"
import { Xiao } from "./lib/Xiao"
import { GndVia } from "./lib/GndVia"
import { PCM5102APWR as Dac } from "./imports/PCM5102APWR"
import { PEC11R_4015F_S0024 as Ec11 } from "./imports/PEC11R_4015F_S0024"
import { PJ_325C5_G as Jack } from "./imports/PJ_325C5_G"
import { TS_1187A_B_A_B as Button } from "./imports/TS_1187A_B_A_B"
import { S2B_PH_SM4_TB_LF__SN_ as Jst } from "./imports/S2B_PH_SM4_TB_LF__SN_"

const W = 72, H = 46
const XIAO = { x: -22, y: 12.5 }
const DAC = { x: -6, y: -14 }
const ENC = { x: 27, y: 10 }
const BTN_Y = -17
const BUTTONS = [
  // name, x, ladder resistor (none = 0 R, straight to the ladder), firmware mV
  { n: "PLAY", x: 4, r: null },     // 0 mV
  { n: "CUE", x: 12, r: "2.2k" },  // 595 mV
  { n: "KEY", x: 20, r: "4.7k" },  // 1055 mV
  { n: "LIST", x: 28, r: "10k" },  // 1650 mV
]
// PJ-325C5-G contacts: CHECK against the part before ordering (no datasheet drawing found).
// Assumed CUI-style: 1 sleeve, 2 tip (L), 3 ring (R), 4/5 switches (unused).
const JACK_PIN = { GND: "pin1", L: "pin2", R: "pin3" }

export default () => (
  <board width={`${W}mm`} height={`${H}mm`} layers={2} autorouter="sequential-trace" {...JLCPCB}>
    {/* M2 mounting holes */}
    {[[-33.5, 20.5], [33.5, 20.5], [-33.5, -20.5], [33.5, -20.5]].map(([x, y], i) => (
      <hole key={i} name={`H${i + 1}`} diameter="2.2mm" pcbX={`${x}mm`} pcbY={`${y}mm`} />
    ))}

    {/* ---------- XIAO (socketed), USB-C out of the top edge ---------- */}
    <Xiao name="U2" pcbX={XIAO.x} pcbY={XIAO.y} />
    <trace from="U2.V3V3" to="net.V3V3" /><trace from="U2.GND" to="net.GND" />
    <trace from="U2.D1" to="net.BCK" /><trace from="U2.D2" to="net.LRCK" /><trace from="U2.D10" to="net.DIN" />
    <trace from="U2.D4" to="net.SDA" /><trace from="U2.D5" to="net.SCL" />
    <trace from="U2.D8" to="net.ENC_A" /><trace from="U2.D9" to="net.ENC_B" /><trace from="U2.D3" to="net.ENC_SW" />
    <trace from="U2.D0" to="net.LADDER" /><trace from="U2.D7" to="net.MIDI_RX" />
    <GndVia name="V4" pcbX={`${XIAO.x + 9.4}mm`} pcbY={`${XIAO.y + 5.08}mm`} />
    <trace from="V4.pin1" to="U2.GND" />
    <capacitor name="C1" capacitance="10uF" footprint="0805" pcbX="-11mm" pcbY="15mm" pcbRotation={90} />
    <trace from="C1.pin1" to="net.V3V3" /><trace from="C1.pin2" to="net.GND" />

    {/* ---------- PCM5102A: hardware mode, SCK to GND (PLL from BCK), I2S format ---------- */}
    <Dac name="U1" pcbX={DAC.x} pcbY={DAC.y} />
    <trace from="U1.BCK" to="net.BCK" /><trace from="U1.LRCK" to="net.LRCK" /><trace from="U1.DIN" to="net.DIN" />
    <trace from="U1.SCK" to="net.GND" /><trace from="U1.FMT" to="net.GND" />
    <trace from="U1.FLT" to="net.GND" /><trace from="U1.DEMP" to="net.GND" />
    <trace from="U1.AGND" to="net.GND" /><trace from="U1.DGND" to="net.GND" /><trace from="U1.CPGND" to="net.GND" />
    <trace from="U1.AVDD" to="net.V3V3" /><trace from="U1.DVDD" to="net.V3V3" /><trace from="U1.CPVDD" to="net.V3V3" />
    {/* XSMT high = unmuted */}
    <resistor name="R1" resistance="10k" footprint="0402" pcbX={`${DAC.x - 1}mm`} pcbY={`${DAC.y + 7.6}mm`} pcbRotation={90} />
    <trace from="R1.pin1" to="U1.XSMT" /><trace from="R1.pin2" to="net.V3V3" />
    {/* GND stitching for the top-row tie-low pins (DGND, FMT, SCK/FLT): the pour is on the bottom */}
    {[[-2.4, 4.2], [-0.3, 4.2], [2.6, 4.2]].map(([dx, dy], i) => (
      <React.Fragment key={i}>
        <GndVia name={`V${i + 1}`} pcbX={`${DAC.x + dx}mm`} pcbY={`${DAC.y + dy}mm`} />
        <trace from={`V${i + 1}.pin1`} to="net.GND" />
      </React.Fragment>
    ))}
    {/* charge pump flying cap, VNEG, LDO output */}
    <capacitor name="C2" capacitance="2.2uF" footprint="0603" pcbX={`${DAC.x - 1.65}mm`} pcbY={`${DAC.y - 5.4}mm`} />
    <trace from="C2.pin1" to="U1.CAPP" /><trace from="C2.pin2" to="U1.CAPM" />
    <capacitor name="C3" capacitance="2.2uF" footprint="0603" pcbX={`${DAC.x + 0.4}mm`} pcbY={`${DAC.y - 7.2}mm`} />
    <trace from="C3.pin1" to="U1.VNEG" /><trace from="C3.pin2" to="net.GND" />
    <capacitor name="C4" capacitance="1uF" footprint="0402" pcbX={`${DAC.x - 1.6}mm`} pcbY={`${DAC.y + 5.7}mm`} pcbRotation={90} />
    <trace from="C4.pin1" to="U1.LDOO" /><trace from="C4.pin2" to="net.GND" />
    {/* supply decoupling */}
    <capacitor name="C5" capacitance="100nF" footprint="0402" pcbX={`${DAC.x - 4.6}mm`} pcbY={`${DAC.y - 2.9}mm`} pcbRotation={270} />
    <trace from="C5.pin1" to="U1.CPVDD" /><trace from="C5.pin2" to="net.GND" />
    <capacitor name="C6" capacitance="100nF" footprint="0402" pcbX={`${DAC.x - 4.6}mm`} pcbY={`${DAC.y + 2.9}mm`} pcbRotation={90} />
    <trace from="C6.pin1" to="U1.DVDD" /><trace from="C6.pin2" to="net.GND" />
    <capacitor name="C7" capacitance="100nF" footprint="0402" pcbX={`${DAC.x + 2.6}mm`} pcbY={`${DAC.y - 4.6}mm`} />
    <trace from="C7.pin1" to="U1.AVDD" /><trace from="C7.pin2" to="net.GND" />
    <capacitor name="C8" capacitance="10uF" footprint="0805" pcbX={`${DAC.x + 5}mm`} pcbY={`${DAC.y - 3}mm`} pcbRotation={90} />
    <trace from="C8.pin1" to="net.V3V3" /><trace from="C8.pin2" to="net.GND" />

    {/* ---------- output RC (470 R + 2.2 nF, as on the PCM5102A modules) -> jack ---------- */}
    <resistor name="R2" resistance="470" footprint="0402" pcbX="-14mm" pcbY="-16mm" />
    <resistor name="R3" resistance="470" footprint="0402" pcbX="-14mm" pcbY="-18.5mm" />
    <trace from="R2.pin2" to="U1.OUTL" /><trace from="R3.pin2" to="U1.OUTR" />
    <trace from="R2.pin1" to="net.JL" /><trace from="R3.pin1" to="net.JR" />
    <capacitor name="C9" capacitance="2.2nF" footprint="0402" pcbX="-16.5mm" pcbY="-20.5mm" pcbRotation={90} />
    <capacitor name="C10" capacitance="2.2nF" footprint="0402" pcbX="-12mm" pcbY="-20.5mm" pcbRotation={90} />
    <trace from="C9.pin1" to="net.JL" /><trace from="C9.pin2" to="net.GND" />
    <trace from="C10.pin1" to="net.JR" /><trace from="C10.pin2" to="net.GND" />
    <Jack name="J1" pcbX="-24.8mm" pcbY="-12mm" />
    <trace from={`J1.${JACK_PIN.L}`} to="net.JL" /><trace from={`J1.${JACK_PIN.R}`} to="net.JR" />
    <trace from={`J1.${JACK_PIN.GND}`} to="net.GND" />

    {/* ---------- OLED module: GND VCC SCL SDA (the usual 0.96"/1.3" order: check yours) ---------- */}
    <pinheader name="J2" pinCount={4} pitch="2.54mm" footprint="pinrow4" pcbX="2mm" pcbY="21mm"
      pinLabels={["GND", "VCC", "SCL", "SDA"]} />
    <trace from="J2.pin1" to="net.GND" /><trace from="J2.pin2" to="net.V3V3" />
    <trace from="J2.pin3" to="net.SCL" /><trace from="J2.pin4" to="net.SDA" />
    <silkscreentext text="OLED  GND VCC SCL SDA" pcbX="2mm" pcbY="18.6mm" fontSize="0.8mm" anchorAlignment="center" />

    {/* ---------- EC11 jog / browse with push ---------- */}
    <Ec11 name="SW5" pcbX={ENC.x} pcbY={ENC.y} />
    <trace from="SW5.A" to="net.ENC_A" /><trace from="SW5.B" to="net.ENC_B" /><trace from="SW5.C" to="net.GND" />
    <trace from="SW5.pin1" to="net.ENC_SW" /><trace from="SW5.pin2" to="net.GND" />
    <trace from="SW5.MT1" to="net.GND" /><trace from="SW5.MT2" to="net.GND" />
    {[["R4", "ENC_A", 17.5, 4], ["R5", "ENC_B", 17.5, 6], ["R6", "ENC_SW", 17.5, 16]].map(([n, net, x, y]) => (
      <React.Fragment key={n as string}>
        <resistor name={n as string} resistance="10k" footprint="0402" pcbX={`${x}mm`} pcbY={`${y}mm`} />
        <trace from={`${n}.pin1`} to={`net.${net}`} /><trace from={`${n}.pin2`} to="net.V3V3" />
      </React.Fragment>
    ))}

    {/* ---------- four buttons on one ADC pin: 10k pull-up, each button to GND via its resistor ---------- */}
    <resistor name="R7" resistance="10k" footprint="0402" pcbX="-17mm" pcbY="-3mm" />
    <trace from="R7.pin1" to="net.LADDER" /><trace from="R7.pin2" to="net.V3V3" />
    {BUTTONS.map((b, i) => (
      <React.Fragment key={b.n}>
        <Button name={`SW${i + 1}`} pcbX={`${b.x}mm`} pcbY={`${BTN_Y}mm`} />
        <trace from={`SW${i + 1}.C`} to="net.GND" />
        {b.r
          ? <>
              <resistor name={`R${8 + i}`} resistance={b.r} footprint="0402" pcbX={`${b.x}mm`} pcbY={`${BTN_Y + 4.5}mm`} />
              <trace from={`R${8 + i}.pin1`} to="net.LADDER" /><trace from={`R${8 + i}.pin2`} to={`SW${i + 1}.A`} />
            </>
          : <trace from={`SW${i + 1}.A`} to="net.LADDER" />}
        <silkscreentext text={b.n} pcbX={`${b.x}mm`} pcbY={`${BTN_Y - 4.2}mm`} fontSize="1mm" anchorAlignment="center" />
      </React.Fragment>
    ))}

    {/* ---------- MIDI in from the bridge (3V3 UART) ---------- */}
    <pinheader name="J3" pinCount={3} pitch="2.54mm" footprint="pinrow3" pcbX="26mm" pcbY="21mm"
      pinLabels={["GND", "RX", "V3V3"]} />
    <trace from="J3.pin1" to="net.GND" /><trace from="J3.pin2" to="net.MIDI_RX" /><trace from="J3.pin3" to="net.V3V3" />
    <silkscreentext text="MIDI GND RX 3V3" pcbX="26mm" pcbY="18.6mm" fontSize="0.8mm" anchorAlignment="center" />

    {/* ---------- LiPo: JST-PH in, wired by hand to the XIAO's BAT pads (underneath it) ---------- */}
    <Jst name="J4" pcbX="31mm" pcbY="-6mm" pcbRotation={90} />
    <trace from="J4.pin1" to="net.VBAT" /><trace from="J4.pin2" to="net.GND" />
    <pinheader name="J5" pinCount={2} pitch="2.54mm" footprint="pinrow2" pcbX="23.5mm" pcbY="-6mm" pcbRotation={90}
      pinLabels={["VBAT", "GND"]} />
    <trace from="J5.pin1" to="net.VBAT" /><trace from="J5.pin2" to="net.GND" />
    <silkscreentext text="BAT to XIAO" pcbX="23.5mm" pcbY="-9.6mm" fontSize="0.8mm" anchorAlignment="center" />
    <silkscreentext text="freedj-nano XIAO mini" pcbX="2mm" pcbY="-6mm" fontSize="1.2mm" anchorAlignment="center" />
  </board>
)
