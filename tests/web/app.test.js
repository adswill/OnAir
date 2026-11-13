// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web 3.9: unit tests of the page's pure code (web/onair-app/lib.js, worklet.js). Node built-ins only: node tests/web/app.test.js
"use strict";
const assert = require("assert");
const path = require("path");
const L = require(path.join(__dirname, "../../web/onair-app/lib.js"));
const { Ring } = require(path.join(__dirname, "../../web/onair-app/worklet.js"));

let passed = 0;
function test(name, fn) { try { fn(); passed++; } catch (e) { console.error("FAIL " + name + "\n  " + (e && e.stack || e)); process.exitCode = 1; } }
const near = (a, b, eps, m) => assert.ok(Math.abs(a - b) <= eps, (m || "") + " " + a + " vs " + b);

// ---- frequencies
test("parseFreq accepts the usual ways people type a frequency", () => {
  assert.strictEqual(L.parseFreq("100.5"), 100500000);
  assert.strictEqual(L.parseFreq("100,5"), 100500000);
  assert.strictEqual(L.parseFreq("100.5 MHz"), 100500000);
  assert.strictEqual(L.parseFreq("145800k"), 145800000);
  assert.strictEqual(L.parseFreq("145800 kHz"), 145800000);
  assert.strictEqual(L.parseFreq("7074000"), 7074000);
  assert.strictEqual(L.parseFreq("7.074M"), 7074000);
  assert.strictEqual(L.parseFreq("1.2 GHz"), 1200000000);
  assert.strictEqual(L.parseFreq("14074 "), 14074000);   // 5 digits: kHz
  assert.ok(isNaN(L.parseFreq("abc")));
  assert.ok(isNaN(L.parseFreq("")));
  assert.ok(isNaN(L.parseFreq("-5")));
});
test("formatMhz keeps 6 decimals below 1 GHz", () => {
  assert.strictEqual(L.formatMhz(100500000), "100.500000");
  assert.strictEqual(L.formatMhz(1090e6), "1090.0000");
  assert.strictEqual(L.formatMhz(NaN), "---.------");
});
test("tuneStep scales with the span", () => {
  assert.strictEqual(L.tuneStep(10e6), 25e3);
  assert.strictEqual(L.tuneStep(2.4e6), 5e3);
  assert.strictEqual(L.tuneStep(250e3), 1e3);
  assert.strictEqual(L.tuneStep(50e3), 100);
});
test("x <-> Hz round trip", () => {
  near(L.hzToX(L.xToHz(0.25, 100e6, 2e6), 100e6, 2e6), 0.25, 1e-9);
  assert.strictEqual(L.xToHz(0.5, 100e6, 2e6), 100e6);
  assert.strictEqual(L.hzToX(1, 0, 0), 0.5);
});

// ---- frames
test("decodeSpectrum reads the server's u8 frame", () => {
  const n = 8, buf = new ArrayBuffer(21 + n), d = new DataView(buf);
  d.setUint8(0, 1); d.setUint32(1, 42, true); d.setFloat32(5, 100e6, true); d.setFloat32(9, 2e6, true); d.setFloat32(13, -120, true); d.setFloat32(17, 0, true);
  for (let i = 0; i < n; i++) d.setUint8(21 + i, i * 32);
  const f = L.decodeSpectrum(buf);
  assert.strictEqual(f.seq, 42); near(f.centreHz, 100e6, 10); near(f.rateHz, 2e6, 1); assert.strictEqual(f.db.length, n);
  near(f.db[0], -120, 1e-3); near(f.db[4], -120 + 128 / 255 * 120, 1e-3);
  assert.strictEqual(L.decodeSpectrum(new ArrayBuffer(3)), null);
  d.setUint8(0, 9); assert.strictEqual(L.decodeSpectrum(buf), null);
});
test("decodeAudio reads PCM16 with its rate", () => {
  const buf = new ArrayBuffer(5 + 4), d = new DataView(buf);
  d.setUint8(0, 2); d.setUint32(1, 16000, true); d.setInt16(5, 16384, true); d.setInt16(7, -32768, true);
  const a = L.decodeAudio(buf);
  assert.strictEqual(a.rate, 16000); assert.strictEqual(a.samples.length, 2); near(a.samples[0], 0.5, 1e-6); near(a.samples[1], -1, 1e-6);
});

// ---- mode search
const groups = ["TV", "AVIATION", "HF"];
const modes = [
  { family: 11, name: "ADS-B", group: 1, blurb: "aircraft positions on 1090 MHz", sub: "" },
  { family: 25, name: "FT8", group: 2, blurb: "weak-signal contacts", sub: "ft8" },
  { family: 25, name: "WSPR", group: 2, blurb: "propagation beacons", sub: "wspr" },
  { family: 0, name: "DVB", group: 0, blurb: "DVB-T2 and DVB-T television", sub: "" },
];
test("modeSearch ranks exact names first and searches blurbs and groups", () => {
  assert.deepStrictEqual(L.modeSearch(modes, groups, "ft8").map(m => m.name), ["FT8"]);
  assert.deepStrictEqual(L.modeSearch(modes, groups, "aircraft").map(m => m.name), ["ADS-B"]);
  assert.deepStrictEqual(L.modeSearch(modes, groups, "hf").map(m => m.name), ["FT8", "WSPR"]);
  assert.deepStrictEqual(L.modeSearch(modes, groups, "tele").map(m => m.name), ["DVB"]);
  assert.strictEqual(L.modeSearch(modes, groups, "").length, 4);
  assert.strictEqual(L.modeSearch(modes, groups, "nothing here").length, 0);
  assert.deepStrictEqual(L.modeSearch(modes, groups, "weak signal").map(m => m.name), ["FT8"]);   // every word must match
});

// ---- the panel model
const schema = [
  { n: "seq", k: "num", l: "Seq" }, { n: "snrDb", k: "num", l: "SNR", u: "dB" },
  { n: "msgsPerSec", k: "num", l: "Messages per second" }, { n: "noiseDbfs", k: "num", l: "Noise floor", u: "dBFS" },
  { n: "refValid", k: "bool", l: "Reference position" }, { n: "name", k: "text", l: "Name" },
  { n: "aircraft", k: "table", l: "Aircraft", map: { lat: "lat", lon: "lon", label: "callsign" } },
  { n: "frames", k: "table", l: "Frames" }, { n: "trace", k: "list", l: "Trace" }, { n: "log", k: "lines", l: "Log" },
  { n: "iq", k: "const", l: "Constellation" }, { n: "stats", k: "obj", l: "Stats" }, { n: "image", k: "image", l: "Picture" },
  { n: "a1", k: "num", l: "a1" }, { n: "a2", k: "num", l: "a2" }, { n: "a3", k: "num", l: "a3" }, { n: "a4", k: "num", l: "a4" }, { n: "a5", k: "num", l: "a5" },
  { n: "a6", k: "num", l: "a6" }, { n: "a7", k: "num", l: "a7" }, { n: "a8", k: "num", l: "a8" }, { n: "a9", k: "num", l: "a9" },
];
const fields = {
  seq: 5, snrDb: 12.5, msgsPerSec: 3.25, noiseDbfs: -98, refValid: true, name: "x",
  aircraft: [{ icao: "4CA1B2", callsign: "RYR1", lat: 54.7, lon: 25.3, alt: 35000 }, { icao: "4CA1B3", callsign: "", lat: 0, lon: 0, alt: 1000, extra: { a: 1 } }],
  frames: [], trace: [1, 2, 3], log: ["a", "b"], iq: [1, 0, -1, 0, 0, 1, 0, -1], stats: { max: 3 }, image: "data:image/png;base64,AAAA",
  a1: 1, a2: 2, a3: 3, a4: 4, a5: 5, a6: 6, a7: 7, a8: 8, a9: 9,
};
const common = { snrDb: 12.5, locked: true, state: 2, cfoHz: -120.5, blocksOk: 10, blocksBad: 1, quality: { label: "good", percent: 80 } };
test("buildPanel: common block first, simple tier limits plain fields, tables / lists / const / image always shown", () => {
  const w = L.buildPanel(schema, fields, common, false);
  const names = w.map(x => x.name);
  assert.deepStrictEqual(names.slice(0, 6), ["snrDb", "locked", "state", "cfoHz", "blocksOk", "blocksBad"]);
  assert.ok(names.includes("quality"));
  assert.ok(!names.includes("seq"), "seq is not shown");
  assert.ok(names.includes("aircraft") && names.includes("trace") && names.includes("log") && names.includes("iq") && names.includes("stats") && names.includes("image"));
  assert.ok(!names.includes("frames"), "an empty table is hidden in the simple tier");
  assert.ok(!names.includes("a9"), "the 10th own plain field and beyond are advanced");
  const adv = L.buildPanel(schema, fields, common, true);
  assert.ok(adv.map(x => x.name).includes("a9"));
  assert.ok(adv.map(x => x.name).includes("frames"), "advanced shows the empty table as a note");
  const t = w.find(x => x.name === "aircraft");
  assert.deepStrictEqual(t.cols.map(c => c.name), ["icao", "callsign", "lat", "lon", "alt"], "columns are the rows' primitive fields");
  assert.deepStrictEqual(t.map, { lat: "lat", lon: "lon", label: "callsign" });
  const snr = w.find(x => x.name === "snrDb");
  assert.strictEqual(snr.unit, "dB");
});
test("buildPanel tolerates a missing schema and empty data", () => {
  assert.deepStrictEqual(L.buildPanel(null, null, null, false), []);
  const w = L.buildPanel([], {}, { snrDb: 3 }, false);
  assert.strictEqual(w.length, 1);
});
test("formatValue", () => {
  assert.strictEqual(L.formatValue(12.3456, ""), "12.35");
  assert.strictEqual(L.formatValue(0.12345, ""), "0.123");
  assert.strictEqual(L.formatValue(1234567, ""), "1,234,567");
  assert.strictEqual(L.formatValue(145800000, "Hz"), "145.8000 M");
  assert.strictEqual(L.formatValue(12500, "Hz"), "12.50 k");
  assert.strictEqual(L.formatValue(true, ""), "yes");
  assert.strictEqual(L.formatValue(null, ""), "");
  assert.strictEqual(L.formatValue(NaN, ""), "-");
});
test("humanize", () => {
  assert.strictEqual(L.humanize("msgsPerSec"), "Msgs Per Sec");
  assert.strictEqual(L.humanize("snrDb"), "SNR dB");
  assert.strictEqual(L.humanize("icao"), "Icao");
});

// ---- settings
test("groupSettings keeps the server's group order and drops empty groups", () => {
  const g = L.groupSettings([{ key: "a", group: "Radio" }, { key: "b", group: "Access" }], ["Access", "Server", "Radio"]);
  assert.deepStrictEqual(g.map(x => x.group), ["Access", "Radio"]);
});
test("settingValue types the inputs", () => {
  assert.strictEqual(L.settingValue("bool", true), true);
  assert.strictEqual(L.settingValue("int", "12"), 12);
  assert.ok(isNaN(L.settingValue("int", "1.5")));
  assert.strictEqual(L.settingValue("text", 5), "5");
});
test("permissions", () => {
  assert.deepStrictEqual(L.permissions({ role: "viewer", control: true, owner: false }), { control: true, owner: false, role: "viewer" });
  assert.deepStrictEqual(L.permissions(null), { control: false, owner: false, role: "none" });
});

// ---- the audio ring
test("Ring: fills to the target before playing, resamples, counts underruns and drops", () => {
  const r = new Ring(48000, 0.1, 0.4);
  const out = new Float32Array(480);
  r.pull(out);
  assert.ok(out.every(v => v === 0), "silence before the target is reached");
  // 200 ms of a 1 kHz tone at 16 kHz
  const tone = new Float32Array(3200);
  for (let i = 0; i < tone.length; i++) tone[i] = Math.sin(2 * Math.PI * 1000 * i / 16000);
  r.push(tone, 16000);
  near(r.seconds, 0.2, 1e-6);
  r.pull(out);
  // the output is the tone at 48 kHz: one period is 48 samples
  let maxErr = 0;
  for (let i = 0; i < 480; i++) maxErr = Math.max(maxErr, Math.abs(out[i] - Math.sin(2 * Math.PI * 1000 * i / 48000)));
  assert.ok(maxErr < 0.1, "resampled tone error " + maxErr);
  near(r.seconds, 0.19, 1e-3);
  // drain -> underrun
  for (let i = 0; i < 30; i++) r.pull(out);
  assert.ok(r.underruns >= 1);
  // too much queued -> drops
  for (let i = 0; i < 10; i++) r.push(new Float32Array(1600), 16000);
  assert.ok(r.drops >= 1 && r.seconds <= 0.41, "drops " + r.drops + " seconds " + r.seconds);
  // a rate change flushes
  r.push(new Float32Array(100), 8000);
  assert.strictEqual(r.inRate, 8000);
  near(r.seconds, 100 / 8000, 1e-9);
});
test("Ring: gain applies", () => {
  const r = new Ring(16000, 0.01, 0.4);
  r.gain = 0.5;
  r.push(new Float32Array(1600).fill(1), 16000);
  const out = new Float32Array(16);
  r.pull(out);
  near(out[0], 0.5, 1e-6);
});

test("waterfall zoom: steps, limits, centre inside the capture, zoom about the pointer, follow the tuning", () => {
  assert.strictEqual(L.zoomStep(1, 1), 2);
  assert.strictEqual(L.zoomStep(2, 1), 4);
  assert.strictEqual(L.zoomStep(4, -1), 2);
  assert.strictEqual(L.zoomStep(1, -1), 1);
  assert.strictEqual(L.zoomStep(L.ZOOM_MAX, 1), L.ZOOM_MAX);
  assert.strictEqual(L.zoomStep(3, 0), 4);   // an odd value snaps to a power of two
  // a 2.4 MHz capture at 100 MHz, zoom 4 (600 kHz shown)
  assert.strictEqual(L.zoomCentre(100e6, 2.4e6, 4, 100.5e6), 100.5e6);
  assert.strictEqual(L.zoomCentre(100e6, 2.4e6, 4, 101.2e6), 100.9e6);   // the part stays inside: 101.2 - 0.3
  assert.strictEqual(L.zoomCentre(100e6, 2.4e6, 4, 98e6), 99.1e6);
  assert.strictEqual(L.zoomCentre(100e6, 2.4e6, 1, 98e6), 100e6);
  assert.strictEqual(L.zoomCentre(100e6, 2.4e6, 4, NaN), 100e6);
  // about the pointer: the frequency under it stays at the same place
  for (const [hz, x, z] of [[100.3e6, 0.75, 2], [99.5e6, 0.1, 8], [100e6, 0.5, 4]]) {
    const c = L.zoomAbout(hz, x, 100e6, 2.4e6, z), span = 2.4e6 / z;
    near(L.hzToX(hz, c, span), x, 1e-9, "pointer stays");
  }
  near(L.zoomAbout(98.85e6, 0.6, 100e6, 2.4e6, 2), 99.4e6, 1, "at the edge the part is held inside");
  assert.strictEqual(L.zoomFollow(100e6, 600e3, 100.1e6), null);
  assert.strictEqual(L.zoomFollow(100e6, 600e3, 100.29e6), 100.29e6);
  assert.strictEqual(L.zoomFollow(100e6, 600e3, 99.5e6), 99.5e6);
  assert.strictEqual(L.zoomFollow(100e6, 0, 99.5e6), null);
});
test("mode options: Simple / Advanced, values, kinds", () => {
  const schema = [
    { n: "setThresholdDb", k: "num", l: "Threshold Db", u: "dB", h: "over the floor", t: "s" },
    { n: "setScan", k: "bool", l: "Scan", t: "s" },
    { n: "setStandardHint", k: "choice", l: "Standard Hint", c: [{ v: 0, l: "automatic" }, { v: 1, l: "DVB-S only" }], t: "s" },
    { n: "setProtocols", k: "mask", l: "Protocols", c: [{ v: 1, l: "POCSAG" }, { v: 2, l: "FLEX" }], t: "s" },
    { n: "setEffort", k: "int", l: "Effort", t: "a" },
  ];
  const simple = L.buildOptions(schema, { setScan: 1 }, false);
  assert.deepStrictEqual(simple.map(o => o.name), ["setThresholdDb", "setScan", "setStandardHint", "setProtocols"]);
  assert.strictEqual(simple[1].value, 1);
  assert.strictEqual(simple[0].value, null);
  assert.strictEqual(simple[0].unit, "dB");
  assert.strictEqual(simple[2].choices.length, 2);
  assert.strictEqual(L.buildOptions(schema, {}, true).length, 5);
  assert.strictEqual(L.buildOptions(null, {}, true).length, 0);
  assert.strictEqual(L.optionValue("bool", true), 1);
  assert.strictEqual(L.optionValue("bool", false), 0);
  assert.strictEqual(L.optionValue("num", " 12.5 "), 12.5);
  assert.ok(isNaN(L.optionValue("num", "")));
  assert.ok(isNaN(L.optionValue("num", "abc")));
  assert.strictEqual(L.optionValue("int", "3"), 3);
  assert.ok(isNaN(L.optionValue("int", "3.5")));
  assert.strictEqual(L.optionValue("choice", "1"), 1);
  assert.strictEqual(L.maskToggle(1, 2, true), 3);
  assert.strictEqual(L.maskToggle(3, 1, false), 2);
  assert.strictEqual(L.maskToggle(0, 2, false), 0);
});

console.log("app.test.js: " + passed + " passed" + (process.exitCode ? ", some FAILED" : ""));
