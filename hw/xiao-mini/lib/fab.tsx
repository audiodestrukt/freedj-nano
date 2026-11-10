/**
 * Fab DRC presets — board/subcircuit routing props that make the GENERATED
 * geometry legal for a given fab, so `tsci export -f kicad_pcb` needs no KiCad
 * rule cleanup. Spread into EVERY <board> and <subcircuit> (each subcircuit has
 * its own autorouter, so the board-level prop alone won't reach within-block
 * traces/vias):  <subcircuit {...JLCPCB} name=… pcbX=… pcbY=…>
 *
 * Pairs with the KiCad-side rules/<fab>.kicad_dru shipped in the pcb-layout skill
 * (same numbers, for checking an already-routed board).
 */
export const JLCPCB = {
  minTraceWidth: "0.15mm",     // JLC 2-layer standard min track (recommend ≥0.15)
  defaultTraceWidth: "0.2mm",  // what the autorouter actually draws
  viaHoleDiameter: "0.3mm",    // JLC min via drill
  viaPadDiameter: "0.6mm",     // → 0.15mm annular ring (≥ JLC's 0.13 min)
}

// Other fabs — same shape; swap which preset you spread.
export const PCBWAY = {
  minTraceWidth: "0.15mm", defaultTraceWidth: "0.2mm",
  viaHoleDiameter: "0.3mm", viaPadDiameter: "0.6mm",
}
export const OSHPARK = {        // 6mil / 0.33mm-drill / 0.2mm-clearance service
  minTraceWidth: "0.1524mm", defaultTraceWidth: "0.2032mm",
  viaHoleDiameter: "0.33mm", viaPadDiameter: "0.7mm",
}
