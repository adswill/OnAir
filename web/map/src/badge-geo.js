// badge-geo.js - the geography the badges need: distance, Maidenhead locators and fields, and the country of registration of an aircraft
// (from its ICAO 24-bit address block) or a ship (from the MID, the first three digits of its MMSI). Pure, covered by test/badge-stats.test.js.
// The tables are partial on purpose: an address outside them simply counts for no country. They list the large blocks of the ICAO Annex 10
// allocation and the ITU MID table as published facts, typed in here (no third-party file is copied).

const R_KM = 6371.0088;
const rad = (d) => (d * Math.PI) / 180;

/** Great-circle distance in km. */
export function distKm(lat1, lon1, lat2, lon2) {
  const dLat = rad(lat2 - lat1), dLon = rad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(rad(lat1)) * Math.cos(rad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 2 * R_KM * Math.asin(Math.min(1, Math.sqrt(a)));
}

/** Centre of a 4- or 6-character Maidenhead locator, or null. */
export function fromLocator(loc) {
  const s = String(loc || "").trim();
  if (!/^[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?$/.test(s)) return null;
  const u = s.toUpperCase();
  let lon = (u.charCodeAt(0) - 65) * 20 - 180 + Number(u[2]) * 2;
  let lat = (u.charCodeAt(1) - 65) * 10 - 90 + Number(u[3]);
  if (u.length === 6) {
    lon += (u.charCodeAt(4) - 65) * (2 / 24) + 1 / 24;
    lat += (u.charCodeAt(5) - 65) * (1 / 24) + 1 / 48;
  } else { lon += 1; lat += 0.5; }
  return { lat, lon };
}

/** The Maidenhead field (two letters, 20 x 10 degrees) of a position. */
export function fieldOf(lat, lon) {
  const x = Math.min(17, Math.max(0, Math.floor((lon + 180) / 20)));
  const y = Math.min(17, Math.max(0, Math.floor((lat + 90) / 10)));
  return String.fromCharCode(65 + x) + String.fromCharCode(65 + y);
}

// [first, last (inclusive), ISO country] of ICAO 24-bit address blocks. Sorted by first.
const ICAO_BLOCKS = [
  [0x008000, 0x00ffff, "ZA"], [0x010000, 0x017fff, "EG"], [0x020000, 0x027fff, "MA"], [0x040000, 0x040fff, "ET"],
  [0x04c000, 0x04cfff, "KE"], [0x064000, 0x064fff, "NG"], [0x06a000, 0x06a3ff, "QA"], [0x0ac000, 0x0acfff, "CO"],
  [0x0d0000, 0x0d7fff, "MX"], [0x100000, 0x1fffff, "RU"], [0x300000, 0x33ffff, "IT"], [0x340000, 0x37ffff, "ES"],
  [0x380000, 0x3bffff, "FR"], [0x3c0000, 0x3fffff, "DE"], [0x400000, 0x43ffff, "GB"], [0x440000, 0x447fff, "AT"],
  [0x448000, 0x44ffff, "BE"], [0x450000, 0x457fff, "BG"], [0x458000, 0x45ffff, "DK"], [0x460000, 0x467fff, "FI"],
  [0x468000, 0x46ffff, "GR"], [0x470000, 0x477fff, "HU"], [0x478000, 0x47ffff, "NO"], [0x480000, 0x487fff, "NL"],
  [0x488000, 0x48ffff, "PL"], [0x490000, 0x497fff, "PT"], [0x498000, 0x49ffff, "CZ"], [0x4a0000, 0x4a7fff, "RO"],
  [0x4a8000, 0x4affff, "SE"], [0x4b0000, 0x4b7fff, "CH"], [0x4b8000, 0x4bffff, "TR"], [0x4c8000, 0x4c83ff, "CY"],
  [0x4ca000, 0x4cafff, "IE"], [0x4cc000, 0x4ccfff, "IS"], [0x4d0000, 0x4d03ff, "LU"], [0x4d2000, 0x4d23ff, "MT"],
  [0x501c00, 0x501fff, "HR"], [0x502c00, 0x502fff, "LV"], [0x503c00, 0x503fff, "LT"], [0x505c00, 0x505fff, "SK"],
  [0x506c00, 0x506fff, "SI"], [0x508000, 0x50ffff, "UA"], [0x510000, 0x5103ff, "BY"], [0x511000, 0x5113ff, "EE"],
  [0x514000, 0x5143ff, "GE"], [0x683000, 0x6833ff, "KZ"], [0x706000, 0x706fff, "KW"], [0x70c000, 0x70c3ff, "OM"],
  [0x710000, 0x717fff, "SA"], [0x718000, 0x71ffff, "KR"], [0x730000, 0x737fff, "IR"], [0x738000, 0x73ffff, "IL"],
  [0x740000, 0x747fff, "JO"], [0x750000, 0x757fff, "MY"], [0x758000, 0x75ffff, "PH"], [0x760000, 0x767fff, "PK"],
  [0x768000, 0x76ffff, "SG"], [0x780000, 0x7bffff, "CN"], [0x7c0000, 0x7fffff, "AU"], [0x800000, 0x83ffff, "IN"],
  [0x840000, 0x87ffff, "JP"], [0x880000, 0x887fff, "TH"], [0x888000, 0x88ffff, "VN"], [0x894000, 0x894fff, "BH"],
  [0x896000, 0x896fff, "AE"], [0x899000, 0x8993ff, "TW"], [0x8a0000, 0x8a7fff, "ID"], [0xa00000, 0xafffff, "US"],
  [0xc00000, 0xc3ffff, "CA"], [0xc80000, 0xc87fff, "NZ"], [0xe00000, 0xe3ffff, "AR"], [0xe40000, 0xe7ffff, "BR"],
  [0xe80000, 0xe80fff, "CL"], [0xe84000, 0xe84fff, "PE"],
];

/** Country of registration of an ICAO 24-bit address (6 hex digits), or "". */
export function icaoCountry(hex) {
  const v = parseInt(String(hex || ""), 16);
  if (!Number.isFinite(v) || String(hex).length !== 6) return "";
  let lo = 0, hi = ICAO_BLOCKS.length - 1;
  while (lo <= hi) {
    const m = (lo + hi) >> 1, b = ICAO_BLOCKS[m];
    if (v < b[0]) hi = m - 1; else if (v > b[1]) lo = m + 1; else return b[2];
  }
  return "";
}

// MID (first three MMSI digits of a ship station) -> ISO country. Partial; flags of convenience included.
const MID = {
  205: "BE", 207: "BG", 209: "CY", 210: "CY", 211: "DE", 212: "CY", 215: "MT", 218: "DE", 219: "DK", 220: "DK", 224: "ES", 225: "ES",
  226: "FR", 227: "FR", 228: "FR", 229: "MT", 230: "FI", 231: "FO", 232: "GB", 233: "GB", 234: "GB", 235: "GB", 236: "GI", 237: "GR",
  238: "HR", 239: "GR", 240: "GR", 241: "GR", 244: "NL", 245: "NL", 246: "NL", 247: "IT", 248: "MT", 249: "MT", 250: "IE", 251: "IS",
  253: "LU", 254: "MC", 256: "MT", 257: "NO", 258: "NO", 259: "NO", 261: "PL", 263: "PT", 265: "SE", 266: "SE", 269: "CH", 271: "TR",
  272: "UA", 273: "RU", 275: "LV", 276: "EE", 277: "LT", 303: "US", 308: "BS", 309: "BS", 311: "BS", 316: "CA", 338: "US", 345: "MX",
  351: "PA", 352: "PA", 353: "PA", 354: "PA", 355: "PA", 356: "PA", 357: "PA", 366: "US", 367: "US", 368: "US", 369: "US", 370: "PA",
  371: "PA", 372: "PA", 373: "PA", 403: "SA", 408: "BH", 412: "CN", 413: "CN", 414: "CN", 419: "IN", 422: "IR", 431: "JP", 432: "JP",
  440: "KR", 441: "KR", 447: "KW", 461: "OM", 466: "QA", 470: "AE", 471: "AE", 477: "HK", 503: "AU", 512: "NZ", 525: "ID", 533: "MY",
  538: "MH", 563: "SG", 564: "SG", 565: "SG", 566: "SG", 601: "ZA", 622: "EG", 636: "LR", 637: "LR", 701: "AR", 710: "BR", 725: "CL",
};

/** Country of a ship MMSI (9 digits), or "". Coast stations, SAR aircraft and beacons have no MID at the front and give "". */
export function mmsiCountry(mmsi) {
  const s = String(mmsi || "");
  if (!/^[2-7]\d{8}$/.test(s)) return "";
  return MID[Number(s.slice(0, 3))] || "";
}
