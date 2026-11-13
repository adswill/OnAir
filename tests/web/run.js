// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Unit tests for the OnAir Web client's pure code (web/onair-web/onair-lib.js, onair-worklet.js). Node built-ins only:
//   node tests/web/run.js
"use strict";
const assert = require("assert");
const path = require("path");
const L = require(path.join(__dirname, "../../web/onair-web/onair-lib.js"));
const { RingPlayer } = require(path.join(__dirname, "../../web/onair-web/onair-worklet.js"));

let passed = 0;
function test(name, fn) { try { fn(); passed++; } catch (e) { console.error("FAIL " + name + "\n  " + (e && e.stack || e)); process.exitCode = 1; } }
const near = (a, b, eps, m) => assert.ok(Math.abs(a - b) <= eps, (m || "") + " " + a + " vs " + b);

// reference IMA ADPCM encoder (the standard algorithm), so the decoder is checked against an independent implementation
function adpcmEncode(samples, pred, idx) {
  const out = new Uint8Array((samples.length + 1) >> 1);
  for (let i = 0; i < samples.length; i++) {
    const step = L.STEP_TABLE[idx];
    let diff = samples[i] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let t = step, vd = step >> 3;
    if (diff >= t) { code |= 4; diff -= t; vd += t; }
    t >>= 1; if (diff >= t) { code |= 2; diff -= t; vd += t; }
    t >>= 1; if (diff >= t) { code |= 1; vd += t; }
    pred = Math.max(-32768, Math.min(32767, code & 8 ? pred - vd : pred + vd));
    idx = Math.max(0, Math.min(88, idx + [-1, -1, -1, -1, 2, 4, 6, 8][code & 7]));
    out[i >> 1] |= (i & 1) ? code << 4 : code;
  }
  return out;
}
function audioFrame(codec, samples, o) {
  o = o || {};
  const n = samples.length, payload = codec === 1 ? adpcmEncode(samples, o.pred || 0, o.idx || 0) : null;
  const buf = new ArrayBuffer(24 + (codec === 1 ? payload.length : n * 2)), v = new DataView(buf);
  v.setUint8(0, 0x20); v.setUint8(1, codec); v.setUint16(2, n, true); v.setUint32(4, o.seq || 0, true); v.setUint32(8, o.rate || 12000, true);
  v.setFloat32(12, o.level == null ? -60 : o.level, true); v.setUint8(16, o.flags || 0); v.setInt16(18, o.pred || 0, true); v.setUint8(20, o.idx || 0);
  if (codec === 1) new Uint8Array(buf, 24).set(payload); else samples.forEach((s, i) => v.setInt16(24 + i * 2, s, true));
  return buf;
}
function specFrame(levels, start, stop, floor, top, seq) {
  const buf = new ArrayBuffer(32 + levels.length), v = new DataView(buf);
  v.setUint8(0, 0x10); v.setUint8(1, 1); v.setUint16(2, levels.length, true); v.setUint32(4, seq || 0, true);
  v.setFloat64(8, start, true); v.setFloat64(16, stop, true); v.setFloat32(24, floor, true); v.setFloat32(28, top, true);
  new Uint8Array(buf, 32).set(levels); return buf;
}

test("adpcm: first code of a known vector", () => {
  // predictor 0, index 0 (step 7): code 0x7 = +7+3+1+0... diff = 0 + 7(4) + 3(2) + 1(1) + (7>>3=0) = 11
  const out = L.adpcmDecode(Uint8Array.of(0x07), 1, 0, 0);
  assert.strictEqual(out[0], 11);
  const neg = L.adpcmDecode(Uint8Array.of(0x0f), 1, 0, 0);
  assert.strictEqual(neg[0], -11);
});
test("adpcm: round trip of a sine matches the encoder's own reconstruction and stays close to the input", () => {
  const n = 480, x = new Int16Array(n);
  for (let i = 0; i < n; i++) x[i] = Math.round(12000 * Math.sin(i * 0.19) + 3000 * Math.sin(i * 0.71));
  const enc = adpcmEncode(x, 0, 0), dec = L.adpcmDecode(enc, n, 0, 0);
  let err = 0, sig = 0;
  for (let i = 0; i < n; i++) { err += (dec[i] - x[i]) ** 2; sig += x[i] ** 2; }
  assert.ok(10 * Math.log10(sig / err) > 15, "SNR " + 10 * Math.log10(sig / err));
});
test("adpcm: state clamps and odd sample counts", () => {
  const out = L.adpcmDecode(Uint8Array.of(0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77), 19, 32000, 88);
  for (const s of out) assert.ok(s <= 32767 && s >= -32768);
  assert.strictEqual(out[0], 32767);
  assert.strictEqual(L.adpcmDecode(Uint8Array.of(0x31, 0x2), 3, 0, 200).length, 3);   // index out of range is clamped
});
test("adpcm: every frame is independent (loss of a frame never breaks the next)", () => {
  const x = new Int16Array(480).map((_, i) => Math.round(9000 * Math.sin(i * 0.3)));
  // frame B starts with the decoder state at sample 240
  const encA = adpcmEncode(x.subarray(0, 240), 0, 0), decA = L.adpcmDecode(encA, 240, 0, 0);
  // recompute the index at the end of A with the reference encoder
  let idx = 0, pred = 0;
  for (let i = 0; i < 240; i++) { const d = adpcmEncode(x.subarray(i, i + 1), pred, idx); const c = d[0] & 15; const r = L.adpcmDecode(Uint8Array.of(c), 1, pred, idx)[0]; idx = Math.max(0, Math.min(88, idx + [-1, -1, -1, -1, 2, 4, 6, 8][c & 7])); pred = r; }
  assert.strictEqual(pred, decA[239]);
  const encB = adpcmEncode(x.subarray(240), pred, idx), a = L.adpcmDecode(encB, 240, pred, idx);
  const frame = audioFrame(1, x.subarray(240), { pred, idx, seq: 7 });
  const f = L.parseAudio(frame);
  assert.deepStrictEqual(Array.from(f.samples), Array.from(a));
  assert.strictEqual(f.seq, 7);
});
test("parseAudio: pcm, squelched, malformed", () => {
  const pcm = Int16Array.of(1, -2, 300, -32768, 32767);
  const f = L.parseAudio(audioFrame(0, pcm, { rate: 48000, level: -42.5, seq: 9 }));
  assert.deepStrictEqual(Array.from(f.samples), Array.from(pcm)); assert.strictEqual(f.rate, 48000); near(f.levelDb, -42.5, 1e-4);
  const sq = L.parseAudio(audioFrame(0, new Int16Array(480), { flags: 1 }));
  assert.ok(sq.squelched); assert.strictEqual(sq.samples.length, 480);
  assert.strictEqual(L.parseAudio(new ArrayBuffer(10)), null);
  assert.strictEqual(L.parseAudio(audioFrame(0, pcm).slice(0, 26)), null);          // truncated payload
  const bad = audioFrame(0, pcm); new DataView(bad).setUint8(1, 9); assert.strictEqual(L.parseAudio(bad), null);   // unknown codec
  const badRate = audioFrame(0, pcm); new DataView(badRate).setUint32(8, 0, true); assert.strictEqual(L.parseAudio(badRate), null);
  assert.strictEqual(L.parseAudio(specFrame([1, 2], 0, 1, 0, 1)), null);
});
test("parseSpectrum", () => {
  const r = L.parseSpectrum(specFrame(Uint8Array.of(0, 128, 255), 144e6, 146e6, -100, -20, 5));
  assert.strictEqual(r.bins, 3); assert.strictEqual(r.seq, 5); assert.strictEqual(r.startHz, 144e6); assert.strictEqual(r.levels[2], 255);
  near(L.levelToDb(r, 0), -100, 1e-3); near(L.levelToDb(r, 255), -20, 1e-3); near(L.levelToDb(r, 128), -100 + 80 * 128 / 255, 1e-3);
  assert.strictEqual(L.parseSpectrum(specFrame(Uint8Array.of(1), 5, 4, 0, 1)), null);           // stop <= start
  assert.strictEqual(L.parseSpectrum(specFrame(Uint8Array.of(1, 2, 3), 0, 1, 0, 1).slice(0, 33)), null);
  assert.strictEqual(L.parseSpectrum(new ArrayBuffer(8)), null);
  const v2 = specFrame(Uint8Array.of(1), 0, 1, 0, 1); new DataView(v2).setUint8(1, 2); assert.strictEqual(L.parseSpectrum(v2), null);
});
test("messages follow the protocol's ranges", () => {
  assert.deepStrictEqual(L.msg.hello("Kri<s>", "pw", ""), { type: "hello", proto: 1, name: "Kris", password: "pw" });
  assert.deepStrictEqual(L.msg.hello("", "", "tok"), { type: "hello", proto: 1, invite: "tok" });
  assert.strictEqual(L.msg.hello("x".repeat(80)).name.length, 32);
  assert.deepStrictEqual(L.msg.tune({ freq_hz: 145500000.4, mode: "nfm", low_hz: -6250, high_hz: 6250 }), { type: "tune", freq_hz: 145500000, mode: "nfm", low_hz: -6250, high_hz: 6250 });
  assert.deepStrictEqual(L.msg.tune({ freq_hz: 1, mode: "tx" }), { type: "tune", freq_hz: 1 });       // unknown modes never leave the client
  assert.deepStrictEqual(L.msg.tune({ low_hz: 5 }), { type: "tune" });                               // low without high is dropped
  assert.strictEqual(L.msg.rx({ squelch_db: 5 }).squelch_db, 0);
  assert.strictEqual(L.msg.rx({ gain_db: 99 }).gain_db, 60);
  assert.deepStrictEqual(L.msg.spectrum(true, 10, 99), { type: "spectrum", on: true, bins: 64, fps: 30 });
  assert.deepStrictEqual(L.msg.spectrum(true, 1024, 10, 1e6, 2e6), { type: "spectrum", on: true, bins: 1024, fps: 10, start_hz: 1e6, stop_hz: 2e6 });
  assert.deepStrictEqual(L.msg.audio(true, "adpcm", 12000), { type: "audio", on: true, codec: "adpcm", rate: 12000 });
  assert.deepStrictEqual(L.msg.subscribe("host"), { type: "subscribe", decoder: "host" });
  // none of the forbidden types can be built
  for (const k of Object.keys(L.msg)) assert.ok(!/tx|transmit|ptt|arm|key|record|decrypt/i.test(k), k);
});
test("parseServerText and closeAdvice", () => {
  assert.strictEqual(L.parseServerText("not json"), null);
  assert.strictEqual(L.parseServerText("[1]"), null);
  assert.strictEqual(L.parseServerText('{"a":1}'), null);
  assert.deepStrictEqual(L.parseServerText('{"type":"future","x":1}'), { type: "future", x: 1 });
  assert.strictEqual(L.closeAdvice(4029, 0).retry, true);
  assert.ok(L.closeAdvice(4029, 0).delayMs >= 5000);
  for (const c of [4001, 4040, 4041, 1000]) assert.strictEqual(L.closeAdvice(c, 0).retry, false, String(c));
  assert.strictEqual(L.closeAdvice(1006, 0).delayMs, 1000);
  assert.strictEqual(L.closeAdvice(1006, 20).delayMs, 30000);
});
test("frequency formatting and parsing", () => {
  assert.strictEqual(L.formatFreq(145500000), "0145.500.000");
  assert.strictEqual(L.formatFreq(7074000), "0007.074.000");
  assert.strictEqual(L.parseFreq("145.5"), 145500000);
  assert.strictEqual(L.parseFreq("7074 khz"), 7074000);
  assert.strictEqual(L.parseFreq("145,500"), 145500000);
  assert.strictEqual(L.parseFreq("145.500.000"), 145500000);
  assert.strictEqual(L.parseFreq("1.09ghz"), 1090000000);
  assert.ok(isNaN(L.parseFreq("abc"))); assert.ok(isNaN(L.parseFreq("")));
  assert.strictEqual(L.snap(145512300, 25000), 145500000);
  assert.strictEqual(L.stepLabel(12500), "12.5 kHz"); assert.strictEqual(L.stepLabel(1000000), "1 MHz"); assert.strictEqual(L.stepLabel(10), "10 Hz");
});
test("passbands respect the protocol's per-mode limits", () => {
  for (const m of L.MODES) {
    const [lo, hi] = L.MODE_LIMITS[m], r = L.MODE_BW_RANGE[m];
    for (const bw of [0, r[0], (r[0] + r[1]) / 2, r[1], 1e9]) {
      const p = L.passband(m, bw);
      assert.ok(p.low_hz >= lo && p.high_hz <= hi && p.high_hz - p.low_hz >= 50, m + " " + bw + " " + JSON.stringify(p));
    }
    const d = L.MODE_DEFAULT[m]; assert.ok(d[0] >= lo && d[1] <= hi);
  }
  assert.deepStrictEqual(L.passband("usb", 2700), { low_hz: 100, high_hz: 2800 });
  assert.deepStrictEqual(L.passband("lsb", 2700), { low_hz: -2800, high_hz: -100 });
  assert.deepStrictEqual(L.passband("nfm", 12500), { low_hz: -6250, high_hz: 6250 });
  assert.strictEqual(L.bandwidthOf(-6250, 6250), 12500);
});
test("view: map, zoom, pan, clamp", () => {
  const cap = { centre_hz: 145e6, rate_hz: 2.4e6 }, b = L.captureBounds(cap), f = L.fullBounds(cap);
  near(b.start, 145e6 - 1.152e6, 1); near(f.stop, 145e6 + 1.2e6, 1);
  const v = { start: 144e6, stop: 146e6 };
  assert.strictEqual(L.hzToX(v, 145e6, 1000), 500); assert.strictEqual(L.xToHz(v, 250, 1000), 144.5e6);
  const z = L.zoomView(v, f, 0.5, 145.5e6);
  near(z.stop - z.start, 1e6, 1); near((145.5e6 - z.start) / (z.stop - z.start), 0.75, 1e-9, "anchor stays");
  const zo = L.zoomView(v, f, 100, 145e6); assert.ok(zo.start >= f.start && zo.stop <= f.stop && L.viewIsFull(zo, f));
  const zi = L.zoomView(v, f, 1e-9, 145e6); near(zi.stop - zi.start, L.MIN_SPAN, 1);
  const p = L.panView(v, f, 10e6); assert.strictEqual(p.stop, f.stop); near(p.stop - p.start, 2e6, 1);
  const pl = L.panView(v, f, -10e6); assert.strictEqual(pl.start, f.start);
  const s = L.bandSlice({ start: 144e6, stop: 146e6 }, b); near(s.start, 144e6, 1);
  assert.strictEqual(L.bandSlice({ start: 100e6, stop: 101e6 }, b), null);
  const edge = L.bandSlice({ start: 140e6, stop: 160e6 }, b); assert.ok(edge.start >= b.start && edge.stop <= b.stop);
});
test("bins: frequency to bin and rows onto pixels", () => {
  const row = { bins: 4, startHz: 0, stopHz: 400, levels: Uint8Array.of(10, 200, 30, 40) };
  assert.strictEqual(L.freqToBin(row, 0), 0); assert.strictEqual(L.freqToBin(row, 399), 3); assert.strictEqual(L.freqToBin(row, 400), -1); assert.strictEqual(L.freqToBin(row, -1), -1);
  assert.strictEqual(L.binToFreq(row, 0), 50); assert.strictEqual(L.binToFreq(row, 3), 350);
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, { start: 0, stop: 400 }, 4)), [10, 200, 30, 40]);
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, { start: 0, stop: 400 }, 2)), [200, 40]);          // peak of the bins sharing a pixel
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, { start: 0, stop: 200 }, 4)), [10, 10, 200, 200]);   // zoomed: stretched
  assert.deepStrictEqual(Array.from(L.rowToPixels(row, { start: 300, stop: 500 }, 4)), [40, 40, 0, 0]);   // view partly outside the row: zero outside
});
test("palette", () => {
  for (const n of L.PALETTE_NAMES) {
    const p = L.makePalette(n); assert.strictEqual(p.length, 768);
    // brightness never collapses: the last entry is brighter than the first
    assert.ok(p[765] + p[766] + p[767] > p[0] + p[1] + p[2], n);
  }
  assert.deepStrictEqual(Array.from(L.makePalette("grey").slice(0, 3)), [0, 0, 0]);
  assert.deepStrictEqual(Array.from(L.makePalette("grey").slice(765)), [255, 255, 255]);
  assert.deepStrictEqual(Array.from(L.makePalette("nope")), Array.from(L.makePalette("amber")));
});
test("s-meter", () => {
  assert.strictEqual(L.sMeter(-66).text, "S9"); assert.strictEqual(L.sMeter(-120).text, "S0"); assert.strictEqual(L.sMeter(-200).text, "S0");
  assert.strictEqual(L.sMeter(-90).s, 5); assert.strictEqual(L.sMeter(-46).text, "S9+20"); assert.strictEqual(L.sMeter(NaN).text, "S0");
  assert.ok(L.sMeter(-30).frac <= 1 && L.sMeter(-200).frac === 0);
});
test("decoder tables render any JSON shape", () => {
  const t = L.toTable([{ icao: "4CA123", alt: 36000 }, { icao: "400ABC", spd: 450 }]);
  assert.strictEqual(t.kind, "table"); assert.deepStrictEqual(t.columns, ["icao", "alt", "spd"]); assert.deepStrictEqual(t.rows[1], ["400ABC", "", "450"]);
  const kv = L.toTable({ mode: "fm", telemetry: { snr: 12.5, rds: { ps: "NOOR" } } });
  assert.deepStrictEqual(kv.rows, [["mode", "fm"], ["telemetry.snr", "12.5"], ["telemetry.rds.ps", "NOOR"]]);
  assert.strictEqual(L.toTable("hi").kind, "text"); assert.strictEqual(L.toTable(null).kind, "text");
  assert.strictEqual(L.toTable(Array.from({ length: 1000 }, (_, i) => ({ i }))).rows.length, 200);
  const ft = L.findTables({ mode: "adsb", aircraft: [{ id: 1 }], n: 3 });
  assert.strictEqual(ft.length, 2); assert.strictEqual(ft[0].name, ""); assert.strictEqual(ft[1].name, "aircraft");
  assert.ok(L.findTables(5).length === 1);
});
test("jitter buffer: primes, plays, resamples, underruns, resyncs", () => {
  const p = new RingPlayer(48000, 0.12, 0.45), out = new Float32Array(128);
  p.pull(out); assert.ok(out.every((x) => x === 0), "silent before priming");
  const ramp = (n, v) => new Float32Array(n).fill(v);
  p.push(ramp(480, 0.5), 12000);                      // 40 ms: below the 120 ms target
  p.pull(out); assert.ok(out.every((x) => x === 0), "still waiting for the target");
  p.push(ramp(480, 0.5), 12000); p.push(ramp(480, 0.5), 12000);   // 120 ms
  p.pull(out); near(out[10], 0.5, 1e-6); assert.ok(p.primed);
  // 12 kHz to 48 kHz: 4 output samples per input sample; consumed 128/4 = 32 input samples
  near(p.queued - p.pos, 1440 - 32, 1);
  // drain: underrun is counted and the output is silent until the target is reached again
  const big = new Float32Array(48000); p.pull(big);
  assert.strictEqual(p.underruns, 1); assert.ok(!p.primed);
  // overload: a long backlog is dropped to the newest chunks
  for (let i = 0; i < 40; i++) p.push(ramp(480, 0.25), 12000);
  assert.ok(p.seconds <= 0.45 + 0.04 + 1e-9, "backlog " + p.seconds); assert.ok(p.drops > 0);
  // a rate change flushes (no mixing of rates)
  p.push(ramp(480, 0.1), 48000); assert.strictEqual(p.queued, 480);
  p.flush(); assert.strictEqual(p.queued, 0);
  // interpolation between chunks is continuous
  const q = new RingPlayer(12000, 0.0001, 1); q.push(Float32Array.of(0, 1), 12000); q.push(Float32Array.of(2, 3), 12000);
  const o = new Float32Array(4); q.pull(o); assert.deepStrictEqual(Array.from(o), [0, 1, 2, 3]);
});

test("host frequency ranges: check, snap, hidden parts, modes", () => {
  const r = L.readRanges({ restricted: true, action: "refuse", allow: [{ lo: 144e6, hi: 146e6, modes: ["nfm"] }, { lo: 7e6, hi: 7.2e6, modes: ["usb", "lsb"] }], block: [{ lo: 145.8e6, hi: 146e6 }], notes: ["be nice"] });
  assert.ok(L.rangeActive(r));
  assert.strictEqual(L.readRanges(null), null);
  assert.strictEqual(L.readRanges({ restricted: false, allow: [], block: [] }), null, "nothing to enforce");
  assert.strictEqual(L.rangeCheck(r, "nfm", 145e6, -6250, 6250), "ok");
  assert.strictEqual(L.rangeCheck(r, "nfm", 144e6, -6250, 6250), "not_allowed", "the passband sticks out below");
  assert.strictEqual(L.rangeCheck(r, "nfm", 144.007e6, -6250, 6250), "ok");
  assert.strictEqual(L.rangeCheck(r, "nfm", 145.9e6, -6250, 6250), "blocked");
  assert.strictEqual(L.rangeCheck(r, "am", 7.1e6, -5000, 5000), "mode");
  assert.strictEqual(L.rangeCheck(r, "usb", 7.1e6, 300, 2700), "ok");
  assert.strictEqual(L.rangeCheck(r, "usb", 7.2e6, 300, 2700), "not_allowed");
  assert.strictEqual(L.rangeCheck(r, "lsb", 7.2e6, -2700, -300), "ok");
  assert.strictEqual(L.rangeCheck(r, "nfm", 100e6, -6250, 6250), "not_allowed");
  assert.strictEqual(L.rangeCheck(r, "nfm", 145e6, 0, 0), "invalid");
  assert.strictEqual(L.rangeCheck(r, "nfm", NaN, -1, 1), "invalid");
  assert.strictEqual(L.rangeCheck(r, "nfm", -1, -1, 1), "invalid");
  // snapping
  near(L.rangeSnap(r, "nfm", 150e6, -6250, 6250), 145.8e6 - 6875, 1, "snap above");
  near(L.rangeSnap(r, "nfm", 140e6, -6250, 6250), 144e6 + 6875, 1, "snap below");
  const sn = L.rangeSnap(r, "nfm", 145.9e6, -6250, 6250);
  assert.strictEqual(L.rangeCheck(r, "nfm", sn, -6250, 6250), "ok", "snapped out of the block");
  assert.strictEqual(L.rangeSnap(r, "am", 145e6, -5000, 5000), null, "no range for AM");
  // merging: adjacent and overlapping ranges are one, a gap is not
  assert.strictEqual(L.mergeRanges([{ lo: 144e6, hi: 145e6 }, { lo: 145e6, hi: 146e6 }, { lo: 145.5e6, hi: 145.6e6 }]).length, 1);
  assert.strictEqual(L.mergeRanges([{ lo: 144e6, hi: 145e6 }, { lo: 145.001e6, hi: 146e6 }]).length, 2);
  const adj = L.readRanges({ restricted: true, allow: [{ lo: 144e6, hi: 145e6 }, { lo: 145e6, hi: 146e6 }], block: [] });
  assert.strictEqual(L.rangeCheck(adj, "nfm", 145e6, -6250, 6250), "ok", "across the join");
  // capture edge
  const cap = { centre_hz: 100e6, rate_hz: 2.4e6 }, blk = L.readRanges({ restricted: false, allow: [], block: [{ lo: 101.1e6, hi: 101.2e6 }] });
  assert.strictEqual(L.rangeCheck(blk, "nfm", 98.85e6, -6250, 6250, cap), "capture");
  assert.strictEqual(L.rangeCheck(blk, "nfm", 98.86e6, -6250, 6250, cap), "ok");
  assert.strictEqual(L.rangeCheck(blk, "nfm", 101.14e6, -6250, 6250, cap), "blocked");
  const s2 = L.rangeSnap(blk, "nfm", 90e6, -6250, 6250, cap);
  assert.ok(s2 > 98.848e6 && L.rangeCheck(blk, "nfm", s2, -6250, 6250, cap) === "ok");
  // modes and bands
  assert.deepStrictEqual(L.rangeModes(r), ["nfm", "usb", "lsb"].sort((a, b) => L.MODES.indexOf(a) - L.MODES.indexOf(b)));
  assert.strictEqual(L.rangeModes(blk), null, "no allow list: every mode");
  assert.ok(L.rangeOverlaps(r, 144e6, 146e6, 1000) && !L.rangeOverlaps(r, 87.5e6, 108e6, 1000));
  assert.ok(L.rangeOverlaps(null, 1, 2, 1000), "no ranges: everything");
  // hidden parts (what the spectrum darkens): outside the allowed ranges and inside a block that names no mode
  const hid = L.hiddenRanges(r, 143e6, 147e6);
  assert.deepStrictEqual(hid, [{ lo: 143e6, hi: 144e6 }, { lo: 145.8e6, hi: 147e6 }]);
  assert.deepStrictEqual(L.hiddenRanges(null, 1, 2), [], "no ranges: nothing hidden");
  const modeBlock = L.readRanges({ restricted: false, allow: [], block: [{ lo: 1e6, hi: 2e6, modes: ["usb"] }] });
  assert.deepStrictEqual(L.hiddenRanges(modeBlock, 0, 5e6), [], "a mode-limited block does not hide the spectrum");
  const d = L.describeRanges(r);
  assert.strictEqual(d.allow.length, 2); assert.strictEqual(d.block.length, 1); assert.deepStrictEqual(d.notes, ["be nice"]);
  assert.ok(/NFM/.test(d.allow[0]));
  // defensive reading: junk from a broken server never throws
  const junk = L.readRanges({ restricted: true, allow: [{ lo: "x", hi: 2 }, null, { lo: 5, hi: 1 }], block: "no" });
  assert.ok(junk && junk.restricted && junk.allow.length === 0, "junk entries are dropped; a restricted list with nothing valid allows nothing");
});

// ---------------------------------------------------------------- decoders tab (onair-decoders.js)
const D = require(path.join(__dirname, "../../web/onair-web/onair-decoders.js"));
test("decoder events: envelope check", () => {
  assert.ok(D.validEvent({ v: 1, kind: "adsb", type: "aircraft" }));
  assert.ok(!D.validEvent({ v: 2, kind: "adsb", type: "aircraft" }) && !D.validEvent(null) && !D.validEvent({ mode: "fm", telemetry: {} }) && !D.validEvent("x"));
});
test("decoder default frequencies follow the capture", () => {
  const cap = { centre_hz: 1090e6, rate_hz: 2.4e6 };
  assert.strictEqual(D.defaultFreq("adsb", 100e6, cap), 1090e6);
  assert.strictEqual(D.defaultFreq("adsb", 100e6, { centre_hz: 100e6, rate_hz: 2.4e6 }), 100e6, "out of the capture: the tuned frequency");
  assert.strictEqual(D.defaultFreq("rds", 93.9e6, cap), 93.9e6, "no preset: the tuned frequency");
  assert.strictEqual(D.defaultFreq("ais", 5, null), 5);
});
test("aircraft table and map marks", () => {
  const t = D.aircraftTable({ aircraft_count: 3, with_position: 2, msgs_per_s: 41.6, aircraft: [
    { icao: "4840D6", callsign: "KLM1023", alt_ft: 38000, speed_kt: 450, track_deg: 271, vrate_fpm: -64, lat: 52.3, lon: 4.76, squawk: "7700", msgs: 9, age_s: 3, trail: [[52.2, 4.6], [52.3, 4.76], ["x", 1]] },
    { icao: "ABCDEF", ground: true, msgs: 2, age_s: 130 },
    { icao: "111111", alt_ft: 100, lat: 999, lon: 4, age_s: 1 } ] });
  assert.strictEqual(t.rows.length, 3);
  assert.deepStrictEqual(t.rows[0].slice(0, 3), ["111111", "", "100"], "newest first");
  assert.strictEqual(t.rows[2][2], "ground");
  assert.strictEqual(t.rows[2][8], "2 min");
  assert.strictEqual(t.marks.length, 1, "a position outside the globe is not drawn");
  assert.ok(t.marks[0].alert && t.marks[0].trail.length === 2 && t.marks[0].label === "KLM1023");
  assert.ok(/3 aircraft, 2 with position, 42 msg/.test(t.summary));
  assert.doesNotThrow(() => D.aircraftTable({}));
  assert.doesNotThrow(() => D.aircraftTable({ aircraft: "nope" }));
});
test("vessel and APRS tables", () => {
  const v = D.vesselTable({ vessel_count: 1, messages_ok: 5, vessels: [{ mmsi: 244123456, name: "SEA", ship_type_text: "Cargo", sog_kt: 12.34, cog_deg: 90, heading_deg: 91, lat: 51.9, lon: 4.1, age_s: 4 }] });
  assert.strictEqual(v.rows[0][0], "244123456"); assert.strictEqual(v.rows[0][5], "12.3"); assert.strictEqual(v.marks[0].heading, 91);
  const a = D.aprsTables({ frames_ok: 4, frames_bad: 1, stations: [{ call: "PA1ABC-9", symbol: "/>", lat: 52, lon: 5, count: 3, comment: "hi" }],
    frames: [{ from: "PA1ABC-9", to: "APRS", path: "WIDE1-1*", type: "Position", summary: "x" }] });
  assert.strictEqual(a.rows[0][0], "PA1ABC-9"); assert.strictEqual(a.frames[0][0], "PA1ABC-9>APRS,WIDE1-1*"); assert.strictEqual(a.marks.length, 1);
});
test("FT8 decode history: newest first, no duplicates, capped", () => {
  const mk = (utc, msg) => ({ slot_utc: utc, audio_hz: 1500, snr_db: -10, dt_s: 0.2, rf_hz: 14075500, msg });
  let list = D.mergeDecodes([], { decodes: [mk(1, "A"), mk(1, "B")] }, 5);
  assert.deepStrictEqual(list.map((x) => x.msg), ["B", "A"]);
  list = D.mergeDecodes(list, { decodes: [mk(1, "B"), mk(16, "C")] }, 5);   // a replay for a late listener repeats B
  assert.deepStrictEqual(list.map((x) => x.msg), ["C", "B", "A"]);
  for (let i = 0; i < 20; i++) list = D.mergeDecodes(list, { decodes: [mk(100 + i, "M" + i)] }, 5);
  assert.strictEqual(list.length, 5); assert.strictEqual(list[0].msg, "M19");
  const rows = D.ftxRows(list);
  assert.strictEqual(rows[0][3], "14.0755 MHz");
  assert.strictEqual(D.fmtUtc(0), ""); assert.strictEqual(D.fmtUtc(3661), "01:01:01");
});
test("map fit and projection", () => {
  const b = D.mapFit([{ lat: 52, lon: 4 }, { lat: 53, lon: 6 }]);
  assert.ok(b.lat0 < 52 && b.lat1 > 53 && b.lon0 < 4 && b.lon1 > 6);
  const p = D.mapProject(53.1, 4, b, 400, 300), q = D.mapProject(52, 6, b, 400, 300);
  assert.ok(p.y < q.y, "north is up"); assert.ok(p.x < q.x, "east is right");
  assert.ok(p.x >= 0 && q.x <= 400 && p.y >= 0 && q.y <= 300, "inside the canvas");
  // a degree of longitude is shorter than a degree of latitude at 52 N
  const a = D.mapProject(52, 4, b, 400, 300), c = D.mapProject(52, 5, b, 400, 300), d = D.mapProject(53, 4, b, 400, 300);
  assert.ok(Math.abs(c.x - a.x) < Math.abs(d.y - a.y) * 0.7);
  assert.doesNotThrow(() => D.mapFit([]));
});
test("picture strips: placement, new picture, end, bad input", () => {
  const strip = (id, y, rows, extra) => Object.assign({ v: 1, kind: "wefax", type: "image", id, label: "IOC 576", w: 100, h: 200, ch: 1, y, rows, scale: 1, preview: false, final: false, pw: 100, ph: rows, png: "iVBORw0KGgo=" }, extra || {});
  let r = D.planStrip(null, strip(7, 0, 10));
  assert.ok(r.reset && r.draw && r.draw.dy === 0 && r.draw.dh === 10 && r.draw.dw === 100 && r.draw.url.startsWith("data:image/png;base64,"));
  assert.strictEqual(r.state.rows, 10);
  r = D.planStrip(r.state, strip(7, 10, 10));
  assert.ok(!r.reset && r.draw.dy === 10 && r.state.rows === 20);
  r = D.planStrip(r.state, strip(7, 190, 10, { final: true }));
  assert.ok(r.state.done && r.state.rows === 200);
  const old = r.state;
  r = D.planStrip(old, strip(8, 0, 5));
  assert.ok(r.reset && r.state.id === 8 && !r.state.done, "a new id starts a new picture");
  r = D.planStrip(old, { v: 1, kind: "wefax", type: "image_end", id: 7, rows: 150 });
  assert.ok(r.state.done && !r.draw);
  // a preview covers the rows it describes, scaled up to the strip's size
  r = D.planStrip(null, strip(9, 0, 120, { preview: true, scale: 3, pw: 34, ph: 40 }));
  assert.ok(r.draw && r.draw.dw === 100 && r.draw.dh === 120);
  // refused: out of range, overflow, bad base64, other picture width, wrong type
  for (const bad of [strip(7, -1, 5), strip(7, 198, 5), strip(7, 0, 0), strip(7, 0, 5, { png: "data:evil" }), strip(7, 0, 5, { png: 5 }), strip(7, 0, 5, { w: 99999 }), strip(7, 0, 5, { pw: 200 }),
                     strip(7, 0, 5, { png: "A".repeat(130000) }), strip(7, 0, 5, { w: 50, pw: 50 }), { v: 1, kind: "x", type: "table" }]) {
    const rr = D.planStrip(old, bad);
    assert.ok(!rr.draw, JSON.stringify(bad).slice(0, 60));
  }
  assert.strictEqual(D.planStrip(null, null).state, null);
});

// ---- settings, i18n, map (web/onair-web/onair-settings.js, onair-i18n.js, onair-map.js, onair-world.js) ----
const CFG = require(path.join(__dirname, "../../web/onair-web/onair-settings.js"));
const I18N = require(path.join(__dirname, "../../web/onair-web/onair-i18n.js"));
const MAP = require(path.join(__dirname, "../../web/onair-web/onair-map.js"));
const memStore = (init) => { const m = new Map(init ? [[CFG.KEY, init]] : []); return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, v) }; };

test("settings: defaults are valid and stable through sanitize", () => {
  const d = CFG.defaults();
  assert.deepStrictEqual(CFG.sanitize(d), d);
  for (const k of CFG.KEYS) assert.strictEqual(CFG.coerce(k, d[k]), d[k], k);
});
test("settings: bad values fall back, numbers clamp, unknown keys drop", () => {
  const o = CFG.sanitize({ agc: "loud", gainDb: 999, range: -4, wfFps: NaN, palette: 7, theme: "light", evil: 1, __proto__: { x: 1 }, scale: 50, fftBins: 300 });
  assert.strictEqual(o.agc, "slow"); assert.strictEqual(o.gainDb, 60); assert.strictEqual(o.range, 20); assert.strictEqual(o.wfFps, 12);
  assert.strictEqual(o.palette, "navy"); assert.strictEqual(o.theme, "light"); assert.strictEqual(o.scale, 85); assert.strictEqual(o.fftBins, 0);
  assert.ok(!("evil" in o) && !("x" in o));
  assert.deepStrictEqual(CFG.sanitize(null), CFG.defaults()); assert.deepStrictEqual(CFG.sanitize([1, 2]), CFG.defaults());
  const m = CFG.sanitize({ minDb: -50, maxDb: -55 }); assert.ok(m.maxDb - m.minDb >= 10);
});
test("settings: every palette name exists in the library", () => {
  for (const p of CFG.SCHEMA.palette.values) assert.ok(L.PALETTE_NAMES.includes(p), p);
});
test("settings: load / save survive broken or throwing storage", () => {
  const st = memStore(); const s = CFG.defaults(); s.agc = "fast"; s.step = 12500;
  assert.ok(CFG.save(st, s)); assert.deepStrictEqual(CFG.load(st), CFG.sanitize(s));
  assert.deepStrictEqual(CFG.load(memStore("{not json")), CFG.defaults());
  assert.deepStrictEqual(CFG.load(memStore('{"agc":"off","gainDb":"x"}')).agc, "off");
  const bad = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); } };
  assert.deepStrictEqual(CFG.load(bad), CFG.defaults()); assert.strictEqual(CFG.save(bad, s), false);
});
test("settings: export / import round trip, foreign files refused", () => {
  const s = CFG.defaults(); s.theme = "dark"; s.lang = "lt"; s.minDb = -90;
  assert.deepStrictEqual(CFG.importJson(CFG.exportJson(s)), CFG.sanitize(s));
  assert.strictEqual(CFG.importJson("[]"), null); assert.strictEqual(CFG.importJson("nope"), null); assert.strictEqual(CFG.importJson('{"settings":{}}'), null);
});
test("settings: spectrum request follows FFT size, speed and the server's limits", () => {
  const s = CFG.defaults();
  assert.strictEqual(CFG.binsFor(s, 1500, 2048), 2048); assert.strictEqual(CFG.binsFor(s, 300, 2048), 512); assert.strictEqual(CFG.binsFor(s, 5000, 1024), 1024);
  s.fftBins = 4096; assert.strictEqual(CFG.binsFor(s, 800, 2048), 2048); assert.strictEqual(CFG.binsFor(s, 800, 8192), 4096);
  s.wfFps = 30; assert.strictEqual(CFG.fpsFor(s, 15), 15); s.wfFps = 5; assert.strictEqual(CFG.fpsFor(s, 15), 5);
});
test("settings: rx and audio messages stay inside the protocol", () => {
  const s = CFG.defaults(); s.agc = "off"; s.gainDb = 40;
  const m = L.msg.rx(CFG.rxFields(s, "wfm")); assert.deepStrictEqual(m, { type: "rx", agc: "off", gain_db: 40, deemph_us: 50 });
  assert.ok(!("deemph_us" in CFG.rxFields(s, "nfm")));
  assert.deepStrictEqual(CFG.audioParams(s, "wfm"), { codec: "adpcm", rate: 48000 }); assert.deepStrictEqual(CFG.audioParams(s, "usb"), { codec: "adpcm", rate: 12000 });
  s.audioRate = 24000; s.audioCodec = "pcm16"; assert.deepStrictEqual(CFG.audioParams(s, "usb"), { codec: "pcm16", rate: 24000 });
  for (const r of CFG.SCHEMA.audioRate.values) if (r) assert.ok([8000, 12000, 16000, 24000, 48000].includes(r));
});
test("auto-range: floor under the noise, top over the strongest signal, smooth following", () => {
  const n = 512, lv = new Uint8Array(n).fill(60); lv[200] = 200;
  const row = { bins: n, levels: lv, floorDb: -100, topDb: -20 };
  const r = CFG.autoRange(row); near(r.min, -100 + 60 / 255 * 80 - 4, 0.5, "min"); near(r.max, -100 + 200 / 255 * 80 + 6 > r.min + 20 ? Math.max(-100 + 60 / 255 * 80 + 6, r.min + 20) : r.min + 20, 6, "max");
  assert.ok(r.max - r.min >= 20);
  const f = CFG.followRange({ min: -90, max: -30 }, { min: -100, max: -20 }, 0.05); assert.ok(f.min < -90 && f.min > -100 && f.max > -30 && f.max < -20);
  assert.deepStrictEqual(CFG.followRange(null, r), { min: r.min, max: r.max });
  assert.deepStrictEqual(CFG.levels(Object.assign(CFG.defaults(), { rangeMode: "manual", minDb: -90, maxDb: -40 }), row, null), { lo: -90, span: 50 });
  assert.deepStrictEqual(CFG.levels(CFG.defaults(), row, null), { lo: -100, span: 70 });
  const sm = CFG.smoothTrace(new Float32Array([0, 0, NaN]), new Float32Array([10, 10, 10]), 50); assert.strictEqual(sm[0], 5); assert.strictEqual(sm[2], 10);
});
test("i18n: every language key exists in English, placeholders match, fallback works", () => {
  assert.deepStrictEqual(I18N.orphanKeys(), []);
  for (const l of I18N.LANGS) for (const [k, v] of Object.entries(I18N.CAT[l])) {
    const ph = (x) => (x.match(/\{\w+\}/g) || []).sort().join(","); assert.strictEqual(ph(v), ph(I18N.CAT.en[k]), l + ":" + k);
    assert.ok(typeof v === "string" && v.length && !/[<>]/.test(v), l + ":" + k);
  }
  I18N.setLang("lt"); assert.strictEqual(I18N.t("menu.view"), "Rodinys"); assert.strictEqual(I18N.t("map.wide"), "Expand");   // untranslated: English
  assert.strictEqual(I18N.t("head.listeners", { n: 2, max: 8 }), "2 / 8 klausytojų"); assert.strictEqual(I18N.t("no.such.key"), "no.such.key");
  I18N.setLang("xx"); assert.strictEqual(I18N.lang(), "en");
  assert.strictEqual(I18N.pickLang("auto", ["lt-LT", "en"]), "lt"); assert.strictEqual(I18N.pickLang("auto", ["fr"]), "en"); assert.strictEqual(I18N.pickLang("de", ["lt"]), "de");
  for (const l of Object.keys(I18N.CAT)) assert.ok(I18N.coverage(l) > 0.2, l);
});
test("i18n: the page's data-i18n keys all exist", () => {
  const html = require("fs").readFileSync(path.join(__dirname, "../../web/onair-web/index.html"), "utf8");
  const keys = [...html.matchAll(/data-i18n(?:-title|-aria|-ph)?="([^"]+)"/g)].map((m) => m[1]);
  assert.ok(keys.length > 40); for (const k of keys) assert.ok(k in I18N.CAT.en, k);
  const src = require("fs").readFileSync(path.join(__dirname, "../../web/onair-web/app.js"), "utf8");
  for (const m of src.matchAll(/\bt\("([a-z][\w.]*[\w])"/g)) assert.ok(m[1] in I18N.CAT.en, "app.js: " + m[1]);
});
test("map: project and unproject are inverse, zoom keeps the anchor, pan moves the centre", () => {
  const v = { lat: 54.7, lon: 25.3, z: 8 }, w = 800, h = 600;
  const p = MAP.project(v, w, h, 54.7, 25.3); near(p.x, 400, 1e-6); near(p.y, 300, 1e-6);
  for (const [la, lo] of [[55.1, 24.2], [50, 30], [54.7, 25.3]]) { const q = MAP.project(v, w, h, la, lo), u = MAP.unproject(v, w, h, q.x, q.y); near(u.lat, la, 1e-6); near(u.lon, lo, 1e-6); }
  const a = MAP.unproject(v, w, h, 600, 200), v2 = MAP.zoomAt(v, w, h, 600, 200, 1.5), b = MAP.unproject(v2, w, h, 600, 200);
  near(b.lat, a.lat, 1e-6); near(b.lon, a.lon, 1e-6); near(v2.z, 9.5, 1e-9);
  const v3 = MAP.panBy(v, w, h, 100, 0); assert.ok(v3.lon < v.lon); near(v3.lat, v.lat, 1e-9);
  assert.ok(MAP.clampView({ lat: 89, lon: 0, z: 3 }, w, h).lat < 86 && MAP.clampView({ lat: 0, lon: 200, z: 3 }, w, h).lon < 0);
});
test("map: fit shows every point, one point is a close view, none the world, the date line works", () => {
  const w = 800, h = 600, pts = [{ lat: 54.5, lon: 24.9 }, { lat: 55.2, lon: 26.4 }, { lat: 54.9, lon: 25.6 }];
  const v = MAP.fit(pts, w, h, 30);
  for (const p of pts) { const q = MAP.project(v, w, h, p.lat, p.lon); assert.ok(q.x >= 0 && q.x <= w && q.y >= 0 && q.y <= h, JSON.stringify(q)); }
  assert.ok(MAP.fit([pts[0]], w, h).z >= 10); assert.ok(MAP.fit([], w, h).z < 3);
  const dl = MAP.fit([{ lat: 0, lon: 179.5 }, { lat: 1, lon: -179.5 }], w, h); assert.ok(dl.z > 5, "date line fit z=" + dl.z);
  const q = MAP.project(dl, w, h, 0, 179.5), r = MAP.project(dl, w, h, 1, -179.5); assert.ok(q.x > 0 && q.x < w && r.x > 0 && r.x < w && q.y > 0 && r.y < h);
  assert.strictEqual(MAP.hit(v, w, h, [{ id: "a", lat: 54.9, lon: 25.6 }], MAP.project(v, w, h, 54.9, 25.6).x + 3, MAP.project(v, w, h, 54.9, 25.6).y, 10).id, "a");
  assert.strictEqual(MAP.hit(v, w, h, [{ id: "a", lat: 54.9, lon: 25.6 }], 5, 5, 10), null);
  const sb = MAP.scaleBar(v, 100); assert.ok(sb.px > 40 && sb.px < 260 && /km|m/.test(sb.label));
});
test("map: the bundled world outline is well formed", () => {
  const sandbox = {}; new Function("globalThis", require("fs").readFileSync(path.join(__dirname, "../../web/onair-web/onair-world.js"), "utf8"))(sandbox);
  const w = sandbox.OnAirWorld; assert.ok(Array.isArray(w) && w.length >= 10);
  for (const poly of w) { assert.ok(poly.length >= 3); for (const [lo, la] of poly) assert.ok(lo >= -180 && lo <= 180 && la >= -90 && la <= 90); }
});
test("decoder marks carry a kind and an info line for the map", () => {
  const ac = D.aircraftTable({ aircraft: [{ icao: "4ABCD1", callsign: "TST1", lat: 54.6, lon: 25.2, alt_ft: 31000, speed_kt: 420, track_deg: 90, age_s: 2 }] });
  assert.strictEqual(ac.marks[0].kind, "adsb"); assert.ok(/31000 ft/.test(ac.marks[0].info));
  const ve = D.vesselTable({ vessels: [{ mmsi: 244123456, name: "SEA", lat: 55, lon: 20, sog_kt: 8.2, ship_type_text: "Cargo", destination: "KLAIPEDA", age_s: 5 }] });
  assert.strictEqual(ve.marks[0].kind, "ais"); assert.ok(/KLAIPEDA/.test(ve.marks[0].info));
  const ap = D.aprsTables({ stations: [{ call: "LY1AA", lat: 54.7, lon: 25.3, comment: "iGate", count: 3 }], frames: [] });
  assert.strictEqual(ap.marks[0].kind, "aprs");
});
test("the page loads its scripts in dependency order and has no external references", () => {
  const html = require("fs").readFileSync(path.join(__dirname, "../../web/onair-web/index.html"), "utf8");
  const srcs = [...html.matchAll(/<script src="([^"]+)"/g)].map((m) => m[1]);
  assert.ok(srcs.indexOf("onair-i18n.js") < srcs.indexOf("app.js") && srcs.indexOf("onair-lib.js") < srcs.indexOf("onair-map.js") && srcs.indexOf("onair-world.js") < srcs.indexOf("onair-map.js"));
  for (const f of srcs) assert.ok(require("fs").existsSync(path.join(__dirname, "../../web/onair-web", f)), f);
  assert.ok(!/(src|href)="https?:\/\/(?!github\.com\/adswill|discord\.gg\/)/.test(html), "no runtime internet use");
  for (const f of ["app.js", "style.css", "onair-map.js", "onair-settings.js"]) assert.ok(!/https?:\/\/(?!github|www\.w3\.org|<)/.test(require("fs").readFileSync(path.join(__dirname, "../../web/onair-web", f), "utf8").replace(/\/\*[\s\S]*?\*\//g, "")), f + " has a URL");
});

console.log(passed + " tests passed" + (process.exitCode ? ", some FAILED" : ""));
