// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// types.test.js - tests for public/types.js: ICAO type / ADS-B category / wake / motion / AIS type -> icon shape.
import test from "node:test";
import assert from "node:assert/strict";
import {
  aircraftShape, vesselShape, vesselScale, aprsShape, droneShape, iconShape, familyOfCategory, familyOfType, familyOfWake, familyOfMotion,
  isMilitaryHex,
} from "../public/types.js";
import { SHAPES } from "../public/shapes.js";

test("aircraft by ICAO type code", () => {
  const want = {
    A320: "narrow", B738: "narrow", B77W: "wide", A359: "wide", A388: "quad", B744: "quad", B52: "quad", E190: "regional", CRJ9: "regional",
    GLF5: "bizjet", C56X: "bizjet", AT72: "turboprop", DH8D: "turboprop", C172: "light", PA28: "light", BE58: "lighttwin", PA34: "lighttwin",
    EC45: "heli", UH60: "heli", F16: "fighter", F35: "fighter", EUFI: "delta", RFAL: "delta", C130: "transport", C17: "transport", A400: "transport",
    GLID: "glider", BALL: "balloon", ARSH: "airship", MQ9: "uav", DRON: "drone",
  };
  for (const [code, shape] of Object.entries(want)) assert.equal(aircraftShape({ icaoType: code }), shape, code);
  assert.equal(familyOfType("zzzz"), "");
});

test("order of evidence: type code, then category, military hex, wake class, motion, generic dart", () => {
  assert.equal(aircraftShape({ icaoType: "A320", cat: "A5" }), "narrow", "type code wins");
  assert.equal(aircraftShape({ cat: "A1" }), "light");
  assert.equal(aircraftShape({ cat: "A2" }), "regional");
  assert.equal(aircraftShape({ cat: "A5" }), "wide");
  assert.equal(aircraftShape({ cat: "A6" }), "fighter");
  assert.equal(aircraftShape({ cat: "A7" }), "heli");
  assert.equal(aircraftShape({ cat: "B1" }), "glider");
  assert.equal(aircraftShape({ cat: "B2" }), "balloon");
  assert.equal(aircraftShape({ cat: "B6" }), "uav");
  assert.equal(aircraftShape({ cat: "C1" }), "vehicle");
  assert.equal(aircraftShape({ cat: "C3" }), "obstacle");
  assert.equal(aircraftShape({ hex: "AE1234", speed: 520 }), "fighter", "military hex, fast");
  assert.equal(aircraftShape({ hex: "AE1234", speed: 250 }), "transport", "military hex, slow");
  assert.equal(aircraftShape({ hex: "AE1234", cat: "A3" }), "transport", "A3 is the weak bucket: the military hex refines it");
  assert.equal(aircraftShape({ wake: "H" }), "wide");
  assert.equal(aircraftShape({ wake: "J" }), "quad");
  assert.equal(aircraftShape({ wake: "L" }), "light");
  assert.equal(aircraftShape({ speed: 450, alt: 36000 }), "narrow", "jet cruise");
  assert.equal(aircraftShape({ speed: 90, alt: 1500 }), "light", "slow and low");
  assert.equal(aircraftShape({}), "plane", "nothing known: generic dart");
  assert.equal(aircraftShape({ speed: 200, alt: 12000 }), "plane", "ambiguous: generic dart");
  assert.equal(familyOfCategory("garbage"), "");
  assert.equal(familyOfWake("x"), "");
  assert.equal(familyOfMotion(NaN, NaN), "");
  assert.equal(isMilitaryHex("3C6444"), false);
});

test("ships by AIS type text or code", () => {
  const want = [
    [{ cat: "Cargo (70)" }, "ship_cargo"], [{ cat: "Tanker" }, "ship_tanker"], [{ extra: "type 60" }, "ship_passenger"], [{ cat: "Ferry" }, "ship_passenger"],
    [{ cat: "Fishing" }, "ship_fishing"], [{ extra: "type 30" }, "ship_fishing"], [{ cat: "Sailing" }, "ship_sailing"], [{ extra: "type 37" }, "ship_pleasure"],
    [{ cat: "Pleasure craft" }, "ship_pleasure"], [{ cat: "Tug" }, "ship_tug"], [{ extra: "type 52" }, "ship_tug"], [{ extra: "type 33" }, "ship_dredger"],
    [{ cat: "Dredger" }, "ship_dredger"], [{ extra: "type 35" }, "ship_military"], [{ cat: "Military ops" }, "ship_military"], [{ extra: "type 51" }, "ship_sar"],
    [{ cat: "Search and rescue" }, "ship_sar"], [{ extra: "type 50" }, "ship_pilot"], [{ cat: "Pilot vessel" }, "ship_pilot"], [{ extra: "type 42" }, "ship_hsc"],
    [{ cat: "High speed craft" }, "ship_hsc"], [{ cat: "Other (90)" }, "ship_other"], [{}, "ship_other"],
  ];
  for (const [t, shape] of want) assert.equal(vesselShape(t), shape, JSON.stringify(t));
});

test("ship size: from a length in the text, else typical for the type, clamped", () => {
  assert.ok(Math.abs(vesselScale({ shape: "ship_cargo", extra: "length 200" }) - 1.8) < 0.01);
  assert.ok(Math.abs(vesselScale({ shape: "ship_cargo", cat: "Cargo 100 m" }) - 1.175) < 0.01);
  assert.equal(vesselScale({ shape: "ship_cargo", length: 4000 }), 2);
  assert.equal(vesselScale({ shape: "ship_sailing", length: 6 }), 0.65, "floor");
  assert.equal(vesselScale({ shape: "ship_tug" }), 1);
  assert.equal(vesselScale({}), 1.05);
});

test("APRS, drones and the other layers", () => {
  assert.equal(aprsShape({ extra: "digipeater WIDE1" }), "aprs_digi");
  assert.equal(aprsShape({ extra: "home weather station" }), "aprs_wx");
  assert.equal(aprsShape({ extra: "house" }), "aprs_house");
  assert.equal(aprsShape({ extra: "mobile" }), "aprs_car");
  assert.equal(aprsShape({ extra: "truck" }), "aprs_truck");
  assert.equal(aprsShape({ extra: "balloon" }), "aprs_balloon");
  assert.equal(aprsShape({ extra: "boat" }), "aprs_ship");
  assert.equal(aprsShape({}), "aprs");
  assert.equal(droneShape({ sub: "Remote ID" }), "drone_rid");
  assert.equal(droneShape({ label: "DJI-MAVIC3" }), "drone_rid");
  assert.equal(droneShape({ extra: "fixed wing" }), "uav");
  assert.equal(droneShape({}), "drone");
  assert.equal(iconShape({ kind: "sonde" }), "sonde");
  assert.equal(iconShape({ kind: "sat" }), "sat");
  assert.equal(iconShape({ kind: "gnss", sub: "Galileo" }), "sat_gnss");
  assert.equal(iconShape({ kind: "flarm", cat: "Glider" }), "glider");
  assert.equal(iconShape({ kind: "aircraft", icaoType: "B738", speed: 5 }), "narrow");
});

test("every shape name the mapping can return has well-formed geometry", () => {
  const names = new Set(Object.keys(SHAPES));
  for (const n of ["narrow", "wide", "quad", "regional", "bizjet", "turboprop", "light", "lighttwin", "heli", "fighter", "delta", "transport", "glider",
    "balloon", "airship", "drone", "drone_rid", "uav", "plane", "vehicle", "obstacle", "ship_cargo", "ship_tanker", "ship_passenger", "ship_hsc",
    "ship_tug", "ship_fishing", "ship_sailing", "ship_pleasure", "ship_military", "ship_sar", "ship_pilot", "ship_dredger", "ship_other", "sonde",
    "sat", "sat_gnss", "aprs", "aprs_car", "aprs_truck", "aprs_house", "aprs_wx", "aprs_digi", "aprs_balloon", "aprs_ship", "ham", "buoy", "dsc", "station"]) {
    assert.ok(names.has(n), "missing " + n);
  }
  for (const [n, sh] of Object.entries(SHAPES)) {
    assert.ok(Array.isArray(sh.body) && sh.body.length, n);
    for (const d of [...sh.body, ...(sh.detail || []), ...(sh.lines || []), ...(sh.disc || []), ...(sh.ring || []), ...(sh.leader ? [sh.leader] : [])]) {
      assert.match(d, /^M[-0-9.]/, n);
      assert.ok(!/NaN|undefined/.test(d), n + " has a bad number");
      for (const v of d.replace(/a[-0-9.,\s]+/g, " ").match(/-?\d+\.?\d*/g)) assert.ok(Math.abs(+v) <= 33, `${n}: ${v} outside the box`);
    }
    if (n.startsWith("ship_")) assert.ok(sh.leader && sh.rotates, n + " needs a course leader");
  }
});
