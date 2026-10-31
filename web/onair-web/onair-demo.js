/* OnAir Web demo mode (?demo=1): a simulated receiver inside the page, so the UI can be previewed without a radio.
 * It replaces WebSocket and the info request with local fakes; nothing is sent over the network and nothing here runs unless the address has ?demo=1. */
(function () {
  "use strict";
  const CENTRE = 100e6, RATE = 2.4e6, FLOOR = -124, TOP = -22;
  // fake FM stations: frequency, strength dB, deviation shape width
  const STATIONS = [[99.0e6, -48, 1], [99.4e6, -71, 1], [99.8e6, -56, 1], [100.4e6, -39, 1], [100.8e6, -78, 1], [101.2e6, -63, 1], [98.6e6, -69, 1], [101.7e6, -83, 1]];
  const NARROW = [[99.52e6, -74], [100.12e6, -81], [100.62e6, -77]];
  const CALLS = ["UAE441", "BAW107", "DLH9XK", "SWR82A", "RYR41NF", "FDB1180", "ETH3AB", "QTR8WC", "THY18R", "UAL918"];

  window.fetch = ((orig) => function (url, opts) {
    if (String(url).indexOf("/web/v1/info") >= 0) {
      return Promise.resolve(new Response(JSON.stringify({ name: "Demo receiver", location: "Vilnius, Lithuania", antenna: "Discone on the roof", users: 3, max_users: 8, auth: "open", capture: { centre_hz: CENTRE, rate_hz: RATE } }), { status: 200, headers: { "content-type": "application/json" } }));
    }
    return orig.apply(this, arguments);
  })(window.fetch);

  let seed = 7; const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  const gauss = () => (rnd() + rnd() + rnd() + rnd() - 2) * 1.2;

  class FakeSocket {
    constructor() {
      this.readyState = 0; this.binaryType = "arraybuffer";
      this.st = { freq: CENTRE, mode: "nfm", low: -6250, high: 6250, sq: -150, specOn: false, bins: 1024, fps: 10, start: CENTRE - RATE / 2, stop: CENTRE + RATE / 2, audio: false, rate: 12000, subs: [] };
      this.seqS = 0; this.seqA = 0; this.t = 0; this.phase = 0; this.timers = [];
      setTimeout(() => { this.readyState = 1; this.onopen && this.onopen({}); }, 350);
    }
    close() { this.readyState = 3; this.timers.forEach(clearInterval); }
    reply(o) { setTimeout(() => this.readyState === 1 && this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 0); }
    bin(buf) { this.readyState === 1 && this.onmessage && this.onmessage({ data: buf }); }
    send(txt) {
      let m; try { m = JSON.parse(txt); } catch (e) { return; }
      const st = this.st;
      if (m.type === "hello") {
        this.reply({ type: "welcome", proto: 1, session: 1, class: "public", server: { name: "Demo receiver", location: "Vilnius, Lithuania", antenna: "Discone on the roof" }, capture: { centre_hz: CENTRE, rate_hz: RATE },
          limits: { max_bins: 2048, max_fps: 15, max_kbps: 512, idle_min: 30, time_limit_min: 0, msg_per_sec: 20, max_message: 4096 }, audio: { codecs: ["adpcm", "pcm16"], rates: [8000, 12000, 16000, 24000, 48000] },
          demods: ["am", "nfm", "wfm", "usb", "lsb", "cw"], decoders: [{ kind: "adsb", name: "ADS-B aircraft", needs_freq: true }, { kind: "aprs", name: "APRS / packet", needs_freq: true }, { kind: "rds", name: "RDS", needs_freq: true }] });
        this.timers.push(setInterval(() => this.specTick(), 1000 / 12), setInterval(() => this.audioTick(), 50), setInterval(() => this.decTick(), 1000));
      } else if (m.type === "tune") {
        if (typeof m.freq_hz === "number") st.freq = m.freq_hz; if (m.mode) st.mode = m.mode;
        if (typeof m.low_hz === "number") { st.low = m.low_hz; st.high = m.high_hz; }
        this.pushState();
      } else if (m.type === "rx") { if (typeof m.squelch_db === "number") st.sq = m.squelch_db; this.pushState(); }
      else if (m.type === "spectrum") { const first = !st.specOn && !!m.on; st.specOn = !!m.on; if (m.bins) st.bins = m.bins; if (m.fps) st.fps = m.fps; st.start = m.start_hz != null ? m.start_hz : CENTRE - RATE / 2; st.stop = m.stop_hz != null ? m.stop_hz : CENTRE + RATE / 2; if (first) { for (let i = 0; i < 140; i++) this.specTick(); } }
      else if (m.type === "audio") { st.audio = !!m.on; if (m.rate) st.rate = m.rate; this.pushState(); }
      else if (m.type === "subscribe") { st.subs.push(m.decoder + "@" + Math.round((m.freq_hz || 0) / 1000) + "k"); this.pushState(); this.decTick(true); }
      else if (m.type === "unsubscribe") { st.subs = st.subs.filter((k) => k.indexOf(m.decoder + "@") !== 0); this.pushState(); }
      else if (m.type === "ping") this.reply({ type: "pong", t: m.t });
    }
    pushState() { const s = this.st; this.reply({ type: "state", freq_hz: s.freq, mode: s.mode, low_hz: s.low, high_hz: s.high, squelch_db: s.sq, subscriptions: s.subs, degrade: 0 }); }
    // power in dB at a frequency, from the fake stations
    level(hz) {
      let p = Math.pow(10, -110 / 10);
      for (const [f, db, w] of STATIONS) { const d = Math.abs(hz - f); if (d < 105e3) p += Math.pow(10, (db + (d < 70e3 ? 0 : -(d - 70e3) / 3e3)) / 10) * (0.85 + 0.15 * Math.sin(this.t * 2.1 + f)); }
      for (const [f, db] of NARROW) { const d = Math.abs(hz - f); if (d < 7e3) p += Math.pow(10, (db - d / 700) / 10) * (Math.sin(this.t * 7 + f) > -0.2 ? 1 : 0.05); }
      p += Math.pow(10, -104 / 10) * (hz % 1e5 < 1.2e3 ? 1 : 0);   // a faint spur near each 100 kHz
      return 10 * Math.log10(p) + gauss() * 1.6;
    }
    specTick() {
      const s = this.st; if (!s.specOn || this.readyState !== 1) return;
      this.t += 1 / 12;
      const n = s.bins, buf = new ArrayBuffer(32 + n), v = new DataView(buf), u = new Uint8Array(buf, 32, n);
      v.setUint8(0, 0x10); v.setUint8(1, 1); v.setUint16(2, n, true); v.setUint32(4, this.seqS++, true);
      v.setFloat64(8, s.start, true); v.setFloat64(16, s.stop, true); v.setFloat32(24, FLOOR, true); v.setFloat32(28, TOP, true);
      for (let i = 0; i < n; i++) { const hz = s.start + ((i + 0.5) / n) * (s.stop - s.start); u[i] = Math.max(0, Math.min(255, Math.round(((this.level(hz) - FLOOR) / (TOP - FLOOR)) * 255))); }
      this.bin(buf);
    }
    audioTick() {
      const s = this.st; if (!s.audio || this.readyState !== 1) return;
      const rate = s.rate, n = Math.round(rate * 0.05), buf = new ArrayBuffer(24 + n * 2), v = new DataView(buf);
      // signal level at the tuned frequency decides loudness: a station nearby sounds like a tone mix with a slow vibrato
      let lv = -200; for (let d = s.low; d <= s.high; d += Math.max(500, (s.high - s.low) / 8)) lv = Math.max(lv, this.level(s.freq + d));
      const squelched = lv < s.sq;
      v.setUint8(0, 0x20); v.setUint8(1, 0); v.setUint16(2, n, true); v.setUint32(4, this.seqA++, true); v.setUint32(8, rate, true); v.setFloat32(12, lv, true); v.setUint8(16, squelched ? 1 : 0);
      if (!squelched) {
        const amp = Math.max(0, Math.min(1, (lv + 90) / 55)) * 0.4;
        for (let i = 0; i < n; i++) {
          this.phase += 1 / rate; const p = this.phase;
          const x = amp * (0.55 * Math.sin(2 * Math.PI * (220 + 40 * Math.sin(p * 0.7)) * p) + 0.3 * Math.sin(2 * Math.PI * 330 * p) * Math.sin(p * 3) + 0.15 * (rnd() - 0.5));
          v.setInt16(24 + i * 2, Math.round(x * 32767), true);
        }
      }
      this.bin(buf);
    }
    decTick(first) {
      const s = this.st; if (this.readyState !== 1) return;
      for (const key of s.subs) {
        const kind = key.split("@")[0];
        if (kind === "adsb") {
          const list = CALLS.map((c, i) => { const a = 0.4 + i * 0.63 + this.t * 0.02 * (i % 3 + 1);
            return { icao: (0x4840d0 + i * 4097).toString(16).toUpperCase(), callsign: c, alt_ft: 9000 + i * 3100, speed_kt: 280 + i * 17, track_deg: Math.round((i * 37 + this.t) % 360), vrate_fpm: (i % 3 - 1) * 640,
              lat: +(54.68 + Math.sin(a) * (0.4 + i * 0.07)).toFixed(4), lon: +(25.28 + Math.cos(a) * (0.7 + i * 0.1)).toFixed(4), squawk: String(1000 + i * 111), msgs: 40 + i * 9, age_s: (i * 3) % 11, level_dbfs: -30 - i * 3 }; });
          this.reply({ type: "event", decoder: key, data: { v: 1, kind: "adsb", type: "aircraft", state: "running", msgs_per_s: 38, snr_db: 21, noise_dbfs: -62, messages_ok: 4180, messages_bad: 112, aircraft_count: list.length, with_position: list.length, aircraft: list } });
        } else if (kind === "aprs") {
          const t = Math.floor(Date.now() / 1000), fr = [];
          for (let i = 0; i < 8; i++) fr.push({ t: t - (8 - i) * 14, from: ["LY1ABC-9", "LY2XY", "OH2RDP", "SP3WR", "LY5G-7", "DL1XYZ", "LY1BWB", "LY3B"][i], to: "APRS", path: "WIDE1-1", type: "position", summary: "Lat 54." + (600 + i * 31) + "N Lon 25." + (200 + i * 17) + "E, 12 km/h", baud: 1200 });
          this.reply({ type: "event", decoder: key, data: { v: 1, kind: "aprs", type: "packets", state: "running", snr_db: 14, frames_ok: 88, frames_bad: 3, frames_1200: 88, frames_9600: 0, frames: fr, stations: [] } });
        } else if (kind === "rds") {
          this.reply({ type: "event", decoder: key, data: { v: 1, kind: "rds", type: "rds", state: "running", pi: "A1B2", ps: "DEMO FM ", radiotext: "OnAir demo receiver: simulated signals only" } });
        }
      }
      void first;
    }
  }
  FakeSocket.CONNECTING = 0; FakeSocket.OPEN = 1; FakeSocket.CLOSING = 2; FakeSocket.CLOSED = 3;
  window.WebSocket = FakeSocket;
  window.__onairDemo = true;
  // preview helpers for screenshots: ?tab=dec&sub=adsb&sheet=1&freq=1090&mode=am&listen=1
  const q = new URLSearchParams(location.search), $ = (id) => document.getElementById(id);
  const wait = setInterval(() => {
    if ($("app") && !$("app").hidden) {
      clearInterval(wait);
      setTimeout(() => {
        if (q.get("tab")) { const t = $("tab" + q.get("tab")[0].toUpperCase() + q.get("tab").slice(1)); if (t) t.click(); }
        if (q.get("sub")) { const b = document.querySelector('#decList button[data-kind="' + q.get("sub") + '"]'); if (b) b.click(); }
        if (q.get("sheet")) $("panelsBtn").click();
      }, 300);
    }
  }, 100);
})();
