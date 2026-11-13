// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ARDOP receiver (see ardop_rx.h).
#include "dect2/ardop/ardop_rx.h"
#include "dect2/ardop/ardop_proto.h"
#include "dect2/ardop/ardop_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kFs = 8000;
constexpr int kWin = 160;                 // 20 ms, one 50 baud symbol
constexpr int kHopN = 8;                  // the leader is measured every millisecond
constexpr int kRingN = 1 << 16, kMask = kRingN - 1;
constexpr int kNumHyp = 7;
constexpr double kHypStep = 15;           // trial tuning errors -45 .. +45 Hz
constexpr int kBpTaps = 191;
constexpr int kHist = 128;                // hops of two-tone history
constexpr int kHeaderSyms = 10;
constexpr int kHeaderSamples = kHeaderSyms * kWin;
constexpr double kLead1 = 1475, kLead2 = 1525;
constexpr double kMaxDist = 0.45;         // soft distance of the frame type above which the header is rejected

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// magnitude of the tone f (Hz) over len samples from the start of the window
double toneMag(const float* ring, int64_t start, int len, double f) {
    const std::complex<double> w = std::polar(1.0, -2 * kPi * f / kFs);
    std::complex<double> ph(1, 0), acc(0, 0);
    for (int n = 0; n < len; n++) {
        acc += (double)ring[(start + n) & kMask] * ph;
        ph *= w;
        if ((n & 63) == 63) ph /= std::abs(ph);
    }
    return std::abs(acc);
}

std::string printable(const std::vector<uint8_t>& p) {
    std::string s;
    for (uint8_t c : p) s += (c == '\n' || c == '\r') ? '\n' : (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    return s;
}
std::string hexOf(const std::vector<uint8_t>& p) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < p.size() && i < 64; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += b; if (i + 1 < p.size()) s += ' '; }
    return s;
}
const char* kindName(ardop::Kind k) {
    switch (k) {
    case ardop::KAck: return "ACK"; case ardop::KNak: return "NAK"; case ardop::KBreak: return "break"; case ardop::KIdle: return "idle";
    case ardop::KDisc: return "disconnect"; case ardop::KEnd: return "end"; case ardop::KConRejBusy: return "rejected (busy)";
    case ardop::KConRejBw: return "rejected (bandwidth)"; case ardop::KId: return "ID"; case ardop::KConReq: return "connect request";
    case ardop::KConAck: return "connect acknowledge"; case ardop::KPing: return "ping"; case ardop::KPingAck: return "ping acknowledge";
    case ardop::KData: return "data"; default: return "?";
    }
}
} // namespace

struct ArdopReceiver::Impl {
    enum St { Search, Dip, HeaderWait, BodyWait };
    std::mutex mu;
    std::function<void(const std::string&)> log;
    std::function<void(const ArdopFrameEvent&)> frameTap;   // setFrameTap
    std::vector<uint8_t> curPayload;                         // the payload of the frame being finished, for the tap
    double rate = 0, offsetHz = 0;
    ArdopTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::atomic<int> demodSet{HfDemodUsb};
    hfdig::Listen listen;
    std::vector<cf32> base;
    std::vector<float> aud;
    std::vector<float> bp, bpHist;
    int bpPos = 0;
    std::vector<float> ring;
    int64_t na = 0;
    double sumsq = 0, secPow = 0;
    int secN = 0;
    double t0 = 0;
    // leader search
    float cT[kNumHyp][2][kWin], sT[kNumHyp][2][kWin];
    float f2[kNumHyp][kHist] = {};
    int64_t hopCount = 0;
    St st = Search;
    int dStar = 3;
    double dipW = 0, dipWt = 0;
    int64_t trigNa = 0, dipPeakAt = 0;
    float dipPeak = 0;
    int dipN = 0;
    int64_t S = 0, skipUntil = 0, endNeed = 0;
    double dHz = 0;
    // header in progress
    ardop::FrameDef def;
    double refPow = 0;
    int lowRun = 0;
    int bodyLen = 0, symLen = 160;
    ArdopFrameLog cur;
    // results
    std::vector<ArdopFrameLog> frames;
    std::vector<ArdopStation> stations;
    std::string text;
    std::vector<float> levelHist;
    uint64_t headersOk = 0, headersBad = 0, leaders = 0, dataOk = 0, dataBad = 0, rsFix = 0, ctl = 0, other = 0;
    double lastFrameSec = -1e9;
    float lastSnr = 0;
    double lastOff = 0;
    bool changed = true;
    int64_t nIn = 0;
    double nextReport = 0, power = 0;
    int64_t nPower = 0;

    Impl() {
        const auto lp = hfdig::lowpass(kBpTaps, 300.0 / kFs, 6.0);
        bp.resize(kBpTaps);
        const double w0 = 2 * kPi * 1500.0 / kFs, c = (kBpTaps - 1) * 0.5;
        for (int k = 0; k < kBpTaps; k++) bp[(size_t)k] = (float)(2 * lp[(size_t)k] * std::cos(w0 * (k - c)));
        bpHist.assign(2 * kBpTaps, 0.f);
        ring.assign(kRingN, 0.f);
        for (int h = 0; h < kNumHyp; h++) {
            const double d = (h - kNumHyp / 2) * kHypStep;
            for (int t = 0; t < 2; t++) {
                const double f = (t ? kLead2 : kLead1) + d;
                for (int n = 0; n < kWin; n++) { cT[h][t][n] = (float)std::cos(2 * kPi * f * n / kFs); sT[h][t][n] = (float)std::sin(2 * kPi * f * n / kFs); }
            }
        }
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }
    double utcOf(int64_t sample) const { return t0 + (double)sample / kFs; }

    void resetState() {
        std::function<void(const std::string&)> cb;
        double off;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        running = curRate >= ardopTuning().minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset(); listen.reset();
        std::fill(bpHist.begin(), bpHist.end(), 0.f); bpPos = 0;
        std::fill(ring.begin(), ring.end(), 0.f);
        na = 0; sumsq = 0; secPow = 0; secN = 0;
        std::fill(&f2[0][0], &f2[0][0] + kNumHyp * kHist, 0.f);
        hopCount = 0; st = Search; skipUntil = 0;
        nIn = 0; nextReport = 0; power = 0; nPower = 0;
        t0 = nowUtc();
        lastFrameSec = -1e9;
        { std::lock_guard<std::mutex> lk(mu); pub.inputRate = curRate; }
        changed = true;
        if (cb && curRate > 0) cb(running ? "ARDOP: 4FSK frame headers at 1425 .. 1575 Hz on the upper sideband (monitor only)"
                                          : "ARDOP: the sample rate is too low (250 kHz at least)");
    }

    // ------------------------------------------------------------ the audio
    void process(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            bpHist[(size_t)bpPos] = x[i]; bpHist[(size_t)(bpPos + kBpTaps)] = x[i];
            if (++bpPos >= kBpTaps) bpPos = 0;
            const float* w = &bpHist[(size_t)bpPos];
            float y = 0;
            for (int k = 0; k < kBpTaps; k++) y += bp[(size_t)k] * w[kBpTaps - 1 - k];
            const float old = ring[(size_t)((na - kWin) & kMask)];
            ring[(size_t)(na & kMask)] = y;
            na++;
            sumsq += (double)y * y - (na > kWin ? (double)old * old : 0.0);
            secPow += (double)y * y; secN++;
            if (secN >= 8000) {
                levelHist.push_back((float)(10 * std::log10(secPow / secN + 1e-12)));
                if (levelHist.size() > kArdopHist) levelHist.erase(levelHist.begin());
                secPow = 0; secN = 0; changed = true;
            }
            if (na >= kWin && (na % kHopN) == 0) hop();
        }
    }

    void hop() {
        if (na < skipUntil) return;
        if (st == HeaderWait) { if (na >= S + kHeaderSamples + 24) header(); return; }
        if (st == BodyWait) {
            // the carrier gone for 40 ms: the frame was cut short (interference, a sender that stopped), do not wait for its nominal end
            lowRun = sumsq < 0.2 * refPow * kWin ? lowRun + 1 : 0;
            if (na >= endNeed || lowRun >= 40) body(na < endNeed);
            return;
        }
        // two-tone energy at each trial offset
        const int64_t w0 = na - kWin;
        const int hi = (int)(hopCount % kHist);
        const double sq = std::max(0.0, sumsq);
        float cur2[kNumHyp];
        for (int h = 0; h < kNumHyp; h++) {
            double e = 0;
            for (int t = 0; t < 2; t++) {
                double re = 0, im = 0;
                for (int n = 0; n < kWin; n++) { const float v = ring[(size_t)((w0 + n) & kMask)]; re += v * cT[h][t][n]; im += v * sT[h][t][n]; }
                e += (re * re + im * im) * (2.0 / kWin);
            }
            cur2[h] = sq > 1e-9 ? (float)std::min(1.5, e / sq) : 0.f;
            f2[h][hi] = cur2[h];
        }
        hopCount++;
        if (st == Search) {
            if (hopCount < 70) return;
            int bestH = -1, bestC = 0;
            double bestSum = 0;
            for (int h = 0; h < kNumHyp; h++) {
                int c = 0;
                double sum = 0;
                for (int k = 8; k <= 55; k++) {
                    const float v = f2[h][(int)((hopCount - 1 - k + 4 * kHist) % kHist)];
                    if (v >= 0.5f) c++;
                    sum += std::min(v, 1.0f);
                }
                if (c >= 25 && sum > bestSum) { bestSum = sum; bestC = c; bestH = h; }   // the trial offset nearest the leader has the most energy
            }
            if (bestH < 0) return;
            const float prev = f2[bestH][(int)((hopCount - 2 + 4 * kHist) % kHist)];
            if (!(cur2[bestH] < 0.3f && prev >= 0.3f)) return;
            st = Dip; dStar = bestH; dipW = dipWt = 0; dipN = 0; trigNa = na; dipPeak = 0; dipPeakAt = 0;
            leaders++; changed = true;
        }
        if (st == Dip) {
            // the window is exactly the sync symbol 160 samples after the reversal entered it: the two-tone energy peaks again there, and the
            // first type symbol starts at that instant. The reversal enters the window about 16 samples before the energy falls under 0.3.
            const float v = cur2[dStar];
            if (na >= trigNa + 96 && v > dipPeak) { dipPeak = v; dipPeakAt = na; }
            if (na < trigNa + 208) return;
            if (dipPeak < 0.5f) { st = Search; clearHist(); return; }   // no sync symbol: not a frame
            S = dipPeakAt;
            dHz = (dStar - kNumHyp / 2) * kHypStep;
            st = HeaderWait;
        }
    }

    // ------------------------------------------------------------ the frame type
    void tones(double base, double step, double d, int64_t start, int len, double m[4]) const {
        for (int k = 0; k < 4; k++) m[k] = toneMag(ring.data(), start, len, base + step * k + d);
    }

    void clearHist() { hopCount = 0; std::fill(&f2[0][0], &f2[0][0] + kNumHyp * kHist, 0.f); }
    void giveUp() { headersBad++; st = Search; skipUntil = na + kWin * 4; clearHist(); changed = true; }

    void header() {
        // 1. the tuning error from the continuous part of the leader (the sync symbol starts at S - 160)
        const int64_t F = S - kWin;
        double bestE = -1, bestD = dHz;
        double eAt[40];
        int nGrid = 0;
        for (double dd = -15; dd <= 15.001; dd += 1.25) {
            const double d = dHz + dd;
            const double e1 = toneMag(ring.data(), F - 480, 464, kLead1 + d), e2 = toneMag(ring.data(), F - 480, 464, kLead2 + d);
            eAt[nGrid] = e1 * e1 + e2 * e2;
            if (eAt[nGrid] > bestE) { bestE = eAt[nGrid]; bestD = d; }
            nGrid++;
        }
        {   // parabola through the peak
            int k = (int)std::lround((bestD - (dHz - 15)) / 1.25);
            if (k > 0 && k < nGrid - 1) {
                const double a = eAt[k - 1], b = eAt[k], c = eAt[k + 1], den = a - 2 * b + c;
                if (den < 0) bestD += 1.25 * 0.5 * (a - c) / den;
            }
        }
        dHz = bestD;
        // 2. the symbol timing from the ten type symbols
        auto timingMetric = [&](int sh) {
            double acc = 0;
            for (int k = 0; k < kHeaderSyms; k++) {
                double m[4];
                tones(1425, 50, dHz, S + sh + (int64_t)k * kWin, kWin, m);
                double sum = 0, mx = 0;
                for (int j = 0; j < 4; j++) { sum += m[j] * m[j]; mx = std::max(mx, m[j] * m[j]); }
                acc += sum > 0 ? mx / sum : 0;
            }
            return acc;
        };
        int bestSh = 0;
        double bestM = -1;
        for (int sh = -24; sh <= 24; sh += 3) {   // coarse: the dip centre is good to a few milliseconds
            const double v = timingMetric(sh);
            if (v > bestM + 1e-9) { bestM = v; bestSh = sh; }
        }
        {
            const int c0 = bestSh;
            for (int sh = c0 - 3; sh <= c0 + 3; sh++) {
                if (sh == c0) continue;
                const double v = timingMetric(sh);
                if (v > bestM + 1e-9) { bestM = v; bestSh = sh; }
            }
        }
        S += bestSh;
        double m[kHeaderSyms][4];
        double snrNum = 0, snrDen = 0;
        for (int k = 0; k < kHeaderSyms; k++) {
            tones(1425, 50, dHz, S + (int64_t)k * kWin, kWin, m[k]);
            double mx = 0, oth = 0;
            for (int j = 0; j < 4; j++) { const double e = m[k][j] * m[k][j]; mx = std::max(mx, e); oth += e; }
            snrNum += mx; snrDen += (oth - mx) / 3.0;
        }
        // 3. the type: smallest soft distance among the table's codes (type symbols 0-3, its parity, the second parity)
        auto dist = [&](int k, int sym) {
            double sum = 0;
            for (int j = 0; j < 4; j++) sum += m[k][j];
            return sum > 0 ? 1.0 - m[k][sym] / sum : 1.0;
        };
        int bestCode = -1;
        double bestDist = 9;
        for (int c = 0; c < 256; c++) {
            ardop::FrameDef fd;
            if (!ardop::frameDef(c, fd)) continue;
            const int par = ardop::typeParity((uint8_t)c);
            double d = dist(4, par) + dist(9, par);
            for (int k = 0; k < 4; k++) d += dist(k, (c >> (6 - 2 * k)) & 3);
            d /= 6.0;
            if (d < bestDist) { bestDist = d; bestCode = c; }
        }
        if (bestCode < 0 || bestDist > kMaxDist) { giveUp(); return; }
        int b1 = 0;
        for (int k = 0; k < 4; k++) {
            int s = 0;
            for (int j = 1; j < 4; j++) if (m[5 + k][j] > m[5 + k][s]) s = j;
            b1 = (b1 << 2) | s;
        }
        ardop::frameDef(bestCode, def);
        cur = ArdopFrameLog();
        cur.code = bestCode;
        cur.sid = bestCode ^ b1;
        cur.name = def.name;
        cur.kind = kindName(def.kind);
        cur.mod = def.mod;
        cur.baud = def.baud; cur.bandwidth = def.bandwidth;
        cur.headerOk = true;
        cur.offsetHz = dHz;
        cur.toneSnrDb = snrDen > 0 ? (float)std::max(-20.0, std::min(60.0, 10 * std::log10(snrNum / snrDen))) : 40.f;
        cur.timeSec = (double)(S - 160) / kFs;
        cur.utc = (int64_t)utcOf(S);
        if (def.kind == ardop::KAck || def.kind == ardop::KNak) cur.quality = ardop::ackQuality(bestCode);
        bodyLen = ardop::bodyLength(bestCode);
        if (bodyLen < 0) {   // PSK, QAM, 600 baud FSK: the header only; stay quiet for the frame's length
            other++;
            const int bps = !strcmp(def.mod, "4PSK") ? 2 : !strcmp(def.mod, "8PSK") ? 3 : !strcmp(def.mod, "16QAM") ? 4 : 2;
            const double bytes = 1 + def.dataBytes + 2 + def.rsBytes;
            const double secs = def.baud > 0 ? bytes * 8.0 / bps / def.baud * (def.baud == 600 ? 1.0 : 1.0) + 0.1 : 0.5;
            finish(false, S + kHeaderSamples + (int64_t)(secs * kFs));
            return;
        }
        if (bodyLen == 0) { ctl++; finish(false, S + kHeaderSamples); return; }
        symLen = (def.kind == ardop::KData && def.baud == 100) ? 80 : kWin;
        endNeed = S + kHeaderSamples + (int64_t)bodyLen * 4 * symLen + 8;
        refPow = std::max(1e-12, sumsq / kWin);
        lowRun = 0;
        st = BodyWait;
    }

    void body(bool cut) {
        if (cut) { cur.bodyRead = true; if (def.kind == ardop::KData) dataBad++; finish(true, na); return; }
        std::vector<uint8_t> by((size_t)bodyLen);
        const double base = symLen == 80 ? 1350 : 1425, step = symLen == 80 ? 100 : 50;
        int64_t pos = S + kHeaderSamples;
        for (int i = 0; i < bodyLen; i++) {
            int v = 0;
            for (int k = 0; k < 4; k++) {
                double m[4];
                tones(base, step, dHz, pos, symLen, m);
                int s = 0;
                for (int j = 1; j < 4; j++) if (m[j] > m[s]) s = j;
                v = (v << 2) | s;
                pos += symLen;
            }
            by[(size_t)i] = (uint8_t)v;
        }
        cur.bodyRead = true;
        ardop::Parsed p;
        if (ardop::parseBody(cur.code, by.data(), by.size(), p) && p.bodyOk) {
            cur.bodyOk = true; cur.rsFixed = p.rsFixed; rsFix += (uint64_t)p.rsFixed;
            cur.from = p.from; cur.to = p.to; cur.locator = p.locator;
            cur.leaderMs = p.leaderMs; cur.snrDb = p.snrDb;
            if (p.quality >= 0) cur.quality = p.quality;
            if (!p.payload.empty()) { cur.text = printable(p.payload); cur.hex = hexOf(p.payload); }
            curPayload = p.payload;
            if (def.kind == ardop::KData) {
                dataOk++;
                text += cur.text;
                if (text.size() > kArdopTextMax) text.erase(0, text.size() - kArdopTextMax);
                if (!text.empty() && text.back() != '\n') text += ' ';
            }
        } else if (def.kind == ardop::KData) dataBad++;
        finish(true, pos + kWin);
    }

    void finish(bool bodyRead, int64_t resume) {
        (void)bodyRead;
        headersOk++;
        lastFrameSec = (double)na / kFs;
        lastSnr = cur.toneSnrDb; lastOff = cur.offsetHz;
        auto upd = [&](const std::string& call, const std::string& loc, const std::string& to) {
            if (call.empty()) return;
            auto it = std::find_if(stations.begin(), stations.end(), [&](const ArdopStation& s) { return s.call == call; });
            if (it == stations.end()) {
                ArdopStation s;
                s.call = call; s.firstSeen = cur.utc;
                stations.insert(stations.begin(), s);
                it = stations.begin();
            } else if (it != stations.begin()) std::rotate(stations.begin(), it, it + 1), it = stations.begin();
            it->count++; it->lastSeen = cur.utc; it->last = cur.name;
            if (!loc.empty()) it->locator = loc;
            if (!to.empty()) it->heardTo = to;
            if (stations.size() > kArdopStationsMax) stations.pop_back();
        };
        if (cur.bodyOk) upd(cur.from, cur.locator, cur.to);
        if (frameTap && cur.headerOk) {
            ArdopFrameEvent ev;
            ev.code = cur.code; ev.sid = cur.sid; ev.bodyOk = cur.bodyOk; ev.from = cur.from; ev.to = cur.to; ev.timeSec = cur.timeSec;
            ev.payload = curPayload;
            frameTap(ev);
        }
        curPayload.clear();
        frames.insert(frames.begin(), cur);
        if (frames.size() > kArdopFramesMax) frames.pop_back();
        char b[160];
        snprintf(b, sizeof b, "ARDOP: %s%s%s%s, %.0f dB, offset %+.1f Hz", cur.name.c_str(), cur.from.empty() ? "" : " ", cur.from.c_str(), cur.bodyRead && !cur.bodyOk ? " (body unreadable)" : "", cur.toneSnrDb, cur.offsetHz);
        say(b);
        changed = true;
        st = Search; skipUntil = resume; clearHist();
    }

    void report() {
        const double secs = nIn / std::max(1.0, curRate);
        std::lock_guard<std::mutex> lk(mu);
        pub.seq++;
        pub.timeSec = secs;
        pub.levelDb = nPower ? (float)(10 * std::log10(power / nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        const bool busy = st != Search;
        pub.state = !running ? 0 : busy ? 1 : (secs - lastFrameSec < 30 ? 2 : 0);
        pub.cfoHz = lastOff; pub.snrDb = lastSnr;
        pub.dataValid = pub.state == 2;
        pub.blocksOk = headersOk; pub.blocksBad = headersBad;
        pub.leaders = leaders; pub.dataOk = dataOk; pub.dataBad = dataBad; pub.rsCorrections = rsFix; pub.controlFrames = ctl; pub.otherModulations = other;
        if (changed) { pub.frames = frames; pub.stations = stations; pub.text = text; pub.levelHist = levelHist; changed = false; }
    }
};

ArdopReceiver::ArdopReceiver() : p_(std::make_unique<Impl>()) {}
ArdopReceiver::~ArdopReceiver() = default;

void ArdopReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void ArdopReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool ArdopReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= ardopTuning().minSampleRate - 1; }
void ArdopReceiver::reset() { p_->resetReq = true; }

void ArdopReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.frames.clear(); m.stations.clear(); m.text.clear(); m.levelHist.clear(); m.changed = true; }
    if (m.curRate <= 0) return;
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
    }
    m.nPower += (int64_t)n;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.aud.clear();
        double pw = 0;
        if (const int d = hfDemodResolve(m.demodSet.load(), 0); d != m.usb.demod()) m.usb.setDemod(d);
        m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
        m.listen.play(m.aud.data(), m.aud.size());
        m.process(m.aud.data(), m.aud.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool ArdopReceiver::telemetry(ArdopTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void ArdopReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void ArdopReceiver::setFrameTap(std::function<void(const ArdopFrameEvent&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->frameTap = std::move(cb); }
void ArdopReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.frames.clear(); p_->pub.stations.clear(); p_->pub.text.clear(); p_->pub.levelHist.clear();
    p_->pub.seq++;
}
void ArdopReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    m.changed = true;
    m.report();
}
void ArdopReceiver::setListen(bool on) { p_->listen.on = on; }
void ArdopReceiver::setDemod(int d) { p_->demodSet = d; }
int ArdopReceiver::demod() const { return p_->demodSet.load(); }
bool ArdopReceiver::listening() const { return p_->listen.on.load(); }
void ArdopReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void ArdopReceiver::setMuted(bool m) { p_->listen.muted = m; }
void ArdopReceiver::setSilent(bool s) { p_->listen.silent = s; }
void ArdopReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning ardopTuning() {
    ModeTuning t;
    t.stdMode = 142; t.id = "ardop"; t.name = "ARDOP";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = 14.103;   // the digital part of 20 m (upper sideband dial); gateways publish their own frequencies
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;                              // as the other HF digital modes: the radio sits 20 kHz above the dial, away from its DC spike
    return t;
}

const ArdopPreset* ardopPresets(int& count) {
    static const ArdopPreset kP[] = {{"80 m", 3.595}, {"40 m", 7.0975}, {"30 m", 10.145}, {"20 m", 14.103}, {"17 m", 18.1}, {"15 m", 21.1}, {"10 m", 28.12}};
    count = (int)(sizeof kP / sizeof *kP);
    return kP;
}

} // namespace dect2
