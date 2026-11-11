/* OnAir Web 3.9: audio playback through an AudioWorklet (worklet.js) with a small jitter buffer; a ScriptProcessor fallback where
 * worklets are missing. Also records what is heard to a WAV file in the browser (nothing is written on the server). */
(function (root) {
  "use strict";
  class AudioOut {
    constructor() { this.ctx = null; this.node = null; this.gain = 1; this.rate = 16000; this.rec = null; this.fallback = null; }
    async start() {
      if (this.ctx) { if (this.ctx.state === "suspended") await this.ctx.resume(); return; }
      const AC = root.AudioContext || root.webkitAudioContext;
      this.ctx = new AC({ latencyHint: "interactive" });
      try {
        await this.ctx.audioWorklet.addModule("worklet.js");
        this.node = new AudioWorkletNode(this.ctx, "onair-player", { outputChannelCount: [1] });
        this.node.connect(this.ctx.destination);
        this.node.port.postMessage({ gain: this.gain });
      } catch (e) {
        // no worklet (old browser, or http without a secure context): a ScriptProcessor with the same ring
        const Ring = root.OnAirWorklet.Ring;
        this.fallback = new Ring(this.ctx.sampleRate, 0.1, 0.4);
        this.fallback.gain = this.gain;
        const sp = this.ctx.createScriptProcessor(2048, 0, 1);
        sp.onaudioprocess = (ev) => this.fallback.pull(ev.outputBuffer.getChannelData(0));
        sp.connect(this.ctx.destination);
        this.node = sp;
      }
      if (this.ctx.state === "suspended") await this.ctx.resume();
    }
    push(samples, rate) {
      if (!this.ctx) return;
      this.rate = rate;
      if (this.fallback) this.fallback.push(samples, rate);
      else this.node.port.postMessage({ samples, rate }, [samples.buffer]);
      if (this.rec) this.rec.push(samples);
    }
    setGain(g) { this.gain = g; if (this.fallback) this.fallback.gain = g; else if (this.node && this.node.port) this.node.port.postMessage({ gain: g }); }
    flush() { if (this.fallback) this.fallback.flush(); else if (this.node && this.node.port) this.node.port.postMessage({ flush: true }); }
    async stop() { if (this.ctx) { try { await this.ctx.suspend(); } catch (e) {} } this.flush(); }
    get running() { return !!this.ctx && this.ctx.state === "running"; }
    // ---- a WAV recording in the browser
    recordStart() { this.rec = { chunks: [], n: 0, rate: this.rate, push(s) { this.chunks.push(Float32Array.from(s)); this.n += s.length; } }; }
    recordStop() {
      const r = this.rec; this.rec = null;
      if (!r || !r.n) return null;
      const buf = new ArrayBuffer(44 + 2 * r.n), d = new DataView(buf);
      const str = (o, s) => { for (let i = 0; i < s.length; i++) d.setUint8(o + i, s.charCodeAt(i)); };
      str(0, "RIFF"); d.setUint32(4, 36 + 2 * r.n, true); str(8, "WAVE"); str(12, "fmt "); d.setUint32(16, 16, true); d.setUint16(20, 1, true); d.setUint16(22, 1, true);
      d.setUint32(24, r.rate, true); d.setUint32(28, r.rate * 2, true); d.setUint16(32, 2, true); d.setUint16(34, 16, true); str(36, "data"); d.setUint32(40, 2 * r.n, true);
      let o = 44;
      for (const c of r.chunks) for (let i = 0; i < c.length; i++, o += 2) d.setInt16(o, Math.max(-32768, Math.min(32767, Math.round(c[i] * 32767))), true);
      return new Blob([buf], { type: "audio/wav" });
    }
  }
  root.AudioOut = AudioOut;
})(typeof globalThis !== "undefined" ? globalThis : this);
