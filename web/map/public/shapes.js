// shapes.js - original vector artwork for the map icons, hand-authored as SVG path data in a -32..32 box, nose up.
// Pure data (no DOM). Each shape = {
//   body:   [path...]  filled with the track colour and outlined (1.2 px at icon-size 1),
//   detail: [path...]  thin dark strokes inside the body (windows, hatches, panel cells),
//   lines:  [path...]  thin strokes in the track colour (propellers, antennas, rotor blades, arms),
//   disc:   [path...]  translucent discs (rotor disc, drone rotors),
//   ring:   [path...]  an outer ring in the track colour (status ring),
//   leader: path       a dashed course vector ahead of the bow (ships),
//   rotates: bool }.
// icons.js draws them on a canvas for MapLibre's sprite atlas; card.js reuses them as inline SVG silhouettes.

/** Mirror a right-half polyline (nose to tail, x >= 0) into one closed symmetric path. */
function mirror(pts) {
  const right = pts.map(([x, y]) => x.toFixed(2) + "," + y.toFixed(2));
  const left = pts.slice().reverse().map(([x, y]) => (-x).toFixed(2) + "," + y.toFixed(2));
  return "M" + right.join("L") + "L" + left.join("L") + "Z";
}

const rect = (cx, cy, hw, hh) => `M${cx - hw},${cy - hh}H${cx + hw}V${cy + hh}H${cx - hw}Z`;
const circ = (cx, cy, r) => `M${cx - r},${cy}a${r},${r} 0 1,0 ${2 * r},0a${r},${r} 0 1,0 ${-2 * r},0Z`;
/** Capsule (rounded at both ends), half width hw < half height hh. */
const pod = (cx, cy, hw, hh) =>
  `M${cx - hw},${cy - hh + hw}A${hw},${hw} 0 0,1 ${cx + hw},${cy - hh + hw}V${cy + hh - hw}A${hw},${hw} 0 0,1 ${cx - hw},${cy + hh - hw}Z`;
/** A shape and its mirror image about x = 0. */
const pair = (fn, cx, ...rest) => fn(cx, ...rest) + fn(-cx, ...rest);

/**
 * Swept-wing airliner seen from above.
 * fus: fuselage half width; span: wing half span; lead0: wing root leading edge y; tipLead: tip leading edge y;
 * rootChord/tipChord; tail: tailplane half span; tailY: tailplane leading edge y; engines: [x positions] of under-wing pods.
 */
function airliner({ fus, span, lead0, tipLead, rootChord, tipChord, tail, tailY, nose = 30, tailEnd = 29.5, engines = [], podW = 1.3, podH = 3.2, extra = [] }) {
  const leadAt = (x) => lead0 + ((x - fus) / (span - fus)) * (tipLead - lead0);
  const half = [
    [0, -nose], [fus * 0.5, -nose + 0.8], [fus * 0.9, -nose + 3.6], [fus, -nose + 8],
    [fus, lead0], [span, tipLead], [span + 0.6, tipLead + tipChord], [fus + 0.3, lead0 + rootChord],
    [fus, lead0 + rootChord + 5], [fus * 0.9, tailY - 1],
    [tail, tailY + 5.4], [tail, tailY + 7.4], [fus * 0.75, tailY + 6.4],
    [fus * 0.45, tailEnd - 1.2], [0, tailEnd],
  ];
  return {
    rotates: true,
    body: [mirror(half)].concat(engines.map((x) => pair(pod, x, leadAt(x) - 0.4, podW, podH))).concat(extra),
    detail: [`M${-fus * 0.55},${-nose + 5.2}H${fus * 0.55}`],
  };
}

export const SHAPES = {
  // ---------- aircraft ----------
  narrow: airliner({ fus: 2.2, span: 22, lead0: -5, tipLead: 8, rootChord: 11, tipChord: 3, tail: 9, tailY: 20.5, engines: [8.6] }),
  wide: airliner({ fus: 3, span: 28, lead0: -8, tipLead: 9.5, rootChord: 15, tipChord: 3.6, tail: 11, tailY: 20, engines: [10.5], podW: 1.9, podH: 3.8 }),
  quad: airliner({ fus: 3.3, span: 29.5, lead0: -9.5, tipLead: 10.5, rootChord: 16, tipChord: 3.6, tail: 12, tailY: 19.5, engines: [8.2, 15.4], podW: 1.6, podH: 3.6, nose: 31 }),
  regional: airliner({ fus: 2.1, span: 16, lead0: -4, tipLead: 4, rootChord: 9, tipChord: 2.8, tail: 6.2, tailY: 21.5, nose: 29, tailEnd: 28.5, extra: [pair(pod, 3.7, 14.5, 1.4, 3.6)] }),
  bizjet: airliner({ fus: 1.7, span: 12.5, lead0: -2, tipLead: 5.5, rootChord: 7, tipChord: 2.4, tail: 5.2, tailY: 19, nose: 26, tailEnd: 25.5, extra: [pair(pod, 3.1, 13, 1.15, 3)] }),
  turboprop: {
    rotates: true,
    body: [mirror([[0, -29], [1.3, -28], [2, -24], [2.2, -18], [2.2, -6], [21, -5], [21.3, 0.8], [2.5, 2.6], [2.2, 12], [2, 20],
      [8.5, 23.6], [8.5, 26.2], [1.8, 25.4], [1, 27.5], [0, 28.5]]), pair(pod, 7.4, -4.2, 1.5, 5.6)],
    detail: ["M-1.2,-24H1.2"],
    lines: ["M-9.8,-11.6H-5.4M5.4,-11.6H9.8M-17,-6V0M17,-6V0"],
  },
  light: {
    rotates: true,
    body: [mirror([[0, -24], [1.5, -23.4], [1.95, -19], [2, -9], [17.5, -7.6], [17.5, -2.4], [2, -2.2], [1.3, 8], [0.9, 18],
      [5.8, 20.6], [5.8, 23.2], [0.8, 23], [0, 24.5]])],
    detail: ["M-1.1,-16H1.1"],
    lines: ["M-5,-25.2H5"],
  },
  lighttwin: {
    rotates: true,
    body: [mirror([[0, -24], [1.5, -23.4], [1.95, -19], [2, -9], [19, -7.8], [19, -2.2], [2, -2], [1.3, 8], [0.9, 18],
      [6, 20.6], [6, 23.2], [0.8, 23], [0, 24.5]]), pair(pod, 6.6, -6.2, 1.3, 6)],
    detail: [],
    lines: ["M-8.7,-13.4H-4.5M4.5,-13.4H8.7"],
  },
  heli: {
    rotates: true,
    body: ["M0,-14C4.4,-14 5.6,-9 5.6,-4C5.6,2 3.6,5 1.2,6V24.5H-1.2V6C-3.6,5 -5.6,2 -5.6,-4C-5.6,-9 -4.4,-14 0,-14Z", "M-4.2,22H4.2V24.6H-4.2Z"],
    detail: ["M-3.6,-9.4C-1.5,-11 1.5,-11 3.6,-9.4"],
    lines: ["M-17,-16L17,8M17,-16L-17,8"],
    disc: [circ(0, -4, 21.5)],
  },
  fighter: {
    rotates: true,
    body: [mirror([[0, -31], [1.1, -26], [1.9, -18], [2.4, -8], [13.5, 6.5], [13.5, 10], [3.2, 9.5], [3, 14], [7.8, 19.5], [7.8, 22.2],
      [2.7, 21.6], [2.2, 26], [1.2, 29.4], [0, 29.4]])],
    detail: ["M-1.1,-22C-1.1,-26 1.1,-26 1.1,-22V-15C1.1,-13 -1.1,-13 -1.1,-15Z"],
  },
  delta: {
    rotates: true,
    body: [mirror([[0, -31], [1, -25], [1.7, -18], [6.8, -13.6], [7.4, -11.8], [2.1, -10], [2.4, -4], [15, 12.4], [15, 15], [4, 14.4],
      [3.2, 19], [2.5, 26], [0, 28.6]])],
    detail: ["M-1,-21C-1,-24 1,-24 1,-21V-15H-1Z"],
  },
  transport: {
    rotates: true,
    body: [mirror([[0, -30], [1.8, -29.2], [3.2, -26], [3.6, -19], [3.6, -6], [27, -1.6], [27.4, 5.4], [3.9, 7.6], [3.6, 16], [3, 22],
      [12.6, 23.4], [12.6, 26], [2.6, 26.4], [1.6, 28.4], [0, 28.4]]),
    pair(pod, 9.6, -3.4, 1.4, 3.8), pair(pod, 17.6, -0.4, 1.4, 3.8)],
    detail: ["M-2,-24H2", "M-1.4,14V22M1.4,14V22"],
    lines: ["M-11,-8.8H-8.2M8.2,-8.8H11M-19,-5.8H-16.2M16.2,-5.8H19"],
  },
  glider: {
    rotates: true,
    body: [mirror([[0, -22], [1.1, -21], [1.3, -14], [1.4, -7], [30, -6.4], [30, -3.6], [1.4, -3.2], [1, 10], [0.7, 20], [5.6, 21.2], [5.6, 23],
      [0.6, 22.8], [0, 24]])],
    detail: ["M-0.7,-17V-11H0.7V-17"],
  },
  balloon: {
    rotates: false,
    body: ["M0,-30C13,-30 17,-18 13,-9C11,-5 7,-2 4,0H-4C-7,-2 -11,-5 -13,-9C-17,-18 -13,-30 0,-30Z", rect(0, 11, 3.2, 3.2)],
    detail: ["M0,-30V0", "M-7,-28C-10,-16 -7,-6 -2,0", "M7,-28C10,-16 7,-6 2,0"],
    lines: ["M-4,0L-3,7.8M4,0L3,7.8"],
  },
  airship: {
    rotates: true,
    body: ["M0,-29C7,-29 8.6,-15 8.6,-2C8.6,12 6,22 0,24C-6,22 -8.6,12 -8.6,-2C-8.6,-15 -7,-29 0,-29Z",
      "M0.8,15L11,25.6V28.6L0.8,22.6ZM-0.8,15L-11,25.6V28.6L-0.8,22.6Z", rect(0, 5, 2.2, 4)],
    detail: ["M-8.4,-6H8.4", "M0,-29V24"],
  },
  drone: {
    rotates: true,
    body: [pod(0, 0, 4.2, 7)],
    detail: ["M-2.2,-5.2L0,-8L2.2,-5.2"],
    lines: ["M-3.4,-4.6L-10.2,-11.4M3.4,-4.6L10.2,-11.4M-3.4,4.6L-10.2,11.4M3.4,4.6L10.2,11.4"],
    disc: [circ(-13, -13, 7.6), circ(13, -13, 7.6), circ(-13, 13, 7.6), circ(13, 13, 7.6)],
  },
  drone_rid: {
    rotates: true,
    body: [pod(0, 0, 3.6, 6)],
    detail: ["M-2,-4.4L0,-7L2,-4.4"],
    lines: ["M-3,-4L-8.4,-9.4M3,-4L8.4,-9.4M-3,4L-8.4,9.4M3,4L8.4,9.4"],
    disc: [circ(-10.6, -10.6, 6), circ(10.6, -10.6, 6), circ(-10.6, 10.6, 6), circ(10.6, 10.6, 6)],
    ring: [circ(0, 0, 28)],
  },
  uav: {
    rotates: true,
    body: [mirror([[0, -18], [1.4, -14], [2, -6], [17, -0.6], [17, 2.8], [2, 5], [0.9, 8], [0.9, 16], [5.2, 17.2], [5.2, 19], [0.8, 18.6], [0, 20]])],
    detail: ["M-1.2,-10H1.2"],
    lines: ["M-3.8,21.6H3.8"],
  },
  plane: {   // unknown aircraft: a plain dart
    rotates: true,
    body: ["M0,-26L9.5,15L0,9.5L-9.5,15Z"],
    detail: ["M0,-14V6"],
  },
  vehicle: {   // airport / ground vehicle
    rotates: true,
    body: ["M-5,-12C-5,-15 5,-15 5,-12V12C5,14 -5,14 -5,12Z"],
    detail: ["M-3.4,-9H3.4V-3H-3.4Z", "M-3.4,2H3.4V10H-3.4Z"],
  },
  obstacle: { rotates: false, body: ["M0,-14L13,10H-13Z"], detail: ["M0,-5V3", "M0,6V7.4"] },

  // ---------- ships: seen from above, bow up; `leader` is the course-over-ground vector ----------
  ship_other: {
    rotates: true,
    body: [mirror([[0, -22], [4, -16], [5, -6], [5, 18], [3.6, 22], [0, 22]])],
    detail: ["M-3,6H3V14H-3Z"],
    leader: "M0,-24.5V-31",
  },
  ship_cargo: {
    rotates: true,
    body: [mirror([[0, -22], [3.2, -18.4], [5.2, -10], [5.6, -4], [5.6, 17], [5.2, 22], [0, 22]])],
    detail: ["M-3.6,-12H3.6V-6.4H-3.6ZM-3.6,-4.6H3.6V1H-3.6ZM-3.6,2.8H3.6V8.4H-3.6Z", "M-4.2,13H4.2V20H-4.2ZM-4.2,15.6H4.2"],
    leader: "M0,-24.5V-31",
  },
  ship_tanker: {
    rotates: true,
    body: [mirror([[0, -22], [3.8, -20.4], [5.4, -13], [5.8, -6], [5.8, 18], [5.2, 22], [0, 22]])],
    detail: ["M0,-17V8", "M-3,-9H3M-3,-3H3M-3,3H3", "M-4.2,13H4.2V20H-4.2Z"],
    leader: "M0,-24.5V-31",
  },
  ship_passenger: {
    rotates: true,
    body: [mirror([[0, -22], [4.4, -17], [7, -8], [7.4, 0], [7.4, 17], [6, 22], [0, 22]])],
    detail: ["M-5.2,-9H5.2V18H-5.2Z", "M-5.2,-3H5.2M-5.2,4H5.2M-5.2,11H5.2", "M-1.8,8.4a1.8,1.8 0 1,0 3.6,0a1.8,1.8 0 1,0 -3.6,0Z"],
    leader: "M0,-24.5V-31",
  },
  ship_hsc: {   // high-speed craft: twin hulls
    rotates: true,
    body: ["M3.6,-21L6,-8V20H1.4V-8Z", "M-3.6,-21L-6,-8V20H-1.4V-8Z", "M-4.4,-4H4.4V14H-4.4Z"],
    detail: ["M-2.8,-1H2.8V4H-2.8Z"],
    leader: "M0,-24V-31",
  },
  ship_tug: {
    rotates: true,
    body: [mirror([[0, -15], [4.8, -12], [6.6, -5], [6.6, 11], [5.2, 15], [0, 15]])],
    detail: ["M-3.6,-3H3.6V7H-3.6Z", "M-4,-12.6C-2,-14.2 2,-14.2 4,-12.6"],
    leader: "M0,-17.5V-31",
  },
  ship_fishing: {
    rotates: true,
    body: [mirror([[0, -18], [3.6, -13], [4.8, -4], [4.6, 14], [3.2, 18], [0, 18]])],
    detail: ["M-3,-8H3V-1H-3Z", "M-4.2,10H4.2"],
    lines: ["M-12,-9L-3,-5M12,-9L3,-5"],
    leader: "M0,-20.5V-31",
  },
  ship_sailing: {
    rotates: true,
    body: [mirror([[0, -20], [2.6, -12], [3.4, 0], [3, 12], [1.8, 18], [0, 18]])],
    detail: ["M0,-14V13"],
    lines: ["M0,-12L5.6,3L0,3M0,4L-4.4,13L0,13"],
    leader: "M0,-22.5V-31",
  },
  ship_pleasure: {
    rotates: true,
    body: [mirror([[0, -18], [3.6, -12], [4.4, -2], [4.4, 14], [3.6, 18], [0, 18]])],
    detail: ["M-3,-2H3V10H-3Z", "M-3,-2L0,-4.4L3,-2", "M-3,16H3"],
    leader: "M0,-20.5V-31",
  },
  ship_military: {
    rotates: true,
    body: [mirror([[0, -24], [2.6, -14], [4.4, -2], [4.6, 14], [3.6, 23], [0, 23]])],
    detail: ["M-1.8,-12.5a1.8,1.8 0 1,0 3.6,0a1.8,1.8 0 1,0 -3.6,0Z", "M-2.6,-4H2.6V7H-2.6Z", "M-3,13.4a3,3 0 1,0 6,0a3,3 0 1,0 -6,0Z"],
    leader: "M0,-26.5V-31.5",
  },
  ship_sar: {
    rotates: true,
    body: [mirror([[0, -18], [3.8, -12], [4.6, -2], [4.6, 14], [3.4, 18], [0, 18]])],
    detail: ["M0,-8V3M-3.6,-2.5H3.6", "M-3,9H3V15H-3Z"],
    leader: "M0,-20.5V-31",
  },
  ship_pilot: {
    rotates: true,
    body: [mirror([[0, -13], [3, -8], [3.6, 0], [3.4, 10], [0, 12]])],
    detail: ["M-3.5,-1H3.5", "M-2,3H2V8H-2Z"],
    leader: "M0,-15.5V-31",
  },
  ship_dredger: {
    rotates: true,
    body: [mirror([[0, -22], [4, -20.4], [6, -12], [6.4, -4], [6.4, 18], [5.4, 22], [0, 22]])],
    detail: ["M-4,-10H4V8H-4Z", "M-2,-8H2V6H-2Z", "M-4.2,14H4.2V20H-4.2Z"],
    lines: ["M0,-21V-26"],
    leader: "M0,-27V-31.5",
  },

  // ---------- ground and infrastructure (drawn for the legend and for layers the hub adds later) ----------
  train: {   // locomotive and cars from above, nose up
    rotates: true,
    body: [pod(0, -10, 5.2, 18), rect(0, 12, 4.6, 7), rect(0, 25.4, 4.6, 3.2)],
    detail: ["M-3.2,-24H3.2", "M-5.2,-6H5.2"],
    lines: ["M0,-28V-30"],
  },
  cell: {   // base station: mast with three sector panels
    rotates: false,
    body: ["M0,-8L5,22H-5Z", rect(-11, -9, 2.6, 7), rect(11, -9, 2.6, 7), rect(0, -17, 2.6, 6)],
    detail: ["M-3,10H3M-4,16H4"],
    lines: ["M-17,-15A18,18 0 0,0 -17,-3M17,-15A18,18 0 0,1 17,-3"],
  },
  tower: {   // lattice tower with a beacon
    rotates: false,
    body: ["M-2.4,-18H2.4L10,24H-10Z"],
    detail: ["M-5,0H5M-7.2,12H7.2M-3.6,-9H3.6M-6,4L5,12M6,4L-5,12"],
    lines: ["M0,-18V-24M-5,-25L5,-25"],
  },
  wx: {   // weather: cloud and rain
    rotates: false,
    body: ["M-18,9A8,8 0 0,1 -14,-5A11,11 0 0,1 8,-9A9,9 0 0,1 18,9Z"],
    detail: [],
    lines: ["M-9,15L-11,22M0,15L-2,22M9,15L7,22"],
  },
  pager: {   // pager with a waves
    rotates: false,
    body: [rect(0, 6, 12, 14)],
    detail: ["M-8,-2H8V8H-8Z", "M-6,13H-2M2,13H6"],
    lines: ["M-8,-12A11,11 0 0,1 8,-12M-13,-17A18,18 0 0,1 13,-17"],
  },
  ham_qso: {   // a contact: two stations and the link
    rotates: false,
    body: [circ(-13, 12, 6), circ(13, -12, 6)],
    detail: [],
    lines: ["M-9,8C-4,-4 4,4 9,-8"],
  },

  // ---------- other layers ----------
  sonde: {   // balloon, cord, parachute, payload
    rotates: false,
    body: [circ(0, -21, 8.4), "M-8,-3C-8,-10 8,-10 8,-3Z", rect(0, 21.5, 3.8, 3.6)],
    detail: ["M-8,-3L0,-3L8,-3"],
    lines: ["M0,-12.6V-8.4M-8,-3L-1.8,18M8,-3L1.8,18M0,-3V18"],
  },
  sat: {   // bus with two three-cell solar wings and a dish
    rotates: false,
    body: [rect(0, 0, 5.4, 6.2), rect(-17.6, 0, 9.4, 4.6), rect(17.6, 0, 9.4, 4.6), circ(0, -11, 3.2)],
    detail: ["M-14.5,-4.6V4.6M-20.7,-4.6V4.6M14.5,-4.6V4.6M20.7,-4.6V4.6", "M-5.4,0H5.4"],
    lines: ["M0,-6.2V-8"],
  },
  sat_gnss: {   // navigation satellite: slim long panels, nadir antenna
    rotates: false,
    body: [rect(0, 0, 4.4, 5.2), rect(-18.8, 0, 12.4, 3.2), rect(18.8, 0, 12.4, 3.2)],
    detail: ["M-12.4,-3.2V3.2M-18.6,-3.2V3.2M-24.8,-3.2V3.2M12.4,-3.2V3.2M18.6,-3.2V3.2M24.8,-3.2V3.2", "M0,-1.4a1.4,1.4 0 1,0 0,2.8a1.4,1.4 0 1,0 0,-2.8Z"],
  },
  aprs: { rotates: false, body: ["M0,-17L17,0L0,17L-17,0Z"], detail: ["M0,-7L7,0L0,7L-7,0Z"] },
  aprs_car: {
    rotates: false,
    body: ["M-7,-16C-7,-21 7,-21 7,-16L8,16C8,21 -8,21 -8,16Z", rect(-8.4, -10, 1.2, 3), rect(8.4, -10, 1.2, 3), rect(-8.4, 11, 1.2, 3), rect(8.4, 11, 1.2, 3)],
    detail: ["M-5.4,-12H5.4L6.4,-5H-6.4Z", "M-5.4,9H5.4V13H-5.4Z"],
  },
  aprs_truck: {
    rotates: false,
    body: [rect(0, -14, 6, 6), rect(0, 7, 6.6, 14)],
    detail: ["M-4.4,-18H4.4L5,-12H-5Z", "M-6.6,-6V-4"],
  },
  aprs_house: {
    rotates: false,
    body: ["M0,-21L21,-2H14.6V19H-14.6V-2H-21Z", rect(9, -14, 2.2, 4)],
    detail: ["M-4,19V8H4V19", "M-11,4H-7V9H-11Z"],
  },
  aprs_wx: {   // weather station: cup anemometer on a mast
    rotates: false,
    body: [circ(0, -10, 4), "M-9,22H9V25H-9Z"],
    detail: ["M0,-10m-1.6,0a1.6,1.6 0 1,0 3.2,0a1.6,1.6 0 1,0 -3.2,0Z"],
    lines: ["M0,-6V22M0,-10L-9,-16M0,-10L9,-16M0,-10L0,-21"],
    disc: [circ(-9, -16, 3.2), circ(9, -16, 3.2), circ(0, -21, 3.2)],
  },
  aprs_digi: {
    rotates: false,
    body: ["M0,-6L9,22H-9Z"],
    detail: ["M-5,14H5"],
    lines: ["M-8,-10A11,11 0 0,1 8,-10M-14,-16A19,19 0 0,1 14,-16"],
  },
  aprs_balloon: {
    rotates: false,
    body: [circ(0, -13, 11), rect(0, 20, 3.4, 3.4)],
    detail: ["M0,-24V-2", "M-9,-8C-5,-3 5,-3 9,-8"],
    lines: ["M-5,-3L-2,16M5,-3L2,16"],
  },
  aprs_ship: {
    rotates: false,
    body: ["M-16,2H16L11,14H-11Z", "M-5,-8H6V2H-5Z"],
    detail: ["M-3,-5H3"],
    lines: ["M-14,19Q-10,16 -6,19T2,19T10,19"],
  },
  ham: {   // yagi on a mast
    rotates: false,
    body: [rect(0, 14, 1.8, 12)],
    detail: [],
    lines: ["M-10,-14H10M-7.4,-8H7.4M-5,-2H5M0,-18V14"],
  },
  buoy: {
    rotates: false,
    body: ["M0,-8L10,14H-10Z", rect(0, -13, 1.6, 6)],
    detail: ["M-5,4H5"],
    lines: ["M-16,19Q-12,16 -8,19T0,19T8,19T16,19"],
  },
  dsc: { rotates: false, body: ["M-16,8L-10,-2H10L16,8L12,18H-12Z"], detail: ["M-5,-2V-10H5V-2"], lines: ["M-6,-17A8,8 0 0,1 6,-17"] },
  station: {
    rotates: false,
    body: ["M0,-20L8,22H-8Z"],
    detail: ["M-4.4,8H4.4M-6.2,15H6.2"],
    lines: ["M-10,-14A13,13 0 0,0 -10,-2M10,-14A13,13 0 0,1 10,-2"],
  },
};

/** Look a shape up with a safe fallback. */
export function shapeOf(name) {
  return SHAPES[name] || SHAPES.plane;
}
