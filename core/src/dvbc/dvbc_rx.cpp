// DVB-C receiver: spectrum -> carrier offset and symbol rate, interpolating matched filter + Gardner timing loop (2 samples a symbol),
// fractionally spaced equaliser (CMA, then decision directed LMS inside a carrier loop), QAM order and spectral inversion tried until the transport
// stream sync bytes appear, differential decoding of the quadrant, then the DVB-T outer decoder (de-interleaver, Reed-Solomon, energy dispersal).
#include "dect2/common/stage_prof.h"
#include "dect2/common/thread_name.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbc/j83b.h"
#include "dect2/dvbc/docsis_down.h"
#include "dect2/dvbc/dvbc.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <complex>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
#if defined(__GNUC__) && !defined(__clang__) && defined(__x86_64__) && defined(__linux__)
#define DVBC_HOT __attribute__((target_clones("arch=haswell", "default")))   // the loops that set the CPU load: use AVX2 and FMA where the CPU has them
#else
#define DVBC_HOT
#endif
constexpr int kTaps = 28;                 // equaliser, half-symbol spacing: 20 symbols
constexpr int kMain = kTaps - 1 - 8;      // the main tap: 4 symbols of pre-cursor
constexpr int kPhases = 256;
constexpr size_t kAcqN = 65536, kAcqBlocks = 4;
constexpr uint64_t kCmaSyms = 400000, kDdSyms = 60000;
constexpr size_t kSyncBlocks = 20;
constexpr int kResyncBad = 12;   // consecutive uncorrectable blocks that start a byte re-alignment (a slipped symbol clock or a gap in the samples shifts the bytes, the carrier is still fine)
constexpr size_t kPhBlock = 1024, kPhMargin = 16;   // phase smoothing: symbols per block, and the overlap on each side
constexpr int kPhHalf = 5;

const std::vector<uint8_t>& prbsKeys() {
    static const std::vector<uint8_t> ks = [] {
        std::vector<uint8_t> t(8 * 188, 0);
        unsigned reg = 0xA9;
        auto clock8 = [&] {
            unsigned res = 0;
            for (int i = 0; i < 8; i++) { const unsigned fb = ((reg >> 13) ^ (reg >> 14)) & 1; reg = ((reg << 1) | fb) & 0x7FFF; res = (res << 1) | fb; }
            return (uint8_t)res;
        };
        for (int p = 0; p < 8; p++) { for (int j = 1; j < 188; j++) t[(size_t)p * 188 + (size_t)j] = clock8(); clock8(); }
        return t;
    }();
    return ks;
}

// sync bytes at 204-byte spacing in a bit stream (one bit per byte): returns the bit offset of the first sync byte, -1 when not found
long findSync(const std::vector<uint8_t>& bits) {
    const size_t nbytes = bits.size() / 8;
    if (nbytes < 204 * kSyncBlocks + 8) return -1;
    const size_t blocks = (nbytes - 8) / 204;
    std::vector<uint8_t> by(nbytes);
    long best = -1; int bestScore = 0;
    for (int off = 0; off < 8; off++) {
        for (size_t i = 0; i + 1 < nbytes; i++) { int v = 0; for (int b = 0; b < 8; b++) v = (v << 1) | bits[(size_t)off + 8 * i + (size_t)b]; by[i] = (uint8_t)v; }
        for (size_t p = 0; p < 204; p++) {
            int sc = 0, b8 = 0;
            for (size_t j = 0; j < blocks; j++) { const uint8_t v = by[p + 204 * j]; sc += v == 0x47 || v == 0xB8; b8 += v == 0xB8; }
            if (b8 == 0) continue;
            if (sc > bestScore) { bestScore = sc; best = off + 8 * (long)p; }
        }
    }
    return bestScore * 10 >= (int)blocks * 8 ? best : -1;
}
} // namespace

struct DvbcReceiver::Impl {
    // settings
    double fs = 10e6;
    std::atomic<double> manualRs{0};
    std::atomic<int> qamSet{-1};
    std::atomic<bool> annexC{false}, blocking{false};
    std::atomic<bool> annexB{false};              // ITU-T J.83 annex B (clear QAM): its own FEC chain (j83b.h) behind the same demodulator
    std::unique_ptr<j83b::J83bDecoder> j83[2] = {std::make_unique<j83b::J83bDecoder>(), std::make_unique<j83b::J83bDecoder>()};                     // the decoders of the two hypotheses of the spectrum's direction while trying, [0] once locked
    std::vector<cf32> j83Trial, j83Block;
    DocsisDownstream docsis;                      // the DOCSIS management messages on PID 0x1FFE, when this is a cable modem carrier
    uint64_t j83BadSeen = 0, j83FramesSeen = 0;
    std::mutex docMu;
    std::function<void(const std::string&)> logCb;
    std::function<void(const uint8_t*, size_t, double)> pktCb;

    // worker
    std::thread th;
    std::mutex qmu;
    std::condition_variable qcv, idleCv;
    std::deque<std::vector<cf32>> q;
    size_t queued = 0;
    bool stop = false, busy = false, resetReq = false;
    std::atomic<uint64_t> dropped{0};
    uint64_t droppedLogged = 0, droppedLogAt = 0;

    // telemetry
    std::mutex tmu;
    DvbcTelemetry tel;
    uint64_t seq = 0;

    // ---- signal processing state (worker only)
    enum Stage { Acq, Cma, Freq, Trial, Locked } stage = Acq;
    uint64_t processed = 0;                // input samples
    std::vector<cf32> acq;
    double cfoCoarse = 0, rs = 0, rsMeasured = 0, width = 0, rsNominal = 0;
    bool usedManual = false;
    int failCycles = 0;
    std::vector<double> rateCands;
    std::string note;
    std::vector<float> psd;
    // front end
    double ncoPh = 0, ncoStep = 0;
    std::complex<double> dcI{0, 0};      // the radio's DC offset (a HackRF's LO leak sits in the middle of a centred channel)
    static constexpr double dcA = 1e-4;   // a one-pole DC blocker: 320 Hz at 20 Msps, well inside the carrier's flat top
    std::vector<cf32> hb;                  // mixed input history
    double tau = 0;                        // next output time, in hb samples
    double sps = 1.44;
    const dvbc::Constellation* cnP = nullptr; int cnQam = -1;
    const dvbc::Constellation& cn() { if (cnQam != qam) { cnP = &dvbc::constellation(qam); cnQam = qam; } return *cnP; }   // the constellation of the order in use, looked up once
    int K = 12;
    std::vector<float> taps;
    double tnu = 0;                        // timing loop: relative rate correction
    bool strobe = true;
    cf32 prevStrobe{0, 0}, mid{0, 0};
    double agcP = 0; uint64_t agcN = 0;
    float lastTed = 0; double tedAvg = 0;
    // equaliser
    cf32 u[2 * kTaps];
    int upos = 0, adaptTick = 0;
    cf32 w[kTaps], wCma[kTaps];
    uint64_t syms = 0, stageSyms = 0;
    double theta = 0, nu = 0;              // carrier loop (radians, radians per symbol)
    double nu0 = 0, theta0 = 0;
    std::vector<cf32> zbuf;                // CMA outputs for the frequency estimate
    int qam = 2;
    std::vector<int> trialOrders;
    size_t trialIdx = 0;
    bool trialInv = false, inv = false;
    dvbc::Demapper dm[2];
    std::vector<uint8_t> tb[2];            // trial bit streams: as is, conjugated
    double errP = 0, sigP = 1;
    std::deque<float> merHist;
    std::vector<cf32> cells; size_t cellPos = 0;
    // outer decoder
    std::vector<uint8_t> bits, after;
    dvbt::ConvInterleaver deint{true};
    int warm = 0, groupIdx = 0, consecBad = 0;
    bool resync = false;                   // looking for the 204-byte frame again in the bits that follow
    std::vector<uint8_t> rsBits;
    std::atomic<uint64_t> resyncs{0};
    bool haveGroup = false;
    uint64_t rsClean = 0, rsCorr = 0, rsFail = 0, packets = 0, packetsBad = 0, reacq = 0;
    double byteErr = 0;
    std::vector<uint8_t> out;
    uint64_t nextTel = 0;
    uint64_t lockedAt = 0;

    void log(const std::string& s) { if (logCb) logCb("DVB-C: " + s); }

    void hardReset() {
        backStop(); lostFlag = false;
        std::lock_guard<std::mutex> bl(bmu);
        resync = false; rsBits.clear();
        stage = Acq; acq.clear(); hb.clear(); tau = 0; tnu = 0; strobe = true; agcP = 0; agcN = 0;
        dcI = {0, 0}; ncoPh = ncoStep = 0; prevStrobe = mid = cf32(0, 0); tedAvg = 0; cfoCoarse = 0; theta = nu = 0;   // nothing numeric survives a reset (a NaN here would poison every later lock)
        syms = stageSyms = 0; zbuf.clear(); bits.clear(); after.clear(); out.clear(); failCycles = 0; usedManual = false; note.clear();
        rsClean = rsCorr = rsFail = packets = packetsBad = 0; cells.clear(); merHist.clear(); errP = 0;
        tb[0].clear(); tb[1].clear();
    }

    // ---------------------------------------------------------------- acquisition
    void analyse() {
        // spectrum for the edges and the picture
        const int N = 4096;
        Fft f(N);
        std::vector<double> p((size_t)N, 0);
        std::vector<cf32> b((size_t)N);
        const size_t nb = acq.size() / N;
        for (size_t k = 0; k < nb; k++) {
            for (int i = 0; i < N; i++) { const float wnd = 0.5f - 0.5f * std::cos(2 * (float)kPi * i / N); b[(size_t)i] = acq[k * N + (size_t)i] * wnd; }
            f.forward(b.data());
            for (int i = 0; i < N; i++) p[(size_t)((i + N / 2) % N)] += std::norm(b[(size_t)i]);
        }
        {   // the radio's DC spike (a HackRF's LO leaks through, and a recording keeps it) would set the plateau and the width: bridge it
            double side = 0;
            for (int j = 4; j <= 8; j++) side += p[(size_t)(N / 2 - j)] + p[(size_t)(N / 2 + j)];
            side /= 10;
            if (p[(size_t)N / 2] > 4 * side) for (int j = -3; j <= 3; j++) p[(size_t)(N / 2 + j)] = side;
        }
        std::vector<double> sm((size_t)N, 0);
        for (int i = 0; i < N; i++) { double s = 0; int c = 0; for (int j = -16; j <= 16; j++) if (i + j >= 0 && i + j < N) { s += p[(size_t)(i + j)]; c++; } sm[(size_t)i] = 10 * std::log10(s / c + 1e-30); }
        const double mx = *std::max_element(sm.begin() + N / 20, sm.end() - N / 20), mn = *std::min_element(sm.begin(), sm.end());
        psd.assign((size_t)N / 4, 0);
        for (int i = 0; i < N; i++) psd[(size_t)i / 4] = (i % 4 == 0) ? (float)(sm[(size_t)i] - mx) : std::max(psd[(size_t)i / 4], (float)(sm[(size_t)i] - mx));
        if (mx - mn < 6) { note = "no carrier in the spectrum"; return; }
        // width and centre from the area of the carrier over the floor: an RRC carrier covers exactly the symbol rate at its plateau level,
        // also when an echo puts ripple on it
        const double floorLin = std::pow(10, mn / 10);
        std::vector<double> ex((size_t)N, 0), top;
        double area = 0, mom = 0;
        for (int i = 0; i < N; i++) {
            if (sm[(size_t)i] < mn + 6) continue;
            ex[(size_t)i] = std::pow(10, sm[(size_t)i] / 10) - floorLin;
            area += ex[(size_t)i]; mom += ex[(size_t)i] * i;
            if (sm[(size_t)i] > mx - 10) top.push_back(ex[(size_t)i]);
        }
        std::sort(top.begin(), top.end());
        double plateau = 0;
        for (size_t k = top.size() / 4; k < top.size() * 3 / 4; k++) plateau += top[k];
        plateau /= std::max<size_t>(1, top.size() * 3 / 4 - top.size() / 4);
        const double binHz = fs / N;
        width = area / (plateau + 1e-30) * binHz;
        cfoCoarse = (mom / (area + 1e-30) - N / 2) * binHz;
        {   // with the neighbouring cable channels in the input their skirts add to the area: then the -10 dB edges of the carrier itself,
            // walking out from its centre (an echo's ripple stays well above -10 dB; the notch between two channels goes below). A raised
            // cosine is 10 dB down at (1 + 1.59 roll-off) / 2 of the symbol rate from its centre.
            const double rollOff = annexC.load() ? 0.13 : 0.15, k10 = 1 - rollOff + 2 * rollOff * std::acos(-0.8) / kPi;
            const double half = 10 * std::log10(plateau + floorLin) - 10;
            const int c0 = std::max(1, std::min(N - 2, (int)std::lround(N / 2 + cfoCoarse / binHz)));
            if (sm[(size_t)c0] > half) {
                auto edge = [&](int dir) {
                    int i = c0, below = 0, last = c0;
                    while (i + dir > 0 && i + dir < N - 1 && below < 24) { i += dir; if (sm[(size_t)i] > half) { below = 0; last = i; } else below++; }
                    const double a = sm[(size_t)last], b = sm[(size_t)(last + dir)];   // interpolate the crossing between last and the bin after it
                    return last + dir * (a - half) / std::max(1e-9, a - b);
                };
                const double lo = edge(-1), hi = edge(+1), w10 = (hi - lo) * binHz / k10;
                if (w10 > 0.3e6 && w10 < 0.92 * width) { width = w10; cfoCoarse = ((lo + hi) / 2 - N / 2) * binHz; }
            }
        }
        // the symbol rate line of the squared envelope
        Fft g((int)kAcqN);
        std::vector<double> pe(kAcqN, 0);
        std::vector<cf32> e(kAcqN);
        for (size_t k = 0; k < kAcqBlocks; k++) {
            double mean = 0;
            for (size_t i = 0; i < kAcqN; i++) mean += std::norm(acq[k * kAcqN + i]);
            mean /= kAcqN;
            for (size_t i = 0; i < kAcqN; i++) e[i] = cf32((float)(std::norm(acq[k * kAcqN + i]) - mean), 0);
            g.forward(e.data());
            for (size_t i = 0; i < kAcqN; i++) pe[i] += std::norm(e[i]);
        }
        const double binE = fs / kAcqN;
        rsMeasured = 0;
        // lines of the squared envelope: a line stands out of its neighbourhood (an echo puts bumps on it too, so every strong line is a candidate,
        // in the order of how well its alias fits the width of the carrier; a candidate that does not lock gives way to the next)
        {
            const int b0 = (int)(0.3e6 / binE), b1 = (int)kAcqN / 2 - 2;
            std::vector<double> cs((size_t)kAcqN / 2 + 1, 0);
            for (size_t i = 0; i < kAcqN / 2; i++) cs[i + 1] = cs[i] + pe[i];
            const int R = 256;
            std::vector<double> ratio((size_t)kAcqN / 2, 0);
            for (int i = b0; i < b1; i++) {
                const int l = std::max(0, i - R), h = std::min((int)kAcqN / 2, i + R);
                const double bg = (cs[(size_t)h] - cs[(size_t)l] - pe[(size_t)i]) / (h - l - 1);
                ratio[(size_t)i] = pe[(size_t)i] / (bg + 1e-30);
            }
            std::vector<std::pair<double, double>> lines;   // ratio, frequency
            for (int i = b0 + 1; i < b1 - 1; i++) {
                if (ratio[(size_t)i] < 30 || pe[(size_t)i] < pe[(size_t)i - 1] || pe[(size_t)i] < pe[(size_t)i + 1]) continue;
                const double a = std::sqrt(pe[(size_t)i - 1]), m = std::sqrt(pe[(size_t)i]), cc = std::sqrt(pe[(size_t)i + 1]);
                lines.push_back({ratio[(size_t)i], (i + 0.5 * (a - cc) / (a - 2 * m + cc)) * binE});
            }
            std::vector<std::pair<double, double>> fit;      // ratio, symbol rate
            for (const auto& ln : lines) for (double c : {ln.second, fs - ln.second, fs + ln.second})
                if (std::fabs(c - width) < 0.2 * width && c < fs * 0.95) fit.push_back({ln.first, c});
            // a rate operators use (within 300 ppm, a transmitter clock is better than that) goes first
            static const double kStd[] = {6.952e6, 6.950e6, 6.900e6, 6.875e6, 6.885e6, 6.111e6, 6.000e6, 5.900e6, 5.361e6, 5.274e6, 5.217e6, 5.057e6, 4.000e6, 3.450e6};
            for (auto& f : fit) for (double r : kStd) if (std::fabs(f.second / r - 1) < 3e-4) f.first *= 1000;
            std::sort(fit.rbegin(), fit.rend());
            rateCands.clear();
            for (size_t k = 0; k < fit.size() && k < 8; k++) rateCands.push_back(fit[k].second);
            // then the usual rates near the width (a line can fade under an echo), then the width itself
            std::vector<double> nearBy;
            for (double r : kStd) if (std::fabs(r - width) < 0.15 * width && r < fs * 0.95) nearBy.push_back(r);
            std::sort(nearBy.begin(), nearBy.end(), [&](double x, double y) { return std::fabs(x - width) < std::fabs(y - width); });
            for (double r : nearBy) if (std::none_of(rateCands.begin(), rateCands.end(), [&](double c) { return std::fabs(c / r - 1) < 3e-4; })) rateCands.push_back(r);
            if (rateCands.empty()) rateCands.push_back(width);
        }
        const double man = manualRs.load();
        if (man > 0 && (failCycles < 2 || failCycles % 2 == 0)) {   // a rate the user set (from the operator's channel list) keeps coming back:
            rs = man; usedManual = true; rsMeasured = rateCands[0];        // a bad signal is more likely than a wrong list
            if (failCycles >= 2) note = std::fabs(rsMeasured / man - 1) > 0.01 ? "no lock at the manual symbol rate (the measured one differs: check it)"
                                                                               : "no lock at the manual symbol rate: the signal may be too weak or noisy for this QAM order";
        } else {   // otherwise the measured candidates in turn (with a manual rate: every other cycle, in case it was typed wrong)
            usedManual = false;
            rs = rsMeasured = rateCands[(size_t)(man > 0 ? (failCycles - 2) / 2 : failCycles) % rateCands.size()];
        }
        rsNominal = rs;
        if (rs < 0.5e6 || rs > fs * 0.95) { note = "symbol rate not usable for this input rate"; return; }
        startFront();
        char m[200];
        snprintf(m, sizeof m, "carrier %+.1f kHz, -3 dB width %.3f MHz, symbol rate line %.4f Msym/s, using %.4f Msym/s%s", cfoCoarse / 1e3, width / 1e6, rsMeasured / 1e6, rs / 1e6, usedManual ? " (manual)" : "");
        log(m);
        stage = Cma; stageSyms = 0;
    }

    void startFront() {
        const double beta = annexB.load() ? (qam == 4 ? j83b::kRollOff256 : j83b::kRollOff64) : annexC.load() ? 0.13 : 0.15;
        sps = fs / rs;
        K = (int)std::ceil(8 * sps);
        taps.assign((size_t)kPhases * 2 * (size_t)K, 0);
        for (int p = 0; p < kPhases; p++) for (int j = 0; j < 2 * K; j++) taps[(size_t)p * 2 * (size_t)K + (size_t)j] = (float)dvbc::rrc(((double)p / kPhases + K - 1 - j) / sps, beta);
        ncoStep = -2 * kPi * cfoCoarse / fs;
        hb.clear(); tau = K; tnu = 0; strobe = true; agcP = 0; agcN = 0; tedAvg = 0;
        for (auto& x : u) x = 0;
        upos = 0;
        for (auto& x : w) x = 0;
        w[kMain] = 1;
        syms = 0; zbuf.clear(); phBuf.clear();
        theta = nu = 0;
    }

    // ---------------------------------------------------------------- per symbol
    DVBC_HOT
    void onSymbol() {
        const cf32* win = &u[upos];
        float zr_[2] = {0, 0}, zi_[2] = {0, 0};
        for (int j = 0; j < kTaps; j += 2)
            for (int q = 0; q < 2; q++) { zr_[q] += w[j + q].real() * win[j + q].real() - w[j + q].imag() * win[j + q].imag(); zi_[q] += w[j + q].real() * win[j + q].imag() + w[j + q].imag() * win[j + q].real(); }
        const cf32 z(zr_[0] + zr_[1], zi_[0] + zi_[1]);
        syms++; stageSyms++;
        const float mu = stageSyms < 120000 ? 4e-4f : stageSyms < 250000 ? 1e-4f : 3e-5f;
        if (stage == Cma || stage == Freq) {
            const float r2 = 1.36f;
            const cf32 e = z * (std::norm(z) - r2);
            if (stage == Cma) for (int j = 0; j < kTaps; j++) w[j] -= mu * e * std::conj(win[j]);   // frozen while the frequency is measured: CMA lets the phase wander
            if (stage == Cma && stageSyms >= kCmaSyms) { stage = Freq; stageSyms = 0; zbuf.clear(); }
            else if (stage == Freq) {
                zbuf.push_back(z);
                if (zbuf.size() == 4 * 16384) estimateFreq();
            }
            return;
        }
        // decision directed
        const cf32 rot(std::cos((float)theta), -std::sin((float)theta));
        const cf32 zr = z * rot;
        const dvbc::Constellation& c = cn();
        const int sym = c.decide(zr);
        const cf32 d = c.pts[(size_t)sym];
        const cf32 err = zr - d;
        const float mud = stage == Locked ? 1.5e-4f : 3e-4f;
        const cf32 eb = err * std::conj(rot);
        if (stage != Locked || (adaptTick ^= 1)) {   // locked: the equaliser has converged, so adapt every second symbol with twice the step
            const cf32 step = (stage == Locked ? 2 * mud : mud) * eb;
            for (int j = 0; j < kTaps; j++) w[j] -= step * std::conj(win[j]);
        }
        const double ph = (double)(zr * std::conj(d)).imag() / (std::norm(d) + 1e-9);
        theta += nu + 0.01 * ph;
        nu += 1e-5 * ph;
        if (theta > kPi) theta -= 2 * kPi; else if (theta < -kPi) theta += 2 * kPi;
        phBuf.push_back(zr);
        if (phBuf.size() >= kPhBlock + 2 * kPhMargin) { if (stage == Locked) offload(); else smoothPhase(); }
    }

    // The loop above follows the carrier only as fast as its decisions allow; a HackRF's synthesiser leaves phase noise out to a few hundred kHz
    // (a bump near 100 kHz), which on 256-QAM costs 2 dB of MER. Here the phase of every symbol is measured again from its neighbours'
    // decisions (both sides, the symbol itself left out), a few times over, before the symbols are decided for the outer decoder.
    std::vector<cf32> phBuf;
    struct Smooth { std::vector<cf32> phOut; std::vector<float> pR, pI, gR, gI, oR, oI; std::vector<int> cell; };
    Smooth smM;   // the receiver thread's scratch; the outer-decoder thread has its own
    DVBC_HOT
    void smooth(Smooth& sm, const std::vector<cf32>& in, const dvbc::Constellation& c) {
        auto& pR = sm.pR; auto& pI = sm.pI; auto& gR = sm.gR; auto& gI = sm.gI; auto& oR = sm.oR; auto& oI = sm.oI; auto& phOut = sm.phOut;
        const size_t n = in.size();
        // separate real / imaginary arrays: the FIR and the rotation below then vectorise
        for (auto* v : {&pR, &pI, &gR, &gI, &oR, &oI}) v->resize(n);
        sm.cell.resize(n);
        int* __restrict cell = sm.cell.data();
        float* __restrict PR = pR.data(); float* __restrict PI = pI.data();
        float* __restrict GR = gR.data(); float* __restrict GI = gI.data();
        float* __restrict OR = oR.data(); float* __restrict OI = oI.data();
        for (size_t i = 0; i < n; i++) { PR[i] = OR[i] = in[i].real(); PI[i] = OI[i] = in[i].imag(); }
        static const float kw[kPhHalf] = {0.97f, 0.88f, 0.75f, 0.59f, 0.41f};   // a Hann window, the centre left out
        const size_t H = kPhHalf;
        for (int it = 0; it < 3; it++) {
            for (size_t i = 0; i < n; i++) cell[i] = c.cellOf(OR[i], OI[i]);   // the decision's arithmetic in a loop of its own: that part vectorises
            for (size_t i = 0; i < n; i++) { const cf32 d = c.pts[(size_t)c.lut[(size_t)cell[i]]]; GR[i] = PR[i] * d.real() + PI[i] * d.imag(); GI[i] = PI[i] * d.real() - PR[i] * d.imag(); }
            auto rotate = [&](size_t i, float ar, float ai) {
                const float a2 = ar * ar + ai * ai;
                const float g = a2 > 0 ? 1.f / std::sqrt(a2) : 0.f;
                OR[i] = a2 > 0 ? (PR[i] * ar + PI[i] * ai) * g : PR[i];
                OI[i] = a2 > 0 ? (PI[i] * ar - PR[i] * ai) * g : PI[i];
            };
            for (size_t i = 0; i < n && i < H; i++) {   // the edges, with bounds checks
                float ar = 0, ai = 0;
                for (size_t k = 1; k <= H; k++) { if (i >= k) { ar += kw[k - 1] * GR[i - k]; ai += kw[k - 1] * GI[i - k]; } if (i + k < n) { ar += kw[k - 1] * GR[i + k]; ai += kw[k - 1] * GI[i + k]; } }
                rotate(i, ar, ai);
            }
            for (size_t i = n > H ? std::max(H, n - H) : n; i < n; i++) {
                float ar = 0, ai = 0;
                for (size_t k = 1; k <= H; k++) { if (i >= k) { ar += kw[k - 1] * GR[i - k]; ai += kw[k - 1] * GI[i - k]; } if (i + k < n) { ar += kw[k - 1] * GR[i + k]; ai += kw[k - 1] * GI[i + k]; } }
                rotate(i, ar, ai);
            }
            for (size_t i = H; i + H < n; i++) {   // the middle: no bounds checks
                float ar = 0, ai = 0;
                for (size_t k = 1; k <= H; k++) { ar += kw[k - 1] * (GR[i - k] + GR[i + k]); ai += kw[k - 1] * (GI[i - k] + GI[i + k]); }
                rotate(i, ar, ai);
            }
        }
        phOut.resize(n);
        for (size_t i = 0; i < n; i++) phOut[i] = cf32(OR[i], OI[i]);
    }

    void smoothPhase() {
        smooth(smM, phBuf, cn());
        for (size_t i = kPhMargin; i < kPhMargin + kPhBlock; i++) decided(smM.phOut[i]);
        if (stage == Acq || phBuf.empty()) return;   // decided() may have restarted the trial
        phBuf.erase(phBuf.begin(), phBuf.begin() + (std::ptrdiff_t)kPhBlock);
    }

    // ---------------------------------------------------------------- second thread: smoothing, decisions and the outer decoder once locked
    // The receiver thread (equaliser, carrier and timing loops) hands blocks of equalised symbols over; this thread smooths their phase, decides
    // them and runs the Reed-Solomon decoder. It owns bits, after, deint, the counters, errP / sigP and the constellation cells while locked;
    // the receiver thread touches those only after backStop(), or with bmu held (publish).
    struct Job { std::vector<cf32> blk; const dvbc::Constellation* c; double blockSecs; uint64_t gen; };   // blockSecs: the time the block's symbols span
    std::thread bth;
    std::mutex jmu, bmu;
    std::condition_variable jcv, jidle;
    std::deque<Job> jobs;
    bool bstop = false, bbusy = false;
    std::atomic<uint64_t> gen{0};
    std::atomic<bool> lostFlag{false};
    Smooth smB;
    uint64_t resyncsLogged = 0;
    double pendSecs = 0;   // stream time handed over since the last packet callback: the callback is told how long the packets span, not when they are

    void offload() {
        Job j{phBuf, &cn(), (double)kPhBlock * (1 + tnu) / rs, gen.load()};
        phBuf.erase(phBuf.begin(), phBuf.begin() + (std::ptrdiff_t)kPhBlock);
        std::unique_lock<std::mutex> lk(jmu);
        jcv.wait(lk, [&] { return jobs.size() < 48 || bstop; });   // the outer thread is far lighter than this one: this never waits for long
        jobs.push_back(std::move(j));
        jcv.notify_all();
    }
    void backStop() {   // drop what is queued, wait for the job in flight: afterwards the receiver thread owns everything again
        std::unique_lock<std::mutex> lk(jmu);
        jobs.clear(); gen++;
        jcv.notify_all();
        jidle.wait(lk, [&] { return !bbusy; });
    }
    void backIdle() {
        std::unique_lock<std::mutex> lk(jmu);
        jidle.wait(lk, [&] { return (jobs.empty() && !bbusy) || bstop; });
    }
    void lockedDecided(cf32 zr, const dvbc::Constellation& c, int cell) {   // cell: c.cellOf(zr), computed for the whole block in one vectorised loop
        const int sym = c.lut[(size_t)cell];
        const cf32 d = c.pts[(size_t)sym];
        errP = 0.999 * errP + 0.001 * std::norm(zr - d);
        sigP = 0.999 * sigP + 0.001 * std::norm(d);
        if (cells.size() < 2048) cells.push_back(zr); else { cells[cellPos] = zr; cellPos = (cellPos + 1) % 2048; }
        dm[0].push(inv ? c.decide(std::conj(zr)) : sym, bits);
    }
    void backRun() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(jmu);
                jcv.wait(lk, [&] { return bstop || !jobs.empty(); });
                if (bstop) return;
                j = std::move(jobs.front()); jobs.pop_front();
                bbusy = true;
                jcv.notify_all();
            }
            if (j.gen == gen.load()) {
                std::vector<uint8_t> pk;
                {
                    std::lock_guard<std::mutex> bl(bmu);
                    DECT2_STAGE_VAR(sm, "dvbc.phase-smooth");
                    smooth(smB, j.blk, *j.c);
                    sm.stop();
                    DECT2_STAGE("dvbc.decide+rs");
                    for (size_t i = kPhMargin; i < kPhMargin + kPhBlock; i++) smB.cell[i] = j.c->cellOf(smB.phOut[i].real(), smB.phOut[i].imag());
                    for (size_t i = kPhMargin; i < kPhMargin + kPhBlock; i++) lockedDecided(smB.phOut[i], *j.c, smB.cell[i]);
                    if (annexB.load()) {
                        j83Block.resize(kPhBlock);
                        for (size_t i = 0; i < kPhBlock; i++) { const cf32 z = smB.phOut[i + kPhMargin]; j83Block[i] = inv ? std::conj(z) : z; }
                        j83[0]->feed(j83Block.data(), j83Block.size());
                        const j83b::DecStats& js = j83[0]->stats();
                        rsClean = js.rsClean; rsCorr = js.rsCorrected; rsFail = js.rsFailed; packets = js.packets;
                        // the trailer of a frame, more than a few times missing: the lock has gone
                        if (js.framesBad > j83BadSeen + 24) lostFlag = true;
                        if (js.frames > j83FramesSeen) { j83FramesSeen = js.frames; if (js.framesBad == j83BadSeen) j83BadSeen = js.framesBad; }
                    } else outer();
                    pk.swap(out);
                }
                pendSecs += j.blockSecs;
                if (!pk.empty() && annexB.load()) { std::lock_guard<std::mutex> bl2(docMu); docsis.feed(pk.data(), pk.size() / 188); }
                if (!pk.empty() && pktCb) { const double dt = pendSecs; pendSecs = 0; pktCb(pk.data(), pk.size() / 188, dt); }
            }
            {
                std::lock_guard<std::mutex> lk(jmu);
                bbusy = false;
                jidle.notify_all();
            }
        }
    }
    void trialCheckB() {
        for (int v = 0; v < 2; v++) {
            j83[v]->configure(qam == 4);
            j83[v]->setPacketCallback([this](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n * 188); });
            std::vector<cf32> buf(j83Trial);
            if (v) for (auto& z : buf) z = std::conj(z);
            if (!j83[v]->trial(buf.data(), buf.size())) continue;
            inv = v == 1;
            if (v == 1) std::swap(j83[0], j83[1]);
            j83[0]->setPacketCallback([this](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n * 188); });
            j83Trial.clear(); j83BadSeen = 0; j83FramesSeen = 0;
            stage = Locked; lockedAt = processed; failCycles = 0; pendSecs = 0; consecBad = 0;
            char m[160];
            const j83b::DecStats& st = j83[0]->stats();
            snprintf(m, sizeof m, "annex B %s locked, %.4f Msym/s, MER %.1f dB, interleaver I %d J %d%s", dvbc::qamName(qam), rs / (1 + tnu) / 1e6, mer(), st.I, st.J, inv ? ", spectrum inverted" : "");
            log(m);
            return;
        }
        j83Trial.clear();
        { char m[120]; snprintf(m, sizeof m, "annex B: no frame sync as %s, MER %.1f dB", dvbc::qamName(qam), mer()); log(m); }
        if (++trialIdx < trialOrders.size()) { startTrial(); return; }
        failCycles++; reacq++;
        stage = Acq; acq.clear(); phBuf.clear();
    }

    void handleLost() {
        backStop();
        log("lost the stream, searching again");
        reacq++;
        { std::lock_guard<std::mutex> bl(bmu); bits.clear(); after.clear(); haveGroup = false; resync = false; rsBits.clear(); }
        lostFlag = false; pendSecs = 0;
        stage = Cma; stageSyms = 0; phBuf.clear();
    }

    void decided(cf32 zr) {
        if (phBuf.empty()) return;   // a stage change dropped the block
        const dvbc::Constellation& c = cn();
        const int sym = c.decide(zr);
        const cf32 d = c.pts[(size_t)sym];
        errP = 0.999 * errP + 0.001 * std::norm(zr - d);
        sigP = 0.999 * sigP + 0.001 * std::norm(d);
        if (cells.size() < 2048) cells.push_back(zr); else { cells[cellPos] = zr; cellPos = (cellPos + 1) % 2048; }
        if (stage == Trial && annexB.load()) {
            if (stageSyms <= kDdSyms) return;
            j83Trial.push_back(zr);
            if (j83Trial.size() >= (qam == 4 ? 30000u : 34000u)) trialCheckB();
            return;
        }
        if (stage == Trial) {
            if (stageSyms > kDdSyms) {
                dm[0].push(sym, tb[0]);
                dm[1].push(c.decide(std::conj(zr)), tb[1]);
                if (tb[0].size() >= 8 * 204 * (kSyncBlocks + 2)) trialCheck();
            }
            return;
        }
        if (stage == Locked) dm[0].push(inv ? c.decide(std::conj(zr)) : sym, bits);
    }

    void estimateFreq() {
        const int N = 16384, B = 4;               // four blocks: the power spectra of the fourth power add up
        Fft f(N);
        std::vector<cf32> v((size_t)N);
        std::vector<float> pw((size_t)N, 0);
        for (int b = 0; b < B; b++) {
            for (int i = 0; i < N; i++) { const cf32 a = zbuf[(size_t)(b * N + i)] * zbuf[(size_t)(b * N + i)]; v[(size_t)i] = a * a; }
            f.forward(v.data());
            for (int i = 0; i < N; i++) pw[(size_t)i] += std::norm(v[(size_t)i]);
        }
        int bi = 0; float bv = 0;
        for (int i = 0; i < N; i++) if (pw[(size_t)i] > bv) { bv = pw[(size_t)i]; bi = i; }
        const double a = std::sqrt(pw[(size_t)((bi + N - 1) % N)]), m = std::sqrt(pw[(size_t)bi]), cc = std::sqrt(pw[(size_t)((bi + 1) % N)]);
        const double dd = 0.5 * (a - cc) / (a - 2 * m + cc);
        const double k = (bi > N / 2 ? bi - N : bi) + dd;
        nu0 = 2 * kPi * k / N / 4;                 // radians per symbol
        // phase of the fourth power at the start of the block, then forward to its end
        cf32 acc(0, 0);
        const size_t o = (size_t)(B - 1) * N;
        for (int i = 0; i < N; i++) { const cf32 a2 = zbuf[o + (size_t)i] * zbuf[o + (size_t)i]; acc += a2 * a2 * std::polar(1.f, (float)(-4 * nu0 * i)); }
        theta0 = (std::arg(acc) - kPi) / 4 + nu0 * N;
        if (!std::isfinite(nu0) || !std::isfinite(theta0)) { failCycles++; reacq++; stage = Acq; acq.clear(); phBuf.clear(); zbuf.clear(); return; }   // the equaliser blew up: measure again

        // move the estimate into the mixer, so that the loops start on a carrier near zero (an echo pulls the spectrum centroid off by tens of kHz).
        // The samples already mixed but not yet filtered get the change too.
        {
            const double dHz = nu0 * rs / (1 + tnu) / (2 * kPi);
            const double dw = -2 * kPi * dHz / fs;
            const size_t n0 = (size_t)std::max(0.0, std::floor(tau));
            for (size_t k = n0; k < hb.size(); k++) hb[k] *= std::polar(1.f, (float)(dw * (double)(k - n0)));
            ncoPh += dw * (double)(hb.size() - n0);
            ncoPh = std::remainder(ncoPh, 2 * kPi);
            cfoCoarse += dHz;
            ncoStep = -2 * kPi * cfoCoarse / fs;
            nu0 = 0;
        }
        memcpy(wCma, w, sizeof w);
        zbuf.clear();
        trialOrders.clear();
        if (qamSet.load() >= 0) trialOrders.push_back(qamSet.load());
        else if (annexB.load()) trialOrders = {4, 2};   // annex B carries 256-QAM and 64-QAM only
        else trialOrders = {4, 2, 0, 3, 1};
        trialIdx = 0;
        startTrial();
    }

    void startTrial() {
        qam = trialOrders[trialIdx];
        memcpy(w, wCma, sizeof w);
        theta = theta0; nu = nu0;
        dm[0].setQam(qam); dm[1].setQam(qam);
        tb[0].clear(); tb[1].clear(); j83Trial.clear();
        errP = 0; sigP = 1; phBuf.clear();
        stage = Trial; stageSyms = 0;
    }

    void trialCheck() {
        for (int v = 0; v < 2; v++) {
            const long off = findSync(tb[v]);
            if (off < 0) continue;
            inv = v == 1;
            bits.assign(tb[v].begin() + off, tb[v].end());
            tb[0].clear(); tb[1].clear();
            dm[0] = dm[v];
            deint = dvbt::ConvInterleaver(true);
            warm = 12; haveGroup = false; after.clear(); consecBad = 0; resync = false; rsBits.clear();
            stage = Locked; lockedAt = processed; failCycles = 0; pendSecs = 0;
            char m[160];
            snprintf(m, sizeof m, "%s locked%s, %.4f Msym/s, MER %.1f dB", dvbc::qamName(qam), inv ? " (spectrum inverted)" : "", rs / (1 + tnu) / 1e6, mer());
            log(m);
            return;
        }
        { char m[160]; snprintf(m, sizeof m, "no sync as %s: MER %.1f dB, clock %+.0f ppm, carrier %+.0f Hz", dvbc::qamName(qam), mer(), -tnu * 1e6, nu * rs / (2 * kPi)); log(m); }
        if (++trialIdx < trialOrders.size()) { startTrial(); return; }
        // nothing fitted: measure again
        failCycles++; reacq++;
        stage = Acq; acq.clear(); phBuf.clear();
    }

    float mer() const { return errP > 0 ? (float)(10 * std::log10(sigP / errP)) : 0.f; }

    // ---------------------------------------------------------------- outer decoder
    void outer() {
        if (resync) {   // collect enough bits to find the frame start again, then carry on from there; failing that, search from scratch
            rsBits.insert(rsBits.end(), bits.begin(), bits.end());
            bits.clear();
            if (rsBits.size() < 8 * 204 * (kSyncBlocks + 4)) return;
            const long off = findSync(rsBits);
            resync = false;
            if (off < 0) { rsBits.clear(); lostFlag = true; return; }
            bits.assign(rsBits.begin() + off, rsBits.end());
            rsBits.clear();
            deint = dvbt::ConvInterleaver(true);
            warm = 12; haveGroup = false; after.clear(); consecBad = 0;
            resyncs++;
        }
        const size_t nb = bits.size() / 8 / 12 * 12;
        if (!nb) return;
        std::vector<uint8_t> by(nb), di(nb);
        for (size_t i = 0; i < nb; i++) { int v = 0; for (int b = 0; b < 8; b++) v = (v << 1) | bits[8 * i + (size_t)b]; by[i] = (uint8_t)v; }
        bits.erase(bits.begin(), bits.begin() + (std::ptrdiff_t)(nb * 8));
        deint.process(by.data(), di.data(), nb);
        after.insert(after.end(), di.begin(), di.end());
        const auto& ks = prbsKeys();
        size_t pos = 0;
        uint64_t fixed = 0, blocksNow = 0;
        while (after.size() - pos >= 204) {
            uint8_t* blk = &after[pos];
            pos += 204;
            if (warm > 0) { warm--; continue; }
            const int r = dvbt::rsDecode(blk);
            blocksNow++;
            uint8_t pkt[188];
            memcpy(pkt, blk, 188);
            if (r < 0) { rsFail++; consecBad++; } else { consecBad = 0; if (r == 0) rsClean++; else { rsCorr++; fixed += (uint64_t)r; } }
            if (r >= 0 && pkt[0] == 0xB8) { groupIdx = 0; haveGroup = true; }
            else if (haveGroup) groupIdx = (groupIdx + 1) % 8;
            if (haveGroup && consecBad < 8) {
                const uint8_t* key = &ks[(size_t)groupIdx * 188];
                pkt[0] = 0x47;
                for (int j = 1; j < 188; j++) pkt[j] ^= key[j];
                if (r < 0) { pkt[1] |= 0x80; packetsBad++; }
                packets++;
                out.insert(out.end(), pkt, pkt + 188);
            }
            if (consecBad >= kResyncBad && !resync) {   // the frame position has probably moved: look for it before giving the lock up
                resync = true; rsBits.clear(); bits.clear(); after.clear(); haveGroup = false;
                return;
            }
            if (consecBad >= 48) {
                lostFlag = true;   // the receiver thread restarts the search
                bits.clear(); after.clear(); haveGroup = false;
                return;
            }
        }
        after.erase(after.begin(), after.begin() + (std::ptrdiff_t)pos);
        if (blocksNow) byteErr = 0.9 * byteErr + 0.1 * ((double)fixed / (double)(blocksNow * 204));
    }

    // ---------------------------------------------------------------- input
    DVBC_HOT
    void process(const cf32* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            if (stage == Acq) {
                const size_t need = kAcqN * kAcqBlocks - acq.size(), take = std::min(need, n - i);
                acq.insert(acq.end(), x + i, x + i + take);
                i += take; processed += take;
                if (acq.size() == kAcqN * kAcqBlocks) { analyse(); acq.clear(); }
                continue;
            }
            if (lostFlag.load() && stage == Locked) handleLost();
            if (const uint64_t r = resyncs.load(); r != resyncsLogged) { resyncsLogged = r; log("bytes re-aligned without losing the lock"); }
            // mix and filter a chunk
            DECT2_STAGE_VAR(mixStage, "dvbc.dc+mix");
            const size_t take = std::min<size_t>(n - i, 16384);
            // the mixer is a rotating phasor (renormalised every 256 samples), not a sin/cos per sample
            double pr = std::cos(ncoPh), pi_ = std::sin(ncoPh);
            const double sr = std::cos(ncoStep), si = std::sin(ncoStep);
            const size_t h0 = hb.size();
            hb.resize(h0 + take);   // one resize per chunk, then plain stores: a push_back per sample re-checks the capacity every time
            cf32* __restrict hbOut = hb.data() + h0;
            for (size_t k = 0; k < take; k++) {
                const std::complex<double> xi(x[i + k].real(), x[i + k].imag());
                dcI += dcA * (xi - dcI);
                const std::complex<double> xd = xi - dcI;
                hbOut[k] = cf32((float)xd.real(), (float)xd.imag()) * cf32((float)pr, (float)pi_);
                const double nr = pr * sr - pi_ * si;
                pi_ = pr * si + pi_ * sr; pr = nr;
                if ((k & 255) == 255) { const double g = 1.0 / std::sqrt(pr * pr + pi_ * pi_); pr *= g; pi_ *= g; }
            }
            ncoPh = std::atan2(pi_, pr);
            i += take; processed += take;
            mixStage.stop();
            DECT2_STAGE("dvbc.timing+equaliser");   // the matched filter / interpolator, timing loop, equaliser and carrier loop, symbol by symbol
            const int L = 2 * K;
            while (stage != Acq && tau + K + 1 < (double)hb.size()) {
                const double fl = std::floor(tau);
                const int p = std::min(kPhases - 1, (int)((tau - fl) * kPhases));
                const float* h = &taps[(size_t)p * (size_t)L];
                const cf32* s = &hb[(size_t)fl - (size_t)K + 1];
                float re[4] = {0, 0, 0, 0}, im[4] = {0, 0, 0, 0};   // independent sums: no dependency chain, so the loop vectorises
                int j = 0;
                for (; j + 4 <= L; j += 4)
                    for (int q = 0; q < 4; q++) { re[q] += h[j + q] * s[j + q].real(); im[q] += h[j + q] * s[j + q].imag(); }
                for (; j < L; j++) { re[0] += h[j] * s[j].real(); im[0] += h[j] * s[j].imag(); }
                cf32 y((re[0] + re[1]) + (re[2] + re[3]), (im[0] + im[1]) + (im[2] + im[3]));
                const double pw = std::norm(y);
                agcN++;
                agcP = agcN < 1000 ? agcP + (pw - agcP) / (double)agcN : agcP + 2e-5 * (pw - agcP);
                y *= (float)(1.0 / std::sqrt(agcP + 1e-20));
                // equaliser input
                u[upos] = u[upos + kTaps] = y;
                upos = (upos + 1) % kTaps;
                double adv = sps / 2 * (1 + tnu);
                if (strobe) {
                    const float e = ((y - prevStrobe) * std::conj(mid)).real();
                    if (!std::isfinite(e) || !std::isfinite(agcP)) { log("the signal overflowed the loops (numbers out of range): starting again"); hardReset(); break; }
                    prevStrobe = y;
                    tedAvg = 0.999 * tedAvg + 0.001 * e;
                    adv -= 0.01 * e * sps / 2;
                    tnu -= 3e-7 * e;
                    tnu = std::clamp(tnu, -3e-3, 3e-3);
                    onSymbol();
                } else mid = y;
                strobe = !strobe;
                tau += adv;
                if (stage == Acq) break;
            }
            if (stage == Acq) { hb.clear(); continue; }
            const size_t drop = (size_t)std::max(0.0, std::floor(tau) - K - 4);
            if (drop > 32768) { hb.erase(hb.begin(), hb.begin() + (std::ptrdiff_t)drop); tau -= (double)drop; }
        }
        if (processed >= nextTel) { nextTel = processed + (uint64_t)(fs / 4); publish(); }
    }

    void publish() {
        std::lock_guard<std::mutex> bl(bmu);   // the outer-decoder thread's counters and cells
        DvbcTelemetry t;
        t.state = stage == Locked ? 2 : stage == Acq ? 0 : 1;
        const double rsNow = rs / (1 + tnu);
        t.cfoHz = cfoCoarse + (stage >= Trial ? nu * rsNow / (2 * kPi) : 0);
        t.merDb = stage >= Trial ? mer() : 0;
        t.snrDb = t.merDb;
        t.qam = stage >= Trial ? qam : -1;
        t.qamManual = qamSet.load() >= 0;
        t.symbolRate = stage == Acq ? 0 : rsNow;
        t.symbolRateManual = usedManual;
        t.symbolRateMeasured = rsMeasured;
        t.width3dBHz = width;
        t.annexC = annexC.load();
        t.annexB = annexB.load();
        { std::lock_guard<std::mutex> bl2(docMu); t.docsis = docsis.status(); }
        t.rollOff = t.annexB ? (float)(qam == 4 ? j83b::kRollOff256 : j83b::kRollOff64) : t.annexC ? 0.13f : 0.15f;
        t.inverted = inv && stage == Locked;
        t.clockPpm = rsNominal > 0 ? (rsNow / rsNominal - 1) * 1e6 + (usedManual ? 0 : 0) : 0;
        t.rsClean = rsClean; t.rsCorrected = rsCorr; t.rsFailed = rsFail;
        t.blocksOk = rsClean + rsCorr; t.blocksBad = rsFail;
        t.packets = packets; t.packetsBad = packetsBad;
        t.preRsByteErr = byteErr;
        t.lockSpectrum = stage != Acq; t.lockTiming = stage != Acq && syms > 20000; t.lockEq = stage >= Freq; t.lockCarrier = stage >= Trial;
        t.lockSync = stage == Locked; t.tsLock = stage == Locked && consecBad == 0 && packets > 0;
        t.dataValid = t.tsLock;
        t.netMbps = stage == Locked ? dvbc::netBitrate(qam, rsNow) / 1e6 : 0;
        static const char* const st[] = {"measuring the spectrum", "blind equaliser (CMA)", "carrier frequency", "trying the QAM order", "decoding"};
        t.stage = st[stage];
        if (stage == Trial) t.stage += std::string(" (") + dvbc::qamName(qam) + ")";
        t.note = note;
        t.reacquisitions = reacq;
        t.dropped = dropped.load();
        double lv = 0; size_t c = 0;
        for (size_t k = hb.size() > 4096 ? hb.size() - 4096 : 0; k < hb.size(); k++, c++) lv += std::norm(hb[k]);
        t.levelDbfs = c ? (float)(10 * std::log10(lv / c + 1e-20)) : -120.f;
        if (stage >= Trial) { merHist.push_back(t.merDb); if (merHist.size() > 240) merHist.pop_front(); }
        t.merHistory.assign(merHist.begin(), merHist.end());
        if (stage >= Trial) t.cells = cells;
        t.psdDb = psd; t.psdSpanHz = fs;
        float mx = 1e-20f;
        for (auto& x : w) mx = std::max(mx, std::abs(x));
        for (auto& x : w) t.eqDb.push_back(20 * std::log10(std::abs(x) / mx + 1e-6f));
        t.eqCentre = kMain;
        std::lock_guard<std::mutex> lk(tmu);
        t.seq = ++seq;
        tel = std::move(t);
    }

    void run() {
        for (;;) {
            std::vector<cf32> v;
            {
                std::unique_lock<std::mutex> lk(qmu);
                busy = false;
                idleCv.notify_all();
                qcv.wait(lk, [&] { return stop || !q.empty() || resetReq; });
                if (stop) return;
                if (resetReq) { resetReq = false; q.clear(); queued = 0; lk.unlock(); hardReset(); continue; }
                v = std::move(q.front()); q.pop_front(); queued -= v.size();
                busy = true;
                qcv.notify_all();
            }
            const uint64_t d = dropped.load();
            if (d != droppedLogged && processed - droppedLogAt >= (uint64_t)fs) {   // at most once per second of signal
                char m[120]; snprintf(m, sizeof m, "the receiver is too slow for the sample rate: %.0f ms of signal dropped", (double)(d - droppedLogged) / fs * 1e3);
                droppedLogged = d; droppedLogAt = processed; log(m);
            }
            process(v.data(), v.size());
        }
    }
};

DvbcReceiver::DvbcReceiver() : p_(new Impl) { p_->bth = dect2::namedThread("onair-dvbc", [this] { p_->backRun(); }); p_->th = std::thread([this] { p_->run(); }); }
DvbcReceiver::~DvbcReceiver() {
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->stop = true; }
    p_->qcv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    { std::lock_guard<std::mutex> lk(p_->jmu); p_->bstop = true; }
    p_->jcv.notify_all(); p_->jidle.notify_all();
    if (p_->bth.joinable()) p_->bth.join();
}
void DvbcReceiver::configure(double fs) { flush(); p_->fs = fs > 0 ? fs : 10e6; reset(); flush(); }
bool DvbcReceiver::ready() const { return p_->fs >= 7e6; }
void DvbcReceiver::reset() { { std::lock_guard<std::mutex> lk(p_->qmu); p_->resetReq = true; } p_->qcv.notify_all(); }
void DvbcReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::unique_lock<std::mutex> lk(s.qmu);
    const size_t cap = (size_t)(s.fs * 1.0);
    if (s.blocking.load()) s.qcv.wait(lk, [&] { return s.queued + n <= cap || s.queued == 0 || s.stop; });
    else if (s.queued + n > cap) { s.dropped += n; return; }
    s.q.emplace_back(x, x + n);
    s.queued += n;
    s.qcv.notify_all();
}
void DvbcReceiver::flush() {
    std::unique_lock<std::mutex> lk(p_->qmu);
    p_->idleCv.wait(lk, [&] { return (p_->q.empty() && !p_->busy && !p_->resetReq) || p_->stop; });
    lk.unlock();
    p_->backIdle();   // the outer-decoder thread has delivered everything handed to it
}
bool DvbcReceiver::telemetry(DvbcTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->tmu);
    if (p_->seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void DvbcReceiver::setLogCallback(std::function<void(const std::string&)> cb) { flush(); p_->logCb = std::move(cb); }
void DvbcReceiver::setPacketCallback(std::function<void(const uint8_t*, size_t, double)> cb) { flush(); p_->pktCb = std::move(cb); }
void DvbcReceiver::setSymbolRate(double hz) { if (hz != p_->manualRs.load()) { p_->manualRs = hz; reset(); } }
void DvbcReceiver::setQam(int q) { if (q != p_->qamSet.load()) { p_->qamSet = q; reset(); } }
void DvbcReceiver::setAnnexC(bool c) { if (c != p_->annexC.load()) { p_->annexC = c; reset(); } }
void DvbcReceiver::setAnnexB(bool b) { if (b != p_->annexB.load()) { p_->annexB = b; reset(); } }
void DvbcReceiver::setBlocking(bool b) { p_->blocking = b; }

double dvbcChannelCentreMhz(double mhz) {
    const double k = std::round((mhz - 114.0) / 8.0);
    return 114.0 + 8.0 * std::clamp(k, 0.0, 93.0);
}

ModeTuning dvbcTuning() {
    ModeTuning t;
    t.stdMode = 42; t.id = "dvbc"; t.name = "DVB-C";
    t.minMhz = 47; t.maxMhz = 1002; t.defMhz = 346.0;
    t.sampleRate = 10e6; t.basebandHz = 8e6; t.bandwidthMhz = 8;
    t.minSampleRate = 7e6;
    return t;
}

namespace {
class DvbcSynth : public ModeSynth {
public:
    DvbcSynth(const dvbc::SignalConfig& c) : s_(c) {}
    double sampleRate() const override { return s_.config().sampleRate; }
    void generate(cf32* out, size_t n) override { s_.generate(out, n); }
private:
    dvbc::Signal s_;
};
} // namespace

static dvbc::SignalConfig dvbcSynthCommon(const SynthConfig& cfg, double sampleRate) {
    static const int qmap[5] = {2, 0, 1, 3, 4};
    dvbc::SignalConfig c;
    c.qam = qmap[std::clamp(cfg.modeOpt[0], 0, 4)];
    const bool annexC = cfg.modeOpt[1] == 1, annexB = cfg.modeOpt[1] == 2;
    c.rollOff = annexB ? (c.qam == 4 ? j83b::kRollOff256 : j83b::kRollOff64) : annexC ? 0.13 : 0.15;
    c.inverted = cfg.modeOpt[2] == 1;
    c.sampleRate = sampleRate > 0 ? sampleRate : 10e6;
    c.symbolRate = cfg.modeVal[0] > 0 ? cfg.modeVal[0] : annexB ? (c.qam == 4 ? j83b::kSymRate256 : j83b::kSymRate64) : annexC ? 5.274e6 : 6.952e6;
    c.symbolRate = std::min(c.symbolRate, c.sampleRate / 1.2);
    return c;
}

double dvbcSynthNetBitrate(const SynthConfig& cfg, double sampleRate) {
    const dvbc::SignalConfig c = dvbcSynthCommon(cfg, sampleRate);
    return dvbc::netBitrate(c.qam, c.symbolRate);
}

std::unique_ptr<ModeSynth> makeDvbcSynth(const SynthConfig& cfg, double sampleRate, std::function<void(uint8_t*)> ts) {
    dvbc::SignalConfig c = dvbcSynthCommon(cfg, sampleRate);
    c.clockPpm = cfg.modeVal[1] + cfg.sroPpm;
    c.snrDb = cfg.snrDb;
    c.cfoHz = cfg.cfoHz;
    if (cfg.echoDb > 0) { c.echoDb = cfg.echoDb; c.echoUs = cfg.echoDelay / c.sampleRate * 1e6; }
    if (ts) c.ts = std::move(ts);
    if (cfg.modeOpt[1] == 2) {   // annex B: 64-QAM or 256-QAM frames instead of the annex A ones
        c.qam = c.qam == 4 ? 4 : 2;
        auto enc = std::make_shared<j83b::J83bEncoder>(j83b::EncConfig{c.qam == 4, 0});
        auto src = c.ts ? c.ts : std::function<void(uint8_t*)>();
        auto n = std::make_shared<uint64_t>(0);
        c.symbols = [enc, src, n](std::vector<cf32>& out) {
            static const int pids[3] = {0x31, 0x34, 0x1FFB};
            for (int i = 0; i < 512 && out.empty(); i++) {
                uint8_t p[188];
                if (src) src(p); else { memset(p, 0xA5, 188); const int pid = pids[(*n) % 3]; p[0] = 0x47; p[1] = (uint8_t)(pid >> 8); p[2] = (uint8_t)pid; p[3] = 0x10 | ((*n / 3) & 15); (*n)++; }
                enc->push(p);
                enc->take(out);
            }
        };
    }
    return std::make_unique<DvbcSynth>(c);
}
std::unique_ptr<ModeSynth> makeDvbcSynth(const SynthConfig& cfg, double sampleRate) {
    return makeDvbcSynth(cfg, sampleRate, {});
}

} // namespace dect2
