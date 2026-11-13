// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

/* OnAir Web: audio player. A jitter buffer with a linear resampler, used inside an AudioWorklet (and by the node tests).
 * Frames of Float32 mono at the stream's rate are pushed; process() pulls them at the context's rate.
 * Target latency about 120 ms; if more than 450 ms piles up the oldest audio is dropped; after an underrun it waits for the target again. */
(function (root, factory) {
  const lib = factory(root);
  if (typeof module === "object" && module.exports) module.exports = lib;
  else root.OnAirPlayer = lib;
})(typeof globalThis !== "undefined" ? globalThis : this, function (root) {
  "use strict";

  class RingPlayer {
    constructor(outRate, targetSec, maxSec) {
      this.outRate = outRate;
      this.targetSec = targetSec || 0.12;
      this.maxSec = maxSec || 0.45;
      this.inRate = 12000;
      this.queue = [];      // Float32Array chunks
      this.queued = 0;      // samples in the queue (input rate)
      this.pos = 0;         // fractional read position inside queue[0]
      this.primed = false;
      this.last = 0;
      this.underruns = 0; this.drops = 0;
      this.gain = 1;
    }
    get seconds() { return this.queued / this.inRate; }
    push(samples, rate) {
      if (rate !== this.inRate) { this.flush(); this.inRate = rate; }
      this.queue.push(samples); this.queued += samples.length;
      // too much waiting: drop the oldest whole chunks down to the target
      while (this.seconds > this.maxSec && this.queue.length > 1) {
        const c = this.queue.shift(); this.queued -= c.length; this.pos = 0; this.drops++;
        if (this.seconds <= this.targetSec) break;
      }
    }
    flush() { this.queue = []; this.queued = 0; this.pos = 0; this.primed = false; }
    /** Fill `out` (Float32Array) at the output rate. */
    pull(out) {
      const n = out.length;
      if (!this.primed) {
        if (this.seconds >= this.targetSec) this.primed = true;
        else { out.fill(0); return; }
      }
      const step = this.inRate / this.outRate;
      for (let i = 0; i < n; i++) {
        const q = this.queue[0];
        if (!q) { this.primed = false; this.underruns++; for (let j = i; j < n; j++) out[j] = 0; return; }
        const i0 = Math.floor(this.pos), frac = this.pos - i0;
        let next = q[i0 + 1];
        if (next === undefined) next = this.queue[1] ? this.queue[1][0] : q[i0];
        out[i] = (q[i0] + (next - q[i0]) * frac) * this.gain;
        this.pos += step;
        while (this.queue[0] && this.pos >= this.queue[0].length) {
          this.pos -= this.queue[0].length; this.queued -= this.queue[0].length; this.queue.shift();
        }
        if (!this.queue.length) this.pos = 0;
      }
    }
  }

  if (typeof registerProcessor === "function" && typeof AudioWorkletProcessor === "function") {
    class OnAirProcessor extends AudioWorkletProcessor {
      constructor() {
        super();
        this.player = new RingPlayer(sampleRate);
        this.port.onmessage = (e) => {
          const d = e.data;
          if (d.pcm) this.player.push(d.pcm, d.rate);
          else if (d.flush) this.player.flush();
          else if (d.gain !== undefined) this.player.gain = d.gain;
        };
        this.n = 0;
      }
      process(inputs, outputs) {
        const out = outputs[0][0];
        this.player.pull(out);
        for (let c = 1; c < outputs[0].length; c++) outputs[0][c].set(out);
        if ((++this.n & 63) === 0) this.port.postMessage({ buffered: this.player.seconds, underruns: this.player.underruns, drops: this.player.drops });
        return true;
      }
    }
    registerProcessor("onair-player", OnAirProcessor);
  }

  return { RingPlayer };
});
