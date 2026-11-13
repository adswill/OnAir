// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drone receiver: see dji_rx.h.
#include "dect2/dji/dji_rx.h"
#include "dect2/dji/dji_dsp.h"
#include "dect2/dji/dji_proto.h"
#include "dect2/dji/dji_tel.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include "dect2/common/timecompat.h"

namespace dect2 {

namespace {

constexpr double kBinSec = 10e-6;          // power envelope step
constexpr double kPadBeforeSec = 120e-6, kPadAfterSec = 120e-6;
constexpr double kMaxBurstSec = 5e-3;
constexpr double kReportSec = 0.25;
constexpr int kPsd = 1024;
constexpr double kMinBwMhz = 3.0;          // narrower signals are not drone video links

int64_t wallNow() { return dect2::unixNow(); }

double db10(double v) { return 10.0 * std::log10(std::max(v, 1e-20)); }

// the DroneID frequencies noted in the public write-ups (MHz); a signal near one is likely DroneID
bool nearDroneIdFreq(double mhz) {
    static const double f[] = {2399.5, 2414.5, 2429.5, 2444.5, 2459.5, 5756.5, 5776.5, 5796.5};
    for (double v : f) if (std::abs(mhz - v) < 3.0) return true;
    return false;
}

std::string bandOf(double mhz) {
    if (mhz >= 2400 && mhz <= 2483.5) return "2.4 GHz";
    if (mhz >= 5725 && mhz <= 5850) return "5.8 GHz";
    if (mhz >= 5150 && mhz <= 5350) return "5.2 GHz";
    return "";
}

// OFDM-likeness: the cyclic prefix repeats the end of every symbol, so the product of a sample and the one a symbol length later, over the prefix, adds up
// over symbols at the right alignment whatever the carrier offset (magnitudes of the symbols' sums). 0 for noise and non-OFDM signals, up to 1.
float cpMetric(const cf32* x, int n, int nfft, int cp) {
    const int P = nfft + cp;
    if (n < nfft + 4 * P) return 0.f;
    const int len = n - nfft;
    std::vector<cf32> pre((size_t)len + 1, cf32(0, 0));
    std::vector<float> en((size_t)len + 1, 0.f);
    for (int i = 0; i < len; i++) {
        {   // x[i] * conj(x[i + nfft]) with the products apart as std::complex does them, without its NaN check (the same result for finite samples)
            const cf32 a = x[i], b = x[i + nfft];
            const float re = a.real() * b.real() - a.imag() * (-b.imag()), im = a.real() * (-b.imag()) + a.imag() * b.real();
            pre[(size_t)i + 1] = pre[(size_t)i] + cf32(re, im);
        }
        en[(size_t)i + 1] = en[(size_t)i] + 0.5f * (std::norm(x[i]) + std::norm(x[i + nfft]));
    }
    float best = 0;
    for (int tau = 0; tau < 2 * P; tau++) {
        float num = 0, den = 0;
        int m = 0;
        for (int i = 0; i < 8; i++) {
            const int a = tau + i * P;
            if (a + cp > len) break;
            num += std::abs(pre[(size_t)(a + cp)] - pre[(size_t)a]);
            den += en[(size_t)(a + cp)] - en[(size_t)a];
            m++;
        }
        if (m < 3 || den <= 0) continue;
        best = std::max(best, num / den);
    }
    return best;
}

} // namespace

struct DjiReceiver::Impl {
    struct Obs { double t, dur; float ofdm; };
    struct TrackState {
        DjiTrack pub;
        std::deque<Obs> obs;
        double lastRssiT = -1e9;
        float rssiMax = -200;
        bool allowDecode() const { return pub.zc || pub.periodMs == 0 || pub.periodMs > 50.0; }
    };
    struct Pending { uint64_t start, end; };

    mutable std::mutex mu, telMu;
    double fs = 0, offsetHz = 0, centerHz = 0;
    bool isReady = false, wallClock = true, decodeId = true;
    std::function<void(const std::string&)> logCb;
    int binLen = 200;
    std::vector<cf32> ring;
    uint64_t total = 0;
    double binSum = 0;
    int binN = 0;
    uint64_t binIdx = 0;
    // noise floor
    double nf = 0;
    bool nfReady = false;
    std::vector<double> warm;
    // the burst in progress
    bool inBurst = false;
    uint64_t burstStart = 0;
    int offBins = 0;
    std::deque<Pending> pending;
    // idle spectrum
    std::vector<double> noisePsd;
    int noiseBlocks = 0;
    uint64_t nextNoiseAt = 0;
    Fft fft{kPsd};
    std::vector<float> hann;
    std::unique_ptr<dji::Resampler> rsNarrow, rsWide;
    // results
    std::vector<TrackState> tracks;
    int nextTrackId = 1;
    std::deque<DjiBurst> bursts;
    std::vector<DjiDrone> drones;
    std::deque<float> levelHist;
    double repMax = -200;
    uint64_t burstsTotal = 0, burstsOfdm = 0;
    uint64_t okCount = 0, badCount = 0;
    double lastIdSec = -1e9, lastCfo = 0;
    float lastSnr = 0, lastLevel = -200;
    double nextReport = kReportSec;
    uint64_t seq = 0;
    DjiTelemetry pub;
    int lastState = 0;

    double now() const { return fs > 0 ? (double)total / fs : 0.0; }
    void say(const std::string& s) { if (logCb) logCb(s); }

    void configure(double rate) {
        std::lock_guard<std::mutex> lk(mu);
        fs = rate;
        isReady = rate >= djiTuning().minSampleRate - 1;
        binLen = std::max(32, (int)std::lround(rate * kBinSec));
        ring.assign((size_t)(0.012 * rate) + 4096, cf32(0, 0));
        total = 0;
        resetDetector();
        hann.resize(kPsd);
        for (int i = 0; i < kPsd; i++) hann[(size_t)i] = 0.5f - 0.5f * (float)std::cos(2 * 3.14159265358979 * (i + 0.5) / kPsd);
        if (isReady) {
            rsNarrow = std::make_unique<dji::Resampler>(rate, dji::kFs, 7.0e6);
            rsWide = std::make_unique<dji::Resampler>(rate, 2 * dji::kFs, std::min(0.46 * rate, 14.0e6));
        }
        pub = DjiTelemetry();
        pub.inputRate = rate;
        pub.centerMhz = centerHz / 1e6;
        publish();
    }

    void resetDetector() {
        binSum = 0; binN = 0; binIdx = 0;
        nf = 0; nfReady = false; warm.clear();
        inBurst = false; offBins = 0;
        pending.clear();
        noisePsd.assign(kPsd, 0.0); noiseBlocks = 0; nextNoiseAt = 0;
        repMax = -200; nextReport = now() + kReportSec;
    }

    // ---- detection
    void copyOut(uint64_t from, size_t n, std::vector<cf32>& out) const {
        out.resize(n);
        const size_t R = ring.size();
        for (size_t i = 0; i < n; i++) out[i] = ring[(size_t)((from + i) % R)];
    }

    void processBin(double p, uint64_t endAbs) {
        binIdx++;
        if (!nfReady) {
            warm.push_back(p);
            if (warm.size() >= 64) {
                std::sort(warm.begin(), warm.end());
                double s = 0;
                for (size_t i = 0; i < warm.size() / 2; i++) s += warm[i];
                nf = s / (double)(warm.size() / 2) * 1.1;
                nfReady = true;
            }
            return;
        }
        if (!inBurst) {
            if (p > nf * 2.5) {
                inBurst = true; offBins = 0;
                burstStart = endAbs - (uint64_t)binLen;
            } else if (p < nf * 0.6) nf += 0.2 * (p - nf);
            else if (p < nf * 1.6) nf += 0.01 * (p - nf);
            else nf += 1e-4 * (p - nf);
        } else {
            if (p < nf * 1.8) offBins++; else offBins = 0;
            if (offBins >= 3) {
                inBurst = false;
                const uint64_t end = endAbs - (uint64_t)(3 * binLen);
                if (end > burstStart + (uint64_t)(2 * binLen)) pending.push_back({burstStart, end});
            } else if ((double)(endAbs - burstStart) > kMaxBurstSec * fs) {   // a continuous signal: close it, and take it for the floor from now on
                inBurst = false;
                pending.push_back({burstStart, endAbs});
                nf = std::max(nf, p * 0.9);
            }
        }
    }

    void noiseBlock() {
        if (inBurst || !pending.empty() || !nfReady) return;
        if (total < nextNoiseAt || total < (uint64_t)kPsd) return;
        nextNoiseAt = total + (uint64_t)(0.0005 * fs);
        std::vector<cf32> blk;
        copyOut(total - (uint64_t)kPsd, kPsd, blk);
        // only a block that is quiet: no burst edge in it
        double p = 0;
        for (auto& v : blk) p += std::norm(v);
        if (p / kPsd > nf * 1.5) return;
        for (int i = 0; i < kPsd; i++) blk[(size_t)i] *= hann[(size_t)i];
        fft.forward(blk.data());
        const double a = noiseBlocks < 16 ? 1.0 / (noiseBlocks + 1) : 0.05;
        for (int k = 0; k < kPsd; k++) noisePsd[(size_t)k] += a * ((double)std::norm(blk[(size_t)k]) - noisePsd[(size_t)k]);
        noiseBlocks++;
    }

    // ---- analysis of one burst
    struct Spec { bool ok = false; double offHz = 0, bwHz = 0; bool clipped = false; };

    // The signals in a burst's spectrum: clusters of bins that stand out from the noise (up to three, strongest first)
    std::vector<Spec> measure(const std::vector<cf32>& seg, size_t n0, size_t n1) {
        std::vector<Spec> out;
        if (noiseBlocks < 8) return out;
        std::vector<double> P(kPsd, 0.0);
        int nseg = 0;
        std::vector<cf32> blk(kPsd);
        long a = (long)n0 + 8, b = (long)n1 - 8;
        if (b - a < kPsd) { const long mid = (a + b) / 2; a = mid - kPsd / 2; b = a + kPsd; }
        a = std::max(0L, std::min(a, (long)seg.size() - kPsd));
        b = std::min((long)seg.size(), std::max(b, a + kPsd));
        for (long s = a; s + kPsd <= b && nseg < 48; s += kPsd / 2) {
            for (int i = 0; i < kPsd; i++) blk[(size_t)i] = seg[(size_t)(s + i)] * hann[(size_t)i];
            fft.forward(blk.data());
            for (int k = 0; k < kPsd; k++) P[(size_t)k] += std::norm(blk[(size_t)k]);
            nseg++;
        }
        if (!nseg) return out;
        std::vector<double> e(kPsd);   // fft-shifted, noise taken off
        for (int k = 0; k < kPsd; k++) {
            const int src = (k + kPsd / 2) % kPsd;
            e[(size_t)k] = std::max(0.0, P[(size_t)src] / nseg - noisePsd[(size_t)src]);
        }
        std::vector<double> sm(kPsd, 0.0);
        const int hw = 8;
        double run = 0;
        for (int k = 0; k < kPsd + hw; k++) {
            if (k < kPsd) run += e[(size_t)k];
            if (k - 2 * hw - 1 >= 0) run -= e[(size_t)(k - 2 * hw - 1)];
            const int c = k - hw;
            if (c >= 0 && c < kPsd) sm[(size_t)c] = run / (2 * hw + 1);
        }
        std::vector<double> sorted(sm);
        std::sort(sorted.begin(), sorted.end());
        const double L = sorted[(size_t)(kPsd * 0.97)];
        if (L <= 0) return out;
        const double thr = L * 0.1;
        // clusters of bins above the threshold, gaps of up to 24 bins (about half a MHz) bridged
        struct Cl { int a, b; double sum; };
        std::vector<Cl> cl;
        int ca = -1, cb = -1;
        double cs = 0;
        auto close = [&]() { if (ca >= 0) cl.push_back({ca, cb, cs}); ca = -1; cs = 0; };
        for (int k = 0; k < kPsd; k++) {
            if (sm[(size_t)k] > thr) {
                if (ca >= 0 && k - cb > 24) close();
                if (ca < 0) ca = k;
                cb = k; cs += sm[(size_t)k];
            }
        }
        close();
        std::sort(cl.begin(), cl.end(), [](const Cl& x, const Cl& y) { return x.sum > y.sum; });
        const double binW = fs / kPsd;
        for (const Cl& c : cl) {
            if (out.size() >= 3) break;
            Spec r;
            r.ok = true;
            r.bwHz = (c.b - c.a + 1) * binW;
            if (r.bwHz < kMinBwMhz * 1e6) continue;
            r.offHz = ((c.a + c.b) / 2.0 - kPsd / 2) * binW;
            r.clipped = c.a <= 6 || c.b >= kPsd - 7;
            out.push_back(r);
        }
        return out;
    }

    TrackState* associate(double offHz, double bwHz, double t) {
        TrackState* best = nullptr;
        double bd = 1e18;
        for (auto& k : tracks) {
            const double r = bwHz / std::max(1.0, k.pub.bwMhz * 1e6);
            if (std::abs(k.pub.offsetHz - offHz) < 1.5e6 && r > 0.7 && r < 1.43) {
                const double d = std::abs(k.pub.offsetHz - offHz);
                if (d < bd) { bd = d; best = &k; }
            }
        }
        if (best) return best;
        if (tracks.size() >= kDjiTracksMax) {
            size_t old = 0;
            for (size_t i = 1; i < tracks.size(); i++) if (tracks[i].pub.lastSec < tracks[old].pub.lastSec) old = i;
            tracks.erase(tracks.begin() + (long)old);
        }
        TrackState k;
        k.pub.id = nextTrackId++;
        k.pub.firstSec = t;
        tracks.push_back(std::move(k));
        return &tracks.back();
    }

    static double median(std::vector<double> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    void classify(TrackState& k, int64_t wall) {
        DjiTrack& p = k.pub;
        const size_t n = k.obs.size();
        // timing
        p.periodMs = 0; p.periodQ = 0; p.duty = 0;
        std::vector<double> durs, rhos;
        for (auto& o : k.obs) { durs.push_back(o.dur); rhos.push_back(o.ofdm); }
        p.durUs = median(durs) * 1e6;
        p.ofdm = (float)median(rhos);
        if (n >= 3) {
            std::vector<double> d;
            for (size_t i = 1; i < n; i++) d.push_back(k.obs[i].t - k.obs[i - 1].t);
            const double m = median(d);
            int good = 0;
            for (double v : d) {
                const int mult = std::max(1, (int)std::lround(v / m));
                if (mult <= 8 && std::abs(v / mult - m) < 0.06 * m) good++;
            }
            p.periodMs = m * 1e3;
            p.periodQ = (float)good / (float)d.size();
            double on = 0;
            for (auto& o : k.obs) on += o.dur;
            const double span = k.obs.back().t + k.obs.back().dur - k.obs.front().t;
            p.duty = span > 0 ? (float)std::min(1.0, on / span) : 0.f;
        }
        const double bw = p.bwMhz;
        const bool bw10 = bw >= 8.0 && bw <= 11.8, bw20 = bw >= 16.0 && bw <= 21.5;
        const bool wide = p.clipped && bw >= 14;
        const bool ofdm = p.ofdm >= (bw > 12.0 ? 0.6f : 0.35f);   // a wide cluster may hold several signals: it must be clearly OFDM
        const float fOfdm = (float)std::max(0.0, std::min(1.0, (p.ofdm - 0.2) / 0.4));
        const float fN = (float)std::min(1.0, n / 10.0);
        std::string note;
        if (p.zc) {
            p.cls = kDjiDroneId;
            p.conf = p.decoded > 0 ? 0.99f : 0.9f;
            note = p.decoded > 0 ? "DroneID packet decoded (the CRC-24 passed): an identification, not a guess" : "the two DroneID Zadoff-Chu symbols were found; the packet did not pass its CRC yet";
        } else if (n >= 3 && ofdm && (bw10 || bw20 || wide) && p.periodQ >= 0.6f) {
            const float fBw = (bw10 || bw20) ? 1.f : 0.6f;
            p.conf = std::min(0.95f, 0.15f + 0.35f * fOfdm + 0.25f * p.periodQ + 0.15f * fBw + 0.10f * fN);
            if (std::abs(p.periodMs - 1.0) <= 0.06) { p.cls = kDjiLightbridge; note = "OFDM bursts every 1 ms, 10 or 20 MHz wide"; }
            else if (p.periodMs >= 0.3 && p.periodMs <= 25.0) { p.cls = kDjiOcusync; note = "periodic OFDM bursts, 10 or 20 MHz wide, 15 kHz-type spacing"; }
            else { p.cls = kDjiUnknownOfdm; p.conf = std::min(p.conf, 0.4f); note = "OFDM bursts with a period outside the OcuSync / Lightbridge range"; }
        } else if (n >= 2 && ofdm && bw >= 5.0) {
            p.cls = kDjiUnknownOfdm;
            p.conf = std::min(0.4f, 0.1f + 0.3f * fOfdm * fN + 0.1f * p.periodQ);
            note = "wideband OFDM-like bursts, not regular enough to tell more";
        } else {
            p.cls = kDjiOther;
            p.conf = 0;
            note = n < 3 ? "not enough bursts yet" : "wideband bursts that do not look like 15 / 30 kHz OFDM (Wi-Fi and the like)";
        }
        if (p.cls != kDjiDroneId && centerHz > 0 && bw >= 8.0 && bw <= 11.8 && nearDroneIdFreq(p.freqMhz) && p.cls >= kDjiUnknownOfdm) note += "; near a frequency reported for DroneID";
        p.note = note;
        p.lastWall = wall;
    }

    void upsertDrone(const dji::DroneId& id, double t, int64_t wall, float snr, double freqMhz, int trackId) {
        DjiDrone* d = nullptr;
        for (auto& x : drones) if (x.serial == id.serial) d = &x;
        if (!d) {
            if (drones.size() >= kDjiDronesMax) {
                size_t old = 0;
                for (size_t i = 1; i < drones.size(); i++) if (drones[i].lastSec < drones[old].lastSec) old = i;
                drones.erase(drones.begin() + (long)old);
            }
            DjiDrone n;
            n.serial = id.serial; n.firstSec = t; n.firstWall = wall;
            drones.push_back(std::move(n));
            d = &drones.back();
        }
        d->last = id; d->packets++; d->lastSec = t; d->lastWall = wall; d->snrDb = snr; d->freqMhz = freqMhz; d->trackId = trackId;
        const double la = id.lat(), lo = id.lon();
        if (std::abs(la) > 1e-4 && std::abs(la) <= 90 && std::abs(lo) > 1e-4 && std::abs(lo) <= 180) {
            if (d->track.empty() || std::abs(d->track.back().lat - la) > 1e-7 || std::abs(d->track.back().lon - lo) > 1e-7) {
                d->track.push_back({la, lo, (float)id.altitudeM()});
                if (d->track.size() > kDjiTrail) d->track.erase(d->track.begin());
            }
        }
    }

    void analyze(const Pending& pb) {
        const uint64_t padB = (uint64_t)(kPadBeforeSec * fs), padA = (uint64_t)(kPadAfterSec * fs);
        const uint64_t oldest = total > ring.size() ? total - ring.size() : 0;
        const uint64_t s0 = std::max(oldest, pb.start > padB ? pb.start - padB : 0);
        const uint64_t s1 = std::min(total, pb.end + padA);
        if (s1 <= s0 + 64 || pb.start < oldest) return;
        std::vector<cf32> seg;
        copyOut(s0, (size_t)(s1 - s0), seg);
        const size_t n0 = (size_t)(pb.start - s0), n1 = (size_t)(pb.end - s0);
        const std::vector<Spec> specs = measure(seg, n0, n1);
        for (const Spec& sp : specs) analyzeSignal(seg, s0, sp);
    }

    void analyzeSignal(const std::vector<cf32>& seg, uint64_t s0, const Spec& sp) {
        // the signal alone: mixed to the centre and filtered, at the nearest rate that gives a whole prefix length
        const bool narrow = sp.bwHz <= 12e6;
        const dji::Resampler& rs = narrow ? *rsNarrow : *rsWide;
        const double ro = narrow ? dji::kFs : 2 * dji::kFs;
        std::vector<cf32> mixed(seg);
        dji::mixInPlace(mixed.data(), mixed.size(), -sp.offHz, fs, 0.0);
        const size_t nOut = (size_t)((double)mixed.size() * ro / fs);
        const std::vector<cf32> r = rs.run(mixed, 0.0, nOut);
        // its envelope in 10 us steps: where it is on
        const int eb = std::max(16, (int)std::lround(ro * kBinSec));
        const int ne = (int)(r.size() / (size_t)eb);
        if (ne < 8) return;
        std::vector<double> env((size_t)ne, 0.0);
        for (int j = 0; j < ne; j++) { double q = 0; for (int i = 0; i < eb; i++) q += std::norm(r[(size_t)(j * eb + i)]); env[(size_t)j] = q / eb; }
        std::vector<double> sorted(env);
        std::sort(sorted.begin(), sorted.end());
        const double lo = sorted[(size_t)(ne / 6)];            // the quiet bins around the burst
        double hi = 0;
        for (int j = ne * 2 / 3; j < ne; j++) hi += sorted[(size_t)j];
        hi /= (double)(ne - ne * 2 / 3);
        if (hi < 2.0 * lo) return;
        const double thr = lo + 0.35 * (hi - lo);
        int bestA = -1, bestB = -1, ca = -1, gap = 0;
        for (int j = 0; j <= ne; j++) {
            const bool on = j < ne && env[(size_t)j] > thr;
            if (on) { if (ca < 0) ca = j; gap = 0; if (bestA < 0 || j - ca > bestB - bestA) { bestA = ca; bestB = j; } }
            else if (ca >= 0) { if (++gap > 2 || j == ne) { ca = -1; gap = 0; } }
        }
        if (bestA < 0 || bestB - bestA < 1) return;
        const double durS = (double)(bestB - bestA + 1) * eb / ro;
        const double t = (double)s0 / fs + (double)bestA * eb / ro;
        if (durS < 20e-6) return;
        double pw = 0;
        for (int j = bestA; j <= bestB; j++) pw += env[(size_t)j];
        pw /= (bestB - bestA + 1);
        const double levelDb = db10(pw), snrDb = db10(std::max(pw - lo, lo * 1e-3) / lo);
        burstsTotal++;
        const size_t o0 = (size_t)bestA * (size_t)eb, o1 = std::min(r.size(), (size_t)(bestB + 1) * (size_t)eb);
        float rho = 0;
        if (o1 > o0 + 1000) {
            const size_t a = o0 > 400 ? o0 - 400 : 0;
            const int nfft = (int)std::lround(ro / 15e3);
            const int cp = (int)std::lround(nfft * 72.0 / 1024.0);
            const int lim = (int)std::min<size_t>(o1 - a, (size_t)(14 * (nfft + cp) + 2 * (nfft + cp) + nfft));
            rho = std::max(cpMetric(&r[a], lim, nfft, cp), cpMetric(&r[a], lim, nfft / 2, std::max(2, cp / 2)));
        }
        if (rho >= (sp.bwHz > 12e6 ? 0.6f : 0.35f)) burstsOfdm++;
        const int64_t wall = wallClock ? wallNow() : 0;
        TrackState* k = associate(sp.offHz, sp.bwHz, t);
        DjiTrack& p = k->pub;
        const double a = p.bursts ? 0.2 : 1.0;
        p.offsetHz += a * (sp.offHz - p.offsetHz);
        p.bwMhz += a * (sp.bwHz / 1e6 - p.bwMhz);
        p.clipped = sp.clipped;
        p.freqMhz = centerHz > 0 ? (centerHz + p.offsetHz) / 1e6 : 0;
        p.band = p.freqMhz > 0 ? bandOf(p.freqMhz) : "";
        p.bursts++;
        p.levelDb = (float)levelDb; p.snrDb = (float)snrDb;
        p.lastSec = t; p.lastWall = wall;
        k->obs.push_back({t, durS, rho});
        if (k->obs.size() > 64) k->obs.pop_front();
        k->rssiMax = std::max(k->rssiMax, (float)levelDb);
        if (t - k->lastRssiT >= kReportSec) {
            p.rssi.push_back(k->rssiMax); p.rssiT.push_back((float)t);
            if (p.rssi.size() > kDjiRssiHist) { p.rssi.erase(p.rssi.begin()); p.rssiT.erase(p.rssiT.begin()); }
            k->lastRssiT = t; k->rssiMax = -200;
        }
        repMax = std::max(repMax, levelDb);
        DjiBurst b;
        b.timeSec = t; b.wallTime = wall; b.track = p.id; b.offsetHz = sp.offHz;
        b.bwMhz = (float)(sp.bwHz / 1e6); b.durUs = (float)(durS * 1e6); b.snrDb = (float)snrDb; b.levelDb = (float)levelDb; b.ofdm = rho;
        lastSnr = (float)snrDb; lastLevel = (float)levelDb;
        // DroneID: a burst of its length and width (longer when something else is on at the same time), on a signal that is not a fast burst train
        const double durUs = durS * 1e6;
        if (decodeId && narrow && durUs >= 520 && durUs <= 1500 && sp.bwHz >= 6.5e6 && sp.bwHz <= 11.8e6 && k->allowDecode()) {
            const dji::BurstResult br = dji::receiveBurst(r);
            b.zc = 1;
            if (br.zc) {
                p.zc = true;
                b.zc = 2;
                lastCfo = br.cfoHz + sp.offHz;
                if (br.crcOk) {
                    b.zc = 3;
                    p.decoded++; okCount++; lastIdSec = t;
                    upsertDrone(br.id, t, wall, br.snrDb, p.freqMhz, p.id);
                    say("DJI DroneID: serial " + br.id.serial);
                } else { p.failed++; badCount++; }
            }
        }
        classify(*k, wall);
        bursts.push_front(b);
        while (bursts.size() > kDjiBurstsMax) bursts.pop_back();
    }

    void flushPending() {
        while (!pending.empty() && total >= pending.front().end + (uint64_t)(kPadAfterSec * fs)) {
            const Pending pb = pending.front();
            pending.pop_front();
            analyze(pb);
        }
    }

    void publish() {
        const double t = now();
        DjiTelemetry o;
        o.timeSec = t; o.inputRate = fs; o.centerMhz = centerHz / 1e6;
        o.noiseDb = nfReady ? (float)db10(nf) : -200.f;
        o.blocksOk = okCount; o.blocksBad = badCount;
        o.burstsTotal = burstsTotal; o.burstsOfdm = burstsOfdm;
        o.lastDroneIdSec = lastIdSec; o.cfoHz = lastCfo; o.snrDb = lastSnr; o.levelDb = lastLevel;
        o.dataValid = t - lastIdSec < 30.0;
        bool dji = false, any = false;
        for (auto& k : tracks) {
            o.tracks.push_back(k.pub);
            if (t - k.pub.lastSec < 5.0) any = true;
            if (t - k.pub.lastSec < 10.0 && k.pub.cls >= kDjiLightbridge) dji = true;
        }
        std::sort(o.tracks.begin(), o.tracks.end(), [](const DjiTrack& a, const DjiTrack& b) { return a.lastSec > b.lastSec; });
        o.state = o.dataValid ? 3 : dji ? 2 : any ? 1 : 0;
        o.bursts.assign(bursts.begin(), bursts.end());
        o.drones = drones;
        o.levelHist.assign(levelHist.begin(), levelHist.end());
        if (o.state != lastState) {
            say(o.state == 3 ? "DJI: DroneID packets decoded" : o.state == 2 ? "DJI: a DJI-like signal (by its shape)" : o.state == 1 ? "DJI: wideband bursts" : "DJI: nothing heard");
            lastState = o.state;
        }
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        if (!isReady || n == 0) return;
        const size_t R = ring.size();
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, (size_t)(binLen - binN));
            double s = 0;
            const size_t w0 = (size_t)(total % R);
            for (size_t k = 0; k < take; k++) {
                const cf32 v = x[i + k];
                s += (double)v.real() * v.real() + (double)v.imag() * v.imag();
                ring[(w0 + k) % R] = v;
            }
            binSum += s; binN += (int)take; total += take; i += take;
            if (binN >= binLen) {
                processBin(binSum / binN, total);
                binSum = 0; binN = 0;
                noiseBlock();
            }
        }
        flushPending();
        while (now() >= nextReport) {
            nextReport += kReportSec;
            levelHist.push_back(repMax > -190 ? (float)repMax : (nfReady ? (float)db10(nf) : -100.f));
            if (levelHist.size() > kDjiRssiHist) levelHist.pop_front();
            repMax = -200;
            publish();
        }
    }
};

DjiReceiver::DjiReceiver() : p_(std::make_unique<Impl>()) {}
DjiReceiver::~DjiReceiver() = default;
void DjiReceiver::configure(double r) { p_->configure(r); }
void DjiReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; }
void DjiReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centerHz = hz; }
void DjiReceiver::setWallClock(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->wallClock = on; }
void DjiReceiver::setDecodeDroneId(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->decodeId = on; }
bool DjiReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->isReady; }
void DjiReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); if (p_->isReady) p_->resetDetector(); }
void DjiReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
void DjiReceiver::clearDrones() { clearResults(); }
void DjiReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tracks.clear(); p_->bursts.clear(); p_->drones.clear();
    p_->burstsTotal = p_->burstsOfdm = p_->okCount = p_->badCount = 0;
    p_->lastIdSec = -1e9;
    if (p_->isReady) p_->publish();
}
bool DjiReceiver::telemetry(DjiTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq == lastSeq || p_->pub.seq == 0) return false;
    out = p_->pub;
    return true;
}
std::string DjiReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->now(), p_->drones);
}
bool DjiReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<DjiDrone> d;
    if (!results::unpack(blob, t0, d)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double dt = p_->now() - t0;
    for (auto& x : d) { x.firstSec += dt; x.lastSec += dt; }
    p_->drones = std::move(d);
    p_->publish();
    return true;
}
void DjiReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

ModeTuning djiTuning() {
    ModeTuning t;
    t.stdMode = 70; t.id = "dji"; t.name = "DJI drones";
    t.minMhz = 2300; t.maxMhz = 6000; t.defMhz = 2440;
    t.sampleRate = 20000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 20;
    t.minSampleRate = 16000000;
    t.tuneOffsetHz = 0;
    return t;
}

} // namespace dect2
