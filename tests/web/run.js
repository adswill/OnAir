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
test("frequency axis ticks are absolute MHz with just enough decimals", () => {
  let t = L.axisTicks({ start: 96e6, stop: 100e6 }, 1000, 90);
  assert.strictEqual(t.step, 5e5); assert.deepStrictEqual(t.major.map((x) => x.label), ["96.0", "96.5", "97.0", "97.5", "98.0", "98.5", "99.0", "99.5", "100.0"]);
  t = L.axisTicks({ start: 97.95e6, stop: 98.05e6 }, 1000, 90);
  assert.strictEqual(t.decimals, 2); assert.strictEqual(t.major[0].label, "97.95"); assert.strictEqual(t.major[5].label, "98.00");
  t = L.axisTicks({ start: 98e6, stop: 98.002e6 }, 400, 90);
  assert.strictEqual(t.decimals, 4); assert.strictEqual(t.major[0].label, "98.0000");
  t = L.axisTicks({ start: 1088e6, stop: 1092e6 }, 800, 90);
  assert.ok(t.major.every((x) => +x.label >= 1088 && +x.label <= 1092));
  assert.ok(L.axisTicks({ start: 0, stop: 30e6 }, 1200, 90).major.length >= 5);
  assert.strictEqual(L.tickDecimals(1e6), 0); assert.strictEqual(L.tickDecimals(25e3), 3); assert.strictEqual(L.formatMHz(98e6, 3), "98.000");
});
test("readout digit groups and passband edge dragging", () => {
  assert.deepStrictEqual(L.readoutDigits(98000000), ["0", "098", "000", "000"]);
  assert.deepStrictEqual(L.readoutDigits(1090000000), ["1", "090", "000", "000"]);
  assert.deepStrictEqual(L.readoutDigits(-5), ["0", "000", "000", "000"]);
  assert.deepStrictEqual(L.dragEdge("nfm", -6250, 6250, "low", -4000), { low_hz: -4000, high_hz: 4000 });
  assert.deepStrictEqual(L.dragEdge("nfm", -6250, 6250, "high", 999999), { low_hz: -10000, high_hz: 10000 });
  assert.deepStrictEqual(L.dragEdge("usb", 100, 2800, "high", 3000), { low_hz: 100, high_hz: 3000 });
  assert.deepStrictEqual(L.dragEdge("usb", 100, 2800, "high", 150), { low_hz: 100, high_hz: 600 });
  assert.deepStrictEqual(L.dragEdge("lsb", -2800, -100, "low", -3200), { low_hz: -3200, high_hz: -100 });
});
test("passband level is the power inside the passband", () => {
  const levels = new Uint8Array(100).fill(0); levels[50] = 255;   // one strong bin at 98.0 MHz
  const row = { bins: 100, startHz: 97e6, stopHz: 99e6, floorDb: -100, topDb: -20, levels };
  const inside = L.passbandLevel(row, 98e6, -50000, 50000), outside = L.passbandLevel(row, 97.3e6, -50000, 50000);
  assert.ok(inside > outside + 20, inside + " vs " + outside);
  assert.ok(Math.abs(L.passbandLevel(row, 98e6, -100, 100) - -20) < 3.5);   // narrower than a bin: the bin under the tuned frequency
  assert.strictEqual(L.passbandLevel(row, 120e6, -100, 100), null);
});
test("palettes: turbo is first, dark blue to red, every name builds 256 colours", () => {
  assert.strictEqual(L.PALETTE_NAMES[0], "turbo");
  for (const n of L.PALETTE_NAMES) assert.strictEqual(L.makePalette(n).length, 768, n);
  const p = L.makePalette("turbo"); assert.ok(p[765] > 180 && p[766] < 80 && p[2] < 100);
});
test("s-meter scale ticks sit where the bar fills", () => {
  const t = L.sMeterTicks(), s9 = t.find((x) => x.label === "9");
  assert.ok(Math.abs(s9.frac - L.sMeter(-66).frac) < 1e-9);
  assert.ok(Math.abs(t.find((x) => x.label === "+20").frac - L.sMeter(-46).frac) < 1e-9);
  assert.ok(t.every((x) => x.frac >= 0 && x.frac <= 1));
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

console.log(passed + " tests passed" + (process.exitCode ? ", some FAILED" : ""));
