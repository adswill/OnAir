/* OnAir Web 3.9 audio: a jitter buffer with a linear resampler. Runs inside an AudioWorklet (registered as "onair-player") and in node
 * for the tests. Mono Float32 frames at the stream's rate are pushed; process() pulls them at the context's rate.
 * Target latency 100 ms; over 400 ms queued the oldest audio is dropped; after an underrun the buffer refills to the target first. */
(function (root, factory) {
  const lib = factory();
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirWorklet = lib;
  if (typeof registerProcessor === "function" && typeof AudioWorkletProcessor === "function") {
    class OnAirPlayer extends AudioWorkletProcessor {
      constructor() {
        super();
        this.ring = new lib.Ring(sampleRate, 0.1, 0.4);
        this.port.onmessage = (e) => {
          const m = e.data;
          if (m.samples) this.ring.push(m.samples, m.rate);
          else if (m.gain !== undefined) this.ring.gain = m.gain;
          else if (m.flush) this.ring.flush();
          else if (m.stats) this.port.postMessage({ queued: this.ring.seconds, underruns: this.ring.underruns, drops: this.ring.drops });
        };
      }
      process(inputs, outputs) {
        const out = outputs[0][0];
        this.ring.pull(out);
        if (outputs[0].length > 1) outputs[0][1].set(out);
        return true;
      }
    }
    registerProcessor("onair-player", OnAirPlayer);
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  class Ring {
    constructor(outRate, targetSec, maxSec) {
      this.outRate = outRate; this.targetSec = targetSec || 0.1; this.maxSec = maxSec || 0.4;
      this.inRate = 16000; this.queue = []; this.queued = 0; this.pos = 0; this.primed = false; this.last = 0;
      this.underruns = 0; this.drops = 0; this.gain = 1;
    }
    get seconds() { return this.queued / this.inRate; }
    push(samples, rate) {
      if (rate && rate !== this.inRate) { this.flush(); this.inRate = rate; }
      if (!samples || !samples.length) return;
      this.queue.push(samples); this.queued += samples.length;
      while (this.seconds > this.maxSec && this.queue.length > 1) { const q = this.queue.shift(); this.queued -= q.length; this.pos = 0; this.drops++; }
    }
    flush() { this.queue = []; this.queued = 0; this.pos = 0; this.primed = false; }
    // fills out (Float32Array at outRate) with linear interpolation; silence while filling up
    pull(out) {
      const step = this.inRate / this.outRate;
      if (!this.primed) {
        if (this.seconds < this.targetSec) { out.fill(0); return; }
        this.primed = true;
      }
      for (let i = 0; i < out.length; i++) {
        if (!this.queue.length) { this.primed = false; this.underruns++; for (; i < out.length; i++) out[i] = 0; return; }
        const q = this.queue[0];
        const i0 = Math.floor(this.pos), frac = this.pos - i0;
        const a = q[i0], b = i0 + 1 < q.length ? q[i0 + 1] : (this.queue.length > 1 ? this.queue[1][0] : a);
        out[i] = (a + (b - a) * frac) * this.gain;
        this.last = out[i];
        this.pos += step;
        while (this.queue.length && this.pos >= this.queue[0].length) { this.pos -= this.queue[0].length; this.queued -= this.queue[0].length; this.queue.shift(); }
      }
    }
  }
  return { Ring };
});
