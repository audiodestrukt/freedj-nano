/** A stitching via as a one-pad part, so it survives the routing strip and the router
 *  sees a GND pad on both layers (0.3 mm drill, 0.6 mm pad: JLC minimum). */
import React from "react"
export const GndVia = (props: any) => (
  <chip pinLabels={{ pin1: ["GND"] }} manufacturerPartNumber="via"
    footprint={<footprint>
      <platedhole portHints={["pin1"]} pcbX="0mm" pcbY="0mm" holeDiameter="0.3mm" outerDiameter="0.6mm" shape="circle" />
    </footprint>} {...props} />
)
