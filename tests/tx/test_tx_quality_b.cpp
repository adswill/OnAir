// Transmit quality of the second half of the TX registry (mode ids "iridium" .. "zwave" in alphabetical order; tests/tx/test_tx_quality_a.cpp has the
// first half). Each mode's generator is run at its nominal rate in memory (NOTHING is transmitted, no radio is opened) and the IQ is measured:
//   speed      (thread CPU time) real-time factor of the median block and the slowest 32768-sample block (the session's block) against its own duration
//   level      peak, rms, peak-to-average ratio, share of samples the output limiter would touch (> 0.9) and that would clip (> 1.0), NaN / Inf
//   spectrum   Welch average (Blackman-Harris 4-term, 4096 bins, 50 % overlap): the 99 % occupied band [lo, hi] of width B and centre fc, then
//              aclr    power in fc +- (0.75 B .. 1.75 B) over the power in the band, the worse side (dBc)
//              far     power beyond fc +- 3 B over the total (dBc)
//              spur    the strongest single bin beyond fc +- 1.5 B over the strongest bin in the band (dBc)
//   dc         |mean| over rms (dB): a carrier leak at the IF centre that the mode does not ask for
//   step       the largest change of the envelope between two samples over the peak (1.0 = a hard key-up / key-down)
//   gap        the longest run of exact zeros (ms): a stream that stops inside a block
// Usage: test_tx_quality_b [--bench] [mode ...]   (--bench prints the table only, no limits)
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_shape.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "../common/ci_slack.h"
#include "../common/cpu_time.h"
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
using namespace dect2;

namespace {
int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// the second half of the sorted registry ids (the split is recomputed, so a new mode lands in one half or the other)
std::vector<std::string> secondHalf() {
    std::vector<std::string> ids;
    for (const auto& m : txModes()) ids.push_back(m.id);
    std::sort(ids.begin(), ids.end());
    const size_t h = (ids.size() + 1) / 2;
    return std::vector<std::string>(ids.begin() + (long)h, ids.end());
}

struct Q {
    double rate = 0, secs = 0, rt = 0, worstBlock = 0;   // rt: seconds of signal per second of CPU; worstBlock: slowest block time / block duration
    double peak = 0, rms = 0, paprDb = 0, over09 = 0, over1 = 0;
    bool nonFinite = false;
    double obw = 0, fc = 0, aclrDb = -200, aclrSteadyDb = -200, farDb = -200, spurDb = -200, dcDb = -200, step = 0, stepAtSec = 0, gapMs = 0;
};

// CPU time of this thread: the machine may be busy with other work, and the wall clock would then measure that instead of the generator
// (std::clock() is that wall clock on Windows)
double cpuNow() { return threadCpuSecs(); }

void welch(const std::vector<cf32>& x, int nfft, std::vector<double>& psd, bool steadyOnly = false) {
    int log2n = 0;
    while ((1 << log2n) < nfft) log2n++;
    psd.assign((size_t)nfft, 0.0);
    std::vector<float> w((size_t)nfft), re((size_t)nfft), im((size_t)nfft);
    for (int i = 0; i < nfft; i++) {
        const double t = 2 * 3.14159265358979 * i / (double)(nfft - 1);
        w[(size_t)i] = (float)(0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t));
    }
    int segs = 0;
    for (size_t s = 0; s + (size_t)nfft <= x.size(); s += (size_t)nfft / 2) {
        double e = 0;
        bool edge = false;
        for (int i = 0; i < nfft; i++) { const float m2 = std::norm(x[s + (size_t)i]); e += m2; edge |= m2 == 0.f; }
        if (e <= 0) continue;   // silence between bursts says nothing about the shape
        if (steadyOnly && edge) continue;   // only the segments inside a burst: the modulation's own shape without the key-up / key-down
        for (int i = 0; i < nfft; i++) { re[(size_t)i] = x[s + (size_t)i].real() * w[(size_t)i]; im[(size_t)i] = x[s + (size_t)i].imag() * w[(size_t)i]; }
        fftSplit(re.data(), im.data(), log2n, false);
        for (int i = 0; i < nfft; i++) psd[(size_t)((i + nfft / 2) % nfft)] += (double)re[(size_t)i] * re[(size_t)i] + (double)im[(size_t)i] * im[(size_t)i];
        segs++;
    }
    if (segs) for (auto& p : psd) p /= segs;   // in frequency order: bin 0 is -rate/2
}

bool measure(const TxModeSpec& m, Q& q, std::string& err) {
    TxRequest r;
    r.modeId = m.id;
    // what an operator fills in (the panel's payload fields): the modes take their own transmit path, not the receiver-test demo
    r.callsign = "LY2TX";
    r.grid = std::string(m.id).rfind("ka", 0) == 0 ? "KO24rm" : "KO24";   // KAAL / KAWS want six characters, the WSJT modes four
    r.text = "CQ CQ DE LY2TX TEST";
    r.address = 2460001;
    q.rate = txRequestRate(m, r, {});
    // the slotted modes wait for their UTC slot: run them long enough for a whole one (JS8 at its lowest rate to keep the buffer small)
    double secs = std::min(2.0, std::max(0.5, 4e6 / q.rate));
    if (!strcmp(m.id, "js8")) { q.rate = 250000; secs = 32; }
    // the WSJT modes start where the system clock is in their slot (Q65: 51 s of tones in a 60 s slot): 2 s fell into the silence about one run
    // in eight. 16 s always holds part of a transmission (as test_tx_quality_a), at a quarter of the rate to keep the buffer small
    if (txPayloadKind(m.id) == TxPayloadKind::Ftx) { q.rate = std::max(250e3, m.minRateHz); secs = 16; }
    if (!strcmp(m.id, "kaws")) secs = 125;
    auto g = makeTxGenerator(m, r, q.rate, err);
    if (!g) return false;
    q.rate = g->sampleRate();
    const size_t blk = 1 << 15;
    q.secs = secs;
    const size_t n = (size_t)(q.secs * q.rate) / blk * blk + blk;
    q.secs = (double)n / q.rate;
    std::vector<cf32> x(n);
    double cpu = 0;
    std::vector<double> per;   // CPU time of each block after the first
    for (size_t i = 0; i < n; i += blk) {
        const double t0 = cpuNow();
        g->generate(x.data() + i, blk);
        const double dt = cpuNow() - t0;
        cpu += dt;
        if (i > 0) { q.worstBlock = std::max(q.worstBlock, dt / (blk / q.rate)); per.push_back(dt); }   // the first block pays the set-up
        if (g->finished()) { x.resize(i + blk); break; }
    }
    // the real-time factor from the median block: even thread CPU time grows when a shared runner's other jobs take the core's caches and
    // its second hardware thread (NR and Remote ID at 20 Msps measured 1.6 .. 1.9 x alone and 0.8 .. 1.1 x beside a full ctest -j); the
    // median keeps the generator's own speed and drops those stretches. A single block (short runs), or a CPU clock in scheduler ticks
    // (Windows: 15.6 ms against 1.6 ms blocks at 20 Msps): the whole-run figure.
    if (per.size() >= 3 && !cpuClockCoarse()) {
        std::nth_element(per.begin(), per.begin() + (long)(per.size() / 2), per.end());
        q.rt = (blk / q.rate) / std::max(per[per.size() / 2], 1e-9);
    } else q.rt = q.secs / std::max(cpu, 1e-9);
    double acc = 0, pk = 0;
    cf32 mean(0, 0);
    size_t o09 = 0, o1 = 0, zr = 0, zmax = 0;
    float prevMag = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const cf32 v = x[i];
        if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) { q.nonFinite = true; x[i] = 0; continue; }
        const float m2 = std::norm(v), mg = std::sqrt(m2);
        acc += m2;
        pk = std::max(pk, (double)m2);
        mean += v;
        if (mg > 0.9f) o09++;
        if (mg > 1.0f) o1++;
        if (m2 == 0.f) { zr++; zmax = std::max(zmax, zr); } else zr = 0;
        if (i && std::fabs(mg - prevMag) > q.step) { q.step = std::fabs(mg - prevMag); q.stepAtSec = (double)i / q.rate; }
        prevMag = mg;
    }
    const double N = (double)x.size();
    q.rms = std::sqrt(acc / N);
    q.peak = std::sqrt(pk);
    q.paprDb = q.rms > 0 ? 20 * std::log10(q.peak / q.rms) : 0;
    q.over09 = o09 / N;
    q.over1 = o1 / N;
    q.step = q.peak > 0 ? q.step / q.peak : 0;
    q.gapMs = zmax / q.rate * 1e3;
    mean /= (float)N;
    q.dcDb = q.rms > 0 && std::abs(mean) > 0 ? 20 * std::log10(std::abs(mean) / q.rms) : -200;
    const int nfft = 4096;
    std::vector<double> p;
    welch(x, nfft, p);
    double tot = 0;
    for (double v : p) tot += v;
    if (tot <= 0) return true;
    size_t a = 0, b = (size_t)nfft - 1;
    double lo = 0, hi = 0;
    while (a < b && lo + p[a] <= tot * 0.005) lo += p[a++];
    while (b > a && hi + p[b] <= tot * 0.005) hi += p[b--];
    const double bin = q.rate / nfft;
    const double B = (double)(b - a + 1);   // bins
    const double c = 0.5 * (a + b);
    q.obw = B * bin;
    q.fc = (c - nfft / 2) * bin;
    auto db = [](double v) { return v > 0 ? 10 * std::log10(v) : -200.0; };
    // a side that falls off the span (a signal near the band edge) does not count
    const bool loFits = c - 1.75 * B >= 0, hiFits = c + 1.75 * B < nfft;
    auto aclrOf = [&](const std::vector<double>& ps) {
        double inb = 0, adjLo = 0, adjHi = 0;
        for (int i = 0; i < nfft; i++) {
            const double d = i - c, ad = std::fabs(d);
            if (ad <= B / 2) inb += ps[(size_t)i];
            if (ad >= 0.75 * B && ad <= 1.75 * B) (d < 0 ? adjLo : adjHi) += ps[(size_t)i];
        }
        if (inb <= 0) return -200.0;
        return std::max(loFits ? db(adjLo / inb) : -200.0, hiFits ? db(adjHi / inb) : -200.0);
    };
    double far = 0, inPk = 0, outPk = 0;
    for (int i = 0; i < nfft; i++) {
        const double ad = std::fabs(i - c);
        if (ad <= B / 2) inPk = std::max(inPk, p[(size_t)i]);
        if (ad > 3 * B) far += p[(size_t)i];
        if (ad > 1.5 * B) outPk = std::max(outPk, p[(size_t)i]);
    }
    q.aclrDb = aclrOf(p);
    std::vector<double> ps;
    welch(x, nfft, ps, true);
    q.aclrSteadyDb = aclrOf(ps);   // inside the bursts only: -200 when no 4096-sample stretch is free of silence
    q.farDb = db(far / tot);
    q.spurDb = outPk > 0 ? db(outPk / inPk) : -200;
    return true;
}
// Round trip: the RTTY transmitter (the HF sideband path all the keyboard / WSJT modes of this half share) into the HF receiver over white
// noise at several signal-to-noise ratios (in 3 kHz) and carrier offsets. The text must come back each time.
void rttyRoundTrips() {
    const double rate = 1e6;
    const double dial = -hfdigTuning().tuneOffsetHz;
    struct Case { double snr3k, offHz; };
    const Case cases[] = {{30, 0}, {15, 0}, {10, 0}, {15, 25}, {15, -25}};
    for (const Case& c : cases) {
        TxRequest r;
        r.modeId = "rtty";
        r.text = "RYRY HELLO TX TEST";
        std::string err;
        auto g = makeTxGenerator(*txModeById("rtty"), r, rate, err);
        CHECK(g != nullptr, "rtty generator: %s", err.c_str());
        if (!g) return;
        HfdigReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(dial);
        rx.setSilent(true);
        rx.setDecoders(1);
        rx.reset();
        std::mt19937 rng(11);
        std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> buf(1 << 14);
        double sig = 0, ph = 0;
        size_t made = 0;
        const size_t need = (size_t)(9 * rate);
        float sigma = 0;
        while (made < need) {
            g->generate(buf.data(), buf.size());
            if (made == 0) {   // the noise from the first block's power: the sideband power over 3 kHz
                for (const auto& v : buf) sig += std::norm(v);
                sig /= (double)buf.size();
                const double n0 = sig / std::pow(10.0, c.snr3k / 10) / 3000.0;   // per Hz
                sigma = (float)std::sqrt(n0 * rate / 2);
            }
            const double w = 2 * 3.14159265358979 * c.offHz / rate;
            for (auto& v : buf) {
                v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sigma * nd(rng), sigma * nd(rng));
                ph = std::fmod(ph + w, 2 * 3.14159265358979);
            }
            rx.feed(buf.data(), buf.size());
            made += buf.size();
        }
        HfdigTelemetry tel;
        rx.telemetry(tel, 0);
        const bool ok = tel.rtty.text.find("HELLO TX") != std::string::npos;
        printf("  rtty round trip at %4.0f dB / %+4.0f Hz: %s\n", c.snr3k, c.offHz, ok ? "decoded" : tel.rtty.text.c_str());
        CHECK(ok, "rtty: the text did not come back at %.0f dB in 3 kHz, %+.0f Hz", c.snr3k, c.offHz);
    }
}
} // namespace

int main(int argc, char** argv) {
    bool bench = false;
    std::vector<std::string> only;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench")) bench = true;
        else only.push_back(argv[i]);
    }
    const auto ids = only.empty() ? secondHalf() : only;
    if (!bench && (only.empty() || std::find(only.begin(), only.end(), "rtty") != only.end())) rttyRoundTrips();
    printf("%-10s %9s %6s %7s %6s %6s %5s %6s %7s %9s %9s %7s %7s %7s %7s %6s %5s %7s\n", "mode", "rate", "secs", "rt", "worst", "peak", "rms", "papr",
           ">0.9", "obw", "fc", "aclr", "aclrSt", "far", "spur", "dc", "step", "gapms");
    for (const auto& id : ids) {
        const TxModeSpec* m = txModeById(id);
        if (!m) { CHECK(false, "%s: not in the registry", id.c_str()); continue; }
        Q q;
        std::string err;
        if (!measure(*m, q, err)) { CHECK(false, "%s: no generator at its nominal rate: %s", id.c_str(), err.c_str()); continue; }
        printf("%-10s %9.0f %6.2f %7.1f %6.3f %6.3f %5.3f %6.1f %7.4f %9.0f %9.0f %7.1f %7.1f %7.1f %7.1f %6.1f %5.2f %7.1f\n", id.c_str(), q.rate, q.secs, q.rt,
               q.worstBlock, q.peak, q.rms, q.paprDb, q.over09, q.obw, q.fc, q.aclrDb, q.aclrSteadyDb, q.farDb, q.spurDb, q.dcDb, q.step, q.gapMs);
        fflush(stdout);
        if (bench) continue;
        // limits every mode meets: the signal exists, it is finite, it never asks the limiter for more than full scale, it is made faster than it is sent
        CHECK(!q.nonFinite, "%s: NaN or Inf in the generated IQ", id.c_str());
        CHECK(q.rms > 1e-4, "%s: no signal (rms %.2g)", id.c_str(), q.rms);
        CHECK(q.over1 == 0, "%s: %.3f %% of the samples are above full scale (clipped by the limiter)", id.c_str(), q.over1 * 100);
        CHECK(q.over09 < 0.01, "%s: %.2f %% of the samples reach the soft limiter", id.c_str(), q.over09 * 100);
        CHECK(q.rt > 1.5 / ciSlack(), "%s: generated at only %.2f x real time (thread CPU time), the limit is %.2f", id.c_str(), q.rt, 1.5 / ciSlack());
        // UAT's generator writes dump978-style phase steps (2 samples a bit, +-12 kHz at 2 Msps instead of 1.041667 Mbit/s at +-312.5 kHz): a
        // carrier with wide, weak data sidebands. Its far power is bounded at -30 dBc until the modulator and the receiver move to the standard
        CHECK(q.farDb < (id == "uat" ? -30 : -40), "%s: %.1f dBc of the power lies more than 3 occupied bandwidths away", id.c_str(), q.farDb);
        // KAAL's own waveform (the owner's specification) and the TETRAPOL test frame sent over and over (its line spectrum has a line at
        // the centre) carry a mean by design, and so does the NFC reader: its unmodulated 13.56 MHz carrier powers the tag (ISO/IEC 14443-2) and is
        // the IF centre; every other mode must not leak a carrier at the IF centre
        if (id != "kaal" && id != "tetrapol" && id != "nfc") CHECK(q.dcDb < -20, "%s: DC offset %.1f dB of the rms", id.c_str(), q.dcDb);
        // a mode on the burst shaper's list must start and end its bursts on a ramp, not a step
        if (txBurstRampSec(id) > 0) CHECK(q.step < 0.5, "%s: the envelope still steps by %.2f of the peak at a burst edge (at %.6f s)", id.c_str(), q.step, q.stepAtSec);
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("tx quality (second half): all limits met\n");
    return 0;
}
