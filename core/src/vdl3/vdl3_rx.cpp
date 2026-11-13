// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VDL Mode 3 receiver: see vdl3_rx.h.
#include "dect2/vdl3/vdl3_rx.h"
#include "dect2/vdl3/vdl3_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <climits>
#include <ctime>
#include <mutex>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using cd = std::complex<double>;
using dect2::math::kPi;
constexpr int kSps = 8;                       // samples per symbol after the filter (84 kHz)
constexpr int kDump = 16;                     // integrate and dump to 16 samples per symbol (168 kHz), then the FIR and every 2nd sample
constexpr int kTaps = 63;
constexpr double kCutHz = 13500;
constexpr double kSyncThr = 0.8;              // normalised differential correlation over the 15 phase changes (random D8PSK symbols
                                              // pass 0.6 several hundred times a second; 0.8 a few times, and those are masked by the burst around them)
constexpr int kWin = 12;                      // samples after the first crossing in which the peak is looked for
constexpr int kMaxPayload = 200;
constexpr double kEvmMax = 0.3;               // rms phase error of the decisions, in symbol steps (pi/4): above this the burst is noise
constexpr int kFloorBlock = 128, kFloorBlocks = 200;   // noise floor: the quietest 128-sample block of the last 0.3 s
constexpr double kNoiseBwRatio = 27000.0 / kVdl3SymbolRate;   // the filter's noise bandwidth over the symbol rate (SNR as Es/N0)

struct Pend { int64_t syncEnd = 0; double mu = 0; cd D; int fam = 0; double m = 0; };

std::vector<double> firTaps() {
    std::vector<double> h(kTaps);
    const double fc = kCutHz / (kDump * kVdl3SymbolRate);
    double s = 0;
    for (int i = 0; i < kTaps; i++) {
        const double x = i - (kTaps - 1) / 2.0;
        const double sinc = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (kTaps - 1));
        h[(size_t)i] = sinc * w;
        s += h[(size_t)i];
    }
    for (auto& v : h) v /= s;
    return h;
}
} // namespace

struct Vdl3Receiver::Impl {
    std::mutex mu;
    double rate = 0, offset = 0, center = 0;
    bool rebuild = true;
    std::function<void(const std::string&)> logCb;
    cd rot = 1, step = 1;
    int renorm = 0;
    cd acc = 0;
    double accN = 0, phase = 0;
    std::vector<double> h = firTaps();
    std::vector<cd> hist = std::vector<cd>(2 * kTaps, cd(0, 0));
    int hpos = 0, dec2 = 0;
    std::vector<cd> z;                        // 8 samples per symbol
    int64_t zBase = 0;                        // absolute index of z[0]
    double floor = 0;
    int winLeft = 0;
    double bestM = 0;
    int64_t bestIdx = 0;
    cd bestD;
    int bestFam = 0;
    int64_t busyUntil = -1;                   // the last symbol of the last good burst: training sequences starting inside it are its own data
    double blkSum = 0;
    int blkN = 0;
    std::vector<double> blkMeans;
    std::vector<Pend> pend;
    std::vector<std::pair<int64_t, double>> syncs;   // recent training ends (absolute) and their correlation, for the S2* length
    uint64_t nIn = 0, serial = 0, seq = 0;
    double lastGood = -1e9;
    Vdl3Telemetry tel;

    void log(const std::string& s) { if (logCb) logCb(s); }

    void doRebuild() {
        rebuild = false;
        const double w = -2 * kPi * offset / (rate > 0 ? rate : 1);
        step = cd(std::cos(w), std::sin(w));
        rot = 1;
        acc = 0; accN = 0; phase = 0;
    }

    cd zAt(double rel) const {                // linear interpolation, rel relative to z[0]
        if (rel < 0) rel = 0;
        const size_t i = (size_t)rel;
        if (i + 1 >= z.size()) return z.empty() ? cd(0, 0) : z.back();
        const double f = rel - (double)i;
        return z[i] * (1 - f) + z[i + 1] * f;
    }

    // differential correlation of the training ending at relative index r against S1 (fam 0) or S2 (fam 1)
    double corr(int64_t r, int fam, cd& D) const {
        D = 0;
        if (r < kSps * (kVdl3SyncSyms - 1) || r >= (int64_t)z.size()) return 0;
        const auto& P = vdl3SyncPhases(fam ? Vdl3Sync::S2 : Vdl3Sync::S1);
        double E = 0;
        for (int q = 0; q < kVdl3SyncSyms; q++) E += std::norm(z[(size_t)(r - kSps * (kVdl3SyncSyms - 1 - q))]);
        for (int q = 0; q + 1 < kVdl3SyncSyms; q++) {
            const cd a = z[(size_t)(r - kSps * (kVdl3SyncSyms - 1 - q))], b = z[(size_t)(r - kSps * (kVdl3SyncSyms - 2 - q))];
            const double dp = (P[(size_t)q + 1] - P[(size_t)q]) * kPi / 4;
            D += b * std::conj(a) * cd(std::cos(dp), -std::sin(dp));
        }
        return E > 0 ? std::abs(D) / (E * (kVdl3SyncSyms - 1) / kVdl3SyncSyms) : 0;
    }

    void decode(const Pend& p) {
        const bool neg = p.D.real() < 0;
        const Vdl3Sync sy = p.fam ? (neg ? Vdl3Sync::S2Star : Vdl3Sync::S2) : (neg ? Vdl3Sync::S1Star : Vdl3Sync::S1);
        const cd Ds = neg ? -p.D : p.D;
        const double fs = std::arg(Ds) / (2 * kPi);   // cycles per symbol
        const auto& P = vdl3SyncPhases(sy);
        const int lmax = sy == Vdl3Sync::S2 ? kMaxPayload : sy == Vdl3Sync::S2Star ? 128 : 16;
        const int nsym = kVdl3SyncSyms + lmax + 8;
        const double r0 = (double)(p.syncEnd - zBase) - kSps * (kVdl3SyncSyms - 1) + p.mu;
        std::vector<cd> s((size_t)nsym);
        for (int j = 0; j < nsym; j++) s[(size_t)j] = zAt(r0 + kSps * j) * std::polar(1.0, -2 * kPi * fs * j);
        cd ref = 0;
        double eTr = 0;
        for (int q = 0; q < kVdl3SyncSyms; q++) { ref += s[(size_t)q] * std::polar(1.0, -P[(size_t)q] * kPi / 4); eTr += std::norm(s[(size_t)q]); }
        eTr /= kVdl3SyncSyms;
        const double ph0 = std::arg(ref);
        // decision-directed loop over the training (known phases) and the payload
        double th = 0, om = 0;
        std::vector<int> d((size_t)nsym, 0);
        std::vector<double> err((size_t)nsym, 0);
        for (int j = 0; j < nsym; j++) {
            const cd r = s[(size_t)j] * std::polar(1.0, -(ph0 + th));
            int dj;
            if (j < kVdl3SyncSyms) dj = P[(size_t)j];
            else dj = ((int)std::lround(std::arg(r) / (kPi / 4)) % 8 + 8) % 8;
            const double e = std::arg(r * std::polar(1.0, -dj * kPi / 4));
            d[(size_t)j] = dj;
            err[(size_t)j] = e;
            th += om + 0.2 * e;
            om += 0.02 * e;
        }
        auto pw = [&](int a, int b) {   // mean power of payload symbols [a, b), relative to the training
            double v = 0; int n = 0;
            for (int k = a; k < b; k++) { const int j = kVdl3SyncSyms + k; if (j >= 0 && j < nsym) { v += std::norm(s[(size_t)j]); n++; } }
            return n && eTr > 0 ? v / n / eTr : 0.0;
        };
        int L = lmax;
        if (sy == Vdl3Sync::S2Star) {
            int64_t nextStart = INT64_MAX;
            // only clear training sequences limit the length: a find of 0.8 - 0.9 inside the payload is the payload's own data
            for (const auto& [q, qm] : syncs) if (qm >= 0.9 && q > p.syncEnd + kSps * 4) nextStart = std::min(nextStart, q - kSps * (kVdl3SyncSyms - 1));
            L = 0;
            for (int c : {128, 40, 32, 16}) {
                if (p.syncEnd + (int64_t)kSps * c > nextStart) continue;
                const double in = pw(c - 4, c), after = pw(c + 2, c + 5);
                if (in > 0.4 && after < 0.5 * in) { L = c; break; }
            }
            if (!L) L = 16;
        }
        std::vector<uint8_t> bits;
        double ev = 0;
        for (int k = 0; k < L; k++) {
            const int j = kVdl3SyncSyms + k;
            const int stp = ((d[(size_t)j] - d[(size_t)j - 1]) % 8 + 8) % 8;
            const uint8_t b = kVdl3GrayBits[stp];
            bits.push_back((uint8_t)((b >> 2) & 1)); bits.push_back((uint8_t)((b >> 1) & 1)); bits.push_back((uint8_t)(b & 1));
            ev += err[(size_t)j] * err[(size_t)j];
        }
        const double evm = std::sqrt(ev / std::max(L, 1)) / (kPi / 4);
        if (evm > kEvmMax) return;    // noise that looked like a training sequence
        vdl3Scramble(bits);
        Vdl3Decoded dec;
        if (!vdl3DecodePayload(sy, L, bits, dec)) return;
        if (dec.golayOk) busyUntil = p.syncEnd + (int64_t)kSps * L;   // its last payload symbol
        Vdl3Burst m;
        m.serial = ++serial;
        m.timeSec = (double)p.syncEnd / (kSps * kVdl3SymbolRate);
        m.wallTime = (int64_t)std::time(nullptr);
        m.kind = (int)dec.kind;
        m.kindName = vdl3KindName(dec.kind);
        m.sync = vdl3SyncName(sy);
        m.symbols = L;
        m.golayOk = dec.golayOk;
        m.golayFixed = dec.golayFixed;
        m.info = vdl3InfoHex(dec.info12);
        m.rsOk = dec.rsOk;
        m.rsFixed = dec.rsFixed;
        m.dataHex = vdl3Hex(dec.rsOk ? dec.data : dec.raw);
        if (dec.rsOk) m.dataText = vdl3PrintableText(dec.data);
        m.levelDb = (float)(10 * std::log10(std::max(eTr, 1e-15)));
        m.snrDb = (float)(10 * std::log10(std::max(eTr / std::max(floor, 1e-15) - 1, 1e-3)) + 10 * std::log10(kNoiseBwRatio));
        m.cfoHz = (float)(fs * kVdl3SymbolRate);
        m.evmPct = (float)(100 * evm);
        const bool good = dec.golayOk && evm < 0.25 && (dec.kind != Vdl3Kind::VdData || dec.rsOk);   // above 0.25 the Golay words may be miscorrected
        char b[200];
        if (dec.kind == Vdl3Kind::VdData) snprintf(b, sizeof b, "header %s, data %s", m.info.c_str(), m.dataText.empty() ? "(binary)" : ("\"" + m.dataText + "\"").c_str());
        else if (dec.kind == Vdl3Kind::VdVoice) snprintf(b, sizeof b, "header %s, vocoder frames (not decoded)", m.info.c_str());
        else snprintf(b, sizeof b, "%s %s", dec.kind == Vdl3Kind::Handoff ? "handoff" : "system data", m.info.c_str());
        m.summary = std::string(b) + (dec.golayOk ? (evm < 0.25 ? "" : " (weak)") : " (Golay failed)");
        if (good) {
            tel.blocksOk++;
            switch (dec.kind) {
            case Vdl3Kind::MUplink: tel.mUplink++; break;
            case Vdl3Kind::MUplink3T: tel.mUplink3T++; break;
            case Vdl3Kind::Handoff: tel.handoff++; break;
            case Vdl3Kind::PollResponse: tel.pollResp++; break;
            case Vdl3Kind::MDownlink: tel.mDownlink++; break;
            case Vdl3Kind::NetEntry: tel.netEntry++; break;
            case Vdl3Kind::VdData: tel.vdData++; break;
            case Vdl3Kind::VdVoice: tel.vdVoice++; break;
            default: break;
            }
            tel.dataValid = true;
            tel.state = 2;
            tel.cfoHz = m.cfoHz;
            tel.snrDb = m.snrDb;
            tel.levelDb = m.levelDb;
            lastGood = tel.nowSec;
        } else {
            tel.blocksBad++;
            if (tel.state == 0) tel.state = 1;
        }
        tel.bursts.insert(tel.bursts.begin(), std::move(m));
        if (tel.bursts.size() > kVdl3BurstMax) tel.bursts.pop_back();
    }

    void onZ(cd v) {
        z.push_back(v);
        const double pw = std::norm(v);
        blkSum += pw;
        if (++blkN >= kFloorBlock) {
            blkMeans.push_back(blkSum / blkN);
            if (blkMeans.size() > (size_t)kFloorBlocks) blkMeans.erase(blkMeans.begin());
            // digital silence between bursts (an 8-bit source rounds a noise floor below one step to zero, a recording can be padded): a floor of zero
            // would keep the squelch shut for ever, so it is never lower than 1e-10 (-100 dB re full scale)
            floor = std::max(*std::min_element(blkMeans.begin(), blkMeans.end()), 1e-10);
            blkSum = 0; blkN = 0;
        }
        const int64_t r = (int64_t)z.size() - 1, a = zBase + r;
        if (r >= kSps * (kVdl3SyncSyms - 1)) {
            cd D0, D1;
            const double m0 = corr(r, 0, D0), m1 = corr(r, 1, D1);
            const int fam = m1 > m0 ? 1 : 0;
            const double m = std::max(m0, m1);
            if (winLeft > 0) {
                if (m > bestM) { bestM = m; bestIdx = a; bestD = fam ? D1 : D0; bestFam = fam; }
                if (--winLeft == 0) {
                    cd t;
                    const int64_t br = bestIdx - zBase;
                    const double mm = corr(br - 1, bestFam, t), mp = corr(br + 1, bestFam, t);
                    const double den = mm - 2 * bestM + mp;
                    const double mu = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (mm - mp) / den, -0.5, 0.5) : 0.0;
                    // two finds within 8 symbols are one training sequence: keep the better
                    if (!pend.empty() && bestIdx - pend.back().syncEnd < kSps * 8) {
                        if (bestM > pend.back().m) pend.back() = {bestIdx, mu, bestD, bestFam, bestM};
                    } else {
                        pend.push_back({bestIdx, mu, bestD, bestFam, bestM});
                        tel.syncs++;
                    }
                    if (syncs.empty() || bestIdx - syncs.back().first >= kSps * 8) syncs.push_back({bestIdx, bestM});
                    else if (bestM > syncs.back().second) syncs.back() = {bestIdx, bestM};
                }
            } else if (m > kSyncThr && floor > 0) {
                double E = 0;
                for (int q = 0; q < kVdl3SyncSyms; q++) E += std::norm(z[(size_t)(r - kSps * q)]);
                if (E / kVdl3SyncSyms > 4 * floor) {   // squelch: noise alone must not start a burst
                    winLeft = kWin; bestM = m; bestIdx = a; bestD = fam ? D1 : D0; bestFam = fam;
                }
            }
        }
        // in the order found, all with the lookahead of the longest burst, so a good burst masks the finds inside it
        while (!pend.empty() && a >= pend.front().syncEnd + (int64_t)kSps * (kMaxPayload + 10) + 2 && (pend.size() > 1 || winLeft == 0)) {
            const Pend p = pend.front();
            pend.erase(pend.begin());
            // a training sequence that starts inside the last good burst is that burst's own data: bursts on one channel never overlap (ramps and
            // a guard time lie between them). The end of the sequence alone let one that began in a burst's last symbols through (vdl3_all)
            if (p.syncEnd - (int64_t)kSps * (kVdl3SyncSyms - 1) > busyUntil) decode(p);
        }
        if (z.size() > 65536) {
            int64_t keep = (int64_t)z.size() - kSps * 40;
            for (const auto& p : pend) keep = std::min(keep, p.syncEnd - zBase - kSps * (kVdl3SyncSyms + 4));
            if (keep > 0) {
                z.erase(z.begin(), z.begin() + keep);
                zBase += keep;
            }
            syncs.erase(std::remove_if(syncs.begin(), syncs.end(), [&](const std::pair<int64_t, double>& q) { return q.first < zBase; }), syncs.end());
        }
    }

    void sample(cf32 x, double sampPerOut) {
        rot *= step;
        if (++renorm >= 1024) { renorm = 0; rot /= std::abs(rot); }
        acc += cd(x.real(), x.imag()) * rot;
        accN += 1;
        phase += 1;
        if (phase < sampPerOut) return;
        phase -= sampPerOut;
        const cd v = acc / std::max(accN, 1.0);
        acc = 0; accN = 0;
        hist[(size_t)hpos] = v;
        hist[(size_t)(hpos + kTaps)] = v;
        hpos = (hpos + 1) % kTaps;
        if (++dec2 < 2) return;
        dec2 = 0;
        cd y = 0;
        for (int i = 0; i < kTaps; i++) y += hist[(size_t)(hpos + i)] * h[(size_t)i];
        onZ(y);
    }
};

Vdl3Receiver::Vdl3Receiver() : p_(new Impl) {}
Vdl3Receiver::~Vdl3Receiver() = default;

void Vdl3Receiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->rebuild = true; }
void Vdl3Receiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->offset != hz) { p_->offset = hz; p_->rebuild = true; } }
void Vdl3Receiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->center = hz; }
bool Vdl3Receiver::ready() const { return std::isfinite(p_->rate) && p_->rate >= vdl3Tuning().minSampleRate - 1 && p_->rate < 1e9; }
void Vdl3Receiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }

void Vdl3Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->z.clear();
    p_->pend.clear();
    p_->syncs.clear();
    p_->winLeft = 0;
    p_->busyUntil = -1;
    p_->floor = 0;
    p_->blkSum = 0;
    p_->blkN = 0;
    p_->blkMeans.clear();
    p_->rebuild = true;
    p_->tel.state = 0;
}

void Vdl3Receiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& I = *p_;
    const bool ok = ready();
    I.tel.inputRate = I.rate;
    I.tel.rateOk = ok;
    I.tel.centerHz = I.center;
    if (!ok) { I.seq++; return; }
    if (I.rebuild) I.doRebuild();
    const double sampPerOut = I.rate / (kDump * kVdl3SymbolRate);
    for (size_t i = 0; i < n; i++) I.sample(x[i], sampPerOut);
    I.nIn += n;
    I.tel.nowSec = (double)I.nIn / I.rate;
    if (I.tel.nowSec - I.lastGood > 30 && I.tel.state == 2) I.tel.state = 1;
    I.tel.noiseDb = (float)(10 * std::log10(std::max(I.floor, 1e-15)));
    I.seq++;
}

void Vdl3Receiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& t = p_->tel;
    t.bursts.clear();
    t.blocksOk = t.blocksBad = t.syncs = 0;
    t.mUplink = t.mUplink3T = t.handoff = t.pollResp = t.mDownlink = t.netEntry = t.vdData = t.vdVoice = 0;
    t.dataValid = false;
}

std::string Vdl3Receiver::saveResults() const { return std::string(); }
bool Vdl3Receiver::loadResults(const std::string&) { return false; }

bool Vdl3Receiver::telemetry(Vdl3Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq) return false;
    out = p_->tel;
    out.seq = p_->seq;
    return true;
}

ModeTuning vdl3Tuning() {
    ModeTuning t;
    t.stdMode = 642;
    t.id = "vdl3";
    t.name = "VDL Mode 3";
    t.minMhz = 118;
    t.maxMhz = 137;
    t.defMhz = 136.900;
    t.sampleRate = 1000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 100000;   // the channel sits 100 kHz below the radio's centre, away from the DC spike
    return t;
}

} // namespace dect2
