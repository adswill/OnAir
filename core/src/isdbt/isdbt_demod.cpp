#include "dect2/dsp/gpu_viterbi.h"
#include "dect2/dsp/gpu_policy.h"
#include <cstdlib>
#include "dect2/common/thread_name.h"
#include "dect2/ofdm/csi_erase.h"
#include "dect2/isdbt/isdbt_demod.h"
#include "dect2/dvbt/dvbt_fec.h"
#include "dect2/t2/t2ofdm.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dect2 {
namespace isdbt {

namespace {
const int kBranchDelay[4][6] = {{0, 120, 0, 0, 0, 0}, {0, 120, 0, 0, 0, 0}, {0, 40, 80, 120, 0, 0}, {0, 24, 48, 72, 96, 120}};
constexpr int kSwitchAfter = 64;   // packets in a row that fail before the other alignment is tried
constexpr float kErasure = 1e9f;   // noise variance given to cells that have not been received
constexpr double kNoiseCal = 1.0;  // correction of the pilot-based noise estimate (calibrated against simulations)

struct Prbs {
    unsigned r = 0;
    void reset() { r = 0x4A80; }
    unsigned bit() { const unsigned b = (r ^ (r >> 1)) & 1; r = (r >> 1) | (b << 14); return b; }
};
}

// ---------------------------------------------------------------- one layer: cells to transport stream packets

struct LayerDecoder {
    Layer cfg;
    int mode = 3, nc = 0, K = 0, m = 2;
    LayerStats st;
    std::vector<std::vector<float>> ring;
    std::vector<size_t> ringPos;
    long skipCells = 120;
    bool twoSymbols = false;        // which delay adjustment the transmitter uses for the bit interleaving; flipped after a long run of failures
    bool restart = false;
    bool modelKnown = false;        // packets have decoded with this alignment: failures after that are the channel's (a dropped stretch of signal, a retune), not a different transmitter model
    bool anyGood = false;           // a packet has decoded since the last configure
    int goodRun = 0;
    bool waitFrame = false;         // after a restart: the layer begins again at the next frame
    long holdoff = 0;               // symbols before failures count: the time de-interleaver is still filling with erasures
    std::vector<float> llrQ;
    std::vector<int8_t> carry;
    std::vector<uint8_t> bits;
    std::vector<uint8_t> byteQ;
    dvbt::ConvInterleaver deint{true};
    uint64_t t = 0;                 // bytes that went through the de-interleaver
    uint64_t u = 0;                 // bytes of the aligned stream (u = 0 starts a frame)
    Prbs prbs;
    std::vector<uint8_t> block;     // bytes of the Reed-Solomon block being collected
    std::vector<uint8_t> out;       // decoded packets
    std::deque<std::pair<double, std::vector<uint8_t>>> merged;   // with their position in time (frames)
    uint64_t packetsOut = 0;
    int failStreak = 0;
    float llrScale = 8.f;

    void configure(int mode_, const Layer& l) {
        cfg = l; mode = mode_;
        if (!anyGood) modelKnown = false;   // the last lock decoded nothing: a different transmitter, perhaps
        anyGood = false;
        nc = dataPerSegment(mode) * l.segments;
        K = packetsPerFrame(mode, l);
        m = bitsPerCell(l.mod);
        ring.assign((size_t)m, {});
        ringPos.assign((size_t)m, 0);
        for (int r = 0; r < m; r++) ring[(size_t)r].assign((size_t)(120 - kBranchDelay[l.mod][r]), 0.f);
        skipCells = twoSymbols ? 2 * nc : 0;   // the bit interleaving with its delay adjustment takes a whole frame or, in the other model, two symbols
        llrQ.clear(); carry.clear(); bits.clear(); byteQ.clear(); out.clear(); merged.clear(); block.clear();
        deint = dvbt::ConvInterleaver(true);
        t = u = 0; packetsOut = 0; failStreak = 0;
        st = LayerStats();
        restart = false;
        goodRun = 0;
        holdoff = 95L * interleavingLength(mode, cfg.ti) + timeInterleaveAdjust(mode, cfg.ti) + 4 * kSymbolsPerFrame;   // the interleavers, the bit delay and the byte delay fill up one after the other
    }

    // Every packet failed for a while: the other bit interleaving delay adjustment may be the one in use. Starts the layer over with it (statistics and
    // the packet position are kept).
    void switchModel() {
        const LayerStats keep = st;
        const uint64_t pos = packetsOut;
        twoSymbols = !twoSymbols;
        configure(mode, cfg);
        st = keep;
        packetsOut = pos;
        waitFrame = true;
        holdoff = 0;
        modelKnown = false;
    }

    std::vector<float> symLlr;      // the bit values of one symbol, m per cell

    void pushSymbol(const cf32* cells, const float* n0, int symIdx) {
        if (waitFrame) { if (symIdx != 0) return; waitFrame = false; }
        if (holdoff > 0) holdoff--;
        symLlr.resize((size_t)nc * (size_t)m);
        demapCells(cfg.mod == kDqpsk ? kQpsk : cfg.mod, cells, n0, nc, symLlr.data());
        // the delay of each bit branch (the bit interleaving), then the cells in order
        const int skip = (int)std::min<long>(skipCells, nc);
        for (int r = 0; r < m; r++) {
            auto& rg = ring[(size_t)r];
            size_t pos = ringPos[(size_t)r];
            const size_t len = rg.size();
            float* d = symLlr.data() + r;
            if (len) {
                for (int c = 0; c < nc; c++) {
                    const float o = rg[pos];
                    rg[pos] = d[(size_t)c * m];
                    d[(size_t)c * m] = o;
                    if (++pos == len) pos = 0;
                }
                ringPos[(size_t)r] = pos;
            }
        }
        if (skip < nc) llrQ.insert(llrQ.end(), symLlr.begin() + (long)skip * m, symLlr.end());
        skipCells -= skip;
        const size_t frameBits = (size_t)kSymbolsPerFrame * nc * m;
        while (llrQ.size() >= frameBits) {
            processFrame(llrQ.data(), frameBits);
            if (restart) { switchModel(); break; }
            llrQ.erase(llrQ.begin(), llrQ.begin() + (long)frameBits);
        }
    }

    void processFrame(const float* llr, size_t n) {
        static const int kRateK[5] = {1, 2, 3, 5, 7};
        double mean = 0;
        for (size_t i = 0; i < n; i++) mean += std::fabs(llr[i]);
        mean /= (double)n;
        const float scale = (float)std::min(40.0, 22.0 / std::max(1e-6, mean));
        llrScale = scale;
        std::vector<int8_t> soft;
        dvbt::Viterbi::depuncture(llr, n, cfg.rate, 0, soft, scale);
        constexpr size_t L = 192;
        std::vector<int8_t> window = carry;
        window.insert(window.end(), soft.begin(), soft.end());
        const size_t steps = window.size() / 2;
        std::vector<uint8_t> dec;
        long metric = 0;
        // the GPU Viterbi gives the exact CPU decoder's bits and metric (viterbi_gpu test); Auto stays on the CPU (measured 5 to 15 times slower on the GPU, GPU-AUDIT-3.7.md): only when GPU is chosen
        static const bool noGpu = getenv("DECT2_NOGPU") != nullptr;
        const int gm = gpupol::liveMode(gpupol::IsdbT);
        const bool gpu = !noGpu && gm != 0 && gm == 1 && GpuViterbi::instance().available() && GpuViterbi::instance().decode(window, dec, &metric);
        gpupol::reportGpu(gpupol::IsdbT, gpu);
        if (!gpu) dvbt::Viterbi::decode(window, dec, 4, &metric);
        st.viterbiMargin = steps ? (double)metric / (double)(steps * 2 * 22) : 0;
        const size_t fromStep = carry.empty() ? 0 : L;
        const size_t toStep = steps > L ? steps - L : 0;
        for (size_t s = fromStep; s < toStep; s++) bits.push_back(dec[s]);
        const size_t keep = std::min(window.size() / 2, 2 * L);
        carry.assign(window.end() - 2 * (long)keep, window.end());
        (void)kRateK;
        // bits to bytes, byte de-interleaving, energy dispersal removal, Reed-Solomon
        const size_t nb = bits.size() / 8 / 12 * 12;
        if (!nb) return;
        std::vector<uint8_t> by(nb), di(nb);
        for (size_t i = 0; i < nb; i++) { unsigned c = 0; for (int j = 0; j < 8; j++) c = (c << 1) | bits[8 * i + (size_t)j]; by[i] = (uint8_t)c; }
        bits.erase(bits.begin(), bits.begin() + (long)(nb * 8));
        deint.process(by.data(), di.data(), nb);
        for (size_t i = 0; i < nb; i++, t++) {
            if (t < (uint64_t)K * 204) continue;   // the byte interleaving and its delay adjustment take one frame: the output before it is empty memory
            uint8_t b = di[i];
            if (u % (uint64_t)(K * 204) == 0) prbs.reset();
            unsigned x = 0;
            for (int j = 0; j < 8; j++) x = (x << 1) | prbs.bit();
            const bool sync = u % 204 == 203;
            if (!sync) b ^= (uint8_t)x;
            if (u >= 203) {
                block.push_back(b);
                if (block.size() == 204) { finishBlock(); block.clear(); if (restart) return; }
            }
            u++;
        }
    }

    void finishBlock() {
        const bool syncOk = block[0] == 0x47;
        const int r = dvbt::rsDecode(block.data());
        st.packets++;
        uint8_t pkt[188];
        std::memcpy(pkt, block.data(), 188);
        if (r < 0) { st.rsFailed++; pkt[1] |= 0x80; goodRun = 0; ++failStreak; if (failStreak >= kSwitchAfter && !modelKnown && holdoff <= 0) restart = true; }
        else { if (r == 0) st.rsClean++; else st.rsCorrected++; failStreak = 0; anyGood = true; if (++goodRun >= 32) modelKnown = true; }
        st.synced = failStreak < 8 && (r >= 0 || syncOk);
        out.insert(out.end(), pkt, pkt + 188);
        packetsOut++;
        // position of the packet in time: packets per frame K, the packet ends at (n + 1) / K frames
        merged.emplace_back((double)packetsOut / (double)K, std::vector<uint8_t>(pkt, pkt + 188));
    }
};

// ---------------------------------------------------------------- demodulator

struct Demod::Impl {
    Params p;
    int mode = 3, cps = 0, dps = 0, K = 0, N = 0, kEdge = 0, posEdge = kSegments - 1;
    // the carrier just above a run of synchronous segments that is a known pilot: the pilot above the band, or (ISDB-TSB, where a run can be a
    // single segment) the continual pilot that starts a differential segment; 0 when there is none
    int runTopPilot(int p1) const {
        if (p1 == posEdge) return kEdge;
        if (p.tsb && p1 + 1 < kSegments) { const int s = kSegmentAtPosition[p1 + 1]; if (seg[s].layer >= 0 && seg[s].diff) return (p1 + 1) * cps; }
        return 0;
    }
    SegmentInfo seg[kSegments];
    LayerDecoder layer[3];
    double tau0 = 0;
    bool started = false;
    uint64_t symbols = 0;
    // channel estimation, one grid per run of synchronous segments
    struct Run {
        int p0, p1; std::vector<cf32> grid; std::vector<uint8_t> gv; std::vector<cf32> H; int filled = 0; std::complex<double> lag{0, 0};
        // time direction (as the DVB-T receiver): the last two raw pilots of each grid point (noise and Doppler statistics, tracking reference),
        // how many pilots the time average has seen, the last kHist averaged estimates and their symbols, the pilot above the band per symbol,
        // and the delay window of the channel once it is known
        std::vector<cf32> raw1, raw2; std::vector<uint8_t> rawN; std::vector<uint16_t> cnt;
        std::vector<cf32> hv; std::vector<int64_t> ht;
        std::deque<std::pair<int64_t, cf32>> edge;
        bool dlyKnown = false; double dlyRel = 0, dlyHalf = 0;
    };
    std::vector<Run> runs;
    GridInterpolator interp, interpN;
    // ---- channel estimation in time. Every grid point (every third carrier) gets a scattered pilot every fourth symbol. The symbol kTiDelay
    // back has two estimates on each grid point before it and two after it: its channel is a cubic through them instead of the newest pilot,
    // which for a moving receiver is up to three symbols (3.4 ms in mode 3) stale: at 10 Hz Doppler an error of about -20 dB, an error floor
    // for 64-QAM 3/4 in TU6 fading. In a still channel the pilots are averaged over time with a gain chosen from how fast the channel moves
    // against the noise, and once the delay spread is known the frequency interpolator passes only that window (less noise in the estimate).
    static constexpr int kHist = 5, kTiDelay = 8;
    struct Pend { std::vector<cf32> Y; int symIdx = 0; int64_t t = 0; };
    std::deque<Pend> pend;
    std::vector<Pend> pendSpare;
    std::vector<cf32> prevOut;            // the last symbol that went to the equaliser (differential segments)
    bool prevOutValid = false;
    double ceD1 = 0, ceD2 = 0, ceAlpha = 1;
    long ceStats = 0;
    int64_t outCount = 0;
    std::vector<cf32> scrG, scrHw;
    const bool ceNoTi = ofdm::envOn("DECT2_ISDBT_NOTI");   // A/B tests only: the newest pilot per grid point, no averaging, full delay window (as 3.9)
    std::vector<cf32> prevY;
    bool prevValid = false;
    double sigma2 = 1e-3, snr = 0;
    ofdm::CarrierInterference carrierErase;   // erases carriers hit by a narrowband interferer (coherent segments)
    const bool flatN0 = ofdm::envOn("DECT2_OFDM_FLAT_N0");   // A/B tests only: no channel-state weighting
    // ---- inter-carrier interference (4.1). A channel that changes inside the FFT window (a car at UHF: 70 Hz Doppler is 0.07 of the carrier
    // spacing in mode 3) leaks every carrier into its neighbours: about -21 dB at 70 Hz, under what 64-QAM 3/4 needs in fading. To first
    // order the channel of carrier k moves linearly through the window, H_k + s_k (n - centre), and carrier m picks up
    // sum_k s_k X_k / (exp(j 2 pi (k - m) / N) - 1) from the others. The slope s_k comes from the time-interpolated channel of the symbols
    // before and after (the pilots are there: the symbol is equalised kTiDelay symbols late), X_k from a first hard decision of every carrier
    // of the run (the known value for the scattered pilots); the estimated leakage of the kIciSpan neighbours on either side is taken off before
    // the cells are equalised again. It runs only while the measured speed of the channel makes the leakage at least a tenth of the noise
    // (it costs two more interpolations and 2 kIciSpan multiplies per carrier). DECT2_ISDBT_NOICI=1: the 4.0 path.
    const bool iciOff = ofdm::envOn("DECT2_ISDBT_NOICI");
    const bool diag41 = ofdm::envOn("DECT2_ISDBT_DIAG");   // prints the channel statistics every 204 symbols (tests)
    static constexpr int kIciSpan = 12;
    std::vector<cf32> iciY, iciX, iciHm, iciHp, iciC;
    std::vector<uint8_t> iciRoles;
    uint64_t iciSyms = 0;
    // the estimated leakage power against the noise of a cell, from the channel's change between two pilots of a grid point (4 symbols)
    bool iciWanted() const {
        if (iciOff || ceNoTi || ceStats < 8 || sigma2 <= 0) return false;
        const double q = std::max(0.0, (ceD2 - ceD1) / 3);   // |H change|^2 over 4 symbols, pilot units
        const double Ts = (double)N + (double)(N >> (5 - p.guard));
        const double pIci = q / 16.0 / (Ts * Ts) * (double)N * (double)N / 12.0;
        return pIci > 0.1 * sigma2;
    }
    static float qamAxis(float x, int M, float n) {   // nearest of the M levels +-1, +-3, .. (times n) of one axis
        float l = 2.f * std::floor(x / n * 0.5f) + 1.f;
        l = std::max(-(float)(M - 1), std::min((float)(M - 1), l));
        return l * n;
    }
    // first-order ICI removal over the carriers of run r (symbol t, channel H of this symbol): writes iciY[k] for the run's carriers
    void iciCancel(const Run& r, const cf32* Y, int64_t t, int symIdx, int carriers, int top, double tauRun) {
        const int k0 = r.p0 * cps;
        // channel of the symbols before and after, interpolated as for this one
        std::vector<cf32>& g = scrG;
        auto chan = [&](int64_t m, std::vector<cf32>& out) {
            g.resize(r.grid.size());
            for (size_t i = 0; i < g.size(); i++) g[i] = timeInterp(r, i, m);
            if (top) g[(size_t)(carriers - 1) / 3] = edgeAt(r, m, r.grid[(size_t)(carriers - 1) / 3]);
            if (r.dlyKnown) interpN.run(g, 3, carriers, N, tauRun + r.dlyRel, out, std::min(1.0, r.dlyHalf / ((double)N / 6.0)));
            else interp.run(g, 3, carriers, N, tauRun, out, 1.0);
        };
        chan(t - 1, iciHm);
        chan(t + 1, iciHp);
        const double Ts = (double)N + (double)(N >> (5 - p.guard));
        const float sc = (float)(1.0 / (2.0 * Ts));
        // first decisions
        iciX.assign((size_t)carriers, cf32(0, 0));
        iciRoles.resize((size_t)cps);
        for (int pos = r.p0; pos <= r.p1; pos++) {
            const int sgi = kSegmentAtPosition[pos];
            segmentRoles(mode, sgi, false, symIdx, iciRoles.data());
            const int mod = seg[sgi].layer >= 0 ? p.layer[seg[sgi].layer].mod : kQpsk;
            const int M = mod == k64Qam ? 8 : mod == k16Qam ? 4 : 2;
            const float n = mod == k64Qam ? 1.f / std::sqrt(42.f) : mod == k16Qam ? 1.f / std::sqrt(10.f) : 1.f / std::sqrt(2.f);   // the axis unit (isdbt.cpp norm())
            const auto& w = prbsW(mode, sgi);
            for (int i = 0; i < cps; i++) {
                const int k = pos * cps + i;
                const cf32 h = r.H[(size_t)(k - k0)];
                const float g2 = std::norm(h);
                if (g2 < 1e-12f) continue;
                const cf32 z = Y[k] * std::conj(h) / g2;
                cf32 x;
                if (iciRoles[(size_t)i] == kData) x = cf32(qamAxis(z.real(), M, n), qamAxis(z.imag(), M, n));
                else if (iciRoles[(size_t)i] == kSP) x = cf32(pilotValue(w[(size_t)i]), 0);
                else x = cf32(z.real() >= 0 ? kPilotAmp : -kPilotAmp, 0);
                iciX[(size_t)(k - k0)] = x;
            }
        }
        if (top) iciX[(size_t)(carriers - 1)] = cpPilot(top);
        // slope times decision, then the leakage into each carrier from its neighbours
        for (int c = 0; c < carriers; c++) iciX[(size_t)c] *= (iciHp[(size_t)c] - iciHm[(size_t)c]) * sc;
        if ((int)iciC.size() != 2 * kIciSpan + 1) {
            iciC.assign((size_t)(2 * kIciSpan + 1), cf32(0, 0));
            for (int d = -kIciSpan; d <= kIciSpan; d++) if (d) iciC[(size_t)(d + kIciSpan)] = cf32(1, 0) / (std::polar(1.0f, (float)(2 * M_PI * d / N)) - cf32(1, 0));
        }
        for (int c = 0; c < carriers; c++) {
            cf32 acc(0, 0);
            const int lo = std::max(0, c - kIciSpan), hi = std::min(carriers - 1, c + kIciSpan);
            for (int j = lo; j <= hi; j++) if (j != c) acc += iciX[(size_t)j] * iciC[(size_t)(j - c + kIciSpan)];
            iciY[(size_t)(k0 + c)] = Y[k0 + c] - acc;
        }
    }
    std::vector<int> ceK;
    // common phase and phase ramp (timing) of the symbols against the pilots, as running totals so that nothing has to be unwrapped
    double accPhi = 0, accSlope = 0;
    bool trackOn = true;
    std::vector<int> cpK;                 // carriers with a constant pilot: continual pilots of differential segments and the last carrier
    std::vector<cf32> cpRef;
    bool cpRefValid = false;
    std::vector<cf32> Yd;                 // the symbol after the correction
    std::vector<cf32> pilotShow;
    bool haveTmcc = false;
    uint8_t tmccBits[2][kSymbolsPerFrame] = {};            // [0] differential segments, [1] synchronous ones
    std::vector<std::pair<int, int>> tmccCar;              // (carrier, 0 differential / 1 synchronous)
    std::vector<cf32> eq;
    std::vector<float> chDb;
    // time de-interleaver: delay lines per segment and data carrier
    std::vector<std::vector<cf32>> tiV;
    std::vector<std::vector<float>> tiN;
    std::vector<size_t> tiPos;
    std::vector<double> mergedLastFrac;

    // ---- the layer decoders (demapping, de-interleaving, Viterbi, Reed-Solomon) run on a worker thread of their own, one symbol per job in
    // order; the front end above hands over the cells and goes on. What the receiver reads (packets, merged packets, statistics) is what the
    // worker has published after each job. DECT2_ISDBT_THREADS=0 runs the jobs in the calling thread instead.
    struct Job { std::vector<cf32> cells[3]; std::vector<float> nn[3]; int symIdx = 0; };
    struct Published { std::vector<uint8_t> out; std::deque<std::pair<double, std::vector<uint8_t>>> merged; uint64_t packetsOut = 0; int K = 1; LayerStats st; };
    static constexpr size_t kKeepOut = 16u << 20;   // bytes of per-layer packets kept for takePackets()
    static constexpr size_t kJobs = 256;    // more than a frame's burst of decoding (about 30 symbols) needs
    std::vector<Job> jobs;
    size_t produced = 0, consumed = 0;      // jobs handed over / finished (guarded by qm)
    std::mutex qm;
    std::condition_variable cvWork, cvSpace;
    bool stopWorker = false;
    std::thread worker;
    bool threaded = true;
    int ncOf[3] = {0, 0, 0};                // cells per symbol of each layer (the front end's copy: the worker owns layer[])
    bool usedL[3] = {false, false, false};
    std::mutex mu;                          // guards pub
    Published pub[3];
    mutable LayerStats statSnap[3];
    std::vector<uint8_t> carryOut;          // merged packets released by a reconfiguration, for the next takeMerged
    size_t mergeInto(std::vector<uint8_t>& out);

    Impl() {
        const char* e = getenv("DECT2_ISDBT_THREADS");
        threaded = !(e && e[0] == '0') && std::thread::hardware_concurrency() > 1;
    }
    ~Impl() {
        if (worker.joinable()) { { std::lock_guard<std::mutex> g(qm); stopWorker = true; } cvWork.notify_all(); worker.join(); }
    }
    void publish() {
        std::lock_guard<std::mutex> g(mu);
        for (int li = 0; li < 3; li++) {
            if (!usedL[li]) continue;
            LayerDecoder& l = layer[li];
            Published& P = pub[li];
            P.out.insert(P.out.end(), l.out.begin(), l.out.end());
            l.out.clear();
            // only the merged output is read by the receiver: keep the per-layer packets for takePackets() within a bound, not for ever
            if (P.out.size() > kKeepOut) P.out.erase(P.out.begin(), P.out.begin() + (long)((P.out.size() - kKeepOut / 2) / 188 * 188));
            for (auto& m : l.merged) P.merged.push_back(std::move(m));
            l.merged.clear();
            P.packetsOut = l.packetsOut; P.K = l.K; P.st = l.st;
        }
    }
    void runJob(Job& j) {
        for (int li = 0; li < 3; li++) if (usedL[li]) layer[li].pushSymbol(j.cells[li].data(), j.nn[li].data(), j.symIdx);
        publish();
    }
    void workerLoop() {
        std::unique_lock<std::mutex> lk(qm);
        for (;;) {
            cvWork.wait(lk, [&] { return stopWorker || consumed != produced; });
            if (consumed == produced) return;   // stopping, and nothing is left
            Job& j = jobs[consumed % kJobs];
            lk.unlock();
            runJob(j);
            lk.lock();
            consumed++;
            cvSpace.notify_all();
        }
    }
    Job& acquire() {   // a free job slot to fill
        if (jobs.empty()) jobs.resize(kJobs);
        if (threaded) {
            std::unique_lock<std::mutex> lk(qm);
            cvSpace.wait(lk, [&] { return produced - consumed < kJobs; });
        }
        return jobs[produced % kJobs];
    }
    void submit(Job& j) {
        if (!threaded) { runJob(j); produced++; consumed++; return; }
        {
            std::lock_guard<std::mutex> g(qm);
            if (!worker.joinable()) worker = dect2::namedThread("onair-isdbt", [this] { workerLoop(); });
            produced++;
        }
        cvWork.notify_one();
    }
    void drain() {   // everything handed over is decoded and published
        if (!threaded) return;
        std::unique_lock<std::mutex> lk(qm);
        cvSpace.wait(lk, [&] { return consumed == produced; });
    }

    void configure(const Params& pp) {
        drain();   // the worker is idle from here on
        mergeInto(carryOut);   // what the receiver would already have taken before the layers start again
        p = pp; mode = p.mode;
        cps = carriersPerSegment(mode); dps = dataPerSegment(mode); K = totalCarriers(mode); N = fftN(mode) / tsbDecimation(p.tsb);
        kEdge = edgeCarrier(mode, p.tsb); posEdge = lastPosition(p.tsb);
        carrierErase.reset(K);
        segmentLayout(p, seg);
        for (int i = 0; i < 3; i++) {
            if (p.layer[i].used()) layer[i].configure(mode, p.layer[i]); else layer[i] = LayerDecoder();
            usedL[i] = p.layer[i].used();
            ncOf[i] = layer[i].nc;
            std::lock_guard<std::mutex> g(mu);
            pub[i] = Published();
            pub[i].K = layer[i].K;
        }
        haveTmcc = false;
        tmccCar.clear();
        for (int pos = 0; pos < kSegments; pos++) {
            const int s = kSegmentAtPosition[pos];
            if (seg[s].layer < 0) continue;
            for (int k : tmccCarrierList(mode, pos, seg[s].diff)) tmccCar.push_back({k, seg[s].diff ? 0 : 1});
        }
        started = false; symbols = 0; prevValid = false; sigma2 = 1e-3;
        for (auto& pe : pend) pendSpare.push_back(std::move(pe));
        pend.clear(); prevOutValid = false; ceD1 = ceD2 = 0; ceAlpha = 1; ceStats = 0; outCount = 0;
        accPhi = accSlope = 0; cpRefValid = false;
        cpK.clear();
        for (int pos = 0; pos < kSegments; pos++) {
            const int s = kSegmentAtPosition[pos];
            if (seg[s].layer >= 0 && seg[s].diff) cpK.push_back(pos * cps);
        }
        cpK.push_back(kEdge);
        cpRef.assign(cpK.size(), cf32(1, 0));
        // runs of adjacent synchronous segments in frequency order
        runs.clear();
        for (int pos = 0; pos < kSegments;) {
            const int s = kSegmentAtPosition[pos];
            if (seg[s].layer >= 0 && !seg[s].diff) {
                int q = pos;
                while (q + 1 < kSegments && seg[kSegmentAtPosition[q + 1]].layer >= 0 && !seg[kSegmentAtPosition[q + 1]].diff) q++;
                Run r; r.p0 = pos; r.p1 = q;
                const int carriers = (q - pos + 1) * cps + (runTopPilot(q) ? 1 : 0);
                r.grid.assign((size_t)(carriers + 2) / 3, cf32(0, 0));
                r.gv.assign(r.grid.size(), 0);
                r.raw1.assign(r.grid.size(), cf32(0, 0)); r.raw2 = r.raw1; r.rawN.assign(r.grid.size(), 0); r.cnt.assign(r.grid.size(), 0);
                r.hv.assign(r.grid.size() * kHist, cf32(0, 0)); r.ht.assign(r.grid.size() * kHist, -1);
                runs.push_back(std::move(r));
                pos = q + 1;
            } else pos++;
        }
        tiV.assign((size_t)kSegments * dps, {});
        tiN.assign(tiV.size(), {});
        tiPos.assign(tiV.size(), 0);
        for (int s = 0; s < kSegments; s++) {
            if (seg[s].layer < 0) continue;
            const int I = interleavingLength(mode, p.layer[seg[s].layer].ti);
            for (int i = 0; i < dps; i++) {
                const size_t d = (size_t)I * (size_t)(95 - (i * 5) % 96);
                tiV[(size_t)s * dps + (size_t)i].assign(d, cf32(0, 0));
                tiN[(size_t)s * dps + (size_t)i].assign(d, kErasure);
            }
        }
        chDb.assign((size_t)K, -100.f);
    }

    // pilot values of the carriers with constant pilots
    cf32 cpPilot(int k) const {
        if (k == kEdge) return cf32(edgeCarrierValue(mode, p.tsb), 0);
        const int pos = k / cps;
        return cf32(pilotValue(prbsW(mode, kSegmentAtPosition[pos])[0]), 0);
    }

    void track(const cf32* Yraw, int symIdx) {
        const int kc = K / 2;
        const int sp = symIdx % 4;
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, tot = 0;
        std::complex<double> acc(0, 0);
        auto addPoint = [&](int k, cf32 z, cf32 ref) {
            // z: pilot-normalised value after the previous correction, ref: its expected value (the channel)
            const double a = accPhi + accSlope * (double)(k - kc);
            const std::complex<double> r = std::complex<double>(z.real(), z.imag()) * std::polar(1.0, -a) * std::conj(std::complex<double>(ref.real(), ref.imag()));
            const double w = std::abs(r);
            if (w < 1e-12) return;
            acc += r / w;
            tot += 1;
            sw += w;
            (void)sx; (void)sy; (void)sxx; (void)sxy;
        };
        (void)addPoint; (void)sp; (void)tot; (void)acc; (void)sw;
        // two passes: a common phase first, then a ramp from the residual
        struct P { int k; double ph; double w; bool decided; };
        std::vector<P> pts;
        auto collect = [&](int k, cf32 z, cf32 ref, bool decided = false) {
            const double a = accPhi + accSlope * (double)(k - kc);
            const std::complex<double> r = std::complex<double>(z.real(), z.imag()) * std::polar(1.0, -a) * std::conj(std::complex<double>(ref.real(), ref.imag()));
            const double w = std::abs(r);
            if (w < 1e-12) return;
            pts.push_back({k, std::arg(r), w, decided});
        };
        for (auto& r : runs) {
            if (r.filled < 4) continue;
            const int k0 = r.p0 * cps;
            for (int pos = r.p0; pos <= r.p1; pos++) {
                const auto& w = prbsW(mode, kSegmentAtPosition[pos]);
                for (int i = 3 * sp; i < cps; i += 12) {
                    const int k = pos * cps + i;
                    // against the same pilot four symbols ago (the equaliser's estimate belongs to a symbol kTiDelay back)
                    const size_t g = (size_t)(k - k0) / 3;
                    if (r.rawN[g]) collect(k, Yraw[k] / pilotValue(w[(size_t)i]), r.raw1[g]);
                }
            }
        }
        // the TMCC carriers: their sign flips are known once the TMCC is decoded, so against the preceding symbol they are pilots at fixed places
        if (haveTmcc && prevValid && symIdx >= 17)
            for (const auto& tc : tmccCar) collect(tc.first, Yraw[tc.first], prevY[(size_t)tc.first] * (tmccBits[tc.second][symIdx] ? -1.f : 1.f), true);
        if (cpRefValid && runs.empty()) for (size_t j = 0; j < cpK.size(); j++) {
            // a continual pilot that belongs to a differential segment (or the last carrier)
            const int k = cpK[j];
            if (k == kEdge || (seg[kSegmentAtPosition[k / cps]].diff)) collect(k, Yraw[k] / cpPilot(k), cpRef[j]);
        }
        if (!pts.empty()) {
            std::complex<double> sum(0, 0);
            // the TMCC carriers' expected flips come from the last decoded TMCC word, which a change of the signalling (a parameter switch, its
            // countdown) makes wrong: a point that lies the wrong way round is such a bit and is left out, so that it cannot pull the
            // tracking away and with it the TMCC that would bring the new word
            {
                std::complex<double> ps(0, 0);
                for (auto& q : pts) if (!q.decided) ps += std::polar(q.w, q.ph);
                if (std::abs(ps) > 0) {
                    const double ref = std::arg(ps);
                    pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const P& q) { return q.decided && std::cos(q.ph - ref) < 0; }), pts.end());
                }
            }
            for (auto& q : pts) sum += std::polar(q.w, q.ph);
            const double phi0 = std::arg(sum);
            double Sw = 0, Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
            for (auto& q : pts) {
                double d = q.ph - phi0;
                while (d > M_PI) d -= 2 * M_PI;
                while (d < -M_PI) d += 2 * M_PI;
                const double x = (double)(q.k - kc);
                Sw += q.w; Sx += q.w * x; Sy += q.w * d; Sxx += q.w * x * x; Sxy += q.w * x * d;
            }
            double dSlope = 0, dPhi = phi0;
            const double det = Sw * Sxx - Sx * Sx;
            if (pts.size() >= 6 && det > 1e-12 * Sw * Sw) { dSlope = (Sw * Sxy - Sx * Sy) / det; dPhi = phi0 + (Sy - dSlope * Sx) / Sw; }
            accPhi += dPhi; accSlope += dSlope;
        }
        Yd.resize((size_t)K);
        for (int k = 0; k < K; k++) { const double a = -(accPhi + accSlope * (double)(k - kc)); Yd[(size_t)k] = Yraw[k] * cf32((float)std::cos(a), (float)std::sin(a)); }
        for (size_t j = 0; j < cpK.size(); j++) {
            const int k = cpK[j];
            const cf32 v = Yd[(size_t)k] / cpPilot(k);
            cpRef[j] = cpRefValid ? cpRef[j] * 0.85f + v * 0.15f : v;
        }
        cpRefValid = true;
    }

    // the time-averaged estimate of grid point g of run r at symbol m: a cubic through the two estimates before m and the two after it, a line
    // where fewer are known, the newest where nothing follows (as dvbt_rx.cpp timeInterp)
    cf32 timeInterp(const Run& r, size_t g, int64_t m) const {
        const cf32* hv = &r.hv[g * kHist]; const int64_t* ht = &r.ht[g * kHist];
        int j1 = -1;   // the first estimate after m
        for (int j = 0; j < kHist; j++) if (ht[j] >= 0 && ht[j] > m) { j1 = j; break; }
        if (j1 < 0) return r.grid[g];
        if (j1 == 0 || ht[j1 - 1] < 0) return hv[j1];
        if (j1 >= 2 && j1 + 1 < kHist && ht[j1 - 2] >= 0 && ht[j1 + 1] >= 0) {
            const double t[4] = {(double)ht[j1 - 2], (double)ht[j1 - 1], (double)ht[j1], (double)ht[j1 + 1]};
            std::complex<double> acc = 0;
            for (int a = 0; a < 4; a++) {
                double w = 1;
                for (int b = 0; b < 4; b++) if (b != a) w *= ((double)m - t[b]) / (t[a] - t[b]);
                const cf32 v = hv[j1 - 2 + a];
                acc += std::complex<double>(v.real(), v.imag()) * w;
            }
            return cf32((float)acc.real(), (float)acc.imag());
        }
        const double f = (double)(m - ht[j1 - 1]) / (double)(ht[j1] - ht[j1 - 1]);
        return hv[j1 - 1] + (hv[j1] - hv[j1 - 1]) * (float)f;
    }
    // the pilot above the band (a pilot in every symbol) at symbol m: the mean of the symbols m - 1 .. m + 1
    cf32 edgeAt(const Run& r, int64_t m, cf32 fallback) const {
        cf32 s(0, 0); int n = 0;
        for (const auto& e : r.edge) if (e.first >= m - 1 && e.first <= m + 1) { s += e.second; n++; }
        return n ? s / (float)n : fallback;
    }

    void pushSymbol(const cf32* Yraw, int symIdx) {
        symbols++;
        const cf32* Y;
        if (trackOn) { track(Yraw, symIdx); Y = Yd.data(); } else Y = Yraw;
        const int sp = symIdx % 4;
        const int64_t now = (int64_t)symbols;
        // ---- the scattered pilots of this symbol refresh a quarter of each run's grid
        double d1 = 0, d2 = 0; long n1 = 0, n2 = 0;
        for (auto& r : runs) {
            const int k0 = r.p0 * cps;
            const int top = runTopPilot(r.p1);
            const int carriers = (r.p1 - r.p0 + 1) * cps + (top ? 1 : 0);
            for (int pos = r.p0; pos <= r.p1; pos++) {
                const int s = kSegmentAtPosition[pos];
                const auto& w = prbsW(mode, s);
                for (int i = 3 * sp; i < cps; i += 12) {
                    const int k = pos * cps + i;
                    const size_t g = (size_t)(k - k0) / 3;
                    const cf32 v = Y[k] / pilotValue(w[(size_t)i]);
                    // the same pilot four and eight symbols ago: noise (twice) plus how far the channel moved
                    if (r.rawN[g] >= 1) { d1 += std::norm(v - r.raw1[g]); n1++; }
                    if (r.rawN[g] >= 2) { d2 += std::norm(v - r.raw2[g]); n2++; }
                    r.raw2[g] = r.raw1[g]; r.raw1[g] = v; if (r.rawN[g] < 2) r.rawN[g]++;
                    if (ceNoTi || r.cnt[g] == 0) { r.grid[g] = v; r.cnt[g] = 1; }
                    else {
                        r.cnt[g] = (uint16_t)std::min(1000, r.cnt[g] + 1);
                        const float a = (float)std::max(ceAlpha, 1.0 / r.cnt[g]);
                        r.grid[g] += a * (v - r.grid[g]);
                    }
                    cf32* hv = &r.hv[g * kHist]; int64_t* ht = &r.ht[g * kHist];
                    for (int j = 0; j + 1 < kHist; j++) { hv[j] = hv[j + 1]; ht[j] = ht[j + 1]; }
                    hv[kHist - 1] = r.grid[g]; ht[kHist - 1] = now;
                    r.gv[g] = 1;
                }
            }
            if (top) {
                const cf32 v = Y[top] / cpPilot(top);
                r.grid[(size_t)(carriers - 1) / 3] = v;
                r.edge.push_back({now, v});
                while (r.edge.size() > (size_t)(2 * kTiDelay + 4)) r.edge.pop_front();
            }
            if (r.filled < 4) r.filled++;
            if (r.filled < 4) continue;
            // the delay of the channel (the phase slope of the pilot grid), so that the interpolator sees a nearly flat grid and the ends of the run are
            // extended correctly; averaged over a few symbols
            {
                std::complex<double> c(0, 0);
                for (size_t g = 0; g + 1 < r.grid.size(); g++) c += std::complex<double>(r.grid[g + 1].real(), r.grid[g + 1].imag()) * std::conj(std::complex<double>(r.grid[g].real(), r.grid[g].imag()));
                const double m = std::abs(c);
                if (m > 0) c /= m;
                r.lag = r.lag == std::complex<double>(0, 0) ? c : 0.85 * r.lag + 0.15 * c;
            }
        }
        // ---- noise and the speed of the channel: E|p - p(-4)|^2 = 2 r + q and E|p - p(-8)|^2 = 2 r + 4 q for a channel that changes smoothly
        // (r: the noise of one pilot, q: the channel's change between two pilots of a grid point). The old estimate, half the first, counted the
        // Doppler as noise. The averaging gain a: its noise r a / (2 - a), its lag behind a moving channel q ((1 - a) / a)^2 (dvbt_rx.cpp).
        if (n1 > 0) {
            const double s2old = std::max(1e-9, 0.5 * d1 / (double)n1 * (16.0 / 9.0) * kNoiseCal);
            if (ceNoTi || n2 == 0) { if (ceStats < 8 || ceNoTi) sigma2 = 0.9 * sigma2 + 0.1 * s2old; }
            else {
                d1 /= (double)n1; d2 /= (double)n2;
                if (ceStats == 0) { ceD1 = d1; ceD2 = d2; } else { ceD1 += 0.08 * (d1 - ceD1); ceD2 += 0.08 * (d2 - ceD2); }
                ceStats++;
                if (ceStats >= 8) {
                    const double rr = std::max(1e-12, (4 * ceD1 - ceD2) / 6), q = std::max(0.0, (ceD2 - ceD1) / 3);
                    double best = 1, bestE = 1e300;
                    for (int i = 1; i <= 20; i++) {
                        const double a = i / 20.0;
                        const double e = q * ((1 - a) / a) * ((1 - a) / a) + rr * a / (2 - a);
                        if (e < bestE) { bestE = e; best = a; }
                    }
                    ceAlpha = std::max(1.0 / 10, best);
                    sigma2 = 0.9 * sigma2 + 0.1 * std::max(1e-9, rr * (16.0 / 9.0) * kNoiseCal);
                } else sigma2 = 0.9 * sigma2 + 0.1 * s2old;
            }
        }
        if (runs.empty() && cpRefValid) {
            double noiseAcc = 0; long noiseCnt = 0;
            for (size_t j = 0; j + 1 < cpK.size(); j++) { noiseAcc += std::norm(Y[(size_t)cpK[j]] / cpPilot(cpK[j]) - cpRef[j]); noiseCnt++; }
            noiseAcc *= 1.2;
            if (noiseCnt > 0) sigma2 = 0.9 * sigma2 + 0.1 * std::max(1e-9, noiseAcc / (double)noiseCnt * (16.0 / 9.0) * kNoiseCal);
        }
        if (runs.empty() && sigma2 > 0) {   // without scattered pilots: the signal power of all carriers against the noise from the continual pilots
            double pw = 0; long n = 0;
            for (int k = firstPosition(p.tsb) * cps; k <= kEdge; k += 5) { pw += std::norm(Y[k]); n++; }
            if (n) snr = 10 * std::log10(std::max(1e-9, (pw / (double)n - sigma2) / sigma2));
        }
        prevY.assign(Y, Y + K);   // the tracking's reference for the TMCC carriers
        prevValid = true;
        // ---- the symbol goes to the equaliser kTiDelay symbols later, when its channel can be interpolated in time
        Pend pe;
        if (!pendSpare.empty()) { pe = std::move(pendSpare.back()); pendSpare.pop_back(); }
        pe.Y.assign(Y, Y + K); pe.symIdx = symIdx; pe.t = now;
        pend.push_back(std::move(pe));
        const int64_t upTo = now - (ceNoTi ? 0 : kTiDelay);
        while (!pend.empty() && pend.front().t <= upTo) {
            Pend q = std::move(pend.front());
            pend.pop_front();
            equalise(q);
            pendSpare.push_back(std::move(q));
        }
    }

    void equalise(const Pend& pq) {
        const cf32* Y = pq.Y.data();
        const int symIdx = pq.symIdx;
        if (!started && symIdx == 0) started = true;   // the layer decoders begin with a whole frame; tracking and channel estimation run all the time
        outCount++;
        const uint16_t* rnd = mode == 1 ? tables::kRandomizing1 : mode == 2 ? tables::kRandomizing2 : tables::kRandomizing3;
        std::vector<uint8_t> roles((size_t)cps);
        ceK.resize((size_t)cps);
        // per segment: received data values and their noise variance, in data-carrier order
        std::vector<std::vector<cf32>> val(kSegments);
        std::vector<std::vector<float>> nv(kSegments);
        const int sp = symIdx % 4;
        const bool ici = iciWanted();
        const cf32* Yq = Y;   // the values that are equalised: Y, or Y with the inter-carrier interference taken off
        if (ici) { iciY.assign(Y, Y + K); Yq = iciY.data(); iciSyms++; }
        // ---- synchronous segments: channel estimate from the scattered pilots
        for (auto& r : runs) {
            const int k0 = r.p0 * cps;
            const int top = runTopPilot(r.p1);
            const int carriers = (r.p1 - r.p0 + 1) * cps + (top ? 1 : 0);
            if (r.filled < 4) continue;
            const double tauRun = r.lag == std::complex<double>(0, 0) ? tau0 : -std::arg(r.lag) * (double)N / (2.0 * M_PI * 3.0);
            std::vector<cf32>& gi = scrG;
            if (ceNoTi) gi = r.grid;
            else {
                gi.resize(r.grid.size());
                for (size_t g = 0; g < gi.size(); g++) gi[g] = timeInterp(r, g, pq.t);
                if (top) gi[(size_t)(carriers - 1) / 3] = edgeAt(r, pq.t, r.grid[(size_t)(carriers - 1) / 3]);
            }
            if (!ceNoTi && r.dlyKnown) interpN.run(gi, 3, carriers, N, tauRun + r.dlyRel, r.H, std::min(1.0, r.dlyHalf / ((double)N / 6.0)));
            else interp.run(gi, 3, carriers, N, tauRun, r.H, 1.0);
            // the delay window: measured on the full-window estimate every 16 symbols (a narrowed one cannot show an echo that has just appeared
            // outside it): the paths' span with margin, as the DVB-T receiver does
            if (!ceNoTi && (outCount == 8 || (outCount > 8 && outCount % 16 == 0))) {
                std::vector<cf32>& Hw = scrHw;
                if (r.dlyKnown) interp.run(gi, 3, carriers, N, tauRun, Hw, 1.0); else Hw = r.H;
                const int c = (int)std::lround(tauRun), lim = N / 6 - 16;
                int tMin = 0, tMax = 0;
                if (delaySpan(Hw, N, -lim, lim, c, tMin, tMax)) {
                    const double centre = 0.5 * (tMin + tMax), half = 0.5 * (tMax - tMin) * 1.4 + 48;
                    r.dlyKnown = half / ((double)N / 6.0) < 0.9;
                    r.dlyRel = (double)c + centre - tauRun; r.dlyHalf = half;
                } else r.dlyKnown = false;
            }
            if (ici) iciCancel(r, Y, pq.t, symIdx, carriers, top, tauRun);
            if (outCount % 4 == 0) {
                if (&r == &runs.front()) pilotShow.clear();
                for (int pos = r.p0; pos <= r.p1; pos++) {
                    const auto& w = prbsW(mode, kSegmentAtPosition[pos]);
                    for (int i = 3 * sp; i < cps; i += 12) { const int k = pos * cps + i; const cf32 h = r.H[(size_t)(k - k0)]; if (std::norm(h) > 1e-12f) pilotShow.push_back(Y[k] / pilotValue(w[(size_t)i]) / h); }
                }
            }
            for (int pos = r.p0; pos <= r.p1; pos++) {
                const int s = kSegmentAtPosition[pos];
                segmentRoles(mode, s, false, symIdx, roles.data());
                val[(size_t)s].assign((size_t)dps, cf32(0, 0));
                nv[(size_t)s].assign((size_t)dps, kErasure);
                int di = 0;
                for (int i = 0; i < cps; i++) {
                    if (roles[(size_t)i] != kData) continue;
                    const int k = pos * cps + i;
                    const cf32 h = r.H[(size_t)(k - k0)];
                    const float g2 = std::max(1e-9f, std::norm(h));
                    val[(size_t)s][(size_t)di] = Yq[k] * std::conj(h) / g2;
                    nv[(size_t)s][(size_t)di] = (float)(sigma2 / g2);
                    ceK[(size_t)di] = k;
                    di++;
                }
                if (flatN0 && di > 0) { double m = 0; for (int j = 0; j < di; j++) m += nv[(size_t)s][(size_t)j]; m /= di; for (int j = 0; j < di; j++) nv[(size_t)s][(size_t)j] = (float)m; }   // A/B: the mean for all
                carrierErase.apply(val[(size_t)s].data(), nv[(size_t)s].data(), ceK.data(), di, K);   // carriers hit by a narrowband interferer
            }
        }
        if (!runs.empty()) {
            // power of the channel against the noise
            double hp = 0; long hn = 0;
            for (auto& rr : runs) for (size_t i = 0; i < rr.H.size(); i += 7) { hp += std::norm(rr.H[i]); hn++; }
            if (hn) snr = 10 * std::log10(std::max(1e-9, hp / (double)hn / sigma2));
        }
        if (diag41 && symbols % 204 == 0)
            fprintf(stderr, "[isdbt] sym %llu snr %.1f dB sigma2 %.2e q %.2e alpha %.2f ici %d (%llu symbols)\n", (unsigned long long)symbols, snr, sigma2,
                    std::max(0.0, (ceD2 - ceD1) / 3), ceAlpha, (int)ici, (unsigned long long)iciSyms);
        if (getenv("ISDBT_EVM")) {
            static double evm[13][8]; static long cnt[13][8];
            for (int pos = 0; pos < kSegments; pos++) {
                const int s = kSegmentAtPosition[pos];
                if (seg[s].layer < 0 || seg[s].diff || val[(size_t)s].empty()) continue;
                const int mod = p.layer[seg[s].layer].mod;
                for (int i = 0; i < dps; i++) {
                    const cf32 v = val[(size_t)s][(size_t)i];
                    if (nv[(size_t)s][(size_t)i] > 1e5f) continue;
                    float best = 1e9f;
                    for (unsigned lab = 0; lab < (1u << bitsPerCell(mod)); lab++) best = std::min(best, std::norm(v - mapLabel(mod, lab)));
                    // position in the segment in 8 slots of data carriers (to see edge effects)
                    const int slot = i * 8 / dps;
                    evm[pos][slot] += best; cnt[pos][slot]++;
                }
            }
            if (symbols % 408 == 0) {
                fprintf(stderr, "[evm dB] ");
                for (int pos = 0; pos < kSegments; pos++) { double a = 0; long n = 0; for (int sl = 0; sl < 8; sl++) { a += evm[pos][sl]; n += cnt[pos][sl]; } fprintf(stderr, "%5.1f ", n ? 10 * std::log10(std::max(1e-12, a / (double)n)) : 0.0); }
                fprintf(stderr, "\n");
                for (auto& r : evm) for (double& x : r) x = 0;
                for (auto& r : cnt) for (long& x : r) x = 0;
            }
        }
        // ---- differential segments: phase step against the preceding symbol
        for (int pos = 0; pos < kSegments; pos++) {
            const int s = kSegmentAtPosition[pos];
            if (seg[s].layer < 0 || !seg[s].diff) continue;
            segmentRoles(mode, s, true, symIdx, roles.data());
            val[(size_t)s].assign((size_t)dps, cf32(0, 0));
            nv[(size_t)s].assign((size_t)dps, kErasure);
            if (!prevOutValid) continue;
            int di = 0;
            for (int i = 0; i < cps; i++) {
                if (roles[(size_t)i] != kData) continue;
                const int k = pos * cps + i;
                const cf32 u = Y[k] * std::conj(prevOut[(size_t)k]);
                const float pw = std::max(1e-9f, 0.5f * (std::norm(Y[k]) + std::norm(prevOut[(size_t)k])) - (float)sigma2);
                const float mag = std::abs(u);
                if (mag > 1e-12f) { val[(size_t)s][(size_t)di] = u / mag; nv[(size_t)s][(size_t)di] = (float)(2.0 * sigma2 / pw + sigma2 * sigma2 / (pw * pw)) ; }
                di++;
            }
        }
        prevOut.assign(Y, Y + K);
        prevOutValid = true;
        // ---- frequency de-interleaving, time de-interleaving, back to the layers
        std::vector<std::vector<cf32>> dv(kSegments);
        std::vector<std::vector<float>> dn(kSegments);
        for (int g = 0; g < 3; g++) {
            std::vector<int> members;
            for (int s = 0; s < kSegments; s++) if (seg[s].layer >= 0 && seg[s].group == g) members.push_back(s);
            const int n = (int)members.size();
            if (!n) continue;
            for (int s : members) { dv[(size_t)s].assign((size_t)dps, cf32(0, 0)); dn[(size_t)s].assign((size_t)dps, kErasure); }
            for (int k = 0; k < n; k++) {
                const int s = members[(size_t)k];
                if (val[(size_t)s].empty()) continue;
                for (int i = 0; i < dps; i++) {
                    const int src = rnd[i];                         // rot[i] = R[rnd[i]]
                    const cf32 v = val[(size_t)s][(size_t)src];
                    const float nn = nv[(size_t)s][(size_t)src];
                    const int ii = (i + k) % dps;                   // inter[(i + k) % dps] = rot[i]
                    if (g == 0) { dv[(size_t)s][(size_t)ii] = v; dn[(size_t)s][(size_t)ii] = nn; }
                    else {
                        const int idx = ii * n + k;                 // original member idx / dps, cell idx % dps
                        dv[(size_t)members[(size_t)(idx / dps)]][(size_t)(idx % dps)] = v;
                        dn[(size_t)members[(size_t)(idx / dps)]][(size_t)(idx % dps)] = nn;
                    }
                }
            }
        }
        // time de-interleaving and assembling the layers' cell vectors
        int firstSeg[3] = {0, 0, 0};
        for (int s = kSegments - 1; s >= 0; s--) if (seg[s].layer >= 0) firstSeg[seg[s].layer] = s;
        Job& job = acquire();
        std::vector<cf32>* cells = job.cells;
        std::vector<float>* nn = job.nn;
        job.symIdx = symIdx;
        for (int li = 0; li < 3; li++) if (p.layer[li].used()) { cells[li].assign((size_t)ncOf[li], cf32(0, 0)); nn[li].assign((size_t)ncOf[li], kErasure); }
        for (int s = 0; s < kSegments; s++) {
            if (seg[s].layer < 0) continue;
            const int li = seg[s].layer, local = s - firstSeg[li];
            for (int i = 0; i < dps; i++) {
                cf32 v = dv[(size_t)s].empty() ? cf32(0, 0) : dv[(size_t)s][(size_t)i];
                float x = dn[(size_t)s].empty() ? kErasure : dn[(size_t)s][(size_t)i];
                auto& fv = tiV[(size_t)s * dps + (size_t)i];
                if (!fv.empty()) {
                    auto& fn_ = tiN[(size_t)s * dps + (size_t)i];
                    size_t& pos = tiPos[(size_t)s * dps + (size_t)i];
                    const cf32 ov = fv[pos]; const float on = fn_[pos];
                    fv[pos] = v; fn_[pos] = x;
                    if (++pos == fv.size()) pos = 0;
                    v = ov; x = on;
                }
                cells[li][(size_t)local * dps + (size_t)i] = v;
                nn[li][(size_t)local * dps + (size_t)i] = x;
            }
        }
        if (started) submit(job);
        if (outCount % 8 == 0) {
            eq.clear();
            for (int s = 0; s < kSegments; s++) if (!val[(size_t)s].empty() && !seg[s].diff) for (int i = 0; i < dps; i += 4) if (nv[(size_t)s][(size_t)i] < 1e6f) eq.push_back(val[(size_t)s][(size_t)i]);
            if (eq.size() > 1500) { std::vector<cf32> e2; for (size_t i = 0; i < eq.size(); i += eq.size() / 1500 + 1) e2.push_back(eq[i]); eq.swap(e2); }
        }
    }
};

Demod::Demod() : impl_(new Impl) {}
Demod::~Demod() = default;
void Demod::configure(const Params& p) { p_ = p; impl_->configure(p); }
void Demod::setTmccInfo(const uint8_t info[kTmccInfoBits]) {
    Impl& I = *impl_;
    tmccFrameBits(info, true, true, I.tmccBits[0]);
    tmccFrameBits(info, true, false, I.tmccBits[1]);
    I.haveTmcc = true;
}
void Demod::pushSymbol(const cf32* Y, int symIdx) { impl_->pushSymbol(Y, symIdx); }
void Demod::setDelayCentre(double tau0) { impl_->tau0 = tau0; }
void Demod::takePackets(int layer, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> g(impl_->mu);
    auto& l = impl_->pub[layer];
    out.insert(out.end(), l.out.begin(), l.out.end());
    l.out.clear();
}
size_t Demod::Impl::mergeInto(std::vector<uint8_t>& out) {
    Impl& I = *this;
    std::lock_guard<std::mutex> g(I.mu);
    // all layers' packets up to the time the slowest active layer has reached
    double watermark = 1e30;
    bool any = false;
    for (int li = 0; li < 3; li++) {
        auto& l = I.pub[li];
        if (!I.p.layer[li].used()) continue;
        any = true;
        if (l.merged.empty()) { watermark = std::min(watermark, l.packetsOut == 0 ? 0.0 : (double)l.packetsOut / std::max(1, l.K)); }
        else watermark = std::min(watermark, l.merged.back().first);
    }
    if (!any) return 0;
    // a layer that has not produced anything for a long time must not hold the others back
    double maxFrac = 0;
    for (int li = 0; li < 3; li++) if (I.p.layer[li].used() && !I.pub[li].merged.empty()) maxFrac = std::max(maxFrac, I.pub[li].merged.back().first);
    if (maxFrac - watermark > 3.0) watermark = maxFrac - 3.0;
    size_t n = 0;
    for (;;) {
        int best = -1;
        double bf = 1e30;
        for (int li = 0; li < 3; li++) {
            auto& l = I.pub[li];
            if (!I.p.layer[li].used() || l.merged.empty()) continue;
            if (l.merged.front().first <= watermark && l.merged.front().first < bf) { bf = l.merged.front().first; best = li; }
        }
        if (best < 0) break;
        auto& l = I.pub[best];
        out.insert(out.end(), l.merged.front().second.begin(), l.merged.front().second.end());
        l.merged.pop_front();
        n++;
    }
    return n;
}
size_t Demod::takeMerged(std::vector<uint8_t>& out) {
    Impl& I = *impl_;
    size_t n = I.carryOut.size() / 188;
    out.insert(out.end(), I.carryOut.begin(), I.carryOut.end());
    I.carryOut.clear();
    return n + I.mergeInto(out);
}
const LayerStats& Demod::layerStats(int layer) const {
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->statSnap[layer] = impl_->pub[layer].st;
    return impl_->statSnap[layer];
}
double Demod::noiseVariance() const { return impl_->sigma2; }
double Demod::snrDb() const { return impl_->snr; }
const std::vector<cf32>& Demod::eqCells() const { return impl_->eq; }
const std::vector<cf32>& Demod::pilotCells() const { return impl_->pilotShow; }
void Demod::channel(std::vector<cf32>& H) const {
    const Impl& I = *impl_;
    H.assign((size_t)I.K, cf32(0, 0));
    std::vector<uint8_t> have((size_t)I.K, 0);
    for (const auto& r : I.runs) {
        const int k0 = r.p0 * I.cps;
        for (size_t i = 0; i < r.H.size() && (size_t)k0 + i < (size_t)I.K; i++) { H[(size_t)k0 + i] = r.H[i]; have[(size_t)k0 + i] = 1; }
    }
    // gaps (differential segments have no scattered pilots): hold the nearest estimate
    cf32 last(0, 0); bool seen = false;
    for (int k = 0; k < I.K; k++) { if (have[(size_t)k]) { last = H[(size_t)k]; seen = true; } else if (seen) H[(size_t)k] = last; }
    seen = false; last = cf32(0, 0);
    for (int k = I.K - 1; k >= 0; k--) { if (have[(size_t)k]) { last = H[(size_t)k]; seen = true; } else if (!have[(size_t)k] && std::norm(H[(size_t)k]) == 0 && seen) H[(size_t)k] = last; }
}
const std::vector<cf32>& Demod::corrected() const { return impl_->trackOn ? impl_->Yd : impl_->prevY; }
const std::vector<float>& Demod::channelDb() const { return impl_->chDb; }
uint64_t Demod::symbolsDone() const { return impl_->symbols; }

} // namespace isdbt
} // namespace dect2
