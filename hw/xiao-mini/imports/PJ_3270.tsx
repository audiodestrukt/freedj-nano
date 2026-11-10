import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin2: ["pin2"],
  pin3: ["pin3"],
  pin4: ["pin4"],
  pin5: ["pin5"],
  pin6: ["pin6"]
} as const

export const PJ_3270 = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      supplierPartNumbers={{
  "jlcpcb": [
    "C381134"
  ]
}}
      manufacturerPartNumber="PJ_3270"
      footprint={<footprint>
        <hole pcbX="-0.100076mm" pcbY="-3.350006mm" diameter="1.1999976mm" />
<hole pcbX="-0.100076mm" pcbY="2.649982mm" diameter="1.1999976mm" />
<platedhole  portHints={["pin2"]} pcbX="2.320036mm" pcbY="2.949956mm" holeWidth="1.1999976mm" holeHeight="0.3999992mm" outerWidth="1.5999968mm" outerHeight="0.7999984mm" pcbRotation="90deg" shape="pill" />
<platedhole  portHints={["pin3"]} pcbX="2.320036mm" pcbY="-3.750056mm" holeWidth="1.1999976mm" holeHeight="0.3999992mm" outerWidth="1.5999968mm" outerHeight="0.7999984mm" pcbRotation="90deg" shape="pill" />
<platedhole  portHints={["pin4"]} pcbX="-2.320036mm" pcbY="-4.649978mm" holeWidth="1.1999976mm" holeHeight="0.3999992mm" outerWidth="1.5999968mm" outerHeight="0.7999984mm" pcbRotation="90deg" shape="pill" />
<platedhole  portHints={["pin5"]} pcbX="-2.320036mm" pcbY="-1.050036mm" holeWidth="1.1999976mm" holeHeight="0.3999992mm" outerWidth="1.5999968mm" outerHeight="0.7999984mm" pcbRotation="90deg" shape="pill" />
<platedhole  portHints={["pin6"]} pcbX="-2.320036mm" pcbY="4.649978mm" holeWidth="1.1999976mm" holeHeight="0.3999992mm" outerWidth="1.5999968mm" outerHeight="0.7999984mm" pcbRotation="90deg" shape="pill" />
<silkscreenpath route={[{"x":2.919983999999886,"y":-6.350000000000023},{"x":-3.080004000000031,"y":-6.350000000000023},{"x":-3.080004000000031,"y":-8.34999599999992}]} />
<silkscreenpath route={[{"x":2.919983999999886,"y":-6.350000000000023},{"x":2.919983999999886,"y":-8.34999599999992}]} />
<silkscreenpath route={[{"x":-2.5800050000000283,"y":-6.350000000000023},{"x":2.419984999999997,"y":-6.350000000000023}]} />
<silkscreenpath route={[{"x":2.919983999999886,"y":3.5400995999999623},{"x":2.919983999999886,"y":5.699988599999983},{"x":-3.080004000000031,"y":5.699988599999983},{"x":-3.080004000000031,"y":-6.350000000000023},{"x":-2.5800050000000283,"y":-6.350000000000023},{"x":-2.5800050000000283,"y":-8.34999599999992},{"x":2.419984999999997,"y":-8.34999599999992},{"x":2.419984999999997,"y":-8.339988400000038}]} />
<silkscreenpath route={[{"x":2.919983999999886,"y":-3.159912400000053},{"x":2.919983999999886,"y":2.35981240000001}]} />
<silkscreenpath route={[{"x":2.419984999999997,"y":-8.34999599999992},{"x":2.419984999999997,"y":-6.350000000000023},{"x":2.919983999999886,"y":-6.350000000000023},{"x":2.919983999999886,"y":-4.340199600000005}]} />
<silkscreentext text="{NAME}" pcbX="-0.061976mm" pcbY="6.7404mm" anchorAlignment="center" fontSize="1mm" />
<courtyardoutline outline={[{"x":-3.3218759999999747,"y":5.990400000000022},{"x":3.1979240000000573,"y":5.990400000000022},{"x":3.1979240000000573,"y":-8.58120000000008},{"x":-3.3218759999999747,"y":-8.58120000000008},{"x":-3.3218759999999747,"y":5.990400000000022}]} />
      </footprint>}
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C381134.obj?uuid=dcd0e4a6b25440fda9febde0c77614a8",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C381134.step?uuid=dcd0e4a6b25440fda9febde0c77614a8",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 0.06700520000003962, y: 1.325003699999911, z: -0.000004000000000115023 },
      }}
      {...props}
    />
  )
}