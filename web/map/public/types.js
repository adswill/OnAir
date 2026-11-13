// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// types.js - pure mapping from ICAO type code / ADS-B emitter category / AIS ship type to an icon shape name.
// No DOM; covered by web/map/test/types.test.js. Shape names are drawn in shapes.js.

const set = (s) => new Set(s.split(/\s+/).filter(Boolean));

const QUAD = set("A388 A38F A124 A225 A346 A345 A343 A342 B741 B742 B743 B744 B748 B74D B74R B74S B703 B720 B52 E3 E3TF E3CF E8 K35R K35T KC10X");
const WIDE = set(`A306 A30B A310 A332 A333 A337 A338 A339 A359 A35K B762 B763 B764 B772 B773 B77L B77W B788 B789 B78X
  DC10 MD11 L101 IL86 A3ST KC10 KC46 B77X IL96 E6`);
const NARROW = set(`A318 A319 A320 A321 A19N A20N A21N A220 BCS1 BCS3 B731 B732 B733 B734 B735 B736 B737 B738 B739 B37M B38M B39M B3XM
  B752 B753 B712 B722 B721 MD80 MD81 MD82 MD83 MD87 MD88 MD90 DC91 DC92 DC93 DC94 DC95 C919 SU95 T154 T134 TU34 YK42 BE40 P8 P8A E7 E737`);
const REGIONAL = set(`E170 E175 E190 E195 E75L E75S E290 E295 E135 E145 E45X E35L F70 F100 F28 RJ70 RJ85 RJ1H CRJ1 CRJ2 CRJ7 CRJ9 CRJX
  B461 B462 B463 AR85 A148 SU9 SSJ`);
const BIZJET = set(`CL60 CL30 CL35 GLEX GL5T GL7T GLF4 GLF5 GLF6 GLF3 GLF2 FA7X FA8X FA50 FA20 FA10 FA6X C25A C25B C25C C25M C510 C525 C550 C560
  C56X C680 C68A C700 C750 LJ35 LJ45 LJ60 LJ75 H25B H25C HDJT E55P E50P PRM1 SF50 EA50 ASTR WW24 C650 BE40`);
const TURBOPROP = set(`AT43 AT44 AT45 AT46 AT72 AT73 AT75 AT76 DH8A DH8B DH8C DH8D DHC6 DHC7 DHC5 SF34 SB20 B190 B350 BE20 BE30 BE99 BE9L BE9T
  JS31 JS32 JS41 E120 PC12 PC6T C208 C212 CN35 D228 D328 F27 F50 F60 SW3 SW4 AN24 AN26 AN32 L410 L188 TBM7 TBM8 TBM9 P180 P46T M600 PAY1
  PAY2 PAY3 PAY4 DC3 DC3T SH36 SH33 ATP BN2P DHC2 DHC3 NOMA B36T AC90 AC95 MU2 SF3 P3 E2 C2 KC2`);
const TRANSPORT = set("C130 C30J A400 C17 C5 C5M IL76 C27J C295 AN12 AN22 AN124 A124X BLCF KC135 K35");
const HELI = set(`R22 R44 R66 EC20 EC25 EC30 EC35 EC45 EC55 EC75 EC13 H125 H130 H135 H145 H160 H175 H225 AS32 AS33 AS35 AS50 AS55 AS65 A109 A119 A139
  A169 A189 AW09 B06 B06T B105 B204 B205 B212 B214 B222 B407 B412 B427 B429 B430 B505 S55 S58 S61 S64 S76 S92 S70 H60 UH1 UH60 H47 H53 H64 H500
  MD52 MD60 MD90X EXPL NH90 NH9 KA32 KA27 MI8 MI17 MI24 MI26 MI2 MI34 LYNX PUMA SCOU GAZL W3 EN28 EN48 CABR BK17 HUCO TIGR AH64 CH47 H46`);
const FIGHTER = set(`F16 F15 F18 FA18 F14 F22 F35 F117 F5 TORN SU27 SU30 SU34 SU35 SU57 MIG2 MIG3 MG29 A10 AV8B B1 B1B F4 F104 EA6B EA18
  T38 HAWK M346 L39 L159 YK30 T45 T37 T33 AJET MB33 A7 A29X`);
const DELTA = set("EUFI RFAL GRIF JAS39 MIR2 M2K F102 A4 B2 VULC");
const GLIDER = set("GLID ASK2 ASK21 ASW2 ASW20 ASW27 ASG29 LS4 LS6 LS8 DG10 DG80 DG50 DISCUS DUOD JANU NIMB VENT U2 SR71");
const BALLOON = set("BALL GBAL HAB");
const AIRSHIP = set("ARSH ZEPP BLMP");
const DRONE = set("DRON QUAD");
const UAV = set("UAV MQ9 MQ1 RQ4 RQ1 MQ4 HRON");
const TRAINER = set("T6 TEX2 PC21 PC9 PC7 TUCA EMB314 A29 SR22X");
const LIGHTTWIN = set("BE55 BE58 BE60 BE76 PA34 PA44 PA31 C310 C337 C340 C402 C414 C421 DA42 DA62 AEST P68 PA30 BE95 BN2A TB2X");
const LIGHT = set(`C150 C152 C162 C170 C172 C175 C177 C180 C182 C185 C188 C195 C206 C207 C210 PA18 PA22 PA24 PA25
  PA28 PA32 PA38 PA46 SR20 SR22 S22T BE23 BE24 BE33 BE35 BE36 DA20 DA40 DR40 TB10 TB20 TB21
  M20P M20T M20J G115 AA5 AT3 RV6 RV7 RV8 RV9 RV10 EVOT J3 PIVI Y12 GA8 COZY CH7A CH7B ULAC AC11 AA1 AAT3 P28A P28R P28B P32R SIRA`);

/** Pick a family for an ICAO type code, or "" when unknown. */
export function familyOfType(code) {
  const c = String(code || "").toUpperCase().trim();
  if (!c) return "";
  if (HELI.has(c)) return "heli";
  if (DELTA.has(c)) return "delta";
  if (FIGHTER.has(c)) return "fighter";
  if (TRANSPORT.has(c)) return "transport";
  if (UAV.has(c)) return "uav";
  if (GLIDER.has(c)) return "glider";
  if (BALLOON.has(c)) return "balloon";
  if (AIRSHIP.has(c)) return "airship";
  if (DRONE.has(c)) return "drone";
  if (QUAD.has(c)) return "quad";
  if (WIDE.has(c)) return "wide";
  if (NARROW.has(c)) return "narrow";
  if (REGIONAL.has(c)) return "regional";
  if (BIZJET.has(c)) return "bizjet";
  if (TURBOPROP.has(c)) return "turboprop";
  if (LIGHTTWIN.has(c)) return "lighttwin";
  if (TRAINER.has(c) || LIGHT.has(c)) return "light";
  return "";
}

/** US military ICAO hex block (AE0000-AFFFFF). */
export function isMilitaryHex(hex) {
  const n = parseInt(String(hex || ""), 16);
  return Number.isFinite(n) && n >= 0xAE0000 && n <= 0xAFFFFF;
}

/** Family from an ADS-B emitter category such as "A3" (A = aircraft, B = other, C = surface), "" when unknown. */
export function familyOfCategory(cat) {
  const m = String(cat || "").toUpperCase().match(/\b([ABC])([0-7])\b/);
  if (!m) return "";
  switch (m[1] + m[2]) {
    case "A1": return "light";      // < 15,500 lb
    case "A2": return "regional";   // 15,500 - 75,000 lb
    case "A3": return "narrow";     // 75,000 - 300,000 lb
    case "A4": return "narrow";     // high-vortex large (B757 class)
    case "A5": return "wide";       // heavy, > 300,000 lb
    case "A6": return "fighter";    // high performance (> 5 g and > 400 kt)
    case "A7": return "heli";
    case "B1": return "glider";
    case "B2": return "balloon";    // lighter than air
    case "B3": return "light";      // parachutist: nearest generic
    case "B4": return "light";      // ultralight
    case "B6": return "uav";
    case "C1": case "C2": return "vehicle";
    case "C3": case "C4": case "C5": return "obstacle";
    default: return "";
  }
}

/** ICAO wake turbulence class (L, M, H, J) -> family. */
export function familyOfWake(w) {
  switch (String(w || "").toUpperCase().trim().charAt(0)) {
    case "L": return "light";
    case "M": return "narrow";
    case "H": return "wide";
    case "J": return "quad";
    default: return "";
  }
}

/** Last resort from motion: jet cruise -> airliner, slow and low -> light aircraft, otherwise the generic dart. */
export function familyOfMotion(speedKt, altFt) {
  const s = Number(speedKt), a = Number(altFt);
  if ((Number.isFinite(a) && a >= 24000) || (Number.isFinite(s) && s >= 300)) return "narrow";
  if (Number.isFinite(s) && s > 0 && s <= 120 && (!Number.isFinite(a) || a < 8000)) return "light";
  return "";
}

/**
 * Aircraft shape: ICAO type code, then the emitter category (when it says more than "large"), a military hex,
 * the wake class, speed / altitude, and finally the generic dart. A military hex with no better information is a
 * fast jet when it flies fast and a transport otherwise.
 */
export function aircraftShape({ icaoType, cat, hex, wake, speed, alt } = {}) {
  const byType = familyOfType(icaoType);
  if (byType) return byType;
  const byCat = familyOfCategory(cat);
  const mil = isMilitaryHex(hex);
  if (byCat && byCat !== "narrow") return byCat;              // A3 is the weak "large" bucket: let a military hex or wake class refine it
  if (mil) return Number(speed) >= 400 && !byCat ? "fighter" : "transport";
  return byCat || familyOfWake(wake) || familyOfMotion(speed, alt) || "plane";
}

/** Ship shape from AIS ship-type text or numeric code (30 fishing, 31-32/52-53 tug, 33 dredger, 35 military, 36 sail, 37 pleasure,
 *  40-49 high-speed, 50 pilot, 51 SAR, 60-69 passenger, 70-79 cargo, 80-89 tanker). */
export function vesselShape({ cat, extra, sub } = {}) {
  const txt = [cat, extra, sub].filter(Boolean).join(" ").toLowerCase();
  const num = txt.match(/\b(\d{2})\b/);
  const code = num ? Number(num[1]) : null;
  if (/tanker/.test(txt) || (code >= 80 && code <= 89)) return "ship_tanker";
  if (/cargo|container|bulk|freight/.test(txt) || (code >= 70 && code <= 79)) return "ship_cargo";
  if (/passenger|ferry|cruise/.test(txt) || (code >= 60 && code <= 69)) return "ship_passenger";
  if (/high.?speed|\bhsc\b|hydrofoil|catamaran|wing in ground|\bwig\b/.test(txt) || (code >= 40 && code <= 49) || (code >= 20 && code <= 29)) return "ship_hsc";
  if (/fishing/.test(txt) || code === 30) return "ship_fishing";
  if (/sailing|sailboat|\bsail\b/.test(txt) || code === 36) return "ship_sailing";
  if (/pleasure|yacht/.test(txt) || code === 37) return "ship_pleasure";
  if (/dredg/.test(txt) || code === 33) return "ship_dredger";
  if (/\btug\b|towing|\btow\b/.test(txt) || code === 52 || code === 31 || code === 32 || code === 53) return "ship_tug";
  if (/military|naval|navy|warship|law enforcement/.test(txt) || code === 35 || code === 55) return "ship_military";
  if (/search|rescue|\bsar\b|medical|anti-?pollution/.test(txt) || code === 51 || code === 54 || code === 58) return "ship_sar";
  if (/pilot/.test(txt) || code === 50) return "ship_pilot";
  return "ship_other";
}

/** Typical hull scale (relative to a 100 m ship) when the length is not known. */
const SHIP_SCALE = {
  ship_cargo: 1.25, ship_tanker: 1.35, ship_passenger: 1.4, ship_hsc: 1, ship_tug: 1, ship_fishing: 0.98, ship_sailing: 0.78,
  ship_pleasure: 0.78, ship_military: 1.25, ship_sar: 0.98, ship_pilot: 0.75, ship_dredger: 1.25, ship_other: 1.05,
};

/** Icon size factor for a ship: from a length in metres written in the text ("183 m", "length 183") or the type's typical size. */
export function vesselScale({ shape, cat, extra, sub, length } = {}) {
  let L = Number(length);
  if (!Number.isFinite(L) || L <= 0) {
    const m = [cat, extra, sub].filter(Boolean).join(" ").match(/(?:\blen(?:gth)?[:= ]+|\bL[:= ]+)(\d{1,3})\b|\b(\d{1,3})\s?m\b/i);
    L = m ? Number(m[1] || m[2]) : NaN;
  }
  if (Number.isFinite(L) && L >= 5) return Math.max(0.65, Math.min(2, 0.55 + L / 160));
  return SHIP_SCALE[shape] || 1.05;
}

/** APRS shape from the symbol / comment text when the data has it. */
export function aprsShape({ cat, extra, sub, label } = {}) {
  const txt = [cat, extra, sub].filter(Boolean).join(" ").toLowerCase();
  if (/digi|relay|wide\d|igate|repeater|node/.test(txt)) return "aprs_digi";
  if (/weather|\bwx\b/.test(txt)) return "aprs_wx";
  if (/balloon|sonde|hab\b/.test(txt)) return "aprs_balloon";
  if (/boat|ship|vessel|yacht|marine/.test(txt)) return "aprs_ship";
  if (/truck|van\b|lorry|bus\b/.test(txt)) return "aprs_truck";
  if (/house|home|qth|fixed/.test(txt)) return "aprs_house";
  if (/car|vehicle|mobile|bike|auto|jeep/.test(txt)) return "aprs_car";
  return "aprs";
}

/** Drone shape: Remote ID / DJI quadcopters get the status ring, fixed-wing UAVs the wing, anything else the plain quad. */
export function droneShape({ cat, extra, sub, src, label } = {}) {
  const txt = [cat, extra, sub, src, label].filter(Boolean).join(" ").toLowerCase();
  if (/fixed.?wing|\bwing\b|plane|vtol/.test(txt)) return "uav";
  if (/remote.?id|\brid\b|dji|opendroneid|mavic|phantom|mini/.test(txt)) return "drone_rid";
  return "drone";
}

/** Shape name for any track ({kind, icaoType?, cat, sub, extra, label, hex?, wake?, speed?, alt?}). */
export function iconShape(t) {
  switch (t.kind) {
    case "aircraft": return aircraftShape({ icaoType: t.icaoType || (t.meta && t.meta.icaoType), cat: t.cat, hex: t.hex, wake: t.wake, speed: t.speed, alt: t.alt });
    case "flarm": return familyOfCategory(t.cat) || (/heli/i.test(t.cat || "") ? "heli" : /balloon/i.test(t.cat || "") ? "balloon" : /drone|uav/i.test(t.cat || "") ? "drone" : /(tow|plane|jet|aircraft|motor)/i.test(t.cat || "") ? "light" : "glider");
    case "vessel": return vesselShape(t);
    case "aprs": return aprsShape(t);
    case "sonde": return "sonde";
    case "sat": return "sat";
    case "gnss": return "sat_gnss";
    case "drone": return droneShape(t);
    case "ham": return "ham";
    case "buoy": return "buoy";
    case "dsc": return "dsc";
    case "station": return "station";
    default: return "station";
  }
}

/** Human names for the card illustration caption. */
export const FAMILY_NAME = {
  narrow: "Narrow-body airliner", wide: "Wide-body airliner", quad: "Four-engine airliner", regional: "Regional jet", bizjet: "Business jet",
  turboprop: "Turboprop", light: "Light aircraft", lighttwin: "Light twin", heli: "Helicopter", fighter: "Military jet", delta: "Delta-wing fighter",
  transport: "Military transport", glider: "Glider", balloon: "Balloon", airship: "Airship", drone: "Drone", drone_rid: "Drone (Remote ID)",
  uav: "Fixed-wing UAV", plane: "Aircraft", vehicle: "Ground vehicle", obstacle: "Obstacle",
};
