// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ sensor monitor (see ant_rx.h).
#include "dect2/ant/ant_rx.h"
#include "dect2/results/results_io.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "dect2/common/bits.h"
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kOut = 4e6;              // demodulator rate: 4 samples per bit
constexpr int kSps = 4;
constexpr int kMean = 24 * kSps;          // the running mean the bits are sliced against: preamble and marker
constexpr int kFrameBits = ant::kFrameBytes * 8;
constexpr int kNeed = (kFrameBits + 2) * kSps + 8;   // samples after the marker a candidate waits for
constexpr int kHist = 24 * kSps + 16;     // samples kept before the oldest pending candidate (the preamble and marker)
constexpr uint32_t kSyncPlus = 0xAAA6C5u; // preamble 0xAA (the marker starts with 1 so the preamble ends with 0) then 0xA6 0xC5

int popc(uint32_t v) { return dect2::bits::popcount(v); }
}

struct AntReceiver::Impl {
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rateSet = 0, offsetSet = 0;
    std::atomic<bool> resetReq{true}, clearReq{false};
    std::atomic<double> centerHz{0};
    std::atomic<bool> anyNet{true};
    std::atomic<int> wheelMm{2096};

    double rate = 0, offset = 0;
    int64_t nIn = 0;
    // front end: mixer, low-pass FIR (computed only where an output sample needs it), linear interpolation to 4 Msps
    std::complex<double> lo{1, 0}, loStep{1, 0};
    int loCount = 0;
    int L = 1;
    std::vector<float> taps, hr, hi;
    int hp = 0;
    double step = 1;
    int64_t outK = 0;
    std::complex<float> yF{0, 0}, prevY{0, 0};
    // demodulator at 4 Msps
    float box[kSps] = {}; int boxPos = 0; float boxSum = 0;
    std::vector<float> meanBuf; int meanPos = 0; double meanSum = 0;
    float pwBuf[kMean] = {}; int pwPos = 0; double pwSum = 0, noiseFloor = 1e30;   // power over the sync window, and the quietest it has been (the noise floor)
    uint32_t reg[kSps] = {};
    std::vector<float> fb, pb;            // smoothed frequency (Hz) and power, from global index base
    int64_t base = 0, g = -1, skipUntil = 0, skipUntilPkt = 0;
    struct Cand { int64_t at; bool plus; };   // at: the sample index of the last marker bit; plus: the ANT+ marker matched (else a preamble only)
    std::deque<Cand> cands;
    // results
    AntTelemetry tel, pub;
    std::unordered_map<uint32_t, size_t> devIdx;
    std::unordered_map<uint32_t, double> devThr;   // per sensor: the decision threshold (its carrier error) of its recent frames
    double power = 0; int64_t nPower = 0;
    double nextReport = 0, lastGood = -100;
    double snrAvg = 0;

    double sigSec() const { return rate > 0 ? nIn / rate : 0; }

    void initFront() {
        lo = 1; loCount = 0;
        loStep = rate > 0 ? std::polar(1.0, -2 * kPi * offset / rate) : std::complex<double>(1, 0);
        step = rate / kOut;
        L = std::max(15, (int)std::ceil(6.5 * rate / 1e6)) | 1;
        taps.assign((size_t)L, 0.f);
        const double fc = 650e3 / rate;   // the GFSK main lobe passes, the noise beyond it does not
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (L - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += s * w;
        }
        for (float& t : taps) t = (float)(t / sum);
        std::reverse(taps.begin(), taps.end());
        hr.assign((size_t)L * 2, 0.f); hi.assign((size_t)L * 2, 0.f); hp = 0;
        outK = 0; nIn = 0;
        yF = prevY = std::complex<float>(0, 0);
    }

    void resetDemod() {
        for (float& b : box) b = 0;
        boxPos = 0; boxSum = 0;
        meanBuf.assign(kMean, 0.f); meanPos = 0; meanSum = 0;
        for (float& p : pwBuf) p = 0;
        pwPos = 0; pwSum = 0; noiseFloor = 1e30;
        for (uint32_t& r : reg) r = 0;
        fb.clear(); pb.clear(); base = 0; g = -1; skipUntil = 0; skipUntilPkt = 0;
        cands.clear();
    }

    bool resClear = false, resLoad = false;
    AntTelemetry loaded;
    double loadedAt = 0;
    static void moveLists(AntTelemetry& to, AntTelemetry& from) {
        to.devices = std::move(from.devices); to.log = std::move(from.log);
        from.devices.clear(); from.log.clear();
    }
    static void shiftTimes(AntTelemetry& t, double dt) {
        for (auto& d : t.devices) { d.firstSec += dt; d.lastSec += dt; for (auto& s : d.history) s.sec += dt; }
        for (auto& p : t.log) p.sec += dt;
    }
    void takeRequests() {
        std::lock_guard<std::mutex> lk(mu);
        if (!resClear && !resLoad) return;
        if (resClear) { AntTelemetry e; moveLists(tel, e); resClear = false; }
        if (resLoad) { shiftTimes(loaded, tel.timeSec - loadedAt); moveLists(tel, loaded); resLoad = false; }
        reindex();
    }
    void resetState() {
        { std::lock_guard<std::mutex> lk(mu); rate = rateSet; offset = offsetSet; }
        initFront();
        resetDemod();
        power = 0; nPower = 0; nextReport = 0.25 * rate; lastGood = -100;
        tel.inputRate = rate;
        shiftTimes(tel, -tel.timeSec);
        tel.timeSec = 0;
    }
    void clearAll() {
        const uint64_t seq = tel.seq;
        tel = AntTelemetry(); tel.seq = seq; tel.inputRate = rate;
        devIdx.clear(); devThr.clear();
        snrAvg = 0;
    }

    // ---------------------------------------------------------------- front end

    void input(float re, float im) {
        // the two complex products written out (the same bits as std::complex<double>, without its NaN-recovery call)
        const double lr = lo.real(), li = lo.imag(), sr = loStep.real(), si = loStep.imag();
        const std::complex<double> v((double)re * lr - (double)im * li, (double)re * li + (double)im * lr);
        lo = std::complex<double>(lr * sr - li * si, lr * si + li * sr);
        if (++loCount >= 4096) { lo /= std::abs(lo); loCount = 0; }
        hr[(size_t)hp] = hr[(size_t)(hp + L)] = (float)v.real();
        hi[(size_t)hp] = hi[(size_t)(hp + L)] = (float)v.imag();
        hp = hp + 1 == L ? 0 : hp + 1;
        const int64_t i = nIn;
        double t = outK * step;
        int64_t f = (int64_t)std::floor(t);
        if (i != f && i != f + 1) return;
        float o[2];
        genutil::dot2(taps.data(), hr.data() + hp, hi.data() + hp, L, o);
        const std::complex<float> y(o[0], o[1]);
        for (;;) {
            t = outK * step; f = (int64_t)std::floor(t);
            const float fr = (float)(t - (double)f);
            if (i == f) {
                yF = y;
                if (fr < 1e-6f) { outK++; demod(y); continue; }
                break;
            }
            if (i == f + 1) { outK++; demod(yF * (1 - fr) + y * fr); continue; }
            break;
        }
    }

    // ---------------------------------------------------------------- demodulator

    __attribute__((always_inline)) void demod(std::complex<float> y) {   // inlined: a call per sample stored the two floats and loaded them as one 8-byte value (store-forwarding stall)
        const std::complex<float> z = y * std::conj(prevY);
        prevY = y;
        const float fHz = (float)(std::atan2(z.imag(), z.real()) * (kOut / (2 * kPi)));
        boxSum += fHz - box[boxPos]; box[boxPos] = fHz; boxPos = (boxPos + 1) % kSps;
        const float s = boxSum / kSps;
        meanSum += s - meanBuf[(size_t)meanPos]; meanBuf[(size_t)meanPos] = s; meanPos = (meanPos + 1) % kMean;
        const float pw = std::norm(y);
        pwSum += pw - pwBuf[pwPos]; pwBuf[pwPos] = pw; pwPos = (pwPos + 1) % kMean;
        const double pAvg = pwSum / kMean;
        if (g < kMean) {}   // the window is not full yet: its average says nothing
        else if (pAvg < noiseFloor) noiseFloor = pAvg;
        else noiseFloor += (pAvg - noiseFloor) * 2e-7;   // creeps up so that a louder band later is not stuck below
        fb.push_back(s);
        pb.push_back(pw);
        g++;
        const int ph = (int)(g & (kSps - 1));
        const uint32_t bit = s > (float)(meanSum / kMean) ? 1u : 0u;
        reg[ph] = (reg[ph] << 1) | bit;   // bits arrive most significant first
        // a sync pattern only counts with a signal under it: in the gaps the discriminator output is noise, and 24 random bits match often enough
        if (g >= skipUntil && g >= kMean && pwSum / kMean > 6.0 * noiseFloor) {
            const uint32_t last24 = reg[ph] & 0xFFFFFFu;
            // the ANT+ sync (preamble and marker): correlation of the 24 smoothed bit values with the known pattern. A hard decision per bit is not
            // enough: the Gaussian filter leaves a few of these bits wrong at some sample phases, and the frame would be missed.
            float v[24]; float m = 0;
            for (int k = 0; k < 24; k++) { v[k] = at(g - (int64_t)(23 - k) * kSps); m += v[k]; }
            m /= 24;
            float dot = 0, en = 0;
            for (int k = 0; k < 24; k++) { const float d = v[k] - m; dot += (kSyncPlus >> (23 - k) & 1) ? d : -d; en += std::fabs(d); }
            if (en > 24 * 20e3f && dot > 0.72f * en) { cands.push_back({g, true}); skipUntil = g + kSps; tel.preambleHits++; }
            else if (anyNet.load(std::memory_order_relaxed)) {
                // another network: a clean alternating run of 9 bits at the start of the 24 (the 8 preamble bits and the marker's first bit, which the
                // preamble is chosen to alternate with; the hit comes when the marker's 16 bits are in). Soft again: correlation with an alternating pattern of either polarity. The CRC decides later.
                float w9[9]; float m9 = 0;
                for (int k = 0; k < 9; k++) { w9[k] = v[k]; m9 += w9[k]; }
                m9 /= 9;
                float d9 = 0, e9 = 0;
                for (int k = 0; k < 9; k++) { const float d = w9[k] - m9; d9 += (k & 1) ? d : -d; e9 += std::fabs(d); }
                if (e9 > 9 * 40e3f && std::fabs(d9) > 0.8f * e9) { cands.push_back({g, false}); skipUntil = g + kSps; }
            }
        }
    }

    static uint32_t key(const ant::Frame& f) { return (uint32_t)f.deviceNumber | (uint32_t)f.type() << 16 | (uint32_t)f.transType << 24; }

    float at(int64_t k) const { return fb[(size_t)(k - base)]; }

    void process() {
        const int64_t end = base + (int64_t)fb.size();
        while (!cands.empty() && cands.front().at + kNeed < end) {
            const Cand c = cands.front();
            cands.pop_front();
            if (c.at < skipUntilPkt) continue;
            decode(c);
        }
        const int64_t keepFrom = (cands.empty() ? end : cands.front().at) - kHist;
        if (keepFrom - base > 65536) {
            const size_t drop = (size_t)(keepFrom - base);
            fb.erase(fb.begin(), fb.begin() + (ptrdiff_t)drop);
            pb.erase(pb.begin(), pb.begin() + (ptrdiff_t)drop);
            base += (int64_t)drop;
        }
    }

    void decode(const Cand& cd) {
        // the known 24 bits: the preamble and marker as sliced (for a foreign network the marker bits come from the hard decision)
        uint32_t known = kSyncPlus;
        if (!cd.plus) {
            // re-read the 24 bits at the candidate's phase with the running-mean threshold they were found with: the preamble is known,
            // the marker is whatever was sliced
            known = 0;
            double m = 0; int n = 0;
            for (int k = 0; k < 24; k++) { const int64_t p = cd.at - (23 - k) * kSps; if (p >= base) { m += at(p); n++; } }
            m /= std::max(1, n);
            for (int k = 0; k < 24; k++) { const int64_t p = cd.at - (23 - k) * kSps; known = known << 1 | (p >= base && at(p) > m ? 1u : 0u); }
        }
        // the sample phase with the widest eye over the known bits
        int64_t best = -1; double bestEye = -1e30, m1b = 0, m0b = 0;
        for (int d = -3; d <= 3; d++) {
            const int64_t c = cd.at + d;
            if (c - 23 * kSps < base) continue;
            double m1 = 0, m0 = 0; int n1 = 0, n0 = 0;
            for (int k = 0; k < 24; k++) {
                const float v = at(c - (23 - k) * kSps);
                if (known >> (23 - k) & 1) { m1 += v; n1++; } else { m0 += v; n0++; }
            }
            if (!n1 || !n0) continue;
            m1 /= n1; m0 /= n0;
            if (m1 - m0 > bestEye) { bestEye = m1 - m0; best = c; m1b = m1; m0b = m0; }
        }
        if (best < 0 || bestEye < 2 * 50e3) return;   // no eye: noise that happened to match
        const double thr = (m1b + m0b) / 2, dev = (m1b - m0b) / 2;
        double var = 0;
        for (int k = 0; k < 24; k++) {
            const double v = at(best - (23 - k) * kSps) - (known >> (23 - k) & 1 ? m1b : m0b);
            var += v * v;
        }
        var /= 24;
        // the frame: the marker again (its two bytes were among the known bits) and the 15 bytes after it
        uint8_t raw[ant::kFrameBytes];
        int64_t markerStart = best - 15 * kSps;   // the first marker bit
        ant::Frame f;
        bool lenOk = false;
        // The sync match tolerates one wrong bit, so the first hit can sit a bit off: try the alignment as found, then one bit later and earlier,
        // and keep the one whose CRC passes.
        for (int shift : {0, 1, -1}) {
            const int64_t ms = best - 15 * kSps + (int64_t)shift * kSps;
            if (ms - 8 * kSps < base) continue;
            for (int j = 0; j < ant::kFrameBytes; j++) {
                uint8_t v = 0;
                for (int b = 0; b < 8; b++) if (at(ms + (int64_t)(8 * j + b) * kSps) > thr) v |= (uint8_t)(0x80 >> b);
                raw[j] = v;
            }
            ant::Frame t;
            const bool ok = ant::parseFrame(raw, t);
            if (shift == 0 || (ok && t.crcOk)) { f = t; lenOk = ok; markerStart = ms; }
            if (ok && t.crcOk) break;
        }
        if (!lenOk) return;
        if (f.crcOk && cd.plus && !f.antPlus()) return;                                  // a sync-like pattern in another network's bits
        if (f.crcOk && !f.antPlus() && !anyNet.load(std::memory_order_relaxed)) return;   // only ANT+ wanted
        if (!f.crcOk) { if (cd.plus) tel.blocksBad++; return; }
        if (!cd.plus && !f.antPlus() && f.marker == 0) return;   // a frame of zeros with a passing CRC does not happen; guard anyway
        const int64_t pEnd = markerStart + (int64_t)kFrameBits * kSps;
        skipUntilPkt = pEnd;
        skipUntil = std::max(skipUntil, pEnd);
        double pw = 0; int np = 0;
        for (int64_t k = std::max(base, markerStart - 8 * kSps); k < pEnd && k < base + (int64_t)pb.size(); k++) { pw += pb[(size_t)(k - base)]; np++; }
        const float rssi = (float)(10 * std::log10(pw / std::max(1, np) + 1e-20));
        const double snr = var > 0 ? 10 * std::log10(dev * dev / var) : 40;
        snrAvg = tel.blocksOk == 0 ? snr : 0.8 * snrAvg + 0.2 * snr;
        {   // each sensor has its own carrier error: the receiver reports the median over the sensors heard
            devThr[key(f)] = 0.7 * (devThr.count(key(f)) ? devThr[key(f)] : thr) + 0.3 * thr;
            std::vector<double> t; for (const auto& kv : devThr) t.push_back(kv.second);
            std::sort(t.begin(), t.end());
            tel.cfoHz = t.size() % 2 ? t[t.size() / 2] : 0.5 * (t[t.size() / 2 - 1] + t[t.size() / 2]);
        }
        tel.deviationHz = (float)dev;
        accept(f, rssi, (double)markerStart / kOut);
    }


    void accept(const ant::Frame& f, float rssi, double sec) {
        tel.blocksOk++;
        lastGood = sigSec();
        const int64_t wall = (int64_t)time(nullptr);
        const ant::Reading r = ant::decodePayload(f.type(), f.payload);
        AntFrameLog p;
        p.wall = wall; p.sec = sec; p.deviceNumber = f.deviceNumber; p.type = f.type(); p.transType = f.transType; p.marker = f.marker;
        p.rssi = rssi; memcpy(p.payload, f.payload, ant::kPayload); p.text = r.pageName + ": " + r.text;
        tel.log.insert(tel.log.begin(), std::move(p));
        if (tel.log.size() > kAntLogMax) tel.log.pop_back();
        AntDevice& d = device(f);
        d.frames++;
        if (d.lastSec > 0 && sec - d.lastSec > 0.05 && sec - d.lastSec < 5) d.periodSec = d.periodSec > 0 ? 0.9 * d.periodSec + 0.1 * (sec - d.lastSec) : sec - d.lastSec;
        d.rssi = rssi; d.lastSec = sec; d.lastWall = wall;
        if (f.pairing()) d.pairing = true;
        if (r.page >= 0 && r.page < 32) d.pagesSeen |= 1u << r.page;
        d.lastPage = r.pageName; d.lastText = r.text;
        if (r.manufacturer >= 0) { const char* m = ant::manufacturerName(r.manufacturer); d.manufacturer = m[0] ? m : ("manufacturer " + std::to_string(r.manufacturer)); }
        if (r.serial >= 0) d.serial = r.serial;
        if (r.model >= 0) d.model = r.model;
        if (r.hwRev >= 0) d.hwRev = r.hwRev;
        if (r.swRev >= 0) d.swRev = r.swRev;
        if (r.batteryStatus >= 0) d.batteryStatus = r.batteryStatus;
        if (r.batteryPct >= 0) d.batteryPct = r.batteryPct;
        if (r.batteryV >= 0) d.batteryV = r.batteryV;
        if (r.operatingTimeSec >= 0) d.operatingTimeSec = r.operatingTimeSec;
        float value = NAN;
        switch (f.type()) {
        case ant::HeartRate:
            d.historyUnit = "bpm";
            if (r.heartRate > 0) { d.heartRate = r.heartRate; value = (float)r.heartRate; }
            if (r.beatCount >= 0) d.beatCount = r.beatCount;
            if (r.rrIntervalSec > 0) d.rrSec = r.rrIntervalSec;
            break;
        case ant::BikePower:
            d.historyUnit = "W";
            if (r.instPowerW >= 0) { d.powerW = r.instPowerW; value = (float)r.instPowerW; }
            if (r.instCadenceRpm >= 0) d.cadenceRpm = r.instCadenceRpm;
            break;
        case ant::BikeSpeedCadence: case ant::BikeSpeed: case ant::BikeCadence: {
            d.historyUnit = "km/h";
            if (r.cadenceEventTime >= 0 && d.lastCadTime >= 0) {
                int dt = r.cadenceEventTime - d.lastCadTime, dr = r.cadenceRevs - d.lastCadRevs;
                if (dt < 0) dt += 65536;
                if (dr < 0) dr += 65536;
                if (dt > 0 && dr > 0 && dr < 50) d.cadenceRpm = (int)std::lround(dr * 60.0 * 1024 / dt);
            }
            if (r.speedEventTime >= 0 && d.lastSpdTime >= 0) {
                int dt = r.speedEventTime - d.lastSpdTime, dr = r.speedRevs - d.lastSpdRevs;
                if (dt < 0) dt += 65536;
                if (dr < 0) dr += 65536;
                if (dt > 0 && dr > 0 && dr < 200) { d.speedKmh = dr * (wheelMm.load() / 1000.0) * 1024 / dt * 3.6; value = (float)d.speedKmh; }
            }
            if (r.cadenceEventTime >= 0) { d.lastCadTime = r.cadenceEventTime; d.lastCadRevs = r.cadenceRevs; }
            if (r.speedEventTime >= 0) { d.lastSpdTime = r.speedEventTime; d.lastSpdRevs = r.speedRevs; }
            if (f.type() == ant::BikeCadence) { d.historyUnit = "rpm"; if (d.cadenceRpm >= 0) value = (float)d.cadenceRpm; }
            break;
        }
        case ant::StrideSdm:
            d.historyUnit = "km/h";
            if (r.sdmSpeedMs >= 0) { d.speedKmh = r.sdmSpeedMs * 3.6; value = (float)d.speedKmh; }
            if (r.sdmDistanceM >= 0) d.distanceM = r.sdmDistanceM;
            if (r.sdmCadenceSpm >= 0) d.cadenceRpm = r.sdmCadenceSpm;
            break;
        case ant::Environment:
            d.historyUnit = "C";
            if (r.temperatureC > -999) { d.temperatureC = r.temperatureC; value = (float)r.temperatureC; }
            break;
        default: break;
        }
        if (std::isfinite(value)) {
            d.history.push_back({sec, value});
            if (d.history.size() > kAntHrHist) d.history.erase(d.history.begin(), d.history.begin() + (ptrdiff_t)(d.history.size() - kAntHrHist));
        }
    }

    AntDevice& device(const ant::Frame& f) {
        const uint32_t k = key(f);
        auto it = devIdx.find(k);
        if (it != devIdx.end()) return tel.devices[it->second];
        if (tel.devices.size() >= kAntDevMax) {
            size_t old = 0;
            for (size_t i = 1; i < tel.devices.size(); i++) if (tel.devices[i].lastSec < tel.devices[old].lastSec) old = i;
            tel.devices.erase(tel.devices.begin() + (ptrdiff_t)old);
            reindex();
        }
        AntDevice d;
        d.deviceNumber = f.deviceNumber; d.type = f.type(); d.transType = f.transType; d.marker = f.marker;
        d.typeName = ant::deviceTypeName(f.type());
        d.firstSec = sigSec(); d.firstWall = (int64_t)time(nullptr);
        tel.devices.push_back(std::move(d));
        devIdx[k] = tel.devices.size() - 1;
        if (log && tel.devices.size() <= 20) {
            char b[120];
            snprintf(b, sizeof b, "ANT+: new sensor %u, %s%s", f.deviceNumber, ant::deviceTypeName(f.type()), f.antPlus() ? "" : " (another ANT network)");
            log(b);
        }
        return tel.devices.back();
    }

    void reindex() {
        devIdx.clear(); devThr.clear();
        for (size_t i = 0; i < tel.devices.size(); i++) {
            const AntDevice& d = tel.devices[i];
            devIdx[(uint32_t)d.deviceNumber | (uint32_t)d.type << 16 | (uint32_t)d.transType << 24] = i;
        }
    }

    void report() {
        tel.seq++;
        tel.timeSec = sigSec();
        tel.levelDb = nPower ? (float)(10 * std::log10(power / (double)nPower + 1e-20)) : -200.f;
        power = 0; nPower = 0;
        const double c = centerHz.load();
        tel.channelMhz = c > 0 ? (c + offset) / 1e6 : 0;
        tel.snrDb = (float)snrAvg;
        tel.state = sigSec() - lastGood < 5 ? 1 : 0;
        tel.dataValid = tel.state >= 1;
        AntTelemetry out = tel;
        std::stable_sort(out.devices.begin(), out.devices.end(), [](const AntDevice& x, const AntDevice& y) { return x.lastSec > y.lastSec; });
        std::lock_guard<std::mutex> lk(mu);
        pub = std::move(out);
    }
};

AntReceiver::AntReceiver() : p_(std::make_unique<Impl>()) {}
AntReceiver::~AntReceiver() = default;

void AntReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rateSet = r; p_->resetReq = true; }
void AntReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetSet) { p_->offsetSet = hz; p_->resetReq = true; } }
void AntReceiver::setCenterHz(double hz) { p_->centerHz = hz; }
bool AntReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rateSet >= antTuning().minSampleRate - 1; }
void AntReceiver::reset() { p_->resetReq = true; }
void AntReceiver::setAnyNetwork(bool on) { p_->anyNet = on; }
void AntReceiver::setWheelMm(int mm) { p_->wheelMm = std::max(500, std::min(3000, mm)); }
void AntReceiver::clearResults() {
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        AntTelemetry e;
        Impl::moveLists(p_->pub, e);
        p_->resLoad = false;
        p_->pub.seq++;
    }
    p_->clearReq = true;
}

void AntReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) m.resetState();
    if (m.clearReq.exchange(false)) m.clearAll();
    m.takeRequests();
    if (m.rate < antTuning().minSampleRate - 1 || n == 0) {
        if (m.rate > 0) { m.nIn += (int64_t)n; if ((double)m.nIn >= m.nextReport) { m.nextReport += 0.25 * m.rate; m.report(); } }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) re = im = 0;
        m.power += (double)re * re + (double)im * im;
        m.input(re, im);
        m.nIn++;
        if ((double)m.nIn >= m.nextReport) { m.process(); m.nextReport += 0.25 * m.rate; m.report(); }
    }
    m.nPower += (int64_t)n;
    m.process();
}

std::string AntReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->pub.timeSec, p_->pub.devices, p_->pub.log);
}
bool AntReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    AntTelemetry l;
    if (!results::unpack(blob, t0, l.devices, l.log)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::shiftTimes(l, p_->pub.timeSec - t0);
    AntTelemetry c = l;
    Impl::moveLists(p_->pub, c);
    Impl::moveLists(p_->loaded, l);
    p_->loadedAt = p_->pub.timeSec;
    p_->resLoad = true;
    p_->pub.seq++;
    return true;
}
bool AntReceiver::telemetry(AntTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void AntReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

ModeTuning antTuning() {
    ModeTuning t;
    t.stdMode = 79; t.id = "ant"; t.name = "ANT+ sensors";
    t.minMhz = 2400; t.maxMhz = 2524; t.defMhz = ant::kChannelMhz;
    t.sampleRate = 4000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 2;
    t.minSampleRate = 4000000;
    t.tuneOffsetHz = 1000000;                // the radio sits 1 MHz above the channel, away from its DC spike
    return t;
}

} // namespace dect2
