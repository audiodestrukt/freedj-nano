/**
 * Seeed XIAO (ESP32-C6, also S3/C3/RP2040/RP2350: same pinout and size), socketed on two
 * 7-pin 2.54 mm rows, 15.24 mm apart. Origin = module centre; the USB end is +Y.
 * Top view, USB up: left row D0..D6 from the USB end, right row 5V, GND, 3V3, D10, D9, D8, D7.
 */
import React from "react"

const ROW = 7.62, PITCH = 2.54, TOP = 3 * PITCH   // first pin 7.62 mm above centre
const LEFT = ["D0", "D1", "D2", "D3", "D4", "D5", "D6"]
const RIGHT = ["V5", "GND", "V3V3", "D10", "D9", "D8", "D7"]

const pinLabels: Record<string, string[]> = {}
LEFT.forEach((l, i) => (pinLabels[`pin${i + 1}`] = [l]))
RIGHT.forEach((l, i) => (pinLabels[`pin${i + 8}`] = [l]))

const hole = (n: number, x: number, y: number) => (
  <platedhole key={n} portHints={[`pin${n}`]} pcbX={`${x}mm`} pcbY={`${y}mm`}
    holeDiameter="1mm" outerDiameter="1.7mm" shape="circle" />
)

export const Xiao = (props: any) => (
  <chip
    pinLabels={pinLabels}
    manufacturerPartNumber="Seeed_XIAO_socketed"
    footprint={<footprint>
      {LEFT.map((_, i) => hole(i + 1, -ROW, TOP - i * PITCH))}
      {RIGHT.map((_, i) => hole(i + 8, ROW, TOP - i * PITCH))}
      <silkscreenpath route={[{ x: -8.75, y: -10.5 }, { x: 8.75, y: -10.5 }, { x: 8.75, y: 10.5 }, { x: -8.75, y: 10.5 }, { x: -8.75, y: -10.5 }]} />
      <silkscreentext text="USB" pcbX="0mm" pcbY="9mm" fontSize="1mm" anchorAlignment="center" />
      <silkscreentext text="XIAO" pcbX="0mm" pcbY="0mm" fontSize="1.2mm" anchorAlignment="center" />
      <courtyardoutline outline={[{ x: -8.9, y: -10.6 }, { x: 8.9, y: -10.6 }, { x: 8.9, y: 10.6 }, { x: -8.9, y: 10.6 }, { x: -8.9, y: -10.6 }]} />
    </footprint>}
    {...props}
  />
)
