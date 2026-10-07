// ADS-B simulation harness (see adsb_sim.h).
#include "dect2/adsb_sim.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cmath>
#include <map>

namespace dect2 {

namespace {
// CPU time of the process, so that a busy machine does not make the receiver look slow (the harness runs on one thread)
double cpuNow() { return (double)std::clock() / CLOCKS_PER_SEC; }

struct Gauss {
    uint64_t s[2];
    explicit Gauss(uint64_t seed) { s[0] = seed * 0x9E3779B97F4A7C15ull + 1; s[1] = seed ^ 0xD1B54A32D192ED03ull; for (int i = 0; i < 20; i++) next(); }
    uint64_t next() { uint64_t x = s[0]; const uint64_t y = s[1]; s[0] = y; x ^= x << 23; s[1] = x ^ y ^ (x >> 17) ^ (y >> 26); return s[1] + y; }
    double uni() { return ((double)(next() >> 11) + 0.5) * (1.0 / 9007199254740992.0); }
    void pair(double& a, double& b) { const double r = std::sqrt(-2.0 * std::log(uni())), t = 2 * M_PI * uni(); a = r * std::cos(t); b = r * std::sin(t); }
};
}

AdsbSimResult adsbSimulate(const AdsbSimConfig& c, AdsbReceiver& rx) {
    AdsbSimResult res;
    std::vector<AdsbTx> tx = c.custom;
    const double a50 = 0.1;
    if (tx.empty()) {
        AdsbAirspaceConfig ac;
        ac.aircraft = c.aircraft; ac.seed = c.seed; ac.rateMultiplier = c.mult; ac.replies = c.replies; ac.refLat = c.refLat; ac.refLon = c.refLon; ac.a50 = a50;
        AdsbAirspace air(ac);
        air.run(c.seconds, tx);
    }
    for (auto& t : tx) t.cfoHz += c.cfoHz;
    // interference (bursts that are only pulses, no message): mixed in like the rest, but not part of the list of what is sent
    std::vector<AdsbTx> interference;
    {
        std::vector<AdsbTx> frames;
        for (auto& t : tx) (t.pulses.empty() ? frames : interference).push_back(t);
        tx.swap(frames);
        std::sort(interference.begin(), interference.end(), [](const AdsbTx& a, const AdsbTx& b) { return a.t < b.t; });
    }
    AdsbNoise noise(c.rate, c.filterMHz, c.snrDb, a50, c.seed * 7919 + 13);
    res.noiseSigma = noise.sigmaPerComponent();
    const double pnoise2m = a50 * a50 * std::pow(10.0, -c.snrDb / 10.0);

    // the list of what is sent
    std::sort(tx.begin(), tx.end(), [](const AdsbTx& a, const AdsbTx& b) { return a.t < b.t; });
    for (const auto& t : tx) {
        AdsbSimFrame f;
        f.t = t.t;
        f.hex = adsb::toHex(t.frame.b, t.frame.bits);
        f.df = (int)adsb::getBits(t.frame.b, 1, 5);
        f.icao = f.df == 11 || f.df >= 17 ? adsb::getBits(t.frame.b, 9, 24) : 0;
        f.amp = t.amp;
        f.snrDb = (float)(10.0 * std::log10((double)t.amp * t.amp / pnoise2m));
        res.sent.push_back(f);
    }
    for (size_t i = 0; i < res.sent.size(); i++) {
        const double endI = res.sent[i].t + (8 + tx[i].frame.bits) * 1e-6;
        for (size_t j = i + 1; j < res.sent.size() && res.sent[j].t < endI + 130e-6; j++) {
            const double endJ = res.sent[j].t + (8 + tx[j].frame.bits) * 1e-6;
            if (res.sent[j].t < endI + 1e-6 && endJ > res.sent[i].t - 1e-6) { res.sent[i].overlapped = true; res.sent[j].overlapped = true; }
        }
        if (i > 0) {   // a burst that began before this one and is still on the air
            for (size_t j = i; j-- > 0 && res.sent[j].t > res.sent[i].t - 130e-6;) {
                const double endJ = res.sent[j].t + (8 + tx[j].frame.bits) * 1e-6;
                if (endJ > res.sent[i].t - 1e-6) { res.sent[i].overlapped = true; res.sent[j].overlapped = true; }
            }
        }
    }

    // a frame that an interfering burst touches is overlapped as well
    for (size_t i = 0; i < res.sent.size() && !interference.empty(); i++) {
        const double a = res.sent[i].t - 1e-6, b = res.sent[i].t + (8 + tx[i].frame.bits) * 1e-6 + 1e-6;
        auto it = std::lower_bound(interference.begin(), interference.end(), a - 60e-6, [](const AdsbTx& x, double v) { return x.t < v; });
        for (; it != interference.end() && it->t < b; ++it) {
            double end = 0;
            for (const auto& p : it->pulses) end = std::max(end, p.second);
            if (it->t + end * 1e-6 > a) { res.sent[i].overlapped = true; break; }
        }
    }

    rx.configure(c.rate);
    rx.setCorrection(c.fixBits);
    if (c.setRef) rx.setReference(c.refLat, c.refLon);
    if (c.kPulse > 0 || c.kGap > 0) rx.setThresholds(c.kPulse > 0 ? c.kPulse : 3.0f, c.kGap > 0 ? c.kGap : 2.0f);
    struct Got { double t; std::string hex; float level, snr; int fixed; };
    std::vector<Got> got;
    rx.setFrameCallback([&](const AdsbFrame& f) { got.push_back({f.timeSec, adsb::toHex(f.bytes, f.bits), f.levelDbfs, f.snrDb, f.corrected}); });

    AdsbMixer mix(c.rate, c.filterMHz, c.sroPpm);
    for (const auto& t : tx) mix.add(t);
    for (const auto& t : interference) mix.add(t);
    const double total = (tx.empty() ? c.seconds : std::max(c.seconds, tx.back().t + 200e-6)) + c.tailSec;
    const size_t nTotal = (size_t)(total * c.rate);
    std::vector<cf32> buf;
    size_t done = 0, ci = 0;
    bool resetDone = c.resetAtSec < 0;
    double cwPhase = 0;
    const double cwAmp = c.cwDb > -900 ? a50 * std::pow(10.0, c.cwDb / 20.0) : 0.0;
    const float g = (float)std::pow(10.0, c.iqGainDb / 20.0), sp = (float)std::sin(c.iqPhaseDeg * M_PI / 180.0), cp = (float)std::cos(c.iqPhaseDeg * M_PI / 180.0);
    double cpu = 0;
    while (done < nTotal) {
        size_t n = std::min(c.chunks[ci++ % c.chunks.size()], nTotal - done);
        buf.resize(n);
        mix.render(buf.data(), n);
        noise.add(buf.data(), n);
        for (size_t i = 0; i < n; i++) {
            float re = buf[i].real(), im = buf[i].imag();
            if (cwAmp > 0) { cwPhase += 2 * M_PI * c.cwHz / c.rate; re += (float)(cwAmp * std::cos(cwPhase)); im += (float)(cwAmp * std::sin(cwPhase)); }
            const float im2 = g * (im * cp + re * sp);
            re += (float)c.dcOffset; float imq = im2 + (float)c.dcOffset;
            const double t = (double)(done + i) / c.rate;
            if (c.gainStepAtSec >= 0 && t >= c.gainStepAtSec) { const float gs = (float)std::pow(10.0, c.gainStepDb / 20.0); re *= gs; imq *= gs; }
            if (c.clipFactor != 1) { re *= (float)c.clipFactor; imq *= (float)c.clipFactor; }
            if (c.gapAtSec >= 0 && t >= c.gapAtSec && t < c.gapAtSec + c.gapMs * 1e-3) { re = 0; imq = 0; }
            if (c.quantise) {
                re = std::round(std::min(127.f, std::max(-128.f, re * 128.f))) / 128.f;
                imq = std::round(std::min(127.f, std::max(-128.f, imq * 128.f))) / 128.f;
            }
            buf[i] = cf32(re, imq);
        }
        if (!resetDone && (double)(done + n) / c.rate > c.resetAtSec) {
            // split the chunk at the reset time
            const size_t k = (size_t)std::max(0.0, c.resetAtSec * c.rate - (double)done);
            const double t0 = cpuNow();
            if (k) rx.feed(buf.data(), std::min(k, n));
            rx.reset();
            if (k < n) rx.feed(buf.data() + k, n - k);
            cpu += cpuNow() - t0;
            resetDone = true;
        } else {
            const double t0 = cpuNow();
            rx.feed(buf.data(), n);
            cpu += cpuNow() - t0;
        }
        done += n;
    }
    res.cpuSec = cpu;
    res.signalSec = (double)nTotal / c.rate;
    for (int k = 0; k < 2; k++) rx.telemetry(res.tel, 0);

    // check off: a decoded message belongs to a transmission with the same bytes that started within 3 us of the preamble time it reports.
    // (A DF11 reply whose parity differs in its last seven bits is accepted as a reply to an interrogator, which sends those bits too: it matches on the
    // first 32 bits.) The receiver's clock is the nominal one, the generator's runs sroPpm fast.
    const double toRx = 1.0 + c.sroPpm * 1e-6;
    std::multimap<std::string, size_t> byHex;
    for (size_t i = 0; i < res.sent.size(); i++) byHex.emplace(res.sent[i].hex.substr(0, res.sent[i].hex.size() == 14 ? 8 : res.sent[i].hex.size()), i);
    for (const auto& gt : got) {
        bool found = false;
        auto range = byHex.equal_range(gt.hex.substr(0, gt.hex.size() == 14 ? 8 : gt.hex.size()));
        for (auto it = range.first; it != range.second; ++it) {
            AdsbSimFrame& f = res.sent[it->second];
            if (std::fabs(f.t * toRx - gt.t) < 3e-6 && !f.decoded) {
                f.decoded = true; f.levelDbfs = gt.level; f.rxSnrDb = gt.snr; f.corrected = gt.fixed;
                found = true;
                break;
            }
        }
        if (!found) { res.phantom++; if (res.phantoms.size() < 20) res.phantoms.push_back(gt.hex); }
    }
    for (const auto& f : res.sent) if (f.decoded) res.decoded++;
    return res;
}

double adsbDetectionRate(const AdsbSimResult& r, double lo, double hi, bool includeOverlapped, size_t* count) {
    size_t n = 0, ok = 0;
    for (const auto& f : r.sent) {
        if (f.snrDb < lo || f.snrDb >= hi) continue;
        if (!includeOverlapped && f.overlapped) continue;
        n++;
        if (f.decoded) ok++;
    }
    if (count) *count = n;
    return n ? (double)ok / (double)n : 1.0;
}

} // namespace dect2
