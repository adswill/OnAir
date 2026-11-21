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
const MHZ = 1000000, KHZ = 1000, EPS = 0.000000001;   // plain literals: PMD misreads exponent notation
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
test("decodeSpectrum keeps the 8-bit bins next to the dB values", () => {
  const buf = new ArrayBuffer(21 + 3), d = new DataView(buf);
  d.setUint8(0, 1); d.setFloat32(5, MHZ, true); d.setFloat32(9, KHZ, true); d.setFloat32(13, -100, true); d.setFloat32(17, -50, true);
  d.setUint8(21, 0); d.setUint8(22, 128); d.setUint8(23, 255);
  const f = L.decodeSpectrum(buf);
  assert.deepStrictEqual(Array.from(f.u8), [0, 128, 255]);
  near(f.db[2], -50, 0.001);
});

// ---- levels and colours from the 8-bit bins
test("binLevels: the 10th percentile and the maximum agree with a sort of the dB values", () => {
  const floor = -120, top = -20, n = 200, u8 = new Uint8Array(n);
  let seed = 7;
  for (let i = 0; i < n; i++) { seed = (seed * 1103515245 + 12345) & 0x7fffffff; u8[i] = seed % 256; }
  for (const frac of [0, 0.1, 0.5, 0.99]) {
    const sorted = Array.from(u8).sort((a, b) => a - b).map(v => floor + v / 255 * (top - floor));
    const r = L.binLevels(u8, frac, floor, top);
    near(r.lowDb, sorted[Math.floor(n * frac)], 1e-9, "frac " + frac);
    near(r.highDb, sorted[n - 1], 1e-9, "max");
  }
});
test("binLevels: flat, single-bin and empty frames", () => {
  const flat = new Uint8Array(50).fill(100);
  const r = L.binLevels(flat, 0.1, -100, 0);
  near(r.lowDb, -100 + 100 / 255 * 100, EPS); near(r.highDb, r.lowDb, EPS);
  near(L.binLevels(Uint8Array.from([255]), 0.1, -90, -30).lowDb, -30, EPS);
  assert.deepStrictEqual(L.binLevels(new Uint8Array(0), 0.1, -90, -30), { lowDb: -90, highDb: -90 });
  near(L.binLevels(Uint8Array.from([0, 0, 0, 0, 0, 0, 0, 0, 0, 255]), 0.1, -100, 0).lowDb, -100, EPS);   // index 1 of 10 is still a 0
});
test("colourTable: bins map through the frame's own floor and top into the display range", () => {
  const pal = new Uint8ClampedArray(768);
  for (let i = 0; i < 256; i++) { pal[3 * i] = i; pal[3 * i + 1] = 255 - i; pal[3 * i + 2] = 7; }
  // the frame spans exactly the display range: bin value = palette index
  const t = L.colourTable(pal, -100, 0, -100, 0);
  assert.strictEqual(t.length, 1024);
  assert.deepStrictEqual([t[0], t[1], t[2], t[3]], [0, 255, 7, 255]);
  assert.deepStrictEqual([t[4 * 255], t[4 * 255 + 1], t[4 * 255 + 3]], [255, 0, 255]);
  assert.strictEqual(t[4 * 128], 128);
  // a frame with another top: bin 255 is -50 dB, halfway up the range
  const h = L.colourTable(pal, -100, -50, -100, 0);
  assert.strictEqual(h[4 * 255], Math.round(0.5 * 255));
  // values outside the range clamp; the out buffer is reused
  const o = new Uint8ClampedArray(1024), r = L.colourTable(pal, -100, 100, -100, 0, o);
  assert.strictEqual(r, o); assert.strictEqual(o[4 * 255], 255); assert.strictEqual(o[4 * 255 + 1], 0);
  // an empty range does not divide by zero
  assert.strictEqual(L.colourTable(pal, -50, -50, -50, -50)[4], 0);
});

test("autoBins: one bin per canvas pixel inside the server's 64..4096", () => {
  assert.strictEqual(L.autoBins(1500), 1500);
  assert.strictEqual(L.autoBins(1500.4), 1500);
  assert.strictEqual(L.autoBins(10), 64);
  assert.strictEqual(L.autoBins(9000), 4096);
  assert.strictEqual(L.autoBins(0), 1024); assert.strictEqual(L.autoBins(NaN), 1024); assert.strictEqual(L.autoBins(undefined), 1024);
});

// ---- the waterfall's row history
test("RowRing keeps the newest rows first and forgets the oldest", () => {
  const r = new L.RowRing(3);
  assert.strictEqual(r.get(0), null);
  for (let i = 1; i <= 5; i++) r.push({ id: i });
  assert.strictEqual(r.count, 3);
  assert.deepStrictEqual([r.get(0).id, r.get(1).id, r.get(2).id], [5, 4, 3]);
  assert.strictEqual(r.get(3), null); assert.strictEqual(r.get(-1), null);
  r.clear(); assert.strictEqual(r.count, 0); assert.strictEqual(r.get(0), null);
});
test("RowRing.grow keeps the stored rows in order and then keeps filling", () => {
  const r = new L.RowRing(3);
  for (let i = 1; i <= 5; i++) r.push({ id: i });   // holds 3, 4, 5
  r.grow(2); assert.strictEqual(r.cap, 3);
  r.grow(6);
  assert.strictEqual(r.cap, 6); assert.strictEqual(r.count, 3);
  assert.deepStrictEqual([r.get(0).id, r.get(1).id, r.get(2).id, r.get(3)], [5, 4, 3, null]);
  for (let i = 6; i <= 9; i++) r.push({ id: i });
  assert.strictEqual(r.count, 6);
  assert.deepStrictEqual([0, 1, 2, 3, 4, 5].map(a => r.get(a).id), [9, 8, 7, 6, 5, 4]);
  const e = new L.RowRing(2); e.grow(4); e.push({ id: 1 }); assert.strictEqual(e.get(0).id, 1);
});
test("rowToPixels: the same span maps bin to pixel, several bins take the maximum", () => {
  const row = { u8: Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8]), centreHz: 100 * MHZ, rateHz: 8 * KHZ, floorDb: -100, topDb: 0 };
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ, 8 * KHZ, 8)), [1, 2, 3, 4, 5, 6, 7, 8]);
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ, 8 * KHZ, 4)), [2, 4, 6, 8]);
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ, 8 * KHZ, 16)).slice(0, 4), [1, 1, 2, 2]);
});
test("rowToPixels: equal spans with as many pixels as bins map bin x to pixel x exactly", () => {
  for (const [n, rate, centre] of [[1337, 2.4e6, 100e6], [1920, 3.2e6, 433.92e6], [1024, 2e6, 98.1e6], [4096, 2.048e6, 1.5e9]]) {
    const u8 = new Uint8Array(n).map((_, i) => (i * 7 + 3) & 255);
    const out = L.rowToPixels({ u8, centreHz: centre, rateHz: rate }, centre, rate, n);
    let bad = 0;
    for (let x = 0; x < n; x++) if (out[x] !== u8[x]) bad++;
    assert.strictEqual(bad, 0, n + " bins: " + bad + " wrong pixels");
  }
});
test("rowToPixels: a zoomed view picks the bins by frequency, uncovered pixels are -1", () => {
  const row = { u8: Uint8Array.from([10, 20, 30, 40]), centreHz: 100 * MHZ, rateHz: 4 * KHZ, floorDb: -100, topDb: 0 };   // 99.998 .. 100.002 MHz
  // the upper half of the row's span
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ + KHZ, 2 * KHZ, 2)), [30, 40]);
  // a view twice as wide, same centre: the row covers the middle half
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ, 8 * KHZ, 4)), [-1, 20, 40, -1]);
  // retuned past the row: nothing is covered
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 120 * MHZ, 4 * KHZ, 4)), [-1, -1, -1, -1]);
  // a view that overlaps the row's upper edge only
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ + 3 * KHZ, 4 * KHZ, 4)), [40, -1, -1, -1]);
  // degenerate input
  assert.deepStrictEqual(Array.from(L.rowToPixels({ u8: new Uint8Array(0), centreHz: 1, rateHz: 1 }, 1, 1, 3)), [-1, -1, -1]);
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, 100 * MHZ, 0, 2)), [-1, -1]);
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

// ---- SpectrumView with fake canvases (no browser)
function spectrumFixture(specW, specH, wfW, wfH) {
  const frames = [];
  globalThis.OnAirLib = L;
  globalThis.requestAnimationFrame = (f) => { frames.push(f); return frames.length; };
  globalThis.cancelAnimationFrame = () => { frames.length = 0; };
  const mk = (w, h) => {
    const ctx = { puts: 0, copies: 0, fills: 0, fillRect() { this.fills++; }, drawImage() { this.copies++; }, putImageData() { this.puts++; },
      createImageData(iw, ih) { return { width: iw, height: ih, data: new Uint8ClampedArray(iw * ih * 4) }; },
      beginPath() {}, moveTo() {}, lineTo() {}, closePath() {}, fill() {}, stroke() {}, fillText() {}, measureText() { return { width: 5 }; }, setTransform() {} };
    const cv = { width: w, height: h, cssW: w, cssH: h, parentElement: { addEventListener() {}, getBoundingClientRect() { return { left: 0, width: cv.cssW, height: cv.cssH }; } },
      getContext() { return ctx; }, getBoundingClientRect() { return { width: cv.cssW, height: cv.cssH }; }, ctx };
    return cv;
  };
  delete require.cache[require.resolve("../../web/onair-app/spectrum.js")];
  require("../../web/onair-app/spectrum.js");
  const spec = mk(specW, specH), wf = mk(wfW, wfH);
  const view = new globalThis.SpectrumView(spec, wf, { style: {} }, () => {});
  const frame = (centreHz, n) => {
    const u8 = new Uint8Array(n || 16).map((_, i) => i * 8);
    return { centreHz, rateHz: 2 * MHZ, floorDb: -100, topDb: -20, u8, db: Float32Array.from(u8, v => -100 + v / 255 * 80) };
  };
  const flush = () => { const f = frames.splice(0); f.forEach(fn => fn()); };
  return { view, spec, wf, frame, flush, frames };
}
test("SpectrumView: an empty frame is ignored", () => {
  const t = spectrumFixture(100, 50, 100, 40);
  t.view.push({ centreHz: MHZ, rateHz: MHZ, floorDb: 0, topDb: 1, u8: new Uint8Array(0), db: new Float32Array(0) });
  assert.strictEqual(t.view.frame, null); assert.strictEqual(t.view.pending, 0); assert.strictEqual(t.frames.length, 0);
  t.view.push(t.frame(100 * MHZ)); t.flush();
  t.view.push({ centreHz: 5 * MHZ, rateHz: MHZ, floorDb: 0, topDb: 1, u8: new Uint8Array(0), db: new Float32Array(0) });
  assert.strictEqual(t.view.frame.centreHz, 100 * MHZ, "the last real frame stays");
  assert.strictEqual(t.frames.length, 0, "no repaint is scheduled");
});
test("SpectrumView: the paint is one animation frame; incremental while the span holds, full after a retune", () => {
  const t = spectrumFixture(100, 50, 100, 40), c = t.wf.ctx;
  t.view.push(t.frame(100 * MHZ)); t.view.push(t.frame(100 * MHZ));
  assert.strictEqual(c.puts, 0, "push alone paints nothing"); assert.strictEqual(t.frames.length, 1);
  t.flush();
  assert.strictEqual(c.puts, 2, "the first paint draws both stored rows"); assert.strictEqual(c.copies, 0);
  c.puts = 0;
  t.view.push(t.frame(100e6)); t.view.push(t.frame(100e6)); t.view.push(t.frame(100e6)); t.flush();
  assert.strictEqual(c.copies, 1, "the picture scrolls once"); assert.strictEqual(c.puts, 3, "and every new row is painted");
  c.puts = 0; c.copies = 0;
  t.view.push(t.frame(101e6)); t.flush();
  assert.strictEqual(c.copies, 0, "a retune paints again from the history"); assert.strictEqual(c.puts, 6);
  c.puts = 0; t.view.setRange(-90, -30); t.flush();
  assert.strictEqual(c.puts, 6, "a range change too");
});
test("SpectrumView: clear forgets the frame, so a resize does not draw the old trace", () => {
  globalThis.devicePixelRatio = 1;
  const t = spectrumFixture(100, 50, 100, 40);
  t.view.push(t.frame(100e6)); t.flush();
  t.view.clear();
  assert.strictEqual(t.view.frame, null);
  t.wf.ctx.puts = 0; t.wf.cssW = 120;
  t.view.resize();
  assert.strictEqual(t.wf.ctx.puts, 0); assert.strictEqual(t.frames.length, 0);
});
test("SpectrumView: a resize repaints at once, keeps the history, grows the ring and tracks the pixel ratio", () => {
  globalThis.devicePixelRatio = 2;
  const t = spectrumFixture(100, 50, 100, 40);
  t.view.push(t.frame(100e6)); t.flush();
  t.view.push(t.frame(100e6));   // a paint is scheduled but has not run
  t.spec.cssW = 150; t.spec.cssH = 60; t.wf.cssW = 150; t.wf.cssH = 3000;
  t.wf.ctx.puts = 0;
  t.view.resize();
  assert.strictEqual(t.wf.width, 300); assert.strictEqual(t.wf.height, 6000); assert.strictEqual(t.view.dpr, 2);
  assert.strictEqual(t.wf.ctx.puts, 2, "painted synchronously from the stored rows"); assert.strictEqual(t.frames.length, 0, "no pending frame left");
  assert.strictEqual(t.view.ring.cap, 4096, "capped"); assert.strictEqual(t.view.ring.count, 2);
  // a hidden canvas (size 0) changes neither size nor ratio
  globalThis.devicePixelRatio = 3; t.spec.cssW = 0; t.wf.cssW = 0;
  t.view.resize();
  assert.strictEqual(t.view.dpr, 2); assert.strictEqual(t.wf.width, 300);
  delete globalThis.devicePixelRatio;
});

console.log("app.test.js: " + passed + " passed" + (process.exitCode ? ", some FAILED" : ""));
