// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBICab / Eurobalise up-link receiver, see ebicab_rx.h
#include "dect2/ebicab/ebicab_rx.h"
#include "dect2/ebicab/ebicab_proto.h"
#include "dect2/ebicab/ebicab_tel.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

constexpr double kTargetRate = 3.0e6;         // the decimated rate, at least
constexpr double kLowpassHz = 900e3;
constexpr double kDevHz = 0.5 * (ebicab::kFreqOneHz - ebicab::kFreqZeroHz);   // 282.5 kHz

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string bitsHex(const std::vector<uint8_t>& b, size_t maxBits) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    const size_t n = std::min(b.size(), maxBits);
    for (size_t i = 0; i < n; i += 4) {
        int v = 0; for (size_t j = 0; j < 4; j++) v = v << 1 | (i + j < n ? b[i + j] : 0);
        s += hx[v];
    }
    return s;
}

} // namespace

struct EbicabReceiver::Impl {
    std::mutex mu, telMu;
    std::function<void(const std::string&)> log;
    double fs = 0, offsetHz = 0;
    bool running = false;
    // front end
    int D = 1;
    double r2 = 0, sps = 6;
    cf32 rot{1, 0}, rotStep{1, 0};
    std::vector<float> taps;
    std::vector<cf32> hist;
    size_t hpos = 0;
    int dcount = 0;
    cf32 prev{1, 0};
    uint64_t nIn = 0, nOut = 0, nextReport = 0;
    // slicer
    std::vector<float> box;
    size_t bpos = 0;
    double bsum = 0;
    float hi = (float)kDevHz, lo = -(float)kDevHz, ph = -0.5f;
    bool lev = false, prevLev = false;
    // burst
    float pe = 0, nf = 0;
    bool nfInit = false, inBurst = false;
    int below = 0;
    uint64_t burstStartOut = 0, burstId = 0;
    std::vector<uint8_t> burstBits;
    int telegramsInBurst = 0;
    double midSum = 0; uint64_t midN = 0;
    float burstSnr = 0;
    ebicab::Stream stream;
    // results
    std::deque<EbicabTelegram> telegrams;
    std::deque<EbicabBurst> bursts;
    std::deque<EbicabBalise> balises;
    uint64_t ok = 0, bad = 0, nBursts = 0, nLong = 0, nShort = 0, nInv = 0;
    double lastTelegramSec = -1e9, lastBurstSec = -1e9, lastRate = 0, lastCfo = 0;
    uint64_t lastBurstIdForTelegram = ~0ull;
    double powAcc = 0; uint64_t powN = 0;
    float snrEma = 0;
    uint64_t seq = 0;
    EbicabTelemetry pub;

    void say(const std::string& s) { if (log) log(s); }
    double nowSec() const { return r2 > 0 ? (double)nOut / r2 : 0; }

    void rebuild() {
        running = fs >= ebicabTuning().minSampleRate - 1;
        if (!running) { if (fs > 0) say("Balise: the sample rate is too low (4 Msps at least)"); return; }
        D = std::max(1, (int)(fs / kTargetRate));
        r2 = fs / D;
        sps = r2 / ebicab::kBitRate;
        const int N = (int)(3.5 * fs / 700e3) | 1;
        taps.assign((size_t)N, 0.f);
        const double fc = kLowpassHz / fs;
        double sum = 0;
        for (int i = 0; i < N; i++) {
            const double x = i - (N - 1) / 2.0;
            const double s = x == 0 ? 2 * fc : std::sin(2 * math::kPi * fc * x) / (math::kPi * x);
            const double w = 0.54 - 0.46 * std::cos(2 * math::kPi * i / (N - 1));
            taps[(size_t)i] = (float)(s * w); sum += s * w;
        }
        for (auto& t : taps) t = (float)(t / sum);
        hist.assign((size_t)N * 2, cf32(0, 0)); hpos = 0; dcount = 0;
        const double w = -2 * math::kPi * offsetHz / fs;
        rotStep = cf32((float)std::cos(w), (float)std::sin(w)); rot = cf32(1, 0);
        box.assign((size_t)std::max(1.0, std::round(sps * 0.5)), 0.f); bpos = 0; bsum = 0;
        hi = (float)kDevHz; lo = -(float)kDevHz; ph = -0.5f; lev = prevLev = false;
        pe = nf = 0; nfInit = false; inBurst = false; below = 0;
        nIn = nOut = 0; nextReport = 0; prev = cf32(1, 0);
        stream.reset();
        say("Balise: listening for the Eurobalise up-link (FSK around 4.234 MHz, 564.48 kbit/s); EBICab formats are not decoded");
    }

    void endBurst() {
        inBurst = false;
        const size_t trim = (size_t)std::lround(below / sps);   // the bits decided while the signal was already gone
        burstBits.resize(burstBits.size() > trim ? burstBits.size() - trim : 0);
        const double dur = (double)(nOut - burstStartOut - (uint64_t)below) / r2;
        const int bits = (int)burstBits.size();
        if (telegramsInBurst == 0 && bits >= 24) {
            bad++;
            EbicabBurst b;
            b.utc = wallNow(); b.timeSec = nowSec(); b.durationUs = dur * 1e6; b.bits = bits; b.head = bitsHex(burstBits, 256); b.snrDb = burstSnr;
            if (bits >= 26 && bits <= 40) b.hint = "about the length of an EBICab 700 telegram (32 bits)";
            else if (bits >= 235 && bits <= 275) b.hint = "about the length of an EBICab 900 telegram (255 bits)";
            else if (bits >= 300 && bits <= 360) b.hint = "short Eurobalise telegram length, but no valid telegram (errors?)";
            else if (bits >= 900) b.hint = "long Eurobalise telegram length, but no valid telegram (errors?)";
            bursts.push_front(b);
            if (bursts.size() > kEbicabBurstsMax) bursts.pop_back();
        }
        if (bits >= 24 && dur > 0) lastRate = bits / dur;
        if (midN) lastCfo = midSum / (double)midN;
    }

    void onTelegram() {
        const ebicab::Telegram& t = stream.last();
        ok++;
        t.longFmt ? nLong++ : nShort++;
        if (t.inverted) nInv++;
        telegramsInBurst++;
        lastTelegramSec = nowSec();
        const int64_t utc = wallNow();
        if (!telegrams.empty() && lastBurstIdForTelegram == burstId && telegrams.front().hex == t.hex && telegrams.front().longFmt == t.longFmt) {
            telegrams.front().repeats++;
            return;
        }
        lastBurstIdForTelegram = burstId;
        EbicabTelegram e;
        e.utc = utc; e.timeSec = nowSec(); e.longFmt = t.longFmt; e.inverted = t.inverted; e.header = t.header; e.packets = t.packets; e.hex = t.hex; e.snrDb = burstSnr;
        telegrams.push_front(e);
        if (telegrams.size() > kEbicabTelegramsMax) telegrams.pop_back();
        if (t.header.valid) {
            EbicabBalise* b = nullptr;
            for (auto& x : balises) if (x.nidC == t.header.nidC && x.nidBg == t.header.nidBg && x.nPig == t.header.nPig) { b = &x; break; }
            if (!b) {
                balises.push_front(EbicabBalise());
                b = &balises.front();
                b->nidC = t.header.nidC; b->nidBg = t.header.nidBg; b->nPig = t.header.nPig; b->firstSeen = utc;
                if (balises.size() > kEbicabBalisesMax) balises.pop_back();
            }
            b->nTotal = t.header.nTotal; b->mVersion = t.header.mVersion; b->count++; b->lastSeen = utc;
        }
        say(std::string("Balise: ") + (t.longFmt ? "long" : "short") + " telegram" + (t.header.valid ? ", NID_C " + std::to_string(t.header.nidC) + " NID_BG " + std::to_string(t.header.nidBg) : ""));
    }

    void onBit(int b) {
        burstBits.push_back((uint8_t)b);
        if (stream.bit(b)) onTelegram();
    }

    void sample(const cf32 y) {
        // discriminator
        const float d = (float)(std::arg(y * std::conj(prev)) * r2 / (2 * math::kPi));
        prev = y;
        const float p = std::norm(y);
        powAcc += p; powN++;
        // burst detector
        pe += (p - pe) * 0.05f;
        if (!nfInit) { nf = pe; if (nOut > 2000) nfInit = true; }
        else if (pe < nf) nf += (pe - nf) * 0.01f;
        else nf += (pe - nf) * 1e-5f;
        const float on = 8.f * nf + 1e-12f, off = 4.f * nf + 1e-12f;
        if (!inBurst) {
            if (nfInit && pe > on) {
                inBurst = true; below = 0; burstStartOut = nOut; burstId++; telegramsInBurst = 0; burstBits.clear();
                midSum = 0; midN = 0; stream.reset();
                hi = (float)kDevHz; lo = -(float)kDevHz; ph = -0.5f; lev = prevLev = false;
                std::fill(box.begin(), box.end(), 0.f); bsum = 0;
                burstSnr = (float)(10 * std::log10(std::max(1e-3f, pe / std::max(nf, 1e-20f) - 1)));
                nBursts++; lastBurstSec = nowSec();
            }
            return;
        }
        if (pe < off) { if (++below >= (int)(30e-6 * r2)) { endBurst(); return; } } else below = 0;
        lastBurstSec = nowSec();
        burstSnr = 0.98f * burstSnr + 0.02f * (float)(10 * std::log10(std::max(1e-3f, pe / std::max(nf, 1e-20f) - 1)));
        snrEma = burstSnr;
        // slicer
        bsum += d - box[bpos]; box[bpos] = d;
        if (++bpos == box.size()) bpos = 0;
        const float v = (float)(bsum / (double)box.size());
        const float dec = (float)(1.0 / (sps * 40.0));
        if (v > hi) hi = v; else hi += (v - hi) * dec;
        if (v < lo) lo = v; else lo += (v - lo) * dec;
        hi = std::min(hi, 3.f * (float)kDevHz); lo = std::max(lo, -3.f * (float)kDevHz);
        const float mid = 0.5f * (hi + lo);
        midSum += mid; midN++;
        lev = v > mid;
        if (lev != prevLev) ph *= 0.75f;
        prevLev = lev;
        ph += (float)(1.0 / sps);
        if (ph >= 0.5f) { ph -= 1.f; onBit(lev ? 1 : 0); }
    }

    void process(const cf32* x, size_t n) {
        const size_t N = taps.size();
        for (size_t i = 0; i < n; i++) {
            const cf32 m = x[i] * rot;
            rot *= rotStep;
            hist[hpos] = m; hist[hpos + N] = m;
            if (++hpos >= N) hpos = 0;
            if (++dcount < D) continue;
            dcount = 0;
            const cf32* w = &hist[hpos];   // the last N samples, oldest first
            float ar = 0, ai = 0;
            for (size_t k = 0; k < N; k++) { ar += taps[k] * w[k].real(); ai += taps[k] * w[k].imag(); }
            nOut++;
            sample(cf32(ar, ai));
        }
        rot /= std::abs(rot);   // keep the oscillator at unit length
        nIn += n;
    }

    void publish() {
        const double t = nowSec();
        EbicabTelemetry o;
        o.timeSec = t; o.inputRate = fs;
        o.dataValid = t - lastTelegramSec < 30.0;
        o.state = o.dataValid ? 2 : (t - lastBurstSec < 30.0 ? 1 : 0);
        o.cfoHz = lastCfo; o.snrDb = snrEma;
        o.levelDb = powN ? (float)(10 * std::log10(powAcc / (double)powN + 1e-20)) : -200.f;
        powAcc = 0; powN = 0;
        o.blocksOk = ok; o.blocksBad = bad; o.bursts = nBursts; o.longTelegrams = nLong; o.shortTelegrams = nShort; o.invertedTelegrams = nInv;
        o.parityHits = stream.parityHits(); o.bitRate = lastRate;
        o.telegrams.assign(telegrams.begin(), telegrams.end());
        o.burstLog.assign(bursts.begin(), bursts.end());
        o.balises.assign(balises.begin(), balises.end());
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }
};

EbicabReceiver::EbicabReceiver() : p_(std::make_unique<Impl>()) {}
EbicabReceiver::~EbicabReceiver() = default;

void EbicabReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->fs = r; p_->rebuild(); }
void EbicabReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetHz) { p_->offsetHz = hz; if (p_->fs > 0) p_->rebuild(); } }
bool EbicabReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->running; }
void EbicabReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); if (p_->fs > 0) p_->rebuild(); }
void EbicabReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void EbicabReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.telegrams.clear(); m.bursts.clear(); m.balises.clear();
    m.ok = m.bad = m.nBursts = m.nLong = m.nShort = m.nInv = 0; m.lastTelegramSec = m.lastBurstSec = -1e9;
    m.publish();
}

void EbicabReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.running || n == 0) return;
    m.process(x, n);
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.fs;
        m.publish();
    }
}

void EbicabReceiver::flush() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->inBurst) p_->endBurst();
    p_->publish();
}

bool EbicabReceiver::telemetry(EbicabTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

ModeTuning ebicabTuning() {
    ModeTuning t;
    t.stdMode = 209; t.id = "ebicab"; t.name = "EBICab / Eurobalise";
    t.minMhz = 1; t.maxMhz = 30; t.defMhz = ebicab::kCentreHz / 1e6;
    t.sampleRate = 10e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 1.8;
    t.minSampleRate = 4e6;
    t.tuneOffsetHz = 1.0e6;   // the radio sits 1 MHz above 4.234 MHz: the up-link (+-0.9 MHz) stays clear of the DC spike
    return t;
}

} // namespace dect2
