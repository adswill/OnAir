// Tone decoders on FM or sound-card audio: see tones_rx.h.
#include "dect2/tones/tones_rx.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/packet/packet_demod.h"
#include "dect2/tones/tones_proto.h"
#include "../audio/ham_listen.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace dect2 {

namespace {

using dect2::math::kPi;
constexpr double kRate = 8000.0;
constexpr double kFull = 5000.0;          // audio units are Hz of deviation; 5 kHz is "0 dB"
constexpr size_t kRing = 4096;            // 8 kHz history (power of two)
constexpr size_t kRing1k = 2048;
constexpr int kDtmfN = 205, kDtmfHop = 80;
constexpr int kSeqN = 128, kSeqHop = 64;
constexpr int kSelN = 800, kSelHop = 400;
constexpr int kLongN = 512, kLongHop = 256;
constexpr int kCtcssWin = 1000, kCtcssHop = 100;
constexpr int kDcsBits = 69, kDcsHop = 64;
constexpr double kDcsSps = 1000.0 / tones::kDcsBaud;   // samples per bit at 1 kHz
constexpr int kSpecBins = 112;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    void highpass(double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
        b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - al) / a0;
    }
    void reset() { z1 = z2 = 0; }
    float step(float xf) {
        const double x = xf, y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
};

// Goertzel power |X(f)|^2 of x[0..n)
double goertzel(const float* x, int n, double f, double fs) {
    const double coeff = 2 * std::cos(2 * kPi * f / fs);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) { const double s0 = x[i] + coeff * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1, 0);
            for (size_t j = 0; j < len / 2; j++) {
                const auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
// USB / LSB / AM front end (HF voice channels, SELCAL): the channel mixed to 0 Hz, decimated to 8 kHz complex, then the sideband
// (300 - 3000 Hz above or below the dial) as real audio, or the AM envelope without its carrier, with an AGC to the FM path's level
struct SsbFront {
    int demod = TonesUsb;
    double fs = 0, offsetHz = 0, offPhase = 0;
    bool ready = false;
    std::vector<dmr::Fir<cf32>> stages;
    ExactResampler rs;
    bool rsPass = true;
    dmr::Fir<cf32> side;
    std::complex<double> sideRot{1, 0}, sidePh{1, 0};
    double agcP = 0, dc = 0;
    double inPower = 0; int64_t inN = 0;
    std::vector<cf32> mix, bufA, bufB, sb, rot;
    std::vector<float> out;

    bool configure(double rate) {
        fs = rate; ready = false; stages.clear();
        if (fs < 8000) return false;
        double r = fs;
        while (true) {
            const int f = std::min(8, (int)std::floor(r / 16000));
            if (f < 2) break;
            const double o = r / f;
            stages.emplace_back();
            stages.back().design(dmr::lowpassTaps(3400.0, std::min(o - 4500.0, r * 0.5), r, 60, 9), f);
            r = o;
        }
        rsPass = std::fabs(r - kRate) < 0.01;
        if (!rsPass && !rs.configure(r, kRate, 8)) return false;
        // the sideband: a low-pass of +-1350 Hz around +-1650 Hz (USB / LSB), the AM channel +-3400 Hz
        side.design(demod == TonesAm ? dmr::lowpassTaps(3400.0, 3900.0, kRate, 50, 9) : dmr::lowpassTaps(1300.0, 1600.0, kRate, 50, 9), 1);
        sideRot = std::polar(1.0, 2 * kPi * (demod == TonesLsb ? -1650.0 : 1650.0) / kRate);
        ready = true;
        reset();
        return true;
    }
    void reset() {
        for (auto& s : stages) s.reset();
        if (!rsPass) rs.reset();
        side.reset();
        offPhase = 0; sidePh = 1; agcP = 0; dc = 0; inPower = 0; inN = 0;
    }
    float takeLevelDb() { const float l = inN ? (float)(10 * std::log10(inPower / (double)inN + 1e-20)) : -200.f; inPower = 0; inN = 0; return l; }
    // the audio at 8 kHz in the FM path's units (kFull is 0 dB)
    const std::vector<float>& process(const cf32* x, size_t n) {
        out.clear();
        if (!ready || n == 0) return out;
        mix.resize(n);
        const double w = -2 * kPi * offsetHz / fs;
        const cf32 stp((float)std::cos(w), (float)std::sin(w));
        cf32 ph((float)std::cos(offPhase), (float)std::sin(offPhase));
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!(std::fabs(v.real()) < 1e4f && std::fabs(v.imag()) < 1e4f)) v = cf32(0, 0);
            inPower += (double)std::norm(v);
            mix[i] = v * ph;
            ph *= stp;
            if ((i & 1023) == 1023) ph /= std::abs(ph);
        }
        offPhase = std::remainder(offPhase + w * (double)n, 2 * kPi);
        inN += (int64_t)n;
        std::vector<cf32>* cur = &mix;
        for (auto& s : stages) {
            std::vector<cf32>& o = cur == &bufA ? bufB : bufA;
            o.clear();
            s.process(cur->data(), cur->size(), o);
            cur = &o;
        }
        if (!rsPass) {
            std::vector<cf32>& o = cur == &bufA ? bufB : bufA;
            o.clear();
            rs.process(cur->data(), cur->size(), o);
            cur = &o;
        }
        sb.clear();
        rot.resize(cur->size());
        if (demod != TonesAm) {   // down by the sideband's middle, low-pass, back up by the same rotor (one output per input): one sideband is left
            for (size_t i = 0; i < cur->size(); i++) {
                rot[i] = cf32((float)sidePh.real(), (float)sidePh.imag());
                (*cur)[i] *= std::conj(rot[i]);
                sidePh *= sideRot;
            }
            sidePh /= std::abs(sidePh);
        }
        side.process(cur->data(), cur->size(), sb);
        out.resize(sb.size());
        for (size_t i = 0; i < sb.size(); i++) {
            double a;
            if (demod == TonesAm) { const double m = std::abs(sb[i]); dc += (m - dc) * (dc == 0 ? 1.0 : 0.0005); a = m - dc; }
            else a = (sb[i] * rot[i]).real();
            const double p = a * a;
            agcP += (p > agcP ? 0.02 : 0.0004) * (p - agcP);   // fast up, slow down (about 0.3 s)
            out[i] = (float)(a * (0.25 * kFull) / std::sqrt(agcP + 1e-20));
        }
        return out;
    }
};

std::string fmt(const char* f, ...) {
    char b[256];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

} // namespace

struct TonesReceiver::Impl {
    std::mutex procMu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0;
    bool isReady = false, audioIn = false;
    int demod = TonesFm;
    bool selcalOnly = false;             // the SELCAL mode: only the SELCAL detector runs
    pktdemod::FmFront fe;
    SsbFront ssb;
    ExactResampler rs;
    hfdig::Listen listen;
    dmr::Fir<float> audio48, audio8;
    std::vector<float> a48, a8, lis;
    std::vector<cf32> rsIn, rsOut;

    // audio history
    std::vector<float> ring = std::vector<float>(kRing, 0.f), ring1k = std::vector<float>(kRing1k, 0.f);
    int64_t n8 = 0, n1k = 0;
    Biquad lp1, lp2, hpListen;
    int decimCnt = 0;
    std::vector<float> win, tmp, tmp2;
    std::vector<double> hann1000, hann800, hann512;
    double sumsq = 0; int64_t cntsq = 0;
    float audioDb = -200;

    // CTCSS
    int cPend = -1, cPendRun = 0, cMiss = 0;
    int ctcssIdx = -1; double ctcssHz = 0; float ctcssDb = -200;
    std::vector<float> ctcssPow = std::vector<float>(tones::kCtcssN, -120.f);
    int ctcssEv = -1;
    // DCS
    int dPendCode = -1; bool dPendInv = false; int dPendRun = 0; int64_t dLastHit = 0;
    int dcsCode = -1; bool dcsInv = false, dcsStd = false; std::string dcsAlias;
    int dcsEv = -1;
    // voice band
    double dtmfOtherUntil = -1e9;        // signal seconds: a DTMF key was heard lately
    int dtmfCand = -1, dtmfRun = 0, dtmfGone = 0; bool dtmfDone = false;
    int dtmfEv = -1; double dtmfLastSec = -1e9; std::string dtmfString;
    // sequential
    struct Seg { int pool; int dwellMs; };
    std::vector<Seg> segs;
    int sqCur = -1, sqRun = 0, sqPend = -2, sqPendRun = 0; int sqSilent = 0;
    // SELCAL
    int slCur = -1, slRun = 0, slPend = -2, slPendRun = 0;
    std::string slFirst; double slFirstEnd = -1e9;
    // long tones
    double ltF = 0; int ltRun = 0; double ltStart = 0; double ltPendF = 0; int ltPendRun = 0; bool ltEmitted = false; int ltEv = -1;
    struct LSeg { double f; double dur; double end; };
    LSeg ltLast{0, 0, -1e9};
    double toneHz = 0; float toneDb = -200;
    std::vector<float> spec = std::vector<float>(kSpecBins, -100.f);
    // gate
    int gateMode = 0, gateKind = 0, gateDcs = 23; bool gateInv = false; double gateHz = 100;
    bool gateOpen = true; int64_t gateHold = 0; float gateGain = 1.f;
    // results
    TonesTelemetry t, pub;
    std::vector<ToneEvent> evs;         // oldest first
    uint64_t seq = 0, okCount = 0;
    int counts[TkKinds] = {};
    int64_t nextReport = 0;
    double lastDecodeSec = -1e9;
    int lastState = -1;

    double now() const { return (double)n8 / kRate; }
    void say(const std::string& s) { if (log) log(s); }

    Impl() {
        hann1000.resize(kCtcssWin); hann800.resize(kSelN); hann512.resize(kLongN);
        for (int i = 0; i < kCtcssWin; i++) hann1000[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kCtcssWin);
        for (int i = 0; i < kSelN; i++) hann800[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kSelN);
        for (int i = 0; i < kLongN; i++) hann512[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kLongN);
        lp1.lowpass(300, kRate, 0.5412); lp2.lowpass(300, kRate, 1.3066);   // 4th order Butterworth
        hpListen.highpass(300, kRate, 0.7071);
    }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        if (audioIn) {
            if (fs < 7999) return;
            if (!rs.configure(fs, kRate, 8)) return;
        } else if (demod != TonesFm) {
            ssb.demod = demod; ssb.offsetHz = fe.offsetHz;
            if (!ssb.configure(fs)) return;
        } else {
            if (!fe.configure(fs, tonesTuning().minSampleRate)) return;
            audio48.design(dmr::lowpassTaps(3300.0, 8000.0, pktdemod::kMidRate, 50, 9), 2);
            audio8.design(dmr::lowpassTaps(3200.0, 800.0, 48000.0, 50, 9), 6);
        }
        isReady = true;
        resetState();
    }

    void resetState() {
        const double off = fe.offsetHz;
        fe.reset(); fe.offsetHz = off;
        rs.reset(); audio48.reset(); audio8.reset(); listen.reset();
        if (ssb.ready) { ssb.offsetHz = off; ssb.reset(); }
        std::fill(ring.begin(), ring.end(), 0.f); std::fill(ring1k.begin(), ring1k.end(), 0.f);
        lp1.reset(); lp2.reset(); hpListen.reset();
        n8 = n1k = 0; decimCnt = 0; sumsq = 0; cntsq = 0; audioDb = -200;
        cPend = -1; cPendRun = cMiss = 0; ctcssIdx = -1; ctcssHz = 0; ctcssDb = -200; std::fill(ctcssPow.begin(), ctcssPow.end(), -120.f);
        dPendCode = -1; dPendRun = 0; dLastHit = 0; dcsCode = -1; dcsInv = dcsStd = false; dcsAlias.clear();
        dtmfCand = -1; dtmfRun = dtmfGone = 0; dtmfDone = false; dtmfString.clear(); dtmfLastSec = -1e9; dtmfOtherUntil = -1e9;
        segs.clear(); sqCur = -1; sqRun = 0; sqPend = -2; sqPendRun = 0; sqSilent = 0;
        slCur = -1; slRun = 0; slPend = -2; slPendRun = 0; slFirst.clear(); slFirstEnd = -1e9;
        ltF = 0; ltRun = 0; ltPendRun = 0; ltEmitted = false; ltLast = {0, 0, -1e9}; toneHz = 0; toneDb = -200;
        std::fill(spec.begin(), spec.end(), -100.f);
        for (auto& e : evs) e.active = false;
        ctcssEv = dcsEv = dtmfEv = ltEv = -1;
        gateOpen = gateMode == 0; gateHold = 0; gateGain = gateOpen ? 1.f : 0.f;
        nextReport = 0; lastState = -1; lastDecodeSec = -1e9;
        t = TonesTelemetry(); t.inputRate = fs; t.audioInput = audioIn; t.demod = demod;
        publish();
    }

    // ---- events
    int addEvent(int kind, const std::string& text, const std::string& detail, float level, bool active, double dur = 0) {
        ToneEvent e;
        e.wall = (int64_t)std::time(nullptr); e.timeSec = now(); e.kind = kind; e.text = text; e.detail = detail; e.levelDb = level; e.active = active; e.durSec = dur;
        evs.push_back(e);
        if (evs.size() > kTonesEventMax) {
            evs.erase(evs.begin());
            for (int* p : {&ctcssEv, &dcsEv, &dtmfEv, &ltEv}) if (*p >= 0) (*p)--;
        }
        counts[kind]++;
        okCount++;
        lastDecodeSec = now();
        return (int)evs.size() - 1;
    }
    ToneEvent* ev(int i) { return i >= 0 && i < (int)evs.size() ? &evs[(size_t)i] : nullptr; }

    void tail(const std::vector<float>& r, int64_t total, int n, float* dst) const {
        const size_t mask = r.size() - 1;
        for (int i = 0; i < n; i++) dst[i] = r[(size_t)(total - n + i) & mask];
    }

    // ---- gate
    void updateGate() {
        bool open = gateMode == 0;
        if (gateMode == 1) open = ctcssIdx >= 0 || dcsCode >= 0;
        else if (gateMode == 2) {
            if (gateKind == 0) open = ctcssIdx >= 0 && ctcssIdx == tones::ctcssIndex(gateHz);
            else open = dcsCode >= 0 && dcsCode == gateDcs && dcsInv == gateInv;
        }
        if (open) { gateHold = n8 + (int64_t)(0.4 * kRate); gateOpen = true; }
        else if (n8 > gateHold) gateOpen = false;
    }

    // ---- CTCSS
    void ctcssHop() {
        if (n1k < kCtcssWin) return;
        win.resize(kCtcssWin);
        tail(ring1k, n1k, kCtcssWin, win.data());
        double mean = 0;
        for (float v : win) mean += v;
        mean /= kCtcssWin;
        double ew = 0;
        tmp.resize(kCtcssWin);
        for (int i = 0; i < kCtcssWin; i++) { tmp[(size_t)i] = (float)((win[(size_t)i] - mean) * hann1000[(size_t)i]); ew += (double)tmp[(size_t)i] * tmp[(size_t)i]; }
        const double nf = 3.0 / kCtcssWin;   // Hann: 2 * sum(w^2) / (sum w)^2
        double p[tones::kCtcssN];
        int top = 0;
        for (int k = 0; k < tones::kCtcssN; k++) {
            p[k] = goertzel(tmp.data(), kCtcssWin, tones::ctcssHz(k), 1000.0);
            if (p[k] > p[top]) top = k;
        }
        double second = 0;
        for (int k = 0; k < tones::kCtcssN; k++)
            if (std::fabs(tones::ctcssHz(k) - tones::ctcssHz(top)) > 4.5) second = std::max(second, p[k]);
        const double amp = 4.0 * std::sqrt(p[top]) / kCtcssWin;   // tone amplitude in the audio's units (Hann window)
        for (int k = 0; k < tones::kCtcssN; k++) ctcssPow[(size_t)k] = (float)(10 * std::log10(16 * p[k] / ((double)kCtcssWin * kCtcssWin * kFull * kFull) + 1e-12));
        const bool good = dcsCode < 0 && ew > 0 && nf * p[top] / ew >= 0.12 && p[top] >= 3 * second && amp >= 25.0;
        const int cand = good ? top : -1;
        if (ctcssIdx >= 0) {
            if (cand == ctcssIdx) cMiss = 0;
            else if (++cMiss >= 8 || dcsCode >= 0) unlockCtcss();
        } else if (cand >= 0) {
            if (cand == cPend) cPendRun++; else { cPend = cand; cPendRun = 1; }
            if (cPendRun >= 4) lockCtcss(cand);
        } else cPend = -1, cPendRun = 0;
        if (ctcssIdx >= 0 && cand == ctcssIdx) {
            // the measured frequency: a fine scan around the tone
            double bf = tones::ctcssHz(ctcssIdx), bp = -1;
            for (double f = tones::ctcssHz(ctcssIdx) - 3; f <= tones::ctcssHz(ctcssIdx) + 3.001; f += 0.25) {
                const double q = goertzel(tmp.data(), kCtcssWin, f, 1000.0);
                if (q > bp) { bp = q; bf = f; }
            }
            ctcssHz = bf;
            ctcssDb = (float)(10 * std::log10(amp * amp / (kFull * kFull) + 1e-12));
            if (ToneEvent* e = ev(ctcssEv)) { e->levelDb = ctcssDb; }
        }
        updateGate();
    }
    void lockCtcss(int idx) {
        ctcssIdx = idx; ctcssHz = tones::ctcssHz(idx); cMiss = 0; cPend = -1; cPendRun = 0;
        ctcssEv = addEvent(TkCtcss, fmt("%.1f Hz", tones::ctcssHz(idx)), tones::ctcssExtended(idx) ? "extended tone" : "standard tone", ctcssDb, true);
        say(fmt("Tones: CTCSS %.1f Hz", tones::ctcssHz(idx)));
    }
    void unlockCtcss() {
        if (ToneEvent* e = ev(ctcssEv)) { e->active = false; e->durSec = std::max(0.0, now() - e->timeSec - 0.8); }
        ctcssEv = -1; ctcssIdx = -1; ctcssHz = 0; ctcssDb = -200; cMiss = 0;
    }

    // ---- DCS
    void dcsHop() {
        if (n1k < 600) return;
        if (ctcssIdx >= 0) { return; }
        const int N = 600;
        win.resize((size_t)N);
        tail(ring1k, n1k, N, win.data());
        tmp2.assign(win.begin(), win.end());
        std::sort(tmp2.begin(), tmp2.end());
        double lo = 0, hi = 0;
        for (int i = 0; i < 60; i++) { lo += tmp2[(size_t)i]; hi += tmp2[(size_t)(N - 1 - i)]; }
        lo /= 60; hi /= 60;
        const double swing = hi - lo, thr = 0.5 * (lo + hi);
        bool ok = false; tones::DcsHit best; int bestDist = 99;
        if (swing >= 80.0) {
            for (double ph = 0; ph < kDcsSps; ph += 0.62) {
                uint8_t b[kDcsBits];
                bool inRange = true;
                for (int k = 0; k < kDcsBits; k++) {
                    const double pos = (double)(N - 1) - (kDcsBits - 0.5 - k) * kDcsSps - ph;
                    if (pos < 0) { inRange = false; break; }
                    const int i0 = (int)pos; const double fr = pos - i0;
                    const double v = win[(size_t)i0] * (1 - fr) + win[(size_t)std::min(N - 1, i0 + 1)] * fr;
                    b[k] = v > thr ? 1 : 0;
                }
                if (!inRange) continue;
                uint32_t w[3] = {0, 0, 0};
                for (int r = 0; r < 3; r++) for (int j = 0; j < 23; j++) w[r] |= (uint32_t)b[r * 23 + j] << j;
                uint32_t m = 0;
                for (int j = 0; j < 23; j++) if (((w[0] >> j) & 1) + ((w[1] >> j) & 1) + ((w[2] >> j) & 1) >= 2) m |= 1u << j;
                int dist = 0, nearBy = 0;
                for (int r = 0; r < 3; r++) { const int d = __builtin_popcount(w[r] ^ m); dist += d; if (d <= 1) nearBy++; }
                if (nearBy < 2 || dist > 3) continue;
                tones::DcsHit h;
                if (tones::dcsDecode(m, h) && dist < bestDist) { best = h; bestDist = dist; ok = true; }
            }
        }
        if (ok) {
            dLastHit = n1k;
            if (best.code == dPendCode && best.inverted == dPendInv) dPendRun++; else { dPendCode = best.code; dPendInv = best.inverted; dPendRun = 1; }
            if (dPendRun >= 2 && (dcsCode != best.code || dcsInv != best.inverted)) {
                if (dcsCode >= 0) unlockDcs();
                lockDcs(best);
            }
        } else if (dcsCode >= 0 && n1k - dLastHit > 1500) unlockDcs();
        else if (dcsCode < 0 && n1k - dLastHit > 1000) dPendCode = -1, dPendRun = 0;
        updateGate();
    }
    void lockDcs(const tones::DcsHit& h) {
        dcsCode = h.code; dcsInv = h.inverted; dcsStd = h.standard;
        dcsAlias = h.aliasCode >= 0 ? tones::dcsName(h.aliasCode, h.aliasInverted) : std::string();
        std::string d = h.standard ? "standard code" : "not in the usual list of 83 codes";
        if (!dcsAlias.empty()) d += "; the same waveform reads as " + dcsAlias;
        dcsEv = addEvent(TkDcs, tones::dcsName(h.code, h.inverted), d, audioDb, true);
        // a CTCSS lock that was really the DCS data
        if (ctcssIdx >= 0) unlockCtcss();
        say("Tones: DCS " + tones::dcsName(h.code, h.inverted));
    }
    void unlockDcs() {
        if (ToneEvent* e = ev(dcsEv)) { e->active = false; e->durSec = std::max(0.0, now() - e->timeSec - 1.5); }
        dcsEv = -1; dcsCode = -1; dcsInv = false; dcsStd = false; dcsAlias.clear(); dPendCode = -1; dPendRun = 0;
    }

    // ---- DTMF
    void dtmfHop() {
        if (n8 < kDtmfN) return;
        win.resize(kDtmfN);
        tail(ring, n8, kDtmfN, win.data());
        double e = 0;
        for (float v : win) e += (double)v * v;
        int cand = -1;
        if (e / kDtmfN >= 30.0 * 30.0) {
            double pr[4], pc[4];
            int br = 0, bc = 0;
            const double nf = 2.0 / kDtmfN;
            for (int i = 0; i < 4; i++) {
                pr[i] = nf * goertzel(win.data(), kDtmfN, tones::kDtmfRows[i], kRate) / e;
                pc[i] = nf * goertzel(win.data(), kDtmfN, tones::kDtmfCols[i], kRate) / e;
                if (pr[i] > pr[br]) br = i;
                if (pc[i] > pc[bc]) bc = i;
            }
            double sr = 0, sc = 0;
            for (int i = 0; i < 4; i++) { if (i != br) sr = std::max(sr, pr[i]); if (i != bc) sc = std::max(sc, pc[i]); }
            const double twist = pc[bc] / std::max(1e-12, pr[br]);   // column against row power
            if (pr[br] + pc[bc] >= 0.6 && pr[br] >= 0.12 && pc[bc] >= 0.12 && pr[br] >= 4 * sr && pc[bc] >= 4 * sc && twist > 0.12 && twist < 8.0)
                cand = br * 4 + bc;
        }
        if (cand >= 0) {
            dtmfOtherUntil = now() + 0.15;
            dtmfGone = 0;
            if (cand == dtmfCand) dtmfRun++; else { dtmfCand = cand; dtmfRun = 1; dtmfDone = false; }
            if (dtmfRun >= 3 && !dtmfDone) {
                dtmfDone = true;
                const char key = tones::kDtmfKeys[cand];
                if (ToneEvent* ee = ev(dtmfEv); ee && now() - dtmfLastSec < 2.5) {
                    ee->text += key; ee->durSec = now() - ee->timeSec; ee->active = true;
                } else {
                    dtmfEv = addEvent(TkDtmf, std::string(1, key), fmt("%d + %d Hz", tones::kDtmfRows[cand / 4], tones::kDtmfCols[cand % 4]), audioDb, true);
                }
                dtmfString = ev(dtmfEv)->text;
                dtmfLastSec = now();
            }
        } else {
            if (dtmfCand >= 0 && ++dtmfGone >= 2) { dtmfCand = -1; dtmfRun = 0; dtmfDone = false; }
            if (dtmfEv >= 0 && now() - dtmfLastSec > 2.5) { if (ToneEvent* ee = ev(dtmfEv)) ee->active = false; dtmfEv = -1; dtmfString.clear(); }
        }
    }

    // ---- five-tone sequential
    void seqHop() {
        if (n8 < kSeqN) return;
        const std::vector<double>& pool = tones::seqPool();
        win.resize(kSeqN);
        tail(ring, n8, kSeqN, win.data());
        double e = 0;
        for (float v : win) e += (double)v * v;
        int cand = -1;
        if (now() > dtmfOtherUntil && e / kSeqN >= 30.0 * 30.0) {
            const double nf = 2.0 / kSeqN;
            double bp = 0;
            for (size_t i = 0; i < pool.size(); i++) {
                const double f = nf * goertzel(win.data(), kSeqN, pool[i], kRate) / e;
                if (f > bp) { bp = f; cand = (int)i; }
            }
            if (bp < 0.55) cand = -1;
        }
        // tracking with a two-hop confirmation of a change
        if (cand == sqCur) { sqRun++; sqPend = -2; sqPendRun = 0; }
        else if (cand == sqPend) {
            if (++sqPendRun >= 2) {
                if (sqCur >= 0 && sqRun >= 2 && sqRun <= 40) segs.push_back({sqCur, sqRun * 1000 * kSeqHop / (int)kRate + 8});
                sqCur = cand; sqRun = sqPendRun; sqPend = -2; sqPendRun = 0;
                sqSilent = 0;
            }
        } else { sqPend = cand; sqPendRun = 1; }
        if (sqRun > 40) { segs.clear(); }   // a tone of 0.3 s and more is no sequential tone (SELCAL, two-tone pages)
        if (sqCur < 0) sqSilent++; else sqSilent = 0;
        if (!segs.empty() && sqCur < 0 && sqSilent * kSeqHop * 1000 / (int)kRate > 160) closeSeq();
        else if (segs.size() > 12) { segs.clear(); }
    }
    void closeSeq() {
        std::vector<Seg> s;
        s.swap(segs);
        if (s.size() < 3) return;
        const std::vector<double>& pool = tones::seqPool();
        std::vector<int> dw;
        for (const Seg& g : s) dw.push_back(g.dwellMs);
        std::sort(dw.begin(), dw.end());
        const int med = dw[dw.size() / 2];
        struct Cand { int set; std::string digits, raw; };
        std::vector<Cand> c;
        for (int si = 0; si < tones::kSeqSetN; si++) {
            const tones::SeqSet& st = tones::kSeqSets[si];
            std::string d, raw; char prev = '?';
            bool all = true;
            for (const Seg& g : s) {
                const double f = pool[(size_t)g.pool];
                int sym = -1;
                for (int k = 0; k < 10; k++) if (std::fabs(st.digit[k] - f) < 0.5) sym = k;
                if (std::fabs(st.rep - f) < 0.5) sym = 10;
                if (sym < 0) { all = false; break; }
                if (sym == 10) { d += prev; raw += 'E'; } else { prev = (char)('0' + sym); d += prev; raw += prev; }
            }
            if (all) c.push_back({si, d, raw});
        }
        if (c.empty()) { t.blocksBad++; return; }
        // CCIR (100 ms) and EEA (40 ms) share their tones: the tone length decides; the others by closeness to their usual length
        auto score = [&](const Cand& k) { return std::fabs((double)med - tones::kSeqSets[k.set].toneMs) + (k.set == 0 ? 0 : 0.01 * k.set); };
        std::sort(c.begin(), c.end(), [&](const Cand& a, const Cand& b) { return score(a) < score(b); });
        std::string also;
        for (size_t i = 1; i < c.size(); i++) also += (also.empty() ? "" : ", ") + std::string(tones::kSeqSets[c[i].set].name);
        const tones::SeqSet& st = tones::kSeqSets[c[0].set];
        std::string detail = fmt("%s, %zu tones of about %d ms", st.name, s.size(), med);
        if (c[0].raw != c[0].digits) detail += ", sent as " + c[0].raw;
        if (!also.empty()) detail += "; the tones also fit " + also;
        addEvent(TkSeq, c[0].digits, detail, audioDb, false, (double)s.size() * med / 1000.0);
    }

    // ---- SELCAL
    void selcalHop() {
        if (n8 < kSelN) return;
        win.resize(kSelN); tmp.resize(kSelN);
        tail(ring, n8, kSelN, win.data());
        double ew = 0;
        for (int i = 0; i < kSelN; i++) { tmp[(size_t)i] = (float)(win[(size_t)i] * hann800[(size_t)i]); ew += (double)tmp[(size_t)i] * tmp[(size_t)i]; }
        int pair = -1;
        if (now() > dtmfOtherUntil && ew / kSelN >= 12.0 * 12.0) {
            const double nf = 3.0 / kSelN;
            double f[tones::kSelcalN];
            // SSB: a mistuned radio moves every tone by the same few Hz; the shift (within +-25 Hz) where the two strongest tones are strongest
            const int maxShift = !audioIn && (demod == TonesUsb || demod == TonesLsb) ? 25 : 0;
            double bestTop = -1;
            for (int d = -maxShift; d <= maxShift; d += 5) {
                double g[tones::kSelcalN], t1 = 0, t2 = 0;
                for (int k = 0; k < tones::kSelcalN; k++) {
                    g[k] = nf * goertzel(tmp.data(), kSelN, tones::selcalHz(k) + d, kRate) / ew;
                    if (g[k] > t1) { t2 = t1; t1 = g[k]; } else if (g[k] > t2) t2 = g[k];
                }
                if (t1 + t2 > bestTop) { bestTop = t1 + t2; std::copy(g, g + tones::kSelcalN, f); }
            }
            int i1 = 0;
            for (int k = 1; k < tones::kSelcalN; k++) if (f[k] > f[i1]) i1 = k;
            int i2 = i1 == 0 ? 1 : 0;
            for (int k = 0; k < tones::kSelcalN; k++) if (k != i1 && f[k] > f[i2]) i2 = k;
            double third = 0;
            for (int k = 0; k < tones::kSelcalN; k++) if (k != i1 && k != i2) third = std::max(third, f[k]);
            if (f[i1] + f[i2] >= 0.65 && f[i2] >= 0.15 && f[i2] >= 0.2 * f[i1] && third <= 0.25 * f[i2]) pair = std::min(i1, i2) * 16 + std::max(i1, i2);
        }
        if (pair == slCur) { slRun++; slPend = -2; slPendRun = 0; }
        else if (pair == slPend) {
            if (++slPendRun >= 2) {
                if (slCur >= 0 && slRun >= 11) {   // at least about 0.55 s
                    const std::string p = std::string(1, tones::selcalLetter(slCur / 16)) + tones::selcalLetter(slCur % 16);
                    const double end = now() - 0.1;
                    if (!slFirst.empty() && end - (double)slRun * 0.05 - slFirstEnd < 0.8 && p != slFirst) {
                        addEvent(TkSelcal, slFirst + "-" + p, fmt("%.1f / %.1f / %.1f / %.1f Hz", tones::selcalHz(letterIdx(slFirst[0])), tones::selcalHz(letterIdx(slFirst[1])),
                                                                  tones::selcalHz(letterIdx(p[0])), tones::selcalHz(letterIdx(p[1]))), audioDb, false, 2.2);
                        slFirst.clear();
                    } else { slFirst = p; slFirstEnd = end; }
                }
                slCur = pair; slRun = slPendRun; slPend = -2; slPendRun = 0;
            }
        } else { slPend = pair; slPendRun = 1; }
        if (!slFirst.empty() && now() - slFirstEnd > 3.0) slFirst.clear();
    }
    static int letterIdx(char c) { for (int i = 0; i < tones::kSelcalN; i++) if (tones::selcalLetter(i) == c) return i; return 0; }

    // ---- long tones: the spectrum, the strongest pure tone and the two-tone sequential detector
    void longHop() {
        if (n8 < kLongN) return;
        win.resize(kLongN);
        tail(ring, n8, kLongN, win.data());
        std::vector<std::complex<double>> a((size_t)kLongN);
        double e = 0;
        for (int i = 0; i < kLongN; i++) { a[(size_t)i] = win[(size_t)i] * hann512[(size_t)i]; e += (double)win[(size_t)i] * win[(size_t)i]; }
        fft(a);
        double pw[kLongN / 2];
        double total = 0;
        for (int k = 0; k < kLongN / 2; k++) { pw[k] = std::norm(a[(size_t)k]); if (k >= 8) total += pw[k]; }
        const double ref = (kFull * (kLongN / 2.0) / 2.0) * (kFull * (kLongN / 2.0) / 2.0);   // a 5 kHz tone with the Hann window
        for (int j = 0; j < kSpecBins; j++) {
            const double p = std::max(pw[2 * j], pw[2 * j + 1]);
            const float db = (float)(10 * std::log10(p / ref + 1e-10));
            spec[(size_t)j] = spec[(size_t)j] * 0.5f + std::max(-100.f, db) * 0.5f;
        }
        // strongest pure tone
        const bool loud = e / kLongN >= 20.0 * 20.0;
        int pk = 20;
        for (int k = 21; k < 225; k++) if (pw[k] > pw[pk]) pk = k;
        const double p3 = pw[pk - 1] + pw[pk] + pw[pk + 1];
        double f = 0;
        bool pure = loud && total > 0 && p3 / total >= 0.5 && now() > dtmfOtherUntil;
        if (pure) {
            const double l0 = std::log(pw[pk - 1] + 1e-30), l1 = std::log(pw[pk] + 1e-30), l2 = std::log(pw[pk + 1] + 1e-30);
            const double den = l0 - 2 * l1 + l2;
            const double d = den < -1e-9 ? 0.5 * (l0 - l2) / den : 0.0;
            f = (pk + std::max(-0.5, std::min(0.5, d))) * kRate / kLongN;
            toneHz = f; toneDb = (float)(10 * std::log10(16 * (p3 / 1.125) / ((double)kLongN * kLongN * kFull * kFull) + 1e-12));
        } else { toneHz = 0; toneDb = -200; }
        const double hopSec = (double)kLongHop / kRate;
        const double tnow = now();
        // tracking
        auto tol = [](double x) { return 0.015 * x + 6; };
        if (pure && ltF > 0 && std::fabs(f - ltF) <= tol(ltF)) { ltF += (f - ltF) / std::min(ltRun + 1, 8); ltRun++; ltGone = 0; ltPendRun = 0; }
        else if (pure) {
            if (ltPendRun > 0 && std::fabs(f - ltPendF) <= tol(ltPendF)) ltPendRun++; else { ltPendF = f; ltPendRun = 1; }
            if (ltF <= 0 || ltPendRun >= 2) {
                if (ltF > 0) endLong(tnow - hopSec * ltPendRun);
                ltF = f; ltRun = ltPendRun; ltStart = tnow - hopSec * (double)ltPendRun; ltPendRun = 0; ltEmitted = false; ltGone = 0;
            }
        } else {
            ltPendRun = 0;
            if (ltF > 0 && ++ltGone >= 2) endLong(tnow - hopSec * 2);
        }
        // the second tone of a pair is long enough: report it, and keep its length up to date
        if (ltF > 0 && ltRun >= 3) {
            const double dur = tnow - ltStart;
            if (!ltEmitted && dur >= 0.8 && ltLast.f > 0 && ltStart - ltLast.end < 0.3 && ltLast.dur >= 0.25 && ltLast.dur <= 2.0 && std::fabs(ltLast.f - ltF) > 0.04 * ltF) {
                ltEv = addEvent(TkTwoTone, fmt("%.1f + %.1f Hz", ltLast.f, ltF), fmt("tone A %.1f s, tone B %.1f s", ltLast.dur, dur), toneDb, true, ltLast.dur + dur);
                ltEmitted = true;
            }
            if (ltEmitted) if (ToneEvent* ee = ev(ltEv)) { ee->durSec = ltLast.dur + dur; ee->detail = fmt("tone A %.1f s, tone B %.1f s", ltLast.dur, dur); }
        }
    }
    int ltGone = 0;
    void endLong(double endSec) {
        if (ltF > 0 && ltRun >= 3) ltLast = {ltF, std::max(0.0, endSec - ltStart), endSec};
        else if (ltF > 0) ltLast = {0, 0, -1e9};
        if (ltEmitted) { if (ToneEvent* ee = ev(ltEv)) ee->active = false; ltLast = {0, 0, -1e9}; }
        ltEv = -1; ltEmitted = false; ltF = 0; ltRun = 0; ltGone = 0;
    }

    // ---- results
    void publish() {
        updateGate();
        const double tnow = now();
        for (ToneEvent* e : {ev(ctcssEv), ev(dcsEv)}) if (e && e->active) e->durSec = tnow - e->timeSec;
        if (ToneEvent* e = ev(dtmfEv)) if (e->active) e->durSec = tnow - e->timeSec;
        t.timeSec = tnow;
        t.audioInput = audioIn;
        t.demod = demod;
        const bool fm = !audioIn && demod == TonesFm;
        t.snrDb = fm ? (float)fe.snrEma : 0.f;
        t.cfoHz = fm ? fe.cfoHz() : 0;
        t.audioDb = audioDb;
        t.levelDb = audioIn ? audioDb : fm ? fe.takeLevelDb() : ssb.takeLevelDb();
        t.dataValid = tnow - lastDecodeSec < 60.0;
        const bool signal = audioDb > -45.f || (fm && fe.carrierRecent(5.0));
        t.state = t.dataValid ? 2 : (signal ? 1 : 0);
        if (t.state != lastState) {
            say(t.state == 2 ? "Tones: tone or code decoded" : t.state == 1 ? "Tones: audio, no tone" : "Tones: no signal");
            lastState = t.state;
        }
        t.blocksOk = okCount;
        for (int k = 0; k < TkKinds; k++) t.counts[k] = counts[k];
        t.ctcssIdx = ctcssIdx; t.ctcssHz = ctcssHz; t.ctcssDb = ctcssDb; t.ctcssPow = ctcssPow;
        t.dcsCode = dcsCode; t.dcsInv = dcsInv; t.dcsStd = dcsStd; t.dcsAlias = dcsAlias;
        t.toneHz = toneHz; t.toneDb = toneDb; t.spec = spec; t.dtmfString = dtmfString;
        t.gateMode = gateMode; t.gateOpen = gateOpen;
        t.events.assign(evs.rbegin(), evs.rend());
        TonesTelemetry o = t;
        std::lock_guard<std::mutex> lk(telMu);
        o.seq = ++seq;
        pub = std::move(o);
    }

    // ---- the stream
    void process8(const float* a, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const float x = a[i];
            sumsq += (double)x * x; cntsq++;
            ring[(size_t)n8 & (kRing - 1)] = x;
            n8++;
            const float y = lp2.step(lp1.step(x));
            if (++decimCnt == 8) {
                decimCnt = 0;
                ring1k[(size_t)n1k & (kRing1k - 1)] = y;
                n1k++;
                if (!selcalOnly && n1k % kDcsHop == 0) dcsHop();
                if (!selcalOnly && n1k % kCtcssHop == 0) ctcssHop();
            }
            if (!selcalOnly && n8 % kDtmfHop == 0) dtmfHop();
            if (!selcalOnly && n8 % kSeqHop == 0) seqHop();
            if (n8 % kSelHop == 0) selcalHop();
            if (!selcalOnly && n8 % kLongHop == 0) longHop();
        }
        if (cntsq >= 2000) {
            audioDb = (float)(10 * std::log10(sumsq / (double)cntsq / (kFull * kFull) + 1e-12));
            sumsq = 0; cntsq = 0;
        }
        if (n8 >= nextReport) { nextReport = n8 + 2000; publish(); }
    }

    void listenOut(const float* a, size_t n) {
        lis.resize(n);
        const float target = gateOpen ? 1.f : 0.f;
        for (size_t i = 0; i < n; i++) {
            gateGain += (target - gateGain) * 0.01f;
            lis[i] = hpListen.step(a[i] / (float)kFull) * gateGain;
        }
        listen.play(lis.data(), lis.size());
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        if (audioIn) {
            rsIn.resize(n);
            for (size_t i = 0; i < n; i++) rsIn[i] = cf32(x[i].real() * (float)kFull, 0.f);
            rsOut.clear();
            rs.process(rsIn.data(), n, rsOut);
            a8.resize(rsOut.size());
            for (size_t i = 0; i < rsOut.size(); i++) a8[i] = rsOut[i].real();
        } else if (demod != TonesFm) {
            a8 = ssb.process(x, n);
        } else {
            const std::vector<float>& disc = fe.process(x, n);
            a48.clear();
            audio48.process(disc.data(), disc.size(), a48);
            a8.clear();
            if (!a48.empty()) audio8.process(a48.data(), a48.size(), a8);
            fe.commit();
        }
        if (a8.empty()) return;
        process8(a8.data(), a8.size());
        if (listen.on.load()) listenOut(a8.data(), a8.size());
    }
};

TonesReceiver::TonesReceiver() : p_(std::make_unique<Impl>()) {}
TonesReceiver::~TonesReceiver() = default;

void TonesReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->configure(inputRateHz);
}
void TonesReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->fe.setOffset(hz);
    p_->ssb.offsetHz = hz; p_->ssb.offPhase = 0;
}
void TonesReceiver::setDemod(int d) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    d = d >= 0 && d < TonesDemodCount ? d : TonesFm;
    if (p_->demod == d) return;
    p_->demod = d;
    if (p_->fs > 0) p_->configure(p_->fs);
}
void TonesReceiver::setSelcalOnly(bool on) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->selcalOnly = on;
}
const char* tonesDemodName(int d) {
    static const char* n[] = {"FM", "USB", "LSB", "AM"};
    return d >= 0 && d < TonesDemodCount ? n[d] : "?";
}
int tonesAutoDemod(double dialHz) { return dialHz > 0 && dialHz < 30e6 ? TonesUsb : TonesFm; }
void TonesReceiver::setAudioInput(bool on) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->audioIn == on) return;
    p_->audioIn = on;
    if (p_->fs > 0) p_->configure(p_->fs);
}
bool TonesReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->procMu);
    return p_->isReady;
}
void TonesReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    if (p_->isReady) p_->resetState();
}
void TonesReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->feed(x, n);
}
bool TonesReceiver::telemetry(TonesTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void TonesReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    Impl& s = *p_;
    s.evs.clear();
    s.ctcssEv = s.dcsEv = s.dtmfEv = s.ltEv = -1;
    for (int& c : s.counts) c = 0;
    s.okCount = 0; s.t.blocksBad = 0;
    s.publish();
}
void TonesReceiver::setGate(int mode, int kind, double hz, int code, bool inv) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    Impl& s = *p_;
    s.gateMode = std::max(0, std::min(2, mode)); s.gateKind = kind; s.gateHz = hz; s.gateDcs = code; s.gateInv = inv;
    s.gateHold = 0;
    s.gateOpen = s.gateMode == 0;
    s.updateGate();
}
void TonesReceiver::setListen(bool on) { p_->listen.on = on; }
bool TonesReceiver::listening() const { return p_->listen.on.load(); }
void TonesReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void TonesReceiver::setMuted(bool m) { p_->listen.muted = m; }
void TonesReceiver::setSilent(bool s) { p_->listen.silent = s; }
void TonesReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }
void TonesReceiver::setLogCallback(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(p_->procMu);
    p_->log = std::move(cb);
}

ModeTuning tonesTuning() {
    ModeTuning t;
    t.stdMode = 91; t.id = "tones"; t.name = "Tone decoders (CTCSS, DCS, DTMF, selcall)";
    t.minMhz = 1.5; t.maxMhz = 1300; t.defMhz = 145.5;   // HF: SELCAL and the voice band decoders in USB (setDemod)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 500000;
    t.tuneOffsetHz = 50000;                  // the radio sits 50 kHz above the channel, away from its DC spike (as APRS / packet and SAME)
    return t;
}

} // namespace dect2
