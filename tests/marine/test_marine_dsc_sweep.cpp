// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DSC robustness sweep through the radio layer: frequency offset, mistuning, off-centre channel, noise, fading, odd sample rates, clipping,
// spectrum inversion (the other sideband), level steps, clock error, chunking. Both variants (MF/HF 100 bd, VHF ch 70). Prints a table of
// every case that does not decode and fails when any case on the must-pass list fails.
#include "../data/marine/rf_util.h"
#include "../common/test_long.h"
#include <functional>
#include <random>
using namespace mt;

struct Imp {
    double rate = 250000, secs = 18, snr = 25, cfo = 0, mist = 0, sro = 0, offErr = 0;   // offErr: setSignalOffset is wrong by this much
    int fade = 0; size_t chunk = 65536;
    float clip = 0;            // hard limit at this level (0 = off)
    bool invert = false;       // conjugate: spectrum mirrored (other sideband / reversed mark and space)
    bool quant8 = false; float dc = 0, gain = 0.2f; // gain scale of the whole signal after the generator
    double stepAt = -1, stepDb = 0;                 // level step
    double burstAt = -1, burstDb = 0;               // a strong impulse noise burst of 100 ms
};

static int decoded(bool vhf, const Imp& m, int* total = nullptr, int* wrong = nullptr) {
    SynthConfig cfg; cfg.mode = 17; cfg.snrDb = m.snr; cfg.cfoHz = m.cfo; cfg.sroPpm = m.sro;
    cfg.modeOpt[0] = vhf ? 4 : 2; cfg.modeOpt[1] = m.fade; cfg.modeVal[0] = m.mist;
    auto syn = makeMarineSynth(cfg, m.rate);
    MarineReceiver rx; rx.configure(m.rate);
    rx.setSignalOffset((m.invert ? 1 : -1) * marineTuning().tuneOffsetHz + m.offErr);
    rx.setService(vhf ? 2 : 2); rx.setFrequencyHz(vhf ? 156.525e6 : 8414.5e3);
    const size_t N = (size_t)(m.secs * m.rate), G = 65536; std::vector<cf32> b(G); size_t done = 0;
    MarineTelemetry t, last; uint64_t seq = 0; std::mt19937 rng(7); std::normal_distribution<float> nd(0, 1);
    while (done < N) {
        const size_t n = std::min(G, N - done); syn->generate(b.data(), n);
        for (size_t i = 0; i < n; i++) {
            float a = b[i].real(), q = b[i].imag(); const double tt = (double)(done + i) / m.rate;
            if (m.invert) q = -q;
            float g = m.gain / 0.2f; if (m.stepAt >= 0 && tt >= m.stepAt) g *= (float)std::pow(10.0, m.stepDb / 20);
            a *= g; q *= g;
            if (m.burstAt >= 0 && tt >= m.burstAt && tt < m.burstAt + 0.1) { const float k = 0.2f * (float)std::pow(10.0, m.burstDb / 20); a += k * nd(rng); q += k * nd(rng); }
            if (m.clip > 0) { a = std::max(-m.clip, std::min(m.clip, a)); q = std::max(-m.clip, std::min(m.clip, q)); }
            a += m.dc; q += m.dc;
            if (m.quant8) { a = std::lrintf(std::max(-1.f, std::min(1.f, a)) * 127.f) / 127.f; q = std::lrintf(std::max(-1.f, std::min(1.f, q)) * 127.f) / 127.f; }
            b[i] = cf32(a, q);
        }
        for (size_t k = 0; k < n; k += m.chunk) rx.feed(b.data() + k, std::min(m.chunk, n - k));
        done += n; while (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    int ok = 0, bad = 0;
    for (const auto& c : last.dsc) { if (!c.eccOk) continue; ok++; if (!((c.fromMmsi == "232123456" || c.fromMmsi == "235012345" || c.fromMmsi == "002320064") && (c.format == 112 || c.format == 116 || c.format == 120))) bad++; }
    if (total) *total = (int)last.dsc.size(); if (wrong) *wrong = bad;
    return ok;
}

static int fails = 0, soft = 0;
static void row(bool vhf, const char* what, Imp m, bool must, int want = 1) {
    int tot = 0, bad = 0; const int ok = decoded(vhf, m, &tot, &bad);
    const bool pass = ok >= want && bad == 0;
    printf("%s %-5s %-44s ok=%d all=%d wrong=%d\n", pass ? "pass" : (must ? "FAIL" : "weak"), vhf ? "VHF" : "MF/HF", what, ok, tot, bad);
    if (!pass) { if (must) fails++; else soft++; }
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // the default run: the ends of each sweep (offsets, noise, rates up to 4 Msps, the largest clock error, the two lowest clip levels) and every single
    // case, chunks of 1 for 2 s; ONAIR_TEST_LONG=1 every point, 12.5 and 20 Msps too
    const bool L = testLong();
    using V = std::vector<double>;
    for (int v = 0; v < 2; v++) {
        const bool vhf = v == 1; char n[96]; const double span = vhf ? 12 : 100;
        Imp base; base.secs = vhf ? 12 : 24;
        row(vhf, "baseline", base, true);
        for (double f : vhf ? (L ? V{-4000, -3000, -1500, 1500, 3000, 4000} : V{-4000, -3000, 3000, 4000}) : (L ? V{-400, -250, -100, 100, 250, 400} : V{-400, -250, 250, 400})) { Imp m = base; m.cfo = f; snprintf(n, sizeof n, "cfo %+.0f Hz", f); row(vhf, n, m, std::fabs(f) <= (vhf ? 3000 : 250)); }
        for (double f : vhf ? std::vector<double>{-2500, 2500} : std::vector<double>{-200, 200}) { Imp m = base; m.mist = f; snprintf(n, sizeof n, "mistuned %+.0f Hz", f); row(vhf, n, m, true); }
        for (double f : vhf ? std::vector<double>{-3000, 3000} : std::vector<double>{-300, 300}) { Imp m = base; m.offErr = f; snprintf(n, sizeof n, "off-centre %+.0f Hz", f); row(vhf, n, m, std::fabs(f) <= (vhf ? 3000 : 300)); }
        for (double s : vhf ? (L ? V{16, 12, 10} : V{16, 10}) : (L ? V{10, 6, 3} : V{10, 3})) { Imp m = base; m.snr = s; snprintf(n, sizeof n, "snr %.0f dB", s); row(vhf, n, m, s >= (vhf ? 14 : 3)); }
        { Imp m = base; m.fade = 1; m.secs = base.secs * 2; row(vhf, "two-path fading", m, true); }
        { Imp m = base; m.fade = 1; m.secs = base.secs * 2; m.snr = vhf ? 18 : 12; row(vhf, "two-path fading, lower snr", m, false); }
        for (double r : L ? V{250000.0, 300000.0, 1024000.0, 1920000.0, 2048000.0, 2400000.0, 3200000.0, 4000000.0, 5000000.0, 12500000.0, 20000000.0} : V{250000.0, 300000.0, 1024000.0, 4000000.0}) { Imp m = base; m.rate = r; m.secs = base.secs * (r > 3e6 ? 0.6 : 1); snprintf(n, sizeof n, "rate %.0f", r); row(vhf, n, m, r >= 240000); }
        for (double f : L ? V{20.0, 50.0} : V{50.0}) for (int sg : {1, -1}) { Imp m = base; m.sro = f * sg; snprintf(n, sizeof n, "clock %+.0f ppm", f * sg); row(vhf, n, m, true); }
        for (float c : L ? std::vector<float>{0.15f, 0.08f, 0.04f, 0.02f} : std::vector<float>{0.04f, 0.02f}) { Imp m = base; m.clip = c; snprintf(n, sizeof n, "clipped at %.2f", c); row(vhf, n, m, c >= 0.04f); }
        { Imp m = base; m.invert = true; row(vhf, "spectrum inverted", m, true); }
        { Imp m = base; m.invert = true; m.cfo = vhf ? 1500 : 100; row(vhf, "inverted + offset", m, true); }
        { Imp m = base; m.quant8 = true; m.gain = 0.02f; row(vhf, "8 bit, weak (0.02 rms)", m, true); }
        { Imp m = base; m.quant8 = true; m.dc = 0.1f; row(vhf, "8 bit, DC 0.1", m, true); }
        for (float g : {0.002f, 0.6f}) { Imp m = base; m.gain = g; snprintf(n, sizeof n, "level %.3f rms", g); row(vhf, n, m, true); }
        { Imp m = base; m.stepAt = base.secs * 0.4; m.stepDb = 25; m.secs = base.secs * 1.5; row(vhf, "level step +25 dB", m, true); }
        { Imp m = base; m.stepAt = base.secs * 0.4; m.stepDb = -25; m.secs = base.secs * 1.5; row(vhf, "level step -25 dB", m, true); }
        { Imp m = base; m.burstAt = 1.0; m.burstDb = 20; m.secs = base.secs * 1.5; row(vhf, "100 ms noise burst +20 dB", m, true); }
        { Imp m = base; m.chunk = 1; m.secs = L ? (vhf ? 4 : 6) : 2; row(vhf, "chunks of 1 (short)", m, false, 0); }
        { Imp m = base; m.cfo = vhf ? 2000 : 150; m.rate = 2.4e6; m.sro = 30; m.fade = 1; m.snr = vhf ? 20 : 15; m.secs = base.secs * 2; row(vhf, "everything at once", m, false); }
        (void)span;
    }
    printf("%d must-pass failures, %d weak\n", fails, soft);
    return fails ? 1 : 0;
}
