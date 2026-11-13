// badges/defs.js - THE badge data file: every badge's name, description, icon and tier thresholds, in one place.
// Shared by the hub (src/badge-stats.js awards them) and the pages (profile, leaderboard). Pure data, no DOM.
// To tune a badge, change its numbers here; the tests in test/badge-stats.test.js read the same table.
//
// stat:   the field of a user's stats the thresholds compare against (see src/badge-stats.js, statValue()).
// tiers:  ascending thresholds for bronze, silver, gold, platinum. A badge with one tier has one threshold.
// unit:   how the progress is written ("h" hours, "d" days, "km", or "" for a count).

export const TIERS = ["bronze", "silver", "gold", "platinum"];
export const TIER_NAMES = { 1: "Bronze", 2: "Silver", 3: "Gold", 4: "Platinum" };

export const BADGES = [
  { id: "first_light", name: "First light", icon: "spark", stat: "shares", unit: "", tiers: [1],
    desc: "Shared your first object to the live map." },
  { id: "airtime", name: "On the air", icon: "clock", stat: "hours", unit: "h", tiers: [10, 100, 1000, 5000],
    desc: "Hours spent sharing to the map." },
  { id: "regular", name: "Regular", icon: "calendar", stat: "days", unit: "d", tiers: [7, 30, 100, 365],
    desc: "Different days you shared on." },
  { id: "streak", name: "On a roll", icon: "flame", stat: "bestStreak", unit: "d", tiers: [3, 7, 30, 100],
    desc: "Days in a row with a share (your longest run)." },
  { id: "plane_spotter", name: "Plane spotter", icon: "plane", stat: "aircraft", unit: "", tiers: [100, 1000, 10000, 100000],
    desc: "Different aircraft (ICAO addresses) you shared." },
  { id: "harbour_master", name: "Harbour master", icon: "ship", stat: "vessel", unit: "", tiers: [10, 100, 1000, 10000],
    desc: "Different ships (MMSI numbers) you shared." },
  { id: "sonde_hunter", name: "Sonde hunter", icon: "sonde", stat: "sonde", unit: "", tiers: [1, 10, 50, 250],
    desc: "Different radiosondes you tracked." },
  { id: "aprs_net", name: "Digipeater", icon: "aprs", stat: "aprs", unit: "", tiers: [25, 250, 2500, 10000],
    desc: "Different APRS stations you shared." },
  { id: "orbit_watcher", name: "Orbit watcher", icon: "sat", stat: "sat", unit: "", tiers: [5, 25, 100, 500],
    desc: "Different satellites you shared." },
  { id: "qso_party", name: "QSO party", icon: "ham", stat: "ham", unit: "", tiers: [100, 1000, 10000, 50000],
    desc: "Different amateur radio contacts you shared." },
  { id: "fox_hunter", name: "Fox hunter", icon: "df", stat: "fix", unit: "", tiers: [1, 10, 100, 1000],
    desc: "Direction-finding fixes your bearings helped make." },
  { id: "text_reader", name: "Text reader", icon: "acars", stat: "acars", unit: "", tiers: [10, 100, 1000, 10000],
    desc: "Different ACARS messages you shared." },
  { id: "long_range", name: "Long range", icon: "range", stat: "rangeKm", unit: "km", tiers: [100, 200, 300, 400],
    desc: "Your record distance from your station to an object you received directly (confirmed by two reports)." },
  { id: "globetrotter", name: "Globetrotter", icon: "globe", stat: "countries", unit: "", tiers: [5, 15, 30, 60],
    desc: "Countries of registration among the aircraft and ships you shared." },
  { id: "field_day", name: "Field day", icon: "grid", stat: "fields", unit: "", tiers: [3, 10, 25, 60],
    desc: "Maidenhead fields (20 x 10 degree squares) your objects were in." },
  { id: "night_owl", name: "Night owl", icon: "moon", stat: "nightHours", unit: "h", tiers: [5, 25, 100, 500],
    desc: "Hours shared between midnight and 4 am, local solar time." },
  { id: "early_bird", name: "Early bird", icon: "sun", stat: "earlyHours", unit: "h", tiers: [5, 25, 100, 500],
    desc: "Hours shared between 4 am and 7 am, local solar time." },
  { id: "rare_catch", name: "Rare catch", icon: "star", stat: "rare", unit: "", tiers: [1, 3, 5, 8],
    desc: "Different kinds of rare catch: emergency squawks (7500, 7600, 7700), AIS SART, man overboard and EPIRB beacons, balloons, gliders, parachutists and space vehicles in ADS-B." },
  { id: "multi_mode", name: "Jack of all modes", icon: "modes", stat: "kinds", unit: "", tiers: [5, 7, 9, 11],
    desc: "Different kinds of object you shared (aircraft, ships, sondes, APRS, satellites, contacts, ...)." },
];

/** The rare catches the hub recognises (src/badge-stats.js rareKinds()). Keys are stored in the user's stats. */
export const RARE = {
  sq7500: "Squawk 7500 (unlawful interference)",
  sq7600: "Squawk 7600 (radio failure)",
  sq7700: "Squawk 7700 (emergency)",
  sart: "AIS SART (search and rescue transmitter)",
  mob: "AIS man overboard beacon",
  epirb: "AIS EPIRB",
  glider: "Glider in ADS-B (category B1)",
  balloon: "Balloon or airship in ADS-B (category B2)",
  para: "Parachutist in ADS-B (category B3)",
  space: "Space or trans-atmospheric vehicle in ADS-B (category B7)",
};

/** Leaderboard categories: the stat each one ranks by, and the periods it exists for. */
export const BOARDS = [
  { id: "hours", name: "Hours shared", stat: "hours", unit: "h", periods: ["week", "month", "all"] },
  { id: "objects", name: "New objects", stat: "objects", unit: "", periods: ["week", "month", "all"] },
  { id: "aircraft", name: "Aircraft", stat: "aircraft", unit: "", periods: ["week", "month", "all"] },
  { id: "ships", name: "Ships", stat: "vessel", unit: "", periods: ["week", "month", "all"] },
  { id: "range", name: "Longest range", stat: "rangeKm", unit: "km", periods: ["week", "month", "all"] },
  { id: "streak", name: "Longest streak", stat: "bestStreak", unit: "d", periods: ["all"] },
  { id: "countries", name: "Countries", stat: "countries", unit: "", periods: ["all"] },
];

export const BADGE_BY_ID = Object.fromEntries(BADGES.map((b) => [b.id, b]));

/** The tier (0 = not earned, 1..4) a value reaches on a badge. */
export function tierFor(badge, value) {
  let t = 0;
  for (let i = 0; i < badge.tiers.length; i++) if (value >= badge.tiers[i]) t = i + 1;
  return t;
}

/** Progress to the next tier: { tier, next (threshold or null), frac 0..1 }. */
export function progressFor(badge, value) {
  const tier = tierFor(badge, value);
  if (tier >= badge.tiers.length) return { tier, next: null, frac: 1 };
  const lo = tier ? badge.tiers[tier - 1] : 0, hi = badge.tiers[tier];
  return { tier, next: hi, frac: Math.max(0, Math.min(1, (value - lo) / (hi - lo))) };
}

/** The badge a map entry carries ("id:tier:login", see src/badge-service.js showcaseFor): { badge, tier, login, title } or null. */
export function parseShowcase(bb) {
  const m = /^([a-z_]{2,24}):([1-4]):([A-Za-z0-9-]{1,39})$/.exec(String(bb || ""));
  if (!m || !BADGE_BY_ID[m[1]]) return null;
  const badge = BADGE_BY_ID[m[1]], tier = Math.min(Number(m[2]), badge.tiers.length);
  return { badge, tier, login: m[3], title: badgeTitle(badge, tier) };
}

/** Name with its tier, e.g. "Plane spotter (Gold)"; single-tier badges have no tier word. */
export function badgeTitle(badge, tier) {
  return badge.tiers.length > 1 && tier ? `${badge.name} (${TIER_NAMES[tier]})` : badge.name;
}
