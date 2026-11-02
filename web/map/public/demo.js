// demo.js - ?demo=1 preview data: ~60 synthetic aircraft, ships, sondes, APRS stations and satellites.
// Everything here is invented for previewing the page offline; the page shows a DEMO banner while it is active.

import { destination } from "./df.js";
import { airportByCode } from "./airports.js";
import { distanceNm, bearing } from "./geo.js";

/** Small deterministic PRNG so the demo looks the same on every load. */
function rng(seed) {
  let s = seed >>> 0;
  return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}

// [flight number, airline ICAO, airline name, aircraft ICAO type, type name, origin, destination, category]
const FLIGHTS = [
  ["BAW117", "BAW", "British Airways", "B77W", "Boeing 777-300ER", "EGLL", "KJFK", "A5"],
  ["DLH400", "DLH", "Lufthansa", "A346", "Airbus A340-600", "EDDF", "KJFK", "A5"],
  ["UAE203", "UAE", "Emirates", "A388", "Airbus A380-800", "OMDB", "EGLL", "A5"],
  ["AFR1180", "AFR", "Air France", "A320", "Airbus A320", "LFPG", "LEMD", "A3"],
  ["RYR45AB", "RYR", "Ryanair", "B738", "Boeing 737-800", "EGSS", "LEBL", "A3"],
  ["EZY81PX", "EZY", "easyJet", "A20N", "Airbus A320neo", "EGKK", "LPPT", "A3"],
  ["KLM1263", "KLM", "KLM", "E190", "Embraer 190", "EHAM", "EGPH", "A3"],
  ["SAS441", "SAS", "SAS", "A21N", "Airbus A321neo", "EKCH", "ESSA", "A3"],
  ["FIN7", "FIN", "Finnair", "A359", "Airbus A350-900", "EFHK", "RJAA", "A5"],
  ["QTR8", "QTR", "Qatar Airways", "B789", "Boeing 787-9", "OTHH", "EGLL", "A5"],
  ["THY1", "THY", "Turkish Airlines", "A333", "Airbus A330-300", "LTFM", "KJFK", "A5"],
  ["SWR38", "SWR", "SWISS", "BCS3", "Airbus A220-300", "LSZH", "EGLC", "A3"],
  ["AAL100", "AAL", "American Airlines", "B772", "Boeing 777-200", "KJFK", "EGLL", "A5"],
  ["UAL901", "UAL", "United Airlines", "B744", "Boeing 747-400", "KSFO", "RJTT", "A5"],
  ["DAL15", "DAL", "Delta Air Lines", "A333", "Airbus A330-300", "KATL", "EHAM", "A5"],
  ["SIA321", "SIA", "Singapore Airlines", "A388", "Airbus A380-800", "WSSS", "EGLL", "A5"],
  ["QFA9", "QFA", "Qantas", "B789", "Boeing 787-9", "YPPH", "EGLL", "A5"],
  ["ETH701", "ETH", "Ethiopian Airlines", "B788", "Boeing 787-8", "HAAB", "OMDB", "A5"],
  ["LOT281", "LOT", "LOT Polish Airlines", "E175", "Embraer 175", "EPWA", "EDDM", "A2"],
  ["WZZ4402", "WZZ", "Wizz Air", "A321", "Airbus A321", "EYVI", "EGSS", "A3"],
  ["BTI600", "BTI", "airBaltic", "BCS3", "Airbus A220-300", "EVRA", "EDDB", "A3"],
  ["NAX1", "NAX", "Norwegian", "B38M", "Boeing 737 MAX 8", "ENGM", "LEMG", "A3"],
  ["AUA77", "AUA", "Austrian", "DH8D", "De Havilland Dash 8-400", "LOWW", "LSZH", "A3"],
  ["VLG8123", "VLG", "Vueling", "A320", "Airbus A320", "LEBL", "LIRF", "A3"],
  ["ITY612", "ITY", "ITA Airways", "A319", "Airbus A319", "LIRF", "LFPG", "A3"],
  ["IBE6251", "IBE", "Iberia", "A332", "Airbus A330-200", "LEMD", "SBGR", "A5"],
  ["CPA251", "CPA", "Cathay Pacific", "B77W", "Boeing 777-300ER", "VHHH", "EGLL", "A5"],
  ["ANA106", "ANA", "All Nippon Airways", "B789", "Boeing 787-9", "RJTT", "KLAX", "A5"],
  ["ACA870", "ACA", "Air Canada", "B77L", "Boeing 777-200LR", "CYYZ", "EGLL", "A5"],
  ["CLX701", "CLX", "Cargolux", "B748", "Boeing 747-8F", "ELLX", "VHHH", "A5"],
];

/** Build the demo set. Returns {tracks, meta} where meta maps track id to the /meta-style object. */
export function generateDemo(nowMs = Date.now()) {
  const R = rng(20261016);
  const tracks = [];
  const meta = {};
  const push = (t) => { tracks.push(t); return t; };
  const sec = (s) => nowMs - s * 1000;

  FLIGHTS.forEach((fl, i) => {
    const [cs, al, alName, ty, tyName, o, d, cat] = fl;
    const A = airportByCode(o), B = airportByCode(d);
    const frac = 0.12 + R() * 0.76;
    const total = distanceNm(A.lat, A.lon, B.lat, B.lon);
    // straight interpolation is good enough for a preview
    const lat = A.lat + (B.lat - A.lat) * frac, lon = A.lon + (B.lon - A.lon) * frac;
    const hex = (0x400000 + i * 4099 + Math.floor(R() * 900)).toString(16).toUpperCase().padStart(6, "0");
    const id = "aircraft:" + hex;
    const climbing = frac < 0.12 + 0.05;
    push({
      id, kind: "aircraft", label: cs, sub: "ADS-B", src: "adsb", cat,
      lat, lon, alt: ty === "DH8D" ? 24000 : 34000 + Math.floor(R() * 6) * 1000, heading: bearing(A.lat, A.lon, B.lat, B.lon),
      speed: ty === "DH8D" ? 290 : 420 + Math.floor(R() * 80), vrate: climbing ? 1800 : Math.round((R() - 0.5) * 300),
      squawk: String(1000 + Math.floor(R() * 6000)), extra: "", t: nowMs, age: 0, path: [],
      first: { ms: sec(1200 + R() * 9000), altFt: i % 3 === 0 ? 400 : 21000, lat: A.lat + 0.05, lon: A.lon + 0.05 },
    });
    meta[id] = {
      type: tyName, icaoType: ty, registration: (["G", "D-A", "F-G", "N", "A6-", "TC-"][i % 6]) + String.fromCharCode(65 + i % 26) + String.fromCharCode(66 + (i * 7) % 24) + String.fromCharCode(67 + (i * 11) % 23),
      operator: alName, origin: A.iata, destination: B.iata, originName: A.city, destinationName: B.city,
      flightNumber: al.slice(0, 2) + cs.replace(/\D/g, ""), airline: { name: alName, icao: al, iata: al.slice(0, 2), iso: "" },
      route: {
        origin: { iata: A.iata, icao: A.icao, name: A.name, city: A.city, country: "", iso: A.country, lat: A.lat, lon: A.lon },
        destination: { iata: B.iata, icao: B.icao, name: B.name, city: B.city, country: "", iso: B.country, lat: B.lat, lon: B.lon },
      },
    };
    void total;
  });
  // an emergency squawk, a ground-level departure, a helicopter, military, glider, balloon, drone
  const emergency = tracks[3]; emergency.squawk = "7700"; emergency.extra = "Demo: general emergency squawk";
  push({ id: "aircraft:AE1234", kind: "aircraft", label: "RCH871", sub: "ADS-B", src: "adsb", cat: "A5", lat: 50.1, lon: 8.9, alt: 28000, heading: 70, speed: 450, vrate: 0, squawk: "4301", t: nowMs, age: 0, path: [] });
  meta["aircraft:AE1234"] = { type: "Boeing C-17A Globemaster III", icaoType: "C17", registration: "05-5143", operator: "US Air Force", origin: "", destination: "", route: null };
  push({ id: "aircraft:3C6444", kind: "aircraft", label: "HELI12", sub: "ADS-B", src: "adsb", cat: "A7", lat: 48.2, lon: 11.6, alt: 1500, heading: 200, speed: 110, vrate: 0, squawk: "7000", t: nowMs, age: 0, path: [] });
  meta["aircraft:3C6444"] = { type: "Airbus Helicopters H145", icaoType: "EC45", registration: "D-HXAB", operator: "ADAC Luftrettung", origin: "", destination: "", route: null };
  push({ id: "aircraft:471F2A", kind: "aircraft", label: "N172SP", sub: "ADS-B", src: "adsb", cat: "A1", lat: 47.4, lon: 8.3, alt: 3200, heading: 310, speed: 105, vrate: 0, squawk: "7000", t: nowMs, age: 0, path: [] });
  meta["aircraft:471F2A"] = { type: "Cessna 172 Skyhawk", icaoType: "C172", registration: "HB-SPA", operator: "Flight school", origin: "", destination: "", route: null };
  push({ id: "aircraft:4B8442", kind: "aircraft", label: "SWR1PA", sub: "ACARS", src: "acars", cat: "position", lat: 46.2, lon: 7.0, alt: 12000, heading: null, speed: null, t: nowMs, age: 0, path: [] });
  push({ id: "flarm:DD8B21", kind: "flarm", label: "D-KABC", sub: "FLARM", src: "flarm", cat: "Glider", lat: 47.8, lon: 10.6, alt: 1900, heading: 120, speed: 55, vrate: 150, t: nowMs, age: 0, path: [] });
  push({ id: "flarm:DD8B22", kind: "flarm", label: "D-HXYZ", sub: "FLARM", src: "flarm", cat: "Helicopter", lat: 47.9, lon: 10.4, alt: 800, heading: 20, speed: 70, vrate: 0, t: nowMs, age: 0, path: [] });
  push({ id: "drone:DJI1", kind: "drone", label: "DJI-MAVIC3", sub: "Remote ID", src: "rid", lat: 52.52, lon: 13.40, alt: 90, heading: 45, speed: 8, t: nowMs, age: 0, path: [] });
  // low, just-departed aircraft: first seen on the ground near its origin
  const dep = FLIGHTS[1], dA = airportByCode("EDDF"), dB = airportByCode("KJFK");
  push({
    id: "aircraft:3C65AB", kind: "aircraft", label: "DLH9XX", sub: "ADS-B", src: "adsb", cat: "A3", lat: dA.lat + 0.12, lon: dA.lon + 0.2, alt: 2400, heading: 275, speed: 230, vrate: 2400,
    squawk: "1000", t: nowMs, age: 0, path: [], first: { ms: sec(150), altFt: 60, lat: dA.lat, lon: dA.lon },
  });
  meta["aircraft:3C65AB"] = { ...meta[tracks[1].id], flightNumber: "LH9XX", registration: "D-AIZZ", type: "Airbus A320neo", icaoType: "A20N",
    route: meta[tracks[1].id].route, origin: "FRA", destination: "JFK" };
  void dep; void dB;

  // ships by AIS type
  const shipTypes = [
    ["Cargo", 70, 51.2, 1.7], ["Cargo", 71, 36.0, -5.6], ["Tanker", 80, 25.9, 55.4], ["Tanker", 81, 1.2, 103.9], ["Passenger", 60, 59.3, 19.2],
    ["Passenger", 69, 37.9, 23.5], ["Fishing", 30, 57.7, 5.9], ["Sailing", 36, 43.3, 5.2], ["Tug", 52, 51.9, 4.1], ["Other", 90, 54.3, 10.2],
  ];
  shipTypes.forEach(([name, code, lat, lon], i) => {
    push({
      id: "vessel:2" + String(10000000 + i * 777 + 123).slice(0, 8), kind: "vessel", label: "DEMO " + name.toUpperCase() + " " + (i + 1), sub: "AIS", src: "ais",
      cat: name + " (" + code + ")", lat, lon, heading: Math.floor(R() * 360), speed: 6 + R() * 12, t: nowMs, age: 0, path: [],
    });
  });
  // sondes, APRS, satellites, buoy
  for (let i = 0; i < 4; i++) {
    push({ id: "sonde:S" + i, kind: "sonde", label: "DEMO-SONDE-" + i, sub: "sonde", src: "sonde", lat: 46 + i * 2.4, lon: 6 + i * 3.1, alt: 8000 + i * 6000, speed: 20 + i * 5, vrate: 5, t: nowMs, age: 0, path: [] });
  }
  [["DL1ABC-9", "car", 48.1, 11.5], ["OE1XYZ", "home weather station", 48.2, 16.4], ["DB0DIG", "digipeater WIDE1", 47.7, 11.9], ["LY1BWM", "", 54.7, 25.3]].forEach(([cs, ex, lat, lon], i) => {
    push({ id: "aprs:" + cs, kind: "aprs", label: cs, sub: "APRS", src: "aprs", extra: ex, lat, lon, t: nowMs, age: 0, path: [] });
  });
  [["ISS (ZARYA)", 51.6, 20.0, 420], ["NOAA 19", 60.0, -40.0, 850], ["IRIDIUM 33", 10.0, 100.0, 780]].forEach(([n, lat, lon, alt]) => {
    push({ id: "sat:" + n, kind: "sat", label: n, sub: "Orbcomm", src: "sat", lat, lon, alt, speed: 0, t: nowMs, age: 0, path: [] });
  });
  push({ id: "buoy:W1", kind: "buoy", label: "DEMO BUOY", sub: "Datawell", src: "buoy", lat: 53.0, lon: -9.9, t: nowMs, age: 0, path: [] });
  push({ id: "station:ST1", kind: "station", label: "DEMO STATION", sub: "OnAir", src: "", lat: 54.69, lon: 25.28, t: nowMs, age: 0, path: [] });

  // fill the rest with scattered aircraft up to ~60 total
  const types = [["A320", "Airbus A320", "A3"], ["B738", "Boeing 737-800", "A3"], ["E170", "Embraer 170", "A2"], ["AT72", "ATR 72-600", "A2"], ["B789", "Boeing 787-9", "A5"]];
  let n = 0;
  while (tracks.length < 62) {
    const ty = types[n % types.length];
    const lat = 36 + R() * 24, lon = -8 + R() * 36;
    const hex = (0x4A0000 + n * 211).toString(16).toUpperCase();
    const id = "aircraft:" + hex.padStart(6, "0");
    push({ id, kind: "aircraft", label: "DMO" + (100 + n), sub: "ADS-B", src: "adsb", cat: ty[2], lat, lon, alt: 20000 + R() * 18000, heading: R() * 360, speed: 380 + R() * 100, vrate: 0, squawk: "2000", t: nowMs, age: 0, path: [] });
    meta[id] = { type: ty[1], icaoType: ty[0], registration: "DM-O" + n, operator: "Demo Air", origin: "", destination: "", route: null };
    n++;
  }
  // ---- icon showcase: one of every aircraft family (altitudes spread so the altitude colours show), ships by AIS type, small craft ----
  const SHOW_AC = [["A320", "A3"], ["B77W", "A5"], ["A388", "A5"], ["E190", "A2"], ["GLF5", "A2"], ["AT72", "A2"], ["C172", "A1"], ["BE58", "A1"],
    ["EC45", "A7"], ["F16", "A6"], ["EUFI", "A6"], ["C17", "A5"], ["GLID", "B1"], ["BALL", "B2"], ["ARSH", "B2"], ["MQ9", "B6"], ["", ""], ["B744", "A5"]];
  SHOW_AC.forEach(([ty, cat], i) => {
    const id = "aircraft:5A" + (1000 + i).toString(16).toUpperCase().padStart(4, "0");
    const lat = 53.3 - Math.floor(i / 6) * 1.0, lon = 0.8 + (i % 6) * 1.5;
    const alt = ty === "BALL" ? 1800 : ty === "C172" || ty === "GLID" ? 3500 : ty === "EC45" ? 900 : [4000, 12000, 26000, 31000, 36000, 41000][i % 6] + (ty === "" ? 0 : 0);
    push({ id, kind: "aircraft", label: ty ? "SHW" + ty : "UNKNWN", sub: "ADS-B", src: "adsb", cat: ty ? cat : "", lat, lon, alt, heading: (i * 47) % 360, speed: ty === "BALL" ? 6 : ty === "C172" ? 95 : ty === "EC45" ? 110 : ty === "GLID" ? 60 : 300 + (i % 5) * 45, vrate: 0, squawk: "2000", t: nowMs, age: 0, path: [] });
    if (ty) meta[id] = { type: ty, icaoType: ty, registration: "SH-" + i, operator: "Showcase", origin: "", destination: "", route: null };
  });
  const SHOW_SHIPS = [["Cargo (70)", "L 250 m"], ["Tanker (80)", "L 180 m"], ["Passenger (60)", "L 160 m"], ["High speed craft (42)", ""], ["Tug (52)", ""],
    ["Fishing (30)", ""], ["Sailing (36)", ""], ["Pleasure craft (37)", ""], ["Military ops (35)", "L 130 m"], ["Search and rescue (51)", ""], ["Pilot vessel (50)", ""],
    ["Dredging (33)", ""], ["Other (90)", ""]];
  SHOW_SHIPS.forEach(([cat, extra], i) => {
    push({ id: "vessel:2" + String(55000000 + i * 913).slice(0, 8), kind: "vessel", label: cat.split(" (")[0].toUpperCase().slice(0, 14), sub: "AIS", src: "ais", cat, extra,
      lat: 55.9 - Math.floor(i / 7) * 0.8, lon: 1.8 + (i % 7) * 0.95, heading: (i * 53) % 360, speed: 5 + (i % 4) * 4, t: nowMs, age: 0, path: [] });
  });
  push({ id: "drone:RID1", kind: "drone", label: "DJI-AIR3", sub: "Remote ID", src: "rid", lat: 51.4, lon: 11.2, alt: 80, heading: 20, speed: 9, t: nowMs, age: 0, path: [] });
  push({ id: "drone:FW1", kind: "drone", label: "WINGTRA", sub: "ADS-B", src: "adsb", extra: "fixed wing", lat: 51.4, lon: 12.8, alt: 120, heading: 250, speed: 40, t: nowMs, age: 0, path: [] });
  [["WX-HOME", "weather station", 51.0, 9.4], ["TRUCK-1", "truck", 51.0, 10.6], ["HAB-1", "balloon", 51.0, 11.8], ["BOAT-1", "boat", 51.0, 13.0]].forEach(([cs, ex, lat, lon]) => {
    push({ id: "aprs:" + cs, kind: "aprs", label: cs, sub: "APRS", src: "aprs", extra: ex, lat, lon, t: nowMs, age: 0, path: [] });
  });
  [["G05", "GPS", 49.0, 14.4], ["E11", "Galileo", 49.0, 15.4], ["C08", "BeiDou", 49.0, 16.4], ["R17", "GLONASS", 49.0, 17.4]].forEach(([n, sub, lat, lon]) => {
    push({ id: "gnss:" + n, kind: "gnss", label: n, sub, src: "gnss", lat, lon, alt: 20000, t: nowMs, age: 0, path: [] });
  });
  // amateur contacts, in the exact form the desktop app posts them (ham_mapfeed.cpp): the station is the track, extra carries the contact hints
  [["LY2XX", "FT8", 14075200, "DL1ABC", 48.5, 11.0, 54.5, 25.0, 40], ["DL1ABC", "FT8", 14076100, "LY2XX", 54.5, 25.0, 48.5, 11.0, 200], ["W1AW", "FT4", 14081000, "EA8ZZ", 28.5, -16.0, 41.5, -73.0, 600],
    ["JA1YY", "WSPR", 14097100, "VK3AA", -37.5, 145.0, 35.5, 139.0, 900], ["OH2BB", "JS8", 7078000, "SM5CC", 59.5, 17.0, 60.5, 25.0, 1500], ["ZL1QQ", "QSO", 21300000, "KH6WW", 21.5, -158.0, -37.5, 175.0, 2400],
    ["F5ABC", "FT8", 28074500, "PY2XX", -23.5, -47.0, 47.5, 5.0, 90], ["SP9ZZ", "WSPR", 10138700, "G4ABC", 52.5, -1.0, 50.5, 19.0, 300]].forEach(([cs, mode, f, to, tlat, tlon, lat, lon, ageS]) => {
    const band = f < 8e6 ? "40m" : f < 11e6 ? "30m" : f < 15e6 ? "20m" : f < 22e6 ? "15m" : "10m";
    push({ id: "ham:" + cs, kind: "ham", label: cs, sub: "ham " + mode, src: "ham", lat, lon, t: nowMs, age: ageS,
      extra: `{"m":"${mode}","f":${f},"b":"${band}","to":"${to}","tlat":${tlat},"tlon":${tlon}}`, path: [] });
  });
  // direction finding demo: three stations around Vilnius hear a 433.92 MHz transmitter; the server would intersect the lines into a fix
  {
    const tx = { lat: 54.70, lon: 25.30 };
    [[250, 24], [120, 31], [10, 19]].forEach(([brg, km], i) => {
      const [lon, lat] = destination(tx.lat, tx.lon, brg, km * 1000);
      const back = (Math.atan2(Math.sin((tx.lon - lon) * Math.PI / 180) * Math.cos(tx.lat * Math.PI / 180),
        Math.cos(lat * Math.PI / 180) * Math.sin(tx.lat * Math.PI / 180) - Math.sin(lat * Math.PI / 180) * Math.cos(tx.lat * Math.PI / 180) * Math.cos((tx.lon - lon) * Math.PI / 180)) * 180 / Math.PI + 360) % 360;
      push({ id: "doa:demo" + i + "-433920", kind: "doa", label: "433.920 MHz", sub: Math.round(back) + "\u00b0", lat, lon, heading: back + (i - 1) * 1.5, alt: null, speed: null,
        extra: JSON.stringify({ f: 433920000, s: 3, c: 0.85 }), t: nowMs, src: "df", age: 0 });
    });
    push({ id: "fix:433920", kind: "fix", label: "433.920 MHz", sub: "3 stations", lat: tx.lat + 0.002, lon: tx.lon - 0.003, alt: null, heading: null, speed: null,
      extra: JSON.stringify({ a: 1900, b: 700, o: 35, n: 3, r: 1.4, f: 433920000 }), t: nowMs, src: "df", age: 0 });
  }
  return { tracks, meta };
}

/** Move every demo track along its heading by dt seconds (speed in knots; ships and aircraft only). */
export function stepDemo(tracks, dtSec, nowMs = Date.now()) {
  for (const t of tracks) {
    if (t.heading == null || !t.speed || (t.kind !== "aircraft" && t.kind !== "vessel" && t.kind !== "flarm")) { t.t = nowMs; continue; }
    const nm = t.speed * dtSec / 3600;
    const rad = Math.PI / 180, r = nm / 3440.065, h = t.heading * rad, p1 = t.lat * rad;
    const lat2 = Math.asin(Math.sin(p1) * Math.cos(r) + Math.cos(p1) * Math.sin(r) * Math.cos(h));
    const lon2 = t.lon * rad + Math.atan2(Math.sin(h) * Math.sin(r) * Math.cos(p1), Math.cos(r) - Math.sin(p1) * Math.sin(lat2));
    t.lat = lat2 / rad; t.lon = lon2 / rad; t.t = nowMs; t.age = 0;
    t.path = (t.path || []).concat([[t.lon, t.lat]]).slice(-80);
  }
}
