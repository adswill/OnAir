// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DVB-C2 receiver (EN 302 769). See dvbc2_rx.h for the chain. Simulated signals only.
#include "dect2/dvbc2/dvbc2_rx.h"
#include "dect2/t2/t2interleave.h"
#include "dect2/t2/t2ofdm.h"
#include "dect2/tv/bbunpack.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/resampler.h"
#include "dect2/dsp/clock_correct.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

using namespace dvbc2;

namespace {
inline int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }
constexpr int kSearchSymbols = 8;          // symbol periods the guard interval correlation is folded over
constexpr float kGiThreshold = 0.22f;      // normalised correlation peak that counts as a signal
constexpr float kPilotPhaseRatio = 3.0f;   // below: a preamble (no boosted pilots), above: a data symbol
constexpr float kKBaseThreshold = 0.35f;   // differential pilot correlation that confirms the absolute carrier index
constexpr int kKBaseSearch = 64;           // carriers either side of the nominal index
constexpr float kHeaderOk = 22.f;          // of 32
constexpr size_t kMaxQueue = 64;           // input chunks waiting for the worker before dropping
} // namespace

std::string dvbc2Summary(const Dvbc2Telemetry& t) {
    char b[256];
    const Dvbc2PlpRow* row = nullptr;
    for (const auto& r : t.plps) if (r.sliceId == t.selectedSlice && r.plpId == t.selectedPlp) row = &r;
    snprintf(b, sizeof b, "DVB-C2 GI 1/%s %s %s  Es/N0 %.1f dB  CFO %+.0f Hz  clock %+.1f ppm  blocks ok %llu bad %llu  packets %llu%s",
             t.guardInterval == 1 ? "64" : t.guardInterval == 0 ? "128" : "?", row ? modName(row->mod) : "", row ? codName(row->fecType, row->cod) : "",
             t.snrDb, t.cfoHz, t.clockPpm, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.packets, t.l1Ok ? "  L1 ok" : "");
    return b;
}

struct Dvbc2Receiver::Impl {
    // ---- configuration and threading
    double inRate = 0;
    bool ok = false;
    double centerHz = 0, sigOffset = 0;
    std::function<void(const std::string&)> logCb;
    std::function<void(const uint8_t*, size_t, double)> pktCb;
    std::function<void(const uint8_t*, size_t)> ipCb;
    // GSE PLPs (payload type 2, BB header TS/GS = 10): the data fields to IP packets (TS 102 606, the DVB-S2 GSE decoder)
    std::unique_ptr<dvbsip::GseIp> gse;
    uint64_t ipPackets = 0, gseFields = 0;
    std::atomic<bool> blocking{false};
    std::thread th;
    std::mutex mu;
    std::condition_variable cv, idleCv;
    std::deque<std::vector<cf32>> queue;
    bool busy = false;
    std::atomic<bool> stop{false}, doReset{false};
    std::atomic<uint64_t> dropped{0};
    int selSlice = -1, selPlp = -1;
    std::atomic<int> wantSlice{-1}, wantPlp{-1};

    // ---- front end (64/7 Msample/s)
    RationalResampler rr;
    bool useRr = false;
    // The sample clock error is taken out at the input rate, before the resampler to 64/7 Msample/s, where the signal is narrow enough for the
    // 24-tap corrector (it is flat to 0.43 of the rate; at the native rate the signal reaches 0.42 of it, so there the error is only followed
    // as a timing drift, which leaves the inter-carrier interference of the clock error: about 26 dB at 25 ppm).
    ClockCorrector clock;
    bool useClock = false;
    double clockInt = 0;          // ppm being corrected
    double clockPpm = 0;          // the residual drift seen per symbol (display: clockInt + clockPpm)
    double cfoHz = 0, ncoPhase = 0;
    double slopeAcc = 0;          // the timing offset of the FFT window as a phase slope (rad per carrier), removed from every symbol
    double mixPhase = 0;
    std::vector<cf32> buf;        // samples waiting for symbol processing
    size_t bufPos = 0;
    uint64_t consumed = 0;        // samples taken out of buf since the start (for the time stamps)
    Fft fft{kFft};

    // ---- sync state
    enum State { Search, Timing, Preamble, Data };
    State state = Search;
    int gi = 1, gs = 64, ns = 4160, dxv = 12;
    int64_t kBase = 0;            // absolute carrier index of bin 0 of the window
    bool haveKBase = false;
    bool prevPreambleLike = false;
    int symIndex = 0;             // within the frame: 0 .. P-1 preamble, then P .. P+447
    int preambleSyms = 1, fecBlocks = 1, cellsPerBlock = 0, ksig = 0, l1Width = 0;
    PreambleHeader hdr;
    bool haveHdr = false;
    L1Part2 l1;
    bool l1Ok = false;
    std::vector<std::vector<cf32>> l1Rows;
    std::vector<std::vector<float>> l1N0;
    int preambleMiss = 0;
    // window tables (valid with haveKBase)
    std::vector<uint8_t> wk, pilotBit, cpMask;
    std::vector<int> qOf;
    // channel
    std::vector<cf32> havg;       // the averaged channel at the D_X grid (and every 6th bin from the preamble)
    std::vector<uint8_t> hvalid;
    bool dataEst = false;         // the data symbols' pilots have given a noise estimate (sigma2) since the timing was found
    std::vector<cf32> H;
    std::vector<uint8_t> presentMask;   // bins whose nearest pilot carries signal (the window reaches beyond the system and into notches)
    GridInterpolator interp;
    float sigma2 = 1;             // noise variance per bin (FFT units)
    float sigPower = 1;           // mean |H|^2 of the present pilots
    // the slice being decoded
    bool sliceOk = false;
    int sliceIdx = -1, plpIdx = -1;
    SliceCells geom;
    int tiDepth = 1;
    std::vector<std::vector<cf32>> tiRows;
    std::vector<std::vector<float>> tiN0;
    std::deque<cf32> stream;
    std::deque<float> streamN0;
    uint64_t frameStreamPos = 0;  // cells put into the stream since the frame start (Type 1 alignment)
    bool aligned = false, plpStartValid = false;
    int plpStart = 0;
    bool headerLocked = false;
    int headerFails = 0;
    Fec curFec;
    int curBlocks = 0, curCells = 0;
    bool curWanted = false;
    int curPlpId = 0;
    int decodeFails = 0;
    BbUnpacker unpack;
    std::vector<uint8_t> pktBatch;
    double pendingSecs = 0;
    // telemetry
    std::mutex telMu;
    Dvbc2Telemetry tel;
    uint64_t seq = 0;
    uint64_t packets = 0, blocksOk = 0, blocksBad = 0, frames = 0, l1Good = 0, l1Bad = 0, syncLosses = 0;
    double merSum = 0; uint64_t merN = 0;
    double preBerSum = 0; uint64_t preBerN = 0;
    std::vector<cf32> cellPic;
    int lastHeaderMetric = 0;
    std::string stage = "waiting for samples";
    double lastPacketTime = -1, now = 0;
    uint64_t packetsAtMark = 0; double timeAtMark = 0, netMbps = 0;
    std::vector<float> psd; double psdSpan = 0; size_t psdCount = 0; std::vector<cf32> psdBuf;
    std::map<std::pair<int, int>, Dvbc2PlpRow> rows;

    void log(const std::string& s) { if (logCb) logCb(s); }

    // ---------------------------------------------------------------- threading
    void start() {
        stop = false;
        th = std::thread([this] { loop(); });
    }
    void join() {
        if (!th.joinable()) return;
        { std::lock_guard<std::mutex> lk(mu); stop = true; }
        cv.notify_all();
        th.join();
    }
    void loop() {
        for (;;) {
            std::vector<cf32> chunk;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] { return stop || !queue.empty(); });
                if (stop) return;
                chunk = std::move(queue.front());
                queue.pop_front();
                busy = true;
            }
            if (doReset.exchange(false)) resetState();
            frontEnd(chunk);
            process();
            publish();
            {
                std::lock_guard<std::mutex> lk(mu);
                busy = false;
            }
            idleCv.notify_all();
        }
    }

    // ---------------------------------------------------------------- configuration
    void configure(double rate) {
        join();
        inRate = rate;
        ok = rate >= 8e6 - 1;
        useRr = ok && std::fabs(rate - kSampleRate) > 1.0;
        if (useRr) { double err = 0; if (!rr.configure(rate, kSampleRate, &err, 7.8e6)) ok = false; }
        useClock = ok && rate >= 1.07 * kSampleRate;
        resetState();
        if (ok) start();
    }
    void resetState() {
        rr.reset();
        clock = ClockCorrector();
        clock.prime(nullptr, 0);
        clockInt = 0; clockPpm = 0; cfoHz = 0; ncoPhase = 0; slopeAcc = 0; mixPhase = 0;
        buf.clear(); bufPos = 0; consumed = 0;
        state = Search; haveKBase = false; prevPreambleLike = false; symIndex = 0; haveHdr = false; l1Ok = false; preambleMiss = 0;
        dataEst = false;
        havg.assign(kFft, cf32(0, 0)); hvalid.assign(kFft, 0); H.assign(kFft, cf32(0, 0)); presentMask.assign(kFft, 0);
        sliceOk = false; sliceIdx = plpIdx = -1; stream.clear(); streamN0.clear(); aligned = false; headerLocked = false; headerFails = 0; decodeFails = 0;
        unpack = BbUnpacker();
        unpack.setSink([this](const uint8_t* p) { pktBatch.insert(pktBatch.end(), p, p + 188); });
        stage = ok ? "searching for a signal" : "the input rate is too low (8 Msample/s needed)";
        rows.clear();
    }

    // ---------------------------------------------------------------- front end
    void frontEnd(const std::vector<cf32>& in) {
        std::vector<cf32> a;
        const cf32* src = in.data();
        size_t n = in.size();
        std::vector<cf32> mixed;
        if (sigOffset != 0) {
            mixed.resize(n);
            const double dph = -2 * math::kPi * sigOffset / inRate;
            for (size_t i = 0; i < n; i++) { mixed[i] = in[i] * cf32((float)std::cos(mixPhase), (float)std::sin(mixPhase)); mixPhase += dph; if (mixPhase > math::kPi) mixPhase -= 2 * math::kPi; else if (mixPhase < -math::kPi) mixPhase += 2 * math::kPi; }
            src = mixed.data();
        }
        // spectrum picture of the input
        if (psdBuf.size() < 1024) psdBuf.insert(psdBuf.end(), src, src + std::min(n, 1024 - psdBuf.size()));
        std::vector<cf32> cc;
        if (useClock) { clock.setPpm(clockInt); clock.process(src, n, cc); src = cc.data(); n = cc.size(); }
        if (useRr) { rr.process(src, n, a); src = a.data(); n = a.size(); }
        // (no time-domain clock correction: the sample clock error is followed as a timing drift, a phase slope per symbol and a whole
        // sample of window shift when it adds up to one; the 24-tap corrector is not flat enough at 0.42 of the native rate)
        buf.insert(buf.end(), src, src + n);
        if (bufPos > (size_t)ns * 16) { buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)bufPos); bufPos = 0; }
    }
    void advance(size_t n) { bufPos += n; consumed += n; now = (double)consumed / kSampleRate; }

    // ---------------------------------------------------------------- symbol timing (guard interval correlation)
    bool search() {
        const size_t need = (size_t)(kFft + 64) * (kSearchSymbols + 1);
        if (buf.size() - bufPos < need) return false;
        const cf32* x = buf.data() + bufPos;
        const size_t L = need;
        std::vector<cf32> c(L - kFft);
        std::vector<float> e(L - kFft);
        for (size_t n = 0; n + kFft < L; n++) { c[n] = x[n] * std::conj(x[n + kFft]); e[n] = 0.5f * (std::norm(x[n]) + std::norm(x[n + kFft])); }
        std::vector<cf32> sc(c.size() + 1, cf32(0, 0));
        std::vector<float> se(e.size() + 1, 0.f);
        for (size_t n = 0; n < c.size(); n++) { sc[n + 1] = sc[n] + c[n]; se[n + 1] = se[n] + e[n]; }
        float bestV = 0; int bestGi = -1, bestM = 0; cf32 bestC(0, 0);
        for (int g = 0; g < 2; g++) {
            const int G = guardSamples(g), Ns = kFft + G;
            int J = 0;
            while ((size_t)(J + 1) * Ns + G <= c.size()) J++;
            J = std::min(J, kSearchSymbols);
            if (J < 2) continue;
            for (int m = 0; m < Ns; m++) {
                float acc = 0;
                cf32 cs(0, 0);
                for (int j = 0; j < J; j++) {
                    const size_t p = (size_t)m + (size_t)j * Ns;
                    const cf32 C = sc[p + G] - sc[p];
                    const float E = se[p + G] - se[p];
                    acc += E > 0 ? std::abs(C) / E : 0.f;
                    cs += C;
                }
                acc /= J;
                if (acc > bestV) { bestV = acc; bestGi = g; bestM = m; bestC = cs; }
            }
        }
        if (bestGi < 0 || bestV < kGiThreshold) { advance((size_t)ns * 4); stage = "searching for a signal"; return true; }
        gi = bestGi; gs = guardSamples(gi); ns = symbolSamples(gi); dxv = dx(gi);
        const double eps = -std::atan2(bestC.imag(), bestC.real()) / (2 * math::kPi);   // carriers
        advance((size_t)bestM);
        cfoHz = eps * kCarrierSpacingHz;
        ncoPhase = 0;
        dataEst = false;
        state = Timing;
        prevPreambleLike = false;
        char b[160];
        snprintf(b, sizeof b, "DVB-C2: symbol timing found, guard interval 1/%d, correlation %.2f, carrier offset %+.0f Hz", gi == 1 ? 64 : 128, bestV, cfoHz);
        log(b);
        stage = "looking for the preamble";
        return true;
    }

    // ---------------------------------------------------------------- one OFDM symbol -> window bins (bin 0 = lowest frequency)
    size_t symStart = 0;          // where the last symbol was taken from (for re-centring the window at a preamble)
    uint64_t consumedAtSym = 0;
    double ncoAtSym = 0;
    bool takeSymbol(std::vector<cf32>& Y) {
        if (buf.size() - bufPos < (size_t)ns) return false;
        symStart = bufPos; consumedAtSym = consumed; ncoAtSym = ncoPhase;
        extractSymbol(Y);
        return true;
    }
    // the last symbol again, with its window moved by `delta` samples (the following symbols keep the new timing); false when out of range
    bool retakeSymbol(std::vector<cf32>& Y, int delta) {
        if ((delta < 0 && (size_t)(-delta) > symStart) || symStart + (size_t)delta + (size_t)ns > buf.size()) return false;
        bufPos = symStart + (size_t)delta; consumed = consumedAtSym + (uint64_t)delta; ncoPhase = ncoAtSym;
        symStart = bufPos; consumedAtSym = consumed;
        extractSymbol(Y);
        return true;
    }
    void extractSymbol(std::vector<cf32>& Y) {
        const int winOff = gs / 2;   // the window starts in the middle of the guard interval: room for echoes and pre-echoes
        const cf32* x = buf.data() + bufPos + (size_t)winOff;
        std::vector<cf32> t(x, x + kFft);
        // the carrier offset: a numerically controlled oscillator with a continuous phase across the symbols (guard intervals included)
        const double dph = -2 * math::kPi * cfoHz / kSampleRate;
        double ph = ncoPhase + dph * winOff;
        for (int i = 0; i < kFft; i++) { t[(size_t)i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; }
        ncoPhase += dph * ns;
        // clause 10.1: the carriers sit on the absolute grid with their phase reference at each symbol's useful part, so a receiver whose
        // oscillator is at carrier k_LO sees a common phase step of -2 pi k_LO N_s / N per symbol (the fractional part of k_LO is the
        // carrier offset the oscillator above removes; the integer part is known once the window's carrier index is)
        if (haveKBase) ncoPhase += 2 * math::kPi * (double)(((kBase + kFft / 2) * (int64_t)ns) % kFft) / kFft;
        if (ncoPhase > 1e6 || ncoPhase < -1e6) ncoPhase = std::fmod(ncoPhase, 2 * math::kPi);
        fft.forward(t.data());
        Y.resize(kFft);
        for (int i = 0; i < kFft; i++) Y[(size_t)((i + kFft / 2) % kFft)] = t[(size_t)i];
        if (slopeAcc != 0) for (int b = 0; b < kFft; b++) { const double p = -slopeAcc * (b - kFft / 2); Y[(size_t)b] *= cf32((float)std::cos(p), (float)std::sin(p)); }
        advance((size_t)ns);
        // a whole sample of timing offset moves the window instead (slope -2 pi d / N for a delay of d samples)
        const double one = 2 * math::kPi / kFft;
        if (slopeAcc <= -one && buf.size() - bufPos > 1) { advance(1); slopeAcc += one; }
        else if (slopeAcc >= one && bufPos >= 1) { bufPos--; consumed--; slopeAcc -= one; }
        pendingSecs += symbolSeconds(gi);
    }
    // The delay of the useful part against the FFT window, from the phase slope between neighbouring preamble pilots (6 carriers apart:
    // unambiguous to +-341 samples). Returns false without enough pilots.
    bool preambleDelay(const std::vector<cf32>& Y, int b0, double& d) const {
        const float app = preamblePilotAmp(gi);
        std::vector<float> pw;
        for (int b = b0; b < kFft; b += 6) pw.push_back(std::norm(Y[(size_t)b]));
        std::vector<float> sorted = pw;
        std::sort(sorted.begin(), sorted.end());
        const float ref = sorted[sorted.size() / 2] * 0.2f;
        cf32 r(0, 0);
        int n = 0;
        for (int b = b0; b + 6 < kFft; b += 6) {
            if (std::norm(Y[(size_t)b]) < ref || std::norm(Y[(size_t)(b + 6)]) < ref) continue;
            r += (Y[(size_t)(b + 6)] / pilotRef(b + 6, app)) * std::conj(Y[(size_t)b] / pilotRef(b, app));
            n++;
        }
        if (n < 50) return false;
        d = -std::atan2(r.imag(), r.real()) * kFft / (2 * math::kPi * 6);
        return true;
    }

    // the boosted scattered pilots of a data symbol: the energy of the strongest of the 4 D_X phases against the median (about 5 for a data
    // symbol, 1.4 .. 1.8 for a preamble whose unboosted pilots sit on every 6th carrier)
    float pilotPhaseRatio(const std::vector<cf32>& Y) const {
        const int m = dxv * kDy;
        std::vector<float> p((size_t)m, 0.f);
        for (int b = 0; b < kFft; b++) p[(size_t)(b % m)] += std::norm(Y[(size_t)b]);
        std::vector<float> s = p;
        std::sort(s.begin(), s.end());
        const float med = s[s.size() / 2];
        return med > 0 ? s.back() / med : 0.f;
    }

    // ---------------------------------------------------------------- the absolute carrier index from the preamble pilots
    // metric of kBase hypothesis: the differential pilot products against the reference sequence
    float kBaseMetric(const std::vector<cf32>& Y, int64_t kb, const std::vector<cf32>& prod) const {
        // prod[b] = Y[b] conj(Y[b - 6]) for b >= 6
        double num = 0, den = 0;
        const int p = (int)floorMod(-kb, 6);
        for (int b = p; b < kFft; b += 6) {
            if (b < 6) continue;
            const int64_t k = kb + b;
            if (floorMod(k, kL1Carriers) == 0) continue;   // the differential reference restarts at the block start
            const uint8_t d = dataScramble(k) ^ pilotPrbs((int)(floorMod(k, kL1Carriers) / 6));   // r_k xor r_(k-6) = w^P_k
            num += d ? -prod[(size_t)b].real() : prod[(size_t)b].real();
            den += std::abs(prod[(size_t)b]);
        }
        return den > 0 ? (float)(num / den) : 0.f;
    }
    bool findKBase(const std::vector<cf32>& Y) {
        std::vector<cf32> prod((size_t)kFft, cf32(0, 0));
        for (int b = 6; b < kFft; b++) prod[(size_t)b] = Y[(size_t)b] * std::conj(Y[(size_t)(b - 6)]);
        float best = 0; int64_t bestK = 0;
        if (centerHz > 0) {
            const int64_t nominal = (int64_t)std::llround(centerHz / kCarrierSpacingHz) - kFft / 2;
            for (int64_t kb = nominal - kKBaseSearch; kb <= nominal + kKBaseSearch; kb++) {
                const float m = kBaseMetric(Y, kb, prod);
                if (m > best) { best = m; bestK = kb; }
            }
        } else {
            // the pilot phase from the (unboosted but still stronger) pilot energy, then every (k mod 3408, k mod 2047)
            std::vector<double> ph(6, 0.0);
            for (int b = 0; b < kFft; b++) ph[(size_t)(b % 6)] += std::norm(Y[(size_t)b]);
            int p = 0;
            for (int i = 1; i < 6; i++) if (ph[(size_t)i] > ph[(size_t)p]) p = i;
            // bins with pilots: b = p + 6 j; the reference bit at k = kb + b depends on kb mod 3408 (w') and kb mod 2047 (w)
            std::vector<float> t;
            std::vector<int> bins;
            for (int b = p; b < kFft; b += 6) if (b >= 6) { bins.push_back(b); t.push_back(prod[(size_t)b].real()); }
            double den = 0;
            for (int b : bins) den += std::abs(prod[(size_t)b]);
            if (den <= 0) return false;
            std::vector<int8_t> wsign(2047);
            for (int i = 0; i < 2047; i++) wsign[(size_t)i] = dataScramble(i) ? -1 : 1;
            // scan c = kb mod 3408 over the 568 values with (c + p) % 6 == 0 and n = kb mod 2047 over its 2047 values (about a second once)
            std::vector<float> u(bins.size());
            float bestFull = 0; int bestC = 0, bestN = 0;
            for (int c = floorMod(-p, 6); c < kL1Carriers; c += 6) {
                for (size_t i = 0; i < bins.size(); i++) {
                    const int kk = (int)((c + bins[i]) % kL1Carriers);
                    u[i] = kk == 0 ? 0.f : (pilotPrbs(kk / 6) ? -t[i] : t[i]);
                }
                for (int n = 0; n < 2047; n++) {
                    double acc = 0;
                    for (size_t i = 0; i < bins.size(); i++) acc += u[i] * wsign[(size_t)((n + bins[i]) % 2047)];
                    if (acc > bestFull) { bestFull = (float)acc; bestC = c; bestN = n; }
                }
            }
            best = (float)(bestFull / den);
            // kb = c (mod 3408) = n (mod 2047): the two moduli are coprime; the smallest positive solution
            int64_t kb = -1;
            for (int64_t i = 0; i < 2047; i++) { const int64_t cand = bestC + (int64_t)i * kL1Carriers; if (floorMod(cand, 2047) == bestN) { kb = cand; break; } }
            if (kb < 0) return false;
            bestK = kb;
        }
        if (best < kKBaseThreshold) return false;
        setKBase(bestK);
        char b[200];
        snprintf(b, sizeof b, "DVB-C2: preamble found, window centre at carrier %lld (%.4f MHz), pilot correlation %.2f", (long long)(kBase + kFft / 2), (kBase + kFft / 2) * kCarrierSpacingHz / 1e6, best);
        log(b);
        return true;
    }
    void setKBase(int64_t kb) {
        kBase = kb; haveKBase = true;
        wk.resize(kFft); pilotBit.resize(kFft); cpMask.resize(kFft); qOf.assign(kFft, -1);
        ReferenceBlock rb;
        for (int b = 0; b < kFft; b++) {
            const int64_t k = kBase + b;
            wk[(size_t)b] = dataScramble(k);
            cpMask[(size_t)b] = isContinualPilot(k);
            if (floorMod(k, 6) == 0) { rb.set(floorDiv(k, kL1Carriers)); pilotBit[(size_t)b] = rb.bit(k); }
            else { pilotBit[(size_t)b] = 0; qOf[(size_t)b] = preambleCellIndex((int)floorMod(k, kL1Carriers)); }
        }
    }
    float pilotRef(int b, float amp) const { return amp * (1.f - 2.f * pilotBit[(size_t)b]); }

    // ---------------------------------------------------------------- channel estimation from a pilot grid (spacing S, first bin b0)
    void interpolateGrid(int S, int b0, std::vector<cf32>& out) {
        const int M = (kFft - b0 + S - 1) / S;
        std::vector<cf32> grid((size_t)M);
        std::vector<float> pw;
        for (int n = 0; n < M; n++) { const int b = b0 + S * n; grid[(size_t)n] = hvalid[(size_t)b] ? havg[(size_t)b] : cf32(0, 0); if (hvalid[(size_t)b]) pw.push_back(std::norm(havg[(size_t)b])); }
        if (pw.empty()) { out.assign(kFft, cf32(0, 0)); presentMask.assign(kFft, 0); return; }
        std::sort(pw.begin(), pw.end());
        float ref = 0; int cnt = 0;
        for (size_t i = pw.size() / 2; i < pw.size(); i++) { ref += pw[i]; cnt++; }
        ref /= cnt;
        sigPower = ref;
        // grid points without signal (outside the system, in a notch) take the nearest present value
        std::vector<uint8_t> present((size_t)M, 0);
        for (int n = 0; n < M; n++) present[(size_t)n] = hvalid[(size_t)(b0 + S * n)] && std::norm(grid[(size_t)n]) > 0.1f * ref;
        presentMask.assign(kFft, 0);
        for (int b = 0; b < kFft; b++) { const int n = std::clamp((b - b0 + S / 2) / S, 0, M - 1); presentMask[(size_t)b] = present[(size_t)n]; }
        int last = -1, first = -1;
        for (int n = 0; n < M; n++) { if (present[(size_t)n]) { last = n; if (first < 0) first = n; } else if (last >= 0) grid[(size_t)n] = grid[(size_t)last]; }
        for (int n = 0; n < first; n++) grid[(size_t)n] = grid[(size_t)first];
        std::vector<cf32> Hs;
        // the window starts half a guard interval before the useful part, so the channel sits at a delay of gs / 2 samples: centre the
        // interpolator's delay passband there (its error grows with the distance from tau0)
        interp.run(grid, S, kFft - b0, kFft, (double)(gs - gs / 2), Hs, 0.6);
        out.assign(kFft, cf32(0, 0));
        for (int b = b0; b < kFft; b++) out[(size_t)b] = Hs[(size_t)(b - b0)];
        for (int b = 0; b < b0; b++) out[(size_t)b] = Hs[0];
    }
    bool present(int b) const { return presentMask[(size_t)b] != 0; }
    float n0Of(int b) const { const float h2 = std::norm(H[(size_t)b]); return h2 > 1e-12f ? 0.5f * sigma2 / h2 : 1e9f; }

    // ---------------------------------------------------------------- preamble symbol lp
    // returns false when the symbol does not decode as a preamble
    bool preambleSymbol(std::vector<cf32>& Y, int lp) {
        const int b0 = (int)floorMod(-kBase, 6);
        const float app = preamblePilotAmp(gi);
        // re-centre the FFT window: the useful part should start gs / 2 samples after it (the timing drifted while the preamble was
        // awaited, and the data symbols' loop only follows changes from here on)
        double d = 0;
        if (preambleDelay(Y, b0, d)) {
            const int delta = (int)std::lround(d - gs / 2);
            if (delta != 0 && std::abs(delta) < 3 * gs) {
                std::vector<cf32> Y2;
                if (retakeSymbol(Y2, delta)) { Y.swap(Y2); slopeAcc = 0; dataEst = false; }
            }
        }
        // channel at the pilots (the preamble gives every 6th carrier: better than the data symbols' grid, so it replaces the average)
        std::vector<cf32> hp;
        for (int b = b0; b < kFft; b += 6) { havg[(size_t)b] = Y[(size_t)b] / pilotRef(b, app); hvalid[(size_t)b] = 1; hp.push_back(havg[(size_t)b]); }
        interpolateGrid(6, b0, H);
        // noise from neighbouring pilot differences (the cable channel is smooth over 6 carriers)
        double acc = 0; int n = 0;
        for (size_t i = 1; i + 1 < hp.size(); i++) {
            if (std::norm(hp[i]) <= 0.1f * sigPower || std::norm(hp[i - 1]) <= 0.1f * sigPower || std::norm(hp[i + 1]) <= 0.1f * sigPower) continue;
            acc += std::norm(hp[i - 1] - 2.f * hp[i] + hp[i + 1]); n++;   // the second difference cancels the timing slope; its noise is 6 sigma^2 / A^2
        }
        // only until the data symbols have their own: this estimate also holds the channel's curvature over 12 carriers and the phase step
        // of the window delay between neighbours, which made it some 16 dB too high at good SNR; the data cells after every preamble then
        // carried LLRs that small for some 20 symbols, and blocks of the high rates across the frame start (4096-QAM 9/10) failed
        if (!dataEst) sigma2 = n > 0 ? (float)(acc / n / 6 * app * app) : sigma2;
        // the 2 840 cells of the L1 block, both copies of a cell combined
        std::vector<cf32> x((size_t)kL1DataCells, cf32(0, 0));
        std::vector<float> w((size_t)kL1DataCells, 0.f);
        for (int b = 0; b < kFft; b++) {
            const int q = qOf[(size_t)b];
            if (q < 0 || !present(b)) continue;
            cf32 z = Y[(size_t)b] / H[(size_t)b];
            if (wk[(size_t)b]) z = -z;
            const float wt = 1.f / n0Of(b);
            x[(size_t)q] += z * wt; w[(size_t)q] += wt;
        }
        std::vector<cf32> recv((size_t)kL1DataCells, cf32(0, 0));
        std::vector<float> rn0((size_t)kL1DataCells, 1e9f);
        for (int q = 0; q < kL1DataCells; q++) if (w[(size_t)q] > 0) { recv[(size_t)q] = x[(size_t)q] / w[(size_t)q]; rn0[(size_t)q] = 1.f / w[(size_t)q]; }
        // frequency de-interleaving (the parity of the preamble symbol); on the first symbol both parities are tried
        auto deinterleave = [&](bool odd, std::vector<cf32>& o, std::vector<float>& on0) {
            const auto& Hs = freqInterleaverSeqRef(2, kL1DataCells, odd);
            o.assign((size_t)kL1DataCells, cf32(0, 0)); on0.assign((size_t)kL1DataCells, 1e9f);
            for (int q = 0; q < kL1DataCells; q++) { o[(size_t)Hs[(size_t)q]] = recv[(size_t)q]; on0[(size_t)Hs[(size_t)q]] = rn0[(size_t)q]; }
        };
        std::vector<cf32> orig; std::vector<float> on0;
        deinterleave((lp & 1) != 0, orig, on0);
        float metric = 0;
        uint16_t hv = decodeHeader(orig.data(), on0.data(), false, &metric);
        if (metric < kHeaderOk) return false;
        PreambleHeader h = PreambleHeader::unpack(hv);
        int fb, ps, ks, cpb;
        if (!l1Geometry(h, fb, ps, ks, cpb)) return false;
        if (lp == 0) {
            hdr = h; haveHdr = true; fecBlocks = fb; preambleSyms = ps; ksig = ks; cellsPerBlock = cpb; l1Width = fb * cpb / ps;
            l1Rows.assign((size_t)ps, {}); l1N0.assign((size_t)ps, {});
        } else if (h.pack() != hdr.pack()) return false;
        if (lp >= preambleSyms) return false;
        // the cells after the header, averaged over the cyclic repetitions
        const int U = kPreambleHeaderCells + l1Width;
        std::vector<cf32> row((size_t)l1Width, cf32(0, 0));
        std::vector<float> rown0((size_t)l1Width, 0.f);
        for (int c = 0; c < l1Width; c++) {
            cf32 s(0, 0); float wt = 0;
            for (int q = kPreambleHeaderCells + c; q < kL1DataCells; q += U) { const float ww = 1.f / on0[(size_t)q]; s += orig[(size_t)q] * ww; wt += ww; }
            row[(size_t)c] = wt > 0 ? s / wt : cf32(0, 0);
            rown0[(size_t)c] = wt > 0 ? 1.f / wt : 1e9f;
        }
        l1Rows[(size_t)lp] = std::move(row);
        l1N0[(size_t)lp] = std::move(rown0);
        return true;
    }

    void applyL1(const L1Part2& nl) {
        const bool changed = !l1Ok || nl.startFrequency != l1.startFrequency || nl.bandwidth != l1.bandwidth || nl.slices.size() != l1.slices.size() || nl.guardInterval != l1.guardInterval;
        l1 = nl; l1Ok = true; l1Good++;
        if (changed) log("DVB-C2 L1: " + l1Summary(l1));
        if (nl.guardInterval != gi) { log("DVB-C2: the L1 signals another guard interval than the one found: resynchronising"); state = Search; return; }
        // the slice and PLP to decode
        int si = -1, pi = -1;
        const int ws = wantSlice.load(), wp = wantPlp.load();
        for (size_t i = 0; i < l1.slices.size() && si < 0; i++) {
            if (ws >= 0 && l1.slices[i].id != ws) continue;
            if (l1.slices[i].offsetRight <= l1.slices[i].offsetLeft) continue;
            si = (int)i;
        }
        if (si >= 0) {
            const auto& s = l1.slices[(size_t)si];
            for (size_t j = 0; j < s.plps.size() && pi < 0; j++) if (wp < 0 || s.plps[j].id == wp) pi = (int)j;
            if (pi < 0 && !s.plps.empty()) pi = 0;
        }
        const bool newSel = si != sliceIdx || pi != plpIdx || changed;
        sliceIdx = si; plpIdx = pi;
        rows.clear();
        for (const auto& s : l1.slices) for (const auto& p : s.plps) {
            Dvbc2PlpRow r; r.sliceId = s.id; r.plpId = p.id; r.type = p.type; r.payloadType = p.payloadType;
            if (s.type == 0) { r.fecType = p.fecType; r.mod = p.mod; r.cod = p.cod; }
            rows[{s.id, p.id}] = r;
        }
        if (si < 0 || pi < 0) { sliceOk = false; stage = "no data slice to decode"; return; }
        const L1DataSlice& s = l1.slices[(size_t)si];
        if (newSel) {
            geom = SliceCells();
            geom.gi = gi; geom.sysKMin = l1.kMin(); geom.sysKMax = l1.kMax(); geom.reservedTones = l1.reservedTone != 0;   // clause 9.7: no data there
            for (const auto& n : l1.notches) if (n.width > 0) geom.notches.push_back({l1.notchKMin(n), l1.notchKMax(n)});
            geom.kMin = l1.sliceKMin(s); geom.kMax = l1.sliceKMax(s);
            tiDepth = s.tiDepth == 0 ? 1 : 4 << (s.tiDepth - 1);
            sliceOk = geom.kMin - kBase >= 0 && geom.kMax - kBase < kFft && geom.width() <= kL1Carriers && geom.width() > 0;
            if (!sliceOk) { stage = "the data slice is outside the input window"; log("DVB-C2: data slice " + std::to_string(s.id) + " is not inside the 7.61 MHz window: tune to its tuning position"); return; }
            stream.clear(); streamN0.clear(); aligned = false; headerLocked = false; headerFails = 0; decodeFails = 0;
            unpack = BbUnpacker();
            gse.reset();
            unpack.setSink([this](const uint8_t* p) { pktBatch.insert(pktBatch.end(), p, p + 188); });
            char b[200];
            snprintf(b, sizeof b, "DVB-C2: decoding data slice %d (%d carriers, %s, time interleaving %d) PLP %d", s.id, geom.width(), s.type ? "Type 2" : "Type 1", tiDepth, s.plps[(size_t)pi].id);
            log(b);
            if (s.type == 0) { curFec.fecType = s.plps[(size_t)pi].fecType; curFec.mod = s.plps[(size_t)pi].mod; curFec.cod = s.plps[(size_t)pi].cod; }
        }
        if (s.type == 0) { plpStart = s.plps[(size_t)pi].start; plpStartValid = true; }
        stage = "decoding";
    }

    // ---------------------------------------------------------------- data symbol ld
    void dataSymbol(std::vector<cf32>& Y, int ld) {
        if (!l1Ok) return;
        const int64_t kmin = l1.kMin(), kmax = l1.kMax();
        // the pilots of this symbol inside the system
        std::vector<int> pil;
        for (int b = 0; b < kFft; b++) {
            const int64_t k = kBase + b;
            if (k < kmin || k > kmax) continue;
            if (floorMod(k, 6) != 0) continue;
            bool p = cpMask[(size_t)b] || k == kmin || k == kmax || isScatteredPilot(k, ld, gi);
            if (!p) for (auto& n : geom.notches) if (k == n.first - 1 || k == n.second + 1) p = true;
            if (!p) continue;
            bool notch = false;
            for (auto& n : geom.notches) if (k >= n.first && k <= n.second) notch = true;
            if (!notch) pil.push_back(b);
        }
        // common phase and timing slope against the averaged channel (weighted least squares on the phase)
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int b : pil) {
            if (!hvalid[(size_t)b] || std::norm(havg[(size_t)b]) < 0.1f * sigPower) continue;
            const cf32 e = Y[(size_t)b] * std::conj(havg[(size_t)b] * pilotRef(b, kPilotAmp));
            const float w = std::abs(e);
            const double ph = std::atan2(e.imag(), e.real()), x = (double)(b - kFft / 2);
            sw += w; sx += w * x; sy += w * ph; sxx += w * x * x; sxy += w * x * ph;
        }
        if (sw > 0) {
            const double det = sw * sxx - sx * sx;
            const double beta = det != 0 ? (sw * sxy - sx * sy) / det : 0, a = (sy - beta * sx) / sw;
            for (int b = 0; b < kFft; b++) { const double ph = -(a + beta * (b - kFft / 2)); Y[(size_t)b] *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
            // the phase and the slope go into the oscillator and the window timing in full, so that the next measurement is the increment
            // (frequency and clock error) and the two integrators below stay first-order loops
            ncoPhase -= a;
            slopeAcc += beta;
            const double ts = symbolSeconds(gi);
            const double dfHz = a / (2 * math::kPi * ts);
            cfoHz += 0.3 * dfHz;
            const double ppm = std::clamp(-beta * kFft / (2 * math::kPi * ns) * 1e6, -2000.0, 2000.0);   // the timing drift per symbol as a clock error
            clockPpm = 0.9 * clockPpm + 0.1 * ppm;
            // the corrector works on the chunks fed later (up to a few symbols behind), so its integrator is slow
            if (useClock) clockInt = std::clamp(clockInt + 0.02 * ppm, -500.0, 500.0);
        }
        // noise from the pilots against the averaged channel, then the average
        double acc = 0; int n = 0;
        for (int b : pil) {
            const cf32 ref = havg[(size_t)b] * pilotRef(b, kPilotAmp);
            if (hvalid[(size_t)b]) { acc += std::norm(Y[(size_t)b] - ref); n++; }
        }
        if (n > 8) sigma2 = 0.8f * sigma2 + 0.2f * (float)(acc / n);
        for (int b : pil) {
            const cf32 h = Y[(size_t)b] / pilotRef(b, kPilotAmp);
            if (hvalid[(size_t)b]) havg[(size_t)b] = 0.75f * havg[(size_t)b] + 0.25f * h; else { havg[(size_t)b] = h; hvalid[(size_t)b] = 1; }
        }
        dataEst = true;
        interpolateGrid(dxv, (int)floorMod(-kBase, dxv), H);
        if (!sliceOk) return;
        // the slice's data cells of this symbol, equalised, in carrier order after the frequency de-interleaver
        std::vector<int64_t> carriers;
        geom.dataCarriers(ld, carriers);
        const int nc = (int)carriers.size();
        const int r = ld % tiDepth;
        if (r == 0) { tiRows.assign((size_t)tiDepth, {}); tiN0.assign((size_t)tiDepth, {}); }
        std::vector<cf32> recv((size_t)nc); std::vector<float> rn0((size_t)nc);
        for (int q = 0; q < nc; q++) {
            const int b = (int)(carriers[(size_t)q] - kBase);
            recv[(size_t)q] = Y[(size_t)b] / H[(size_t)b];
            rn0[(size_t)q] = n0Of(b);
        }
        const auto& Hs = freqInterleaverSeqRef(2, nc, (ld & 1) != 0);
        std::vector<cf32> orig((size_t)nc); std::vector<float> on0((size_t)nc);
        for (int q = 0; q < nc; q++) { orig[(size_t)Hs[(size_t)q]] = recv[(size_t)q]; on0[(size_t)Hs[(size_t)q]] = rn0[(size_t)q]; }
        tiRows[(size_t)r] = std::move(orig);
        tiN0[(size_t)r] = std::move(on0);
        if (cellPic.size() < 2048) for (int q = 0; q < nc; q += 7) cellPic.push_back(recv[(size_t)q]);
        if (r != tiDepth - 1) return;
        // time de-interleaving of the block: the cells go back into the memory row-wise, and come out in the write order
        const int ld0 = ld - r, width = geom.width();
        std::vector<std::vector<cf32>> mem((size_t)tiDepth, std::vector<cf32>((size_t)width, cf32(0, 0)));
        std::vector<std::vector<float>> memN0((size_t)tiDepth, std::vector<float>((size_t)width, 1e9f));
        std::vector<std::vector<uint8_t>> isData((size_t)tiDepth, std::vector<uint8_t>((size_t)width, 0));
        for (int rr2 = 0; rr2 < tiDepth; rr2++) {
            size_t i = 0;
            for (int c = 0; c < width; c++) {
                if (!geom.isData(geom.kMin + c, ld0 + rr2)) continue;
                isData[(size_t)rr2][(size_t)c] = 1;
                if (i < tiRows[(size_t)rr2].size()) { mem[(size_t)rr2][(size_t)c] = tiRows[(size_t)rr2][i]; memN0[(size_t)rr2][(size_t)c] = tiN0[(size_t)rr2][i]; }
                i++;
            }
        }
        std::vector<std::pair<int, int>> order;
        tiWriteOrder(tiDepth, width, order);
        for (auto& rc : order) if (isData[(size_t)rc.first][(size_t)rc.second]) { stream.push_back(mem[(size_t)rc.first][(size_t)rc.second]); streamN0.push_back(memN0[(size_t)rc.first][(size_t)rc.second]); frameStreamPos++; }
        parseStream(ld0 == 0);
    }

    // ---------------------------------------------------------------- Data Slice Packets out of the slice's cell stream
    void dropCells(size_t n) { n = std::min(n, stream.size()); stream.erase(stream.begin(), stream.begin() + (ptrdiff_t)n); streamN0.erase(streamN0.begin(), streamN0.begin() + (ptrdiff_t)n); }
    void parseStream(bool frameStart) {
        const L1DataSlice& s = l1.slices[(size_t)sliceIdx];
        if (s.type == 0) {
            // Type 1: the L1 points at the first complete XFECFrame of the frame
            (void)frameStart;
            if (!aligned) {
                // the stream was emptied at the frame start, so it holds the frame's cells so far: wait for the pointer, then drop up to it
                if (!plpStartValid || stream.size() < (size_t)plpStart) return;
                dropCells((size_t)plpStart);
                aligned = true; curCells = dims(curFec).cells; curWanted = dims(curFec).ok; curBlocks = 1; curPlpId = s.plps[(size_t)plpIdx].id;
            }
            while ((int)stream.size() >= curCells) decodeOne();
            return;
        }
        // Type 2: FECFrame headers
        const bool hem = s.fecHeaderType == 1;
        const int hc = headerCells(hem);
        for (;;) {
            if (!headerLocked) {
                if ((int)stream.size() < hc + 1) return;
                std::vector<cf32> c(stream.begin(), stream.begin() + hc);
                std::vector<float> n0(streamN0.begin(), streamN0.begin() + hc);
                if (headerCorrelation(c.data(), hem) < 0.45f) { dropCells(1); continue; }
                float metric = 0;
                const uint16_t v = decodeHeader(c.data(), n0.data(), hem, &metric);
                FecFrameHeader h = FecFrameHeader::unpack(v);
                const int len = xfecFrameCells(h.fecType, h.mod);
                Fec f; f.fecType = h.fecType; f.mod = h.mod; f.cod = h.cod;
                const bool valid = len > 0 && (h.mod == 0 || modCodValid(f) || (h.mod >= 1 && h.mod <= 5 && h.cod >= 1 && h.cod <= 5));
                if (metric < kHeaderOk || !valid) { dropCells(1); continue; }
                lastHeaderMetric = (int)metric;
                dropCells((size_t)hc);
                headerLocked = true; headerFails = 0;
                curFec = f; curCells = len; curBlocks = h.counter + 1; curPlpId = h.plpId;
                curWanted = h.mod >= 1 && h.mod <= 5 && h.plpId == s.plps[(size_t)plpIdx].id && dims(f).ok;
                auto it = rows.find({s.id, h.plpId});
                if (it != rows.end() && h.mod != 0) { it->second.fecType = h.fecType; it->second.mod = h.mod; it->second.cod = h.cod; }
                continue;
            }
            if (curBlocks > 0) {
                if ((int)stream.size() < curCells) return;
                decodeOne();
                curBlocks--;
                continue;
            }
            // the next header
            if ((int)stream.size() < hc) return;
            std::vector<cf32> c(stream.begin(), stream.begin() + hc);
            std::vector<float> n0(streamN0.begin(), streamN0.begin() + hc);
            float metric = 0;
            const uint16_t v = decodeHeader(c.data(), n0.data(), hem, &metric);
            FecFrameHeader h = FecFrameHeader::unpack(v);
            const int len = xfecFrameCells(h.fecType, h.mod);
            Fec f; f.fecType = h.fecType; f.mod = h.mod; f.cod = h.cod;
            if (metric < kHeaderOk || len <= 0) {
                if (++headerFails >= 2) { headerLocked = false; syncLosses++; }
                else dropCells((size_t)hc);
                continue;
            }
            lastHeaderMetric = (int)metric;
            headerFails = 0;
            dropCells((size_t)hc);
            curFec = f; curCells = len; curBlocks = h.counter + 1; curPlpId = h.plpId;
            curWanted = h.mod >= 1 && h.mod <= 5 && h.plpId == s.plps[(size_t)plpIdx].id && dims(f).ok;
            auto it = rows.find({s.id, h.plpId});
            if (it != rows.end() && h.mod != 0) { it->second.fecType = h.fecType; it->second.mod = h.mod; it->second.cod = h.cod; }
        }
    }
    void decodeOne() {
        std::vector<cf32> c(stream.begin(), stream.begin() + curCells);
        std::vector<float> n0(streamN0.begin(), streamN0.begin() + curCells);
        dropCells((size_t)curCells);
        if (!curWanted) return;
        std::vector<uint8_t> bb;
        BlockResult r = decodeBlock(curFec, c.data(), n0.data(), bb);
        if (!r.ok) {
            blocksBad++; decodeFails++;
            unpack.lost();
            if (l1.slices[(size_t)sliceIdx].type == 0 && decodeFails >= 8) { aligned = false; plpStartValid = false; stream.clear(); streamN0.clear(); decodeFails = 0; syncLosses++; }
            return;
        }
        decodeFails = 0;
        blocksOk++;
        preBerSum += r.preBer; preBerN++;
        // MER against the nearest constellation points of a few cells
        {
            PlpFec t; t.mod = curFec.mod;
            const int step = std::max(1, curCells / 200);
            double sp = 0, ep = 0;
            for (int i = 0; i < curCells; i += step) {
                const cf32 z = c[(size_t)i];
                float bd = 1e30f;
                // per axis: the nearest level (the constellations are square and separable)
                const int nb = curFec.mod + 1;
                const float norm = std::abs(cellPoint(curFec.mod, 0)) / std::abs(cf32(axisLevel(nb, 0), axisLevel(nb, 0)));
                float br = 1e30f, bi = 1e30f;
                for (unsigned l = 0; l < (1u << nb); l++) { const float v = axisLevel(nb, l) * norm; br = std::min(br, (z.real() - v) * (z.real() - v)); bi = std::min(bi, (z.imag() - v) * (z.imag() - v)); }
                bd = br + bi;
                sp += 1.0; ep += bd;
            }
            if (ep > 0) { merSum += 10 * std::log10(sp / ep); merN++; }
        }
        BbHeader h;
        if (!parseBbHeader(bb.data(), h)) { unpack.lost(); return; }
        if (h.tsGs == 2) {   // GSE
            const size_t bytes = (size_t)std::min(h.dfl, (int)bb.size() - 80) / 8;
            std::vector<uint8_t> field(bytes);
            for (size_t i = 0; i < bytes; i++) { unsigned v = 0; for (int q = 0; q < 8; q++) v = v << 1 | (bb[80 + i * 8 + (size_t)q] & 1); field[i] = (uint8_t)v; }
            if (!gse) gse = std::make_unique<dvbsip::GseIp>();
            std::vector<dvbsip::IpPacket> ips;
            gse->dataField(field.data(), field.size(), ips);
            gseFields++;
            for (const auto& ip : ips) { ipPackets++; lastPacketTime = now; if (ipCb) ipCb(ip.data.data(), ip.data.size()); }
            return;
        }
        unpack.push(bb, h);
        flushPackets();
    }
    void flushPackets() {
        if (pktBatch.empty()) return;
        const size_t n = pktBatch.size() / 188;
        packets += n;
        lastPacketTime = now;
        if (pktCb) pktCb(pktBatch.data(), n, pendingSecs);
        pendingSecs = 0;
        pktBatch.clear();
    }

    // ---------------------------------------------------------------- the symbol loop
    void process() {
        for (;;) {
            if (!ok) { bufPos = buf.size(); return; }
            if (state == Search) { if (!search()) return; continue; }
            std::vector<cf32> Y;
            if (!takeSymbol(Y)) return;
            if (state == Timing) {
                const bool pre = pilotPhaseRatio(Y) < kPilotPhaseRatio;
                if (pre && !prevPreambleLike) {
                    bool got = false;
                    if (haveKBase) {
                        std::vector<cf32> prod((size_t)kFft, cf32(0, 0));
                        for (int b = 6; b < kFft; b++) prod[(size_t)b] = Y[(size_t)b] * std::conj(Y[(size_t)(b - 6)]);
                        got = kBaseMetric(Y, kBase, prod) >= kKBaseThreshold;
                    } else got = findKBase(Y);
                    if (got) {
                        preambleMiss = 0;
                        if (preambleSymbol(Y, 0)) { symIndex = 1; state = Preamble; stage = "decoding the L1 signalling"; finishPreambleIfDone(); prevPreambleLike = true; continue; }
                    }
                }
                prevPreambleLike = pre;
                if (++preambleMiss > 3 * (kDataSymbols + 8)) { log("DVB-C2: no preamble found in three frames: searching again"); state = Search; preambleMiss = 0; haveKBase = false; }
                continue;
            }
            if (state == Preamble) {
                if (!preambleSymbol(Y, symIndex)) { l1Bad++; state = Timing; prevPreambleLike = true; stage = "looking for the preamble"; continue; }
                symIndex++;
                finishPreambleIfDone();
                continue;
            }
            // Data
            if (symIndex >= preambleSyms + kDataSymbols) {
                // this symbol must be the next frame's first preamble symbol
                frames++;
                if (pilotPhaseRatio(Y) < kPilotPhaseRatio && preambleSymbol(Y, 0)) { symIndex = 1; state = Preamble; finishPreambleIfDone(); }
                else { syncLosses++; l1Bad++; state = Timing; symIndex = 0; prevPreambleLike = false; stage = "looking for the preamble"; log("DVB-C2: the preamble did not decode: looking for the next frame"); }
                continue;
            }
            dataSymbol(Y, symIndex - preambleSyms);
            symIndex++;
        }
    }
    void finishPreambleIfDone() {
        if (state != Preamble || symIndex < preambleSyms) return;
        L1Part2 nl;
        L1DecodeResult r = decodeL1(hdr, l1Rows, l1N0, nl);
        if (!r.ok) {
            l1Bad++;
            if (l1Ok) { state = Data; symIndex = preambleSyms; frameStreamPos = 0; plpStartValid = false; if (!aligned) { stream.clear(); streamN0.clear(); } return; }   // keep the old L1 (Type 1 alignment waits for a good one)
            state = Timing; prevPreambleLike = true; stage = "looking for the preamble (L1 did not decode)";
            return;
        }
        applyL1(nl);
        if (state == Search) return;
        state = Data;
        symIndex = preambleSyms;
        frameStreamPos = 0;
        if (l1.slices.size() > (size_t)sliceIdx && sliceIdx >= 0 && l1.slices[(size_t)sliceIdx].type == 0 && !aligned) { stream.clear(); streamN0.clear(); }
    }

    // ---------------------------------------------------------------- telemetry
    void publish() {
        std::lock_guard<std::mutex> lk(telMu);
        Dvbc2Telemetry& t = tel;
        t.seq = ++seq;
        t.state = state == Search ? 0 : state == Timing ? 1 : state == Preamble ? 2 : (sliceOk ? 4 : 3);
        t.cfoHz = cfoHz + (centerHz > 0 && haveKBase ? (double)(std::llround(centerHz / kCarrierSpacingHz) - (kBase + kFft / 2)) * kCarrierSpacingHz : 0.0);
        t.snrDb = sigma2 > 0 && sigPower > 0 ? (float)(10 * std::log10(sigPower / sigma2)) : 0.f;
        t.dataValid = lastPacketTime >= 0 && now - lastPacketTime < 2.0;
        t.blocksOk = blocksOk; t.blocksBad = blocksBad;
        t.guardInterval = state == Search ? -1 : gi;
        t.clockPpm = clockInt + clockPpm;
        t.windowCarrier = haveKBase ? kBase + kFft / 2 : 0;
        t.l1Ok = l1Ok; t.l1 = l1;
        t.frames = frames; t.l1Good = l1Good; t.l1Bad = l1Bad; t.syncLosses = syncLosses;
        t.plps.clear();
        for (auto& kv : rows) t.plps.push_back(kv.second);
        t.selectedSlice = l1Ok && sliceIdx >= 0 ? l1.slices[(size_t)sliceIdx].id : -1;
        t.selectedPlp = l1Ok && sliceIdx >= 0 && plpIdx >= 0 ? l1.slices[(size_t)sliceIdx].plps[(size_t)plpIdx].id : -1;
        t.merDb = merN ? merSum / merN : 0;
        t.preBer = preBerN ? preBerSum / preBerN : 0;
        if (merN > 256) { merSum /= 2; merN /= 2; }
        if (preBerN > 256) { preBerSum /= 2; preBerN /= 2; }
        t.packets = packets;
        t.ipPackets = ipPackets; t.gseFields = gseFields;
        if (now - timeAtMark > 1.0) { netMbps = (double)(packets - packetsAtMark) * 188 * 8 / (now - timeAtMark) / 1e6; packetsAtMark = packets; timeAtMark = now; }
        t.netMbps = netMbps;
        for (auto& p : t.plps) if (p.sliceId == t.selectedSlice && p.plpId == t.selectedPlp) p.bitrate = netMbps * 1e6;
        t.headerMetric = lastHeaderMetric;
        t.stage = stage;
        if (!cellPic.empty()) { t.cells = cellPic; cellPic.clear(); }
        if (psdBuf.size() >= 1024) {
            Fft f(1024);
            std::vector<cf32> v(psdBuf.begin(), psdBuf.begin() + 1024);
            for (int i = 0; i < 1024; i++) v[(size_t)i] *= 0.5f * (1.f - std::cos(2 * (float)math::kPi * i / 1024));
            f.forward(v.data());
            std::vector<float> p(1024);
            float mx = -1e30f;
            for (int i = 0; i < 1024; i++) { p[(size_t)((i + 512) % 1024)] = 10 * std::log10(std::norm(v[(size_t)i]) + 1e-20f); mx = std::max(mx, p[(size_t)((i + 512) % 1024)]); }
            for (auto& x : p) x -= mx;
            t.psdDb = p; t.psdSpanHz = inRate;
            psdBuf.clear();
        }
        t.dropped = dropped.load();
    }
};

Dvbc2Receiver::Dvbc2Receiver() : p_(new Impl) {}
Dvbc2Receiver::~Dvbc2Receiver() { p_->join(); }
void Dvbc2Receiver::configure(double rate) { p_->configure(rate); }
bool Dvbc2Receiver::ready() const { return p_->ok; }
void Dvbc2Receiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->queue.clear();
    p_->doReset = true;
}
void Dvbc2Receiver::feed(const cf32* x, size_t n) {
    if (!p_->ok || n == 0) return;
    std::unique_lock<std::mutex> lk(p_->mu);
    if (p_->queue.size() >= kMaxQueue) {
        if (p_->blocking) p_->idleCv.wait(lk, [&] { return p_->queue.size() < kMaxQueue || p_->stop; });
        else { p_->dropped += n; return; }
    }
    p_->queue.emplace_back(x, x + n);
    lk.unlock();
    p_->cv.notify_one();
}
bool Dvbc2Receiver::telemetry(Dvbc2Telemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}
void Dvbc2Receiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void Dvbc2Receiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { p_->pktCb = std::move(cb); }
void Dvbc2Receiver::setIpCallback(std::function<void(const uint8_t*, size_t)> cb) { p_->ipCb = std::move(cb); }
void Dvbc2Receiver::setSignalOffset(double hz) { p_->sigOffset = hz; }
void Dvbc2Receiver::setCenterHz(double hz) { p_->centerHz = hz; }
void Dvbc2Receiver::selectPlp(int sliceId, int plpId) { p_->wantSlice = sliceId; p_->wantPlp = plpId; }
void Dvbc2Receiver::setBlocking(bool b) { p_->blocking = b; }
void Dvbc2Receiver::flush() {
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->idleCv.wait(lk, [&] { return (p_->queue.empty() && !p_->busy) || p_->stop; });
}

} // namespace dect2
