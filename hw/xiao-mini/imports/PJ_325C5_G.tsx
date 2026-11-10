import type { ChipProps } from "@tscircuit/props"

const pinLabels = {
  pin1: ["pin1"],
  pin2: ["pin2"],
  pin3: ["pin3"],
  pin4: ["pin4"],
  pin5: ["pin5"]
} as const

export const PJ_325C5_G = (props: ChipProps<typeof pinLabels>) => {
  return (
    <chip
      pinLabels={pinLabels}
      supplierPartNumbers={{
  "jlcpcb": [
    "C692438"
  ]
}}
      manufacturerPartNumber="PJ_325C5_G"
      footprint={<footprint>
        <platedhole  portHints={["pin2"]} pcbX="-3.599942mm" pcbY="0.999998mm" holeWidth="0.999998mm" holeHeight="2.0999958mm" outerWidth="1.6999966mm" outerHeight="2.7999944mm" shape="pill" />
<platedhole  portHints={["pin4"]} pcbX="5.999988mm" pcbY="-0.999998mm" holeWidth="0.999998mm" holeHeight="2.0999958mm" outerWidth="1.6999966mm" outerHeight="2.7999944mm" shape="pill" />
<platedhole  portHints={["pin3"]} pcbX="1.89992mm" pcbY="0.999998mm" holeWidth="0.999998mm" holeHeight="2.0999958mm" outerWidth="1.6999966mm" outerHeight="2.7999944mm" shape="pill" />
<platedhole  portHints={["pin5"]} pcbX="0.100076mm" pcbY="-0.999998mm" holeWidth="0.999998mm" holeHeight="2.0999958mm" outerWidth="1.6999966mm" outerHeight="2.7999944mm" shape="pill" />
<platedhole  portHints={["pin1"]} pcbX="-5.999988mm" pcbY="-0.999998mm" holeWidth="0.999998mm" holeHeight="2.0999958mm" outerWidth="1.6999966mm" outerHeight="2.7999944mm" shape="pill" />
<silkscreenpath route={[{"x":-11.200028400000065,"y":2.999994000000015},{"x":-11.200028400000065,"y":-2.999994000000015}]} />
<silkscreenpath route={[{"x":-7.200011000000131,"y":-2.999994000000015},{"x":-11.200053800000205,"y":-2.999994000000015}]} />
<silkscreenpath route={[{"x":-7.200011000000131,"y":2.999994000000015},{"x":-11.200028400000065,"y":2.999994000000015}]} />
<silkscreenpath route={[{"x":7.099960399999873,"y":3.299993400000062},{"x":7.099960399999873,"y":-4.8999902000000475}]} />
<silkscreenpath route={[{"x":-7.200011000000131,"y":-4.8999902000000475},{"x":7.099960399999873,"y":-4.8999902000000475}]} />
<silkscreenpath route={[{"x":-7.200011000000131,"y":3.299993400000062},{"x":7.099960399999873,"y":3.299993400000062}]} />
<silkscreenpath route={[{"x":-7.200011000000131,"y":3.299993400000062},{"x":-7.200011000000131,"y":-4.8999902000000475}]} />
<silkscreentext text="{NAME}" pcbX="-2.120646mm" pcbY="4.311398mm" anchorAlignment="center" fontSize="1mm" />
<courtyardoutline outline={[{"x":-11.578146000000174,"y":3.561398000000054},{"x":7.336853999999676,"y":3.561398000000054},{"x":7.336853999999676,"y":-5.168202000000065},{"x":-11.578146000000174,"y":-5.168202000000065},{"x":-11.578146000000174,"y":3.561398000000054}]} />
      </footprint>}
      cadModel={{
        objUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C692438.obj?uuid=8acad79702b4483ebab7a39f4431fee1",
        stepUrl: "https://modelcdn.tscircuit.com/easyeda_models/assets/C692438.step?uuid=8acad79702b4483ebab7a39f4431fee1",
        pcbRotationOffset: 0,
        modelOriginPosition: { x: 2.0499959000001127, y: 0.7999984000000495, z: -0.10000699999999973 },
      }}
      {...props}
    />
  )
}