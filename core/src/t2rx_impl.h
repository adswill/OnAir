#include "dect2/t2rx.h"
#include "dect2/t2ofdm.h"
#include "dect2/resampler.h"
#include "dect2/platform.h"
#include "dect2/t2pilots.h"
#include "dect2/t2l1.h"
#include "dect2/t2interleave.h"

#include "dect2/dsp_compat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <string>
#include <vector>
// The inside of the DVB-T2 receiver (t2rx.cpp has the public interface). The work is spread over t2rx.cpp (driver, resampler thread, telemetry),
// t2rx_sync.cpp (P1 and guard interval), t2rx_symbols.cpp (OFDM symbols), t2rx_l1.cpp (P2 and L1 signalling) and t2rx_data.cpp (data stage).
#pragma once

namespace dect2 {

// Where the receiver thread spends its time (shown in the log when a source stops): resampler, P1 search, guard check, symbols, FFTs, data stage.
namespace t2rxi {
struct StageClock {
        std::chrono::steady_clock::time_point t0;
    int which;
    explicit StageClock(int w) : t0(std::chrono::steady_clock::now()), which(w) {}
    ~StageClock() { g()[which] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
    static double* g() { static double v[9] = {}; return v; }
};
}
using cd = std::complex<double>;
static constexpr double kTwoPi = 6.283185307179586;

namespace t2rxi {

constexpr double kP1Threshold = 0.30; // C-A-B correlation needed to try decoding
constexpr double kP1MinConf = 0.40;   // decoded S1/S2 sequence correlation needed to accept
constexpr int kPeakHalfWidth = 700;
inline bool trackDisabled() { static const bool v = getenv("DECT2_NOTRACK") != nullptr; return v; }   // switch the windowed P1 search off
constexpr int kTrackWindow = 1200;    // half width of the window around the expected P1 once locked (samples at the native rate)

struct Frame {
    int64_t anchor;      // absolute index of the first symbol after P1
    int next = 0;        // next symbol index to process
    int maxSyms = 0;
};

} // namespace t2rxi

using namespace t2rxi;

struct T2Receiver::Impl {
    // ---- configuration
    double inRate = 0, fn = 0;
    bool decimate = false, rateOk = true;
    RationalResampler resampler;
    std::vector<cf32> rsOut;

    // ---- optional resampler stage on its own thread (live radios): input chunks go in, resampled chunks come back in the same order
    struct Stage {
        std::thread th;
        std::mutex mu;      // the queues
        std::mutex rsMu;    // the resampler: held by the worker while it resamples, and by whoever reconfigures or resets it
        std::condition_variable cvIn, cvSpace;
        std::deque<std::pair<uint64_t, std::vector<cf32>>> in, out;
        uint64_t gen = 0;   // bumped by a reset: chunks of an earlier generation are dropped
        bool stop = false;
    } stage;
    static constexpr size_t kStageMaxIn = 32;
    bool pipelined = false;

    void stageLoop();
    void stageStart() {
        { std::lock_guard<std::mutex> lk(stage.mu); stage.stop = false; }
        stage.th = std::thread([this] { stageLoop(); });
    }
    void stageStop() {
        if (!stage.th.joinable()) return;
        { std::lock_guard<std::mutex> lk(stage.mu); stage.stop = true; stage.gen++; stage.in.clear(); stage.out.clear(); }
        stage.cvIn.notify_all();
        stage.cvSpace.notify_all();
        stage.th.join();
    }
    void stageFlush() {   // forget everything that is queued or being resampled
        std::lock_guard<std::mutex> lk(stage.mu);
        stage.gen++; stage.in.clear(); stage.out.clear();
        stage.cvSpace.notify_all();
    }
    ~Impl() { stageStop(); }

    // ---- native-rate buffer
    std::vector<cf32> buf;
    int64_t base = 0; // absolute index of buf[0]
    int64_t end() const { return base + (int64_t)buf.size(); }
    // Samples outside the buffered range read as silence: timing corrections can place a symbol slightly before the oldest retained
    // sample or after the newest one, and that must cost one symbol, not a crash.
    const cf32& at(int64_t abs) const {
        static const cf32 kZero(0.f, 0.f);
        const int64_t i = abs - base;
        if (i >= 0 && i < (int64_t)buf.size()) return buf[(size_t)i];
        return kZero;
    }

    // ---- FFT
    std::vector<float> fr, fi;
    void doFft(int log2n, bool inverse = false) { fftSplit(fr.data(), fi.data(), log2n, inverse); }

    // ---- P1 scanner
    int64_t scanPos = 0;
    bool scanFirst = true;
    std::vector<cd> pq1, pq2;   // scanP1 scratch (prefix sums), kept to avoid reallocating per chunk
    int64_t trackKeep = -1;               // samples from here on stay buffered while tracking, so a missed P1 can be searched for again
    int trackMiss = 0, trackFrames = 0;   // windowed P1 search: expected P1s not found in a row, P1s accepted since the last full pass
    double trackExpect = 0;               // where the next P1 is expected (absolute sample index), 0 = not set
    int64_t trackFullUntil = 0;           // search everything up to here
    std::vector<double> pe;
    std::vector<float> m;
    std::array<cd, 1024> phiTab; // exp(-j 2 pi g / 1024)
    int64_t lastP1Abs = INT64_MIN / 2;
    double rejectedPos = 0;
    bool haveRejected = false;
    std::vector<float> trace;

    // ---- state
    int state = 0;
    P1Info p1;
    uint64_t p1Count = 0;
    double prevPos = -1;
    int64_t giAnchor = 0;
    int giIdx = -1;
    float giScore[kNumGi] = {};
    float giMargin = 0;
    int fftN = 0, guard = 0, carriers = 0, nP2 = 0;
    int fftCode = -1, curS1 = -1;
    int frameSyms = 0;
    double frameLen = 0, sro = 0, frameMsv = 0;
    std::deque<Frame> frames;
    double cfoEst = 0;
    double cpCorrAvg = 0, timingAvg = 0;
    int lowCount = 0;
    int64_t gridOff = 0, pendingGridOff = 0;
    uint64_t symbols = 0, symCounter = 0;
    std::vector<cf32> prevCells;
    int64_t prevSymAbs = -1;
    int64_t lastP1Seen = 0;
    double frameCfo = 0;   // CFO used for every symbol of the current frame (constant, so phase stays continuous)
    int64_t curAnchor = 0;

    // ---- P2 stage
    int kMax = 0;
    std::vector<std::vector<cf32>> p2cells;
    GridInterpolator interp;
    bool chValid = false, extDetected = false;
    std::vector<cf32> chH;
    int chK = 0, irMin = 0;
    std::vector<float> chMag, chPh, irDb, snrDbv;
    float p2Snr = 0;
    std::vector<cf32> eqP2;
    L1Pre l1pre;
    L1Post l1post;
    std::atomic<int> plpSelect{-1};
    bool l1preOk = false, l1postOk = false;
    bool haveGoodL1 = false;       // the last L1 that passed its CRC, kept to bridge frames whose own L1 fails
    L1Pre goodPre; L1Post goodPost;
    int64_t goodAnchor = 0;
    int l1Reuse = 0;               // consecutive frames that used the kept L1
    uint64_t l1Reused = 0;
    uint64_t l1preGood = 0, l1preBad = 0, l1postGood = 0, l1postBad = 0;
    int l1Iters = 0;
    float l1N0 = 0;
    // data stage
    std::vector<std::vector<cf32>> frameCells;
    bool dataValid = false;
    float dataSnr = 0;
    int dataDx = 0, dataDy = 0, dataPp = 0;
    std::vector<cf32> eqData;
    double chDelayCentre = 0, chDelayHalf = 0; // delay support of the channel (samples rel. to the main path)
    bool chDelayKnown = false;
    std::vector<float> dataSnrCar;
    uint64_t dataFrames = 0;
    // PLP
    PlpDecoder plpDec;
    std::function<void(const PlpResult&)> plpCb;
    std::vector<std::vector<cf32>> p2Sym, p2G2;
    uint64_t frameCounter = 0;
    bool plpValid = false;
    int plpId = 0, plpBlocks = 0, plpSkipped = 0, selectedPlpId = -1;
    PlpFec plpFec;
    uint64_t plpFrames = 0, blocksOk = 0, blocksBad = 0, headersOk = 0, plpBchCorr = 0;
    double plpMer = 0, plpPre = 0, plpIters = 0, plpMs = 0;
    bool plpGpu = false;
    std::deque<std::vector<uint8_t>> blockMaps;
    std::vector<cf32> plpConst;
    std::vector<uint8_t> plpConstErr;
    std::vector<uint16_t> plpConstTx;
    uint64_t plpConstSeq = 0;
    int hUpl = 0, hDfl = 0, hSyncd = 0;

    // ---- outputs
    std::mutex mu;
    RxTelemetry tel;
    std::vector<cf32> p1Const, diffCells, rawCells;

    Impl() {
        for (int i = 0; i < 1024; i++) phiTab[i] = std::polar(1.0, -kTwoPi * i / 1024.0);
    }

    void resetAll();

    // ------------------------------------------------------------ P1 detection
    // The P1 correlator metric for the start positions d in [d0, d1] (relative to lo), into m[]. The prefix sums start at d0, so a call that
    // starts at 0 gives exactly the numbers the whole-chunk computation always gave.
    void p1Metric(int64_t lo, int64_t len, int64_t d0, int64_t d1);

    // Picks the P1 candidates among d in [dA, dB] (relative to lo) out of m[] and hands them on; true if one was accepted.
    bool p1Detect(int64_t lo, int64_t lastD, int64_t dA, int64_t dB);

    void scanP1();

    struct P1Decode {
        bool ok = false;
        int s1 = 0, s2 = 0;
        double conf = 0, cfo = 0, frac = 0;
        std::vector<cf32> z;
    };

    P1Decode decodeP1(int64_t d, double cfoC);

    void handleP1Candidate(int64_t d, float metric);

    void onFrameSpacing(double L);

    // ------------------------------------------------------------ guard-interval detection
    bool evaluateGi();

    // ------------------------------------------------------------ OFDM symbols
    void processFrames();

    // FFT of the symbol that starts at s (guard included); returns the K central carriers, CFO-corrected.
    void fftCells(int64_t s, std::vector<cf32>& cells, int K);

    // Channel estimate from the P2 pilots, equalisation, L1-pre / L1-post decoding.
    bool p2Hypothesis(bool ext, bool final);

    // Integer-bin frequency error: the CP correlation only resolves the offset modulo one carrier spacing, so a wrong
    // P1-based estimate can leave the whole spectrum shifted by whole carriers. Correlate the P2 pilots at shifts.
    int p2IntegerShift();

    void runP2Stage();

    // Assemble the frame's cell stream, cut out the PLP and hand it to the decoder threads.
    void submitPlp(const std::vector<cf32>& dstream, const std::vector<float>& dn0, double sigma2);

    void pollPlp();

    // Scattered-pilot channel estimation for the data / frame-closing symbols of the frame just received.
    void runDataStage();

    void processSymbol(int64_t s, int k);

    // ------------------------------------------------------------ driver
    void run();

    void publish();
};

} // namespace dect2
