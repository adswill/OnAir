// DRM receiver (ETSI ES 201 980): acquisition, OFDM tracking, channel estimation, and the hand-over to the back end (FAC, SDC, MSC) in drm_dec.cpp.
//
//   input -> channel filter, 48 kHz (drm_front) -> guard interval correlation (mode, symbol timing, carrier offset modulo the carrier spacing)
//         -> frequency reference triplet (whole carrier spacings) -> OFDM demodulation, one FFT per symbol
//   DRM+ (mode E, 100 kHz) has a lane of its own at 192 kHz, searched alongside while the input rate allows (100 ksps and up): guard interval
//         correlation, then the whole carrier spacings from the pattern of the gain references and the band edges (mode E has no frequency
//         references); the carrier offset is tracked on the gain references (the same carriers every 4 symbols). The lane that finds a
//         signal becomes the one the tracker works on.
//         -> frame start from the time reference cells -> gain reference cells: channel estimate (time, then frequency interpolation)
//         -> equalised cells of a frame -> DrmBackend
//
// The window of every FFT starts half a guard interval into the guard, on whole samples; the window moves by whole samples to follow the channel's
// delay, and the symbols are referred to one fixed grid again by a phase ramp over the carriers, so channel estimates of different symbols stay comparable.
#include "dect2/drm/drm_rx.h"
#include "dect2/audio/audioout.h"
#include "dect2/drm/drm_audio.h"
#include "dect2/drm/drm_dec.h"
#include "dect2/drm/drm_data.h"
#include "dect2/drm/drm_fft.h"
#include "dect2/drm/drm_playout.h"
#include "drm_front.h"
#include "dect2/channels/channel_find.h"
#include "drm_internal.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include "dect2/common/mathconst.h"
#include "dect2/common/window.h"

namespace dect2 {

using namespace drm;

namespace {
constexpr double kFs = 48000.0;
using dect2::math::kPi;

// part of the band that every spectrum occupancy of the mode shares and where no gain reference is boosted: used until the FAC says which occupancy it is
const int kSafe[5][2] = {{8, 96}, {5, 87}, {-63, 63}, {-40, 40}, {-100, 100}};
// widest carrier range of the mode (union over the occupancies)
const int kWide[5][2] = {{-114, 350}, {-103, 311}, {-69, 213}, {-44, 135}, {-106, 106}};
constexpr double kFsE = 192000.0;          // the DRM+ lane
constexpr double kMinRateE = 100000.0;     // the least input rate the DRM+ lane runs at (the 100 kHz channel must fit)

int posMod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int ceilDiv(int a, int b) { return -floorDiv(-a, b); }
double besselI0(double x) { return dect2::window::besselI0(x, 50, 0.0); }
}

ModeTuning drmTuning() {
    ModeTuning t;
    t.stdMode = 12; t.id = "drm"; t.name = "DRM";
    t.minMhz = 0.1; t.maxMhz = 240; t.defMhz = 9.5;   // DRM30 below 30 MHz, DRM+ in VHF bands I (47 - 68 MHz), II (87.5 - 108) and III (174 - 240)
    t.sampleRate = 2000000.0; t.basebandHz = 1750000.0; t.bandwidthMhz = 0.02;
    t.minSampleRate = 48000.0;
    return t;
}

struct DrmReceiver::Impl {
    // ---------------------------------------------------------------- configuration and plumbing
    double inRate = 0;
    bool ready = false;
    DrmFront front;
    DrmBackend be;
    std::function<void(const std::string&)> logCb;
    std::function<void(int, const uint8_t*, int, int, const DrmTelemetry&)> streamCb;
    // Journaline data service (TS 102 979 clause 8.2): packet mode stream, DAB domain, user application 0x44a
    JournalineDecoder jl;
    DrmPacketDemux jlDemux;
    std::atomic<int> jlStream{-1};
    int jlKey = -1;
    void journaline(const LogicalFrame& f, const SdcInfo& sd) {
        int found = -1;
        for (int i = 0; i < 4; i++) {
            const SdcApp& a = sd.app[i];
            if (a.present && a.packet && a.dataUnit && a.packetLen > 0 && a.dabUserApp() == 0x44a) { found = i; break; }
        }
        if (found < 0) { jlStream = -1; return; }
        const SdcApp& a = sd.app[found];
        const int key = (a.streamId << 20) | (a.packetId << 16) | a.packetLen;
        if (key != jlKey) {
            jlKey = key;
            jl.reset();
            int v = 0, x = 0, en = -1;
            if (a.appData.size() > 2) jlUserAppData(a.appData.data() + 2, a.appData.size() - 2, v, x, en);
            jl.setExtHeader(x);
            jlDemux.configure(a.packetLen, a.packetId);
        }
        jlStream = a.streamId;
        if (a.streamId == f.stream) jlDemux.feed(f.data, (size_t)(f.lenA + f.lenB), [this](const uint8_t* g, size_t n) { jl.feedDataGroup(g, n); });
    }
    // the other data services, by short id (ES 201 980 clause 6.4.3.6: one application information entity per data service)
    mutable std::mutex dataMu;
    std::map<int, DrmDataService> dsvc;
    void dataServices(const LogicalFrame& f, const SdcInfo& sd) {
        std::lock_guard<std::mutex> lk(dataMu);
        for (int i = 0; i < 4; i++) {
            const SdcApp& a = sd.app[i];
            if (!a.present) { dsvc.erase(i); continue; }
            DrmDataService& s = dsvc[i];
            s.configure(a);
            if (a.streamId == f.stream && a.packet) s.feed(f.data, (size_t)(f.lenA + f.lenB));
        }
    }
    std::function<void(const float*, const float*, size_t)> tap;
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false}, silent{false};
    std::atomic<int> selected{-1};
    std::mutex mu;
    DrmTelemetry tel;
    uint64_t telSeq = 0;
    uint64_t inSamples = 0, sincePub = 0;

    // ---------------------------------------------------------------- the 48 kHz stream
    std::vector<cf32> buf;           // samples; buf[0] has the absolute index base
    uint64_t base = 0, total = 0;    // absolute index of buf[0], and of the next sample to arrive
    std::vector<cf32> tmp, tmp2;
    SroCorrector corr;               // takes the sample clock offset out of the 48 kHz stream; the offset is measured on its output (what is left)
    double corrPpm = 0;
    double fs = kFs;                 // the rate of the lane the tracker works on (48 kHz, DRM+ 192 kHz)
    // The other lane: the DRM+ one while the tracker is on the 48 kHz one, and the other way round. The members above (front, corr, corrPpm, buf,
    // base, total, fs) are always the active lane; swapLanes() exchanges them.
    struct Lane { DrmFront front; SroCorrector corr; double corrPpm = 0; std::vector<cf32> buf; uint64_t base = 0, total = 0; double fs = kFsE; };
    Lane other;
    bool eLane = false;              // the active lane is the DRM+ one
    bool eOk = false;                // the input rate carries the DRM+ lane
    uint64_t lastSearchE = 0;        // the DRM+ lane's samples at its last search
    uint64_t inCount = 0;            // input samples since configure(): the clock of blockedUntil
    // DRM+ audio: the audio super frame (200 ms) comes in two logical frames
    std::vector<uint8_t> pendE;
    uint64_t pendIdx = 0;
    bool pendHave = false;
    std::vector<uint8_t> joinE;

    enum Phase { kSearch, kTrack } phase = kSearch;
    uint64_t lastSearch = 0;
    uint64_t blockedUntil = 0;       // after a false alarm: no new lock on the same (mode, shift) before this input sample (inCount)
    int blockedMode = -1, blockedShift = 0, trackShift = 0;
    bool mirror = false;                      // the input is conjugated (a mirrored spectrum)
    ChannelCentre cc;                         // moves a channel far from the middle of the sample band there
    std::vector<cf32> cin;
    double lastRho[5] = {};

    // ---------------------------------------------------------------- tracker
    int mode = -1;
    int N = 0, G = 0, Ts = 0, back = 0, ns = 0, gx = 1, gy = 1, gk0 = 0;
    int64_t nS = 0;                  // absolute position of the cyclic prefix start of the next symbol (nominal grid)
    int Dacc = 0;                    // whole samples the window has been moved from the nominal grid
    double fHz = 0;                  // frequency removed from the input (the carrier offset)
    double nRef = 0, phRef = 0;      // phase of the rotation at absolute sample nRef
    int64_t si = 0;                  // symbols demodulated since the tracker started
    int kLoW = 0, kHiW = 0;          // carrier range that is kept of every symbol
    struct SymRec {
        int64_t idx = -1;
        std::vector<cf32> Y;
        std::vector<cf32> Hr;
        bool haveHr = false;
        float cp = 0;
    };
    static constexpr int kRing = 128;
    std::vector<SymRec> ring;
    bool frameSync = false;
    int h0 = 0;                      // symbol counter modulo ns of a frame start
    std::vector<cf32> syncAcc;       // time reference correlation per symbol position
    std::vector<int> syncCnt;
    struct Pair { int a, b; cf32 ra, rb, comp; };
    std::vector<Pair> timePairs;
    int64_t eqNext = -1;             // next symbol to equalise
    int lag = 0;                     // symbols between reception and equalisation
    int framesSync = 0, facOkSince = 0, facBadRun = 0;
    int cpBadRun = 0;
    double tauRef = 0;               // delay of the channel (centroid) relative to the nominal grid, samples (the window starts `back` samples before the useful part)
    double tauMidRel = 0;            // middle of the delay support of the channel relative to the centroid, samples: the window and the interpolation are centred there
    double tauSlope = 0;             // samples per symbol
    std::vector<double> tauHist;
    double sigma2 = 0;               // noise power of a raw channel estimate (pilot amplitude 1)
    double snrSm = 0, hPow = 0;
    double dopplerSm = 0, delaySpreadMs = 0;
    double accR = 0, accP = 0;       // Doppler statistics, smoothed
    double accRr = 0, accRi = 0, acc2r = 0, acc2i = 0;
    double cpRho = 0;
    int kEqLo = 0, kEqHi = 0;        // carriers that are equalised
    int jLo = 0, jHi = 0;
    int kG0 = 0, qOff = 0, nGridW = 0;
    int occRef = -2;
    std::vector<std::vector<cf32>> refCache;   // per frame symbol: reference of every lattice carrier (0 where there is no pilot)
    std::vector<std::vector<float>> kern;      // interpolation kernels per fractional position
    CellGrid grid;
    int gridOcc = -3;
    std::vector<cf32> G1, Hhat;
    std::vector<uint8_t> Gvalid;
    std::vector<cf32> work;
    DrmFft fftN;
    // carrier offset loop
    double freqErr = 0;
    // Wiener filter in time over the pilots of a carrier (kWT taps, gy symbols apart), per offset d of the symbol from the last pilot
    static constexpr int kWT = 5;
    std::vector<float> wien;
    double wienMse = 0;               // its mean square error (power of the channel estimate's error)
    bool wienOk = false;
    int statN = 0;                    // updates of the noise estimate since the tracker started
    double pilotP = 0;                // power of the channel at the pilots (without the noise)
    // fades: the loops (frequency, timing, clock) hold while the pilots of the symbols being equalised are at the noise
    static constexpr double kHoldInDb = -1.0, kHoldOutDb = 1.5;   // cell SNR of a symbol (smoothed) to enter and leave the hold
    static constexpr int kHoldFreezeSyms = 75;                    // the noise and Doppler averages stop for this many symbols of a fade (about 2 s)
    bool hold = false;
    bool nearFade = false;            // the symbol being equalised has pilots of a fade in its neighbourhood (equalize())
    double holdSnrDb = 30;
    int64_t holdStart = 0, holdEnd = -1;   // symbols (counted as si) where the last fade started and ended
    uint64_t holdEvents = 0, holdSyms = 0, holdSymsAtFrame = 0;
    int fadedRun = 0;                 // frames since the last good FAC that were mostly in a fade
    // re-synchronisation while tracking: the acquisition runs in the background while the FAC fails; when it finds the signal at another timing
    // (samples lost by the radio) or frequency, the tracker starts there at once instead of waiting for the FAC to be given up
    uint64_t lastReacq = 0;
    bool offCal = false;              // offset of the acquisition's symbol start from the tracker's grid, measured while the FAC decodes
    double off0 = 0;
    uint64_t lastCal = 0;
    int reacqVotes = 0;
    double reacqOffPrev = 0, reacqFPrev = 0;
    uint64_t resyncs = 0;

    // ---------------------------------------------------------------- results
    uint64_t audioOk = 0, audioBad = 0;
    uint64_t lastMscOk = 0;
    uint64_t sinceMsc = 0;
    TextDecoder textDec;
    std::string textMsg;
    drm::AudioDecoder adec;
    size_t adecKey = 0;                 // identifies the audio description the decoder was set up for (0: none)
    int adecShort = -1;                 // short id of the service that plays
    uint64_t adecOkSeen = 0, adecBadSeen = 0;
    std::unique_ptr<AudioOut> audioOut;
    std::vector<float> pcm, tapL, tapR;
    std::vector<std::pair<size_t, size_t>> gaps;   // lost audio frames in pcm (placeholders the playout conceals)
    drm::Playout playout;                          // queue, concealment and clock drift between the decoder and the sound device
    std::vector<float> outPcm;
    // the share of made-up sound over the last 10 s (publish() every 0.25 s)
    std::vector<std::pair<double, double>> concHist;   // (real, made up) seconds at each report
    std::string audioInfoText;
    int audioStateNow = 0;
    uint64_t firstFacSample = 0;
    std::vector<float> chanDb;
    std::vector<float> cirDb;
    float cirStartMs = 0, cirStepMs = 0;
    std::vector<float> specDb;
    std::vector<cf32> facC, sdcC, mscC;
    uint64_t lastOkSamples = 0;
    double qualitySm = 0;
    double sroPpm = 0;
    int64_t sroRefSi = 0; double sroRefTau = 0;
    bool sroRefValid = false, sroHave = false;
    bool sroWinHave = false;          // the clock offset of the last whole measuring window (sroWinEst), for the consistency test
    double sroWinEst = 0;
    uint64_t sroRejected = 0;
    uint64_t statSearchRuns = 0;
    float levelDb = -120;
    double lastCorr = 0;

    Impl() : fftN(1) {
        ring.resize(kRing);
        be.log = [this](const std::string& s) { log(s); };
        be.onLogical = [this](const LogicalFrame& f) { onLogical(f); };
        adec.setConcealment(true);
    }

    void log(const std::string& s) { if (logCb) logCb(s); }

    // ---------------------------------------------------------------- setup
    void configure(double rate) {
        if (eLane) swapLanes();
        inRate = rate;
        ready = front.configure(rate);
        eOk = other.front.configure(rate, kFsE, kMinRateE);
        other.fs = kFsE; fs = kFs;
        cc.configure(rate, 10000, 12000, 4.0);
        corr.reset(); corrPpm = 0; corr.setPpm(0);
        other.corr.reset(); other.corrPpm = 0; other.corr.setPpm(0);
        inCount = 0;
        resetAll(false);
    }

    void swapLanes() {
        std::swap(front, other.front); std::swap(corr, other.corr); std::swap(corrPpm, other.corrPpm);
        std::swap(buf, other.buf); std::swap(base, other.base); std::swap(total, other.total); std::swap(fs, other.fs);
        eLane = !eLane;
    }
    // the lane that is not active starts afresh when it is next fed (its samples are old)
    void clearOther() {
        other.buf.clear(); other.base = other.total;
        other.front.reset(); other.corr.reset();
    }

    void resetAll(bool bumpSeq) {
        if (eLane) swapLanes();
        front.reset();
        buf.clear(); base = 0; total = 0; lastSearch = 0;
        other.front.reset(); other.buf.clear(); other.base = 0; other.total = 0; lastSearchE = 0;
        pendE.clear(); pendHave = false;
        phase = kSearch;
        mirror = false;
        blockedMode = -1; blockedUntil = 0;
        be.reset();
        be.log = [this](const std::string& s) { log(s); };
        be.onLogical = [this](const LogicalFrame& f) { onLogical(f); };
        mode = -1;
        audioOk = audioBad = 0;
        textDec.reset(); textMsg.clear();
        { std::lock_guard<std::mutex> lk(dataMu); dsvc.clear(); }
        adec.reset(); adecKey = 0; adecShort = -1; adecOkSeen = adecBadSeen = 0; audioInfoText.clear(); audioStateNow = 0;
        playout.reset(); concHist.clear();
        if (audioOut) audioOut->flush();
        sincePub = 0; inSamples = 0;
        qualitySm = 0; snrSm = 0; sroPpm = corrPpm; dopplerSm = 0; delaySpreadMs = 0; cpRho = 0;
        lastOkSamples = 0; lastMscOk = 0; sinceMsc = 0;
        chanDb.clear(); cirDb.clear(); facC.clear(); sdcC.clear(); mscC.clear(); specDb.clear();
        std::lock_guard<std::mutex> lk(mu);
        tel = DrmTelemetry();
        tel.levelDbfs = -120;
        tel.seq = ++telSeq;
        (void)bumpSeq;
    }

    // ---------------------------------------------------------------- acquisition
    struct Acq {
        bool ok = false;
        int mode = 0;
        double rho = 0, score = 0;
        uint64_t symStart = 0;       // absolute index of the cyclic prefix start of a symbol
        double fTotal = 0;
        int shift = 0;
    };

    // guard interval correlation of one mode over z[0, W): folded over the symbol period
    void guardCorr(const cf32* z, size_t W, int m, double& rho, size_t& tau, cf32& peak, int& nsym, int spt = 4) {
        const ModeParams& mp = modeParams(m);
        const int Tu = mp.tu12 * spt, Tg = mp.tg12 * spt, Tsym = Tu + Tg;
        rho = 0; tau = 0; peak = cf32(0, 0); nsym = 0;
        if ((int)W < 4 * Tsym) return;
        const size_t nmax = W - (size_t)Tu - (size_t)Tg;          // window starts 0 .. nmax
        std::vector<std::complex<double>> C(W - Tu + 1);
        std::vector<double> E(W + 1);
        C[0] = 0; E[0] = 0;
        for (size_t n = 0; n < W - (size_t)Tu; n++) C[n + 1] = C[n] + std::complex<double>(z[n]) * std::conj(std::complex<double>(z[n + (size_t)Tu]));
        for (size_t n = 0; n < W; n++) E[n + 1] = E[n] + (double)std::norm(z[n]);
        std::vector<std::complex<double>> F((size_t)Tsym, 0.0);
        std::vector<double> E1((size_t)Tsym, 0.0), E2((size_t)Tsym, 0.0);
        std::vector<int> cnt((size_t)Tsym, 0);
        for (size_t n = 0; n <= nmax; n++) {
            const size_t t = n % (size_t)Tsym;
            F[t] += C[n + (size_t)Tg] - C[n];
            E1[t] += E[n + (size_t)Tg] - E[n];
            E2[t] += E[n + (size_t)Tu + (size_t)Tg] - E[n + (size_t)Tu];
            cnt[t]++;
        }
        const int minCnt = *std::max_element(cnt.begin(), cnt.end()) - 1;
        double best = -1;
        for (int t = 0; t < Tsym; t++) {
            if (cnt[(size_t)t] < minCnt) continue;
            const double d = std::sqrt(E1[(size_t)t] * E2[(size_t)t]) + 1e-30;
            const double r = std::abs(F[(size_t)t]) / d;
            if (r > best) { best = r; tau = (size_t)t; peak = cf32(F[(size_t)t]); }
        }
        rho = best;
        nsym = minCnt;
    }

    // mayMirror: no frequency references found -> the next search looks at the mirrored spectrum (not while the tracker runs)
    Acq acquire(bool mayMirror = true) {
        Acq res;
        const size_t W = 72000;
        if (total - base < W) return res;
        const uint64_t absStart = total - W;
        const cf32* z = buf.data() + (absStart - base);
        double rhoB = 0; int mB = -1; size_t tauB = 0; cf32 pk(0, 0); int nsB = 0;
        for (int m = 0; m < 4; m++) {
            double rho; size_t tau; cf32 p; int nsym;
            guardCorr(z, W, m, rho, tau, p, nsym);
            lastRho[m] = rho;
            if (rho > rhoB) { rhoB = rho; mB = m; tauB = tau; pk = p; nsB = nsym; }
        }
        if (mB < 0 || rhoB < 0.18) return res;
        // a second mode with nearly the same correlation is not trusted (the lengths of A and B symbols are the same)
        const ModeParams& mp = modeParams(mB);
        const int Tu = mp.tu12 * 4, Tg = mp.tg12 * 4, Tsym = Tu + Tg;
        const double fFrac = -std::arg(pk) / (2 * kPi * (double)Tu / kFs);
        // Symbol-synchronous FFT at the guard-correlation lock. The three frequency reference tones are continuous: from one symbol to the next they keep
        // their phase (the references flip their sign where the natural phase step is half a cycle), so the product of a bin with its value one symbol
        // earlier, summed over symbols, stays large for them while it averages out for data cells. A small frequency error only turns that sum by a
        // fixed angle, and the angle is the same for all three tones.
        DrmFft f(Tu);
        std::vector<cf32> bins((size_t)Tu), prev((size_t)Tu);
        std::vector<double> P((size_t)Tu, 0.0), E1((size_t)Tu, 0.0);
        std::vector<std::complex<double>> C1((size_t)Tu, 0.0);
        const int nsy = std::min(40, nsB - 2);
        if (nsy < 8) return res;
        int used = 0;
        for (int j = 0; j < nsy; j++) {
            const size_t st = tauB + (size_t)j * (size_t)Tsym + (size_t)(Tg - Tg / 2);
            if (st + (size_t)Tu > W) break;
            // rotate by the fractional offset so that the carriers fall in the middle of the bins
            double ph = -2 * kPi * fFrac * (double)st / kFs;
            const cf32 step = std::polar(1.0f, (float)(-2 * kPi * fFrac / kFs));
            cf32 r = std::polar(1.0f, (float)std::fmod(ph, 2 * kPi));
            for (int i = 0; i < Tu; i++) { bins[(size_t)i] = z[st + (size_t)i] * r; r *= step; }
            f.forward(bins.data());
            for (int i = 0; i < Tu; i++) {
                const double pw0 = (double)std::norm(bins[(size_t)i]);
                P[(size_t)i] += pw0;
                if (used > 0) {
                    C1[(size_t)i] += std::complex<double>(bins[(size_t)i]) * std::conj(std::complex<double>(prev[(size_t)i]));
                    E1[(size_t)i] += std::sqrt(pw0 * (double)std::norm(prev[(size_t)i]));
                }
            }
            prev = bins;
            used++;
        }
        if (used < 8) return res;
        // noise floor: a low percentile of the bin powers
        std::vector<double> sorted(P);
        std::sort(sorted.begin(), sorted.end());
        const double floorP = sorted[sorted.size() / 5] + 1e-30;
        const int* fk = nullptr;
        static const int fka[3] = {18, 54, 72}, fkb[3] = {16, 48, 64}, fkc[3] = {11, 33, 44}, fkd[3] = {7, 21, 28};
        fk = mB == 0 ? fka : mB == 1 ? fkb : mB == 2 ? fkc : fkd;
        const int half = Tu / 2;
        auto pw = [&](int k) { return P[(size_t)posMod(k, Tu)]; };
        double bestScore = -1e9; int bestShift = 0;
        std::complex<double> bestC = 0;
        const bool blockedNow = mB == blockedMode && inCount < blockedUntil;
        // how the reference of each tone changes from symbol to symbol (mode D flips the sign of two of them)
        cf32 adv[3];
        for (int i = 0; i < 3; i++) {
            cf32 r0, r1;
            adv[i] = (pilotRef(mB, -1, 0, fk[i], r0) && pilotRef(mB, -1, 1, fk[i], r1) && std::abs(r0) > 0) ? r1 / r0 : cf32(1, 0);
        }
        // candidate shifts: all positions where the three tones and the band around them fit. Every tone counts with its own normalised value, so
        // that one strong interferer cannot stand in for the three.
        for (int d = -half + 6; d < half - 80; d++) {
            std::complex<double> acc = 0;
            bool fit = true;
            for (int i = 0; i < 3; i++) {
                const int k = d + fk[i];
                if (k < -half + 6 || k > half - 6) { fit = false; break; }
                const size_t b = (size_t)posMod(k, Tu);
                acc += C1[b] / (E1[b] + 1e-30) * std::conj(std::complex<double>(adv[i]));
            }
            if (!fit) continue;
            const double sc = std::abs(acc) / 3.0;
            if (sc <= bestScore) continue;
            if (blockedNow && std::abs(d - blockedShift) < 3) continue;
            // the band: carriers of the mode's safe range must carry signal clearly above the floor
            double band = 0; int nb = 0;
            for (int k = kSafe[mB][0]; k <= kSafe[mB][1]; k += 3) { band += pw(d + k); nb++; }
            band /= nb;
            if (band < 2.5 * floorP) continue;
            bestScore = sc; bestShift = d; bestC = acc;
        }
        // the angle of the summed products is the frequency error left in the fractional estimate
        // (a carrier d bins from the centre advances by 2 pi d Tsym / Tu from symbol to symbol on top of that)
        const double dPh = 2 * kPi * std::fmod((double)bestShift * (double)Tsym / (double)Tu, 1.0);
        const double fErr = std::arg(bestC * std::polar(1.0, -dPh)) / (2 * kPi * (double)Tsym / kFs);
        if (bestScore < 0.5) { if (mayMirror) toggleMirror(); return res; }
        res.ok = true;
        res.mode = mB; res.rho = rhoB; res.score = bestScore;
        res.symStart = absStart + tauB;
        res.fTotal = fFrac + fErr + bestShift * kFs / (double)Tu;
        res.shift = bestShift;
        return res;
    }

    // DRM+ (mode E) on the 192 kHz lane z[0, W): guard interval correlation for the timing and the offset within a carrier spacing, then the whole
    // spacings. Mode E has no frequency references; the power of the gain references (twice that of a data cell, four times at the boosted edge
    // carriers) in their pattern (every 16 carriers, moved by 4 from symbol to symbol) and the edges of the 213 carriers show where the
    // carriers are. A zero mean template of the expected power per carrier is correlated with the measured power for every shift and every
    // phase of the 4 symbol pattern.
    Acq acquireE(const cf32* z, size_t W, uint64_t absStart) {
        Acq res;
        double rho; size_t tau; cf32 pk; int nsym;
        guardCorr(z, W, kModeE, rho, tau, pk, nsym, 16);
        lastRho[4] = rho;
        if (rho < 0.3 || nsym < 24) return res;
        const ModeParams& mp = modeParams(kModeE);
        const int Tu = mp.tu12 * 16, Tg = mp.tg12 * 16, Tsym = Tu + Tg;
        const double fFrac = -std::arg(pk) / (2 * kPi * (double)Tu / kFsE);
        const int nsy = std::min(64, nsym - 2);
        DrmFft f(Tu);
        std::vector<cf32> bins((size_t)Tu);
        std::vector<std::vector<float>> P((size_t)nsy, std::vector<float>((size_t)Tu, 0.f));
        for (int j = 0; j < nsy; j++) {
            const size_t st = tau + (size_t)j * (size_t)Tsym + (size_t)(Tg - Tg / 2);
            const cf32 step = std::polar(1.0f, (float)(-2 * kPi * fFrac / kFsE));
            cf32 r = std::polar(1.0f, (float)std::fmod(-2 * kPi * fFrac * (double)st / kFsE, 2 * kPi));
            for (int i = 0; i < Tu; i++) { bins[(size_t)i] = z[st + (size_t)i] * r; r *= step; }
            f.forward(bins.data());
            for (int i = 0; i < Tu; i++) P[(size_t)j][(size_t)i] = std::norm(bins[(size_t)i]);
        }
        // the template over -130 .. 130: 0 outside the carriers, 1 for a data cell, |ref|^2 for a gain reference (symbols 4 .. 7 of a frame: no time references)
        const int K = 130, nK = 2 * K + 1;
        const int dMax = 28;               // +-12.4 kHz: 50 ppm at 240 MHz
        // a strong carrier (a tone, a neighbour's pilot) must not stand in for the pattern: every bin is clipped at 8 times the median power
        {
            std::vector<float> all;
            all.reserve((size_t)nsy * (size_t)(2 * (K + dMax) + 1));
            for (int j = 0; j < nsy; j++) for (int k = -K - dMax; k <= K + dMax; k++) all.push_back(P[(size_t)j][(size_t)posMod(k, Tu)]);
            std::nth_element(all.begin(), all.begin() + (ptrdiff_t)(all.size() / 2), all.end());
            const float cap = 8.f * all[all.size() / 2];
            for (auto& Pj : P) for (auto& v : Pj) v = std::min(v, cap);
        }
        // Two templates per symbol of the 4 symbol pattern. Tp: the gain references against the data cells, zero mean over the 213 carriers and 0
        // outside: the pattern itself, which no other signal has (it repeats every 4 carriers of shift with the symbol phase). Tb: the expected
        // power over -130 .. 130 with zero mean (0 outside the carriers): the band edges, which tell the shifts 4 carriers apart.
        std::vector<float> Tp((size_t)4 * nK, 0.f), Tb((size_t)4 * nK, 0.f);
        for (int n = 0; n < 4; n++) {
            double meanB = 0, meanP = 0;
            int inBand = 0;
            for (int k = -K; k <= K; k++) {
                float v = 0;
                cf32 ref;
                if (k >= kWide[kModeE][0] && k <= kWide[kModeE][1] && !carrierUnused(kModeE, k)) {
                    v = pilotRef(kModeE, 0, 4 + n, k, ref) ? std::norm(ref) : 1.f;
                    meanP += v; inBand++;
                }
                Tb[(size_t)(n * nK + k + K)] = v;
                meanB += v;
            }
            meanB /= nK; meanP /= std::max(1, inBand);
            for (int k = -K; k <= K; k++) {
                float& b = Tb[(size_t)(n * nK + k + K)];
                if (b > 0) Tp[(size_t)(n * nK + k + K)] = b - (float)meanP;
                b -= (float)meanB;
            }
        }
        auto score = [&](const std::vector<float>& T, int d, int q) {
            double acc = 0, pw = 0;
            for (int j = 0; j < nsy; j++) {
                const float* t = &T[(size_t)(((j + q) % 4) * nK)];
                const std::vector<float>& Pj = P[(size_t)j];
                for (int k = -K; k <= K; k++) {
                    const float p = Pj[(size_t)posMod(k + d, Tu)];
                    acc += t[k + K] * p; pw += p;
                }
            }
            return acc / (pw + 1e-30);
        };
        double bestP = -1e30; int d0 = 0;
        for (int d = -dMax; d <= dMax; d++)
            for (int q = 0; q < 4; q++) { const double sc = score(Tp, d, q); if (sc > bestP) { bestP = sc; d0 = d; } }
        // the shifts with the same pattern: the band edges decide
        double best = -1e30; int bestD = d0;
        const bool blockedNow = blockedMode == kModeE && inCount < blockedUntil;
        for (int d = d0 - 4 * (dMax / 4 + 1); d <= dMax; d += 4) {
            if (d < -dMax || (blockedNow && d == blockedShift)) continue;
            for (int q = 0; q < 4; q++) { const double sc = score(Tb, d, q); if (sc > best) { best = sc; bestD = d; } }
        }
        lastScoreE = bestP;
        // a clean DRM+ signal scores about 0.055 on the pattern (less with noise: 0.04 at 3 dB); anything else stays near 0 (+-0.005)
        if (bestP < kMinScoreE || best < 0.05) return res;
        res.ok = true;
        res.mode = kModeE; res.rho = rho; res.score = best;
        res.symStart = absStart + tau;
        res.fTotal = fFrac + bestD * kFsE / (double)Tu;
        res.shift = bestD;
        return res;
    }
    double lastScoreE = 0;
    static constexpr double kMinScoreE = 0.025;

    // ---------------------------------------------------------------- tracker set-up
    void startTracking(const Acq& a) {
        mode = a.mode;
        trackShift = a.shift;
        const ModeParams& mp = modeParams(mode);
        const int spt = (int)std::lround(fs / 12000.0);   // samples per T: 4 at 48 kHz, 16 on the DRM+ lane
        N = mp.tu12 * spt; G = mp.tg12 * spt; Ts = N + G; ns = mp.ns; gx = mp.gx; gy = mp.gy; gk0 = mp.gk0;
        back = G / 2;
        fftN = DrmFft(N);
        nS = (int64_t)a.symStart;
        Dacc = 0;
        fHz = a.fTotal;
        nRef = (double)nS; phRef = 0;
        si = 0;
        kLoW = kWide[mode][0]; kHiW = kWide[mode][1];
        for (auto& r : ring) { r.idx = -1; r.haveHr = false; }
        frameSync = false;
        syncAcc.assign((size_t)ns, cf32(0, 0)); syncCnt.assign((size_t)ns, 0);
        timePairs.clear();
        {   // pairs of time reference cells close together: their product does not depend on the channel
            const RefTable& t = kTimeRefs[mode];
            for (int i = 0; i < t.n; i++)
                for (int j = 0; j < t.n; j++) {
                    const int ka = t.data[i][0], kb = t.data[j][0];
                    if (kb > ka && kb - ka <= 3) {
                        Pair p;
                        p.a = ka; p.b = kb;
                        p.ra = std::polar(1.0f, (float)(2 * kPi * t.data[i][1] / 1024.0));
                        p.rb = std::polar(1.0f, (float)(2 * kPi * t.data[j][1] / 1024.0));
                        p.comp = std::polar(1.0f, (float)(2 * kPi * (kb - ka) * (double)back / (double)N));   // the window starts `back` samples before the useful part
                        timePairs.push_back(p);
                    }
                }
        }
        eqNext = -1;
        lag = 2 * gy;                                    // the Wiener filter uses the pilots up to 2 gy symbols ahead
        framesSync = 0; facOkSince = 0; facBadRun = 0; cpBadRun = 0;
        tauRef = 0; tauMidRel = 0; tauSlope = 0; tauHist.clear();
        sigma2 = 0; snrSm = 0; hPow = 0; dopplerSm = 0; delaySpreadMs = 0; accR = accP = accRr = accRi = acc2r = acc2i = 0;
        wien.clear(); wienOk = false; wienMse = 0; statN = 0; pilotP = 0;
        hold = false; holdSnrDb = 30; holdStart = 0; holdEnd = -1; holdSymsAtFrame = holdSyms; fadedRun = 0;
        lastReacq = 0; offCal = false; off0 = 0; lastCal = 0; reacqVotes = 0;
        sroPpm = corrPpm; sroRefSi = 150; sroRefTau = 0; sroRefValid = false; sroHave = false; sroWinHave = false; sroWinEst = 0;
        occRef = -2;
        gridOcc = -3;
        be.setMode(mode);
        be.frameLost();
        setupLattice();
        phase = kTrack;
        char b[160];
        snprintf(b, sizeof b, "DRM: signal found: mode %c%s, carrier offset %+.1f Hz, guard correlation %.2f", mp.name, mode == kModeE ? " (DRM+)" : "", fHz, a.rho);
        log(b);
    }

    // the interpolation kernels and lattice of the current equalisation range
    void setupLattice() {
        int lo, hi;
        auto L = be.layout();
        if (L) { lo = L->kmin; hi = L->kmax; } else { lo = kSafe[mode][0]; hi = kSafe[mode][1]; }
        kEqLo = lo; kEqHi = hi;
        // lattice carriers over the whole kept range
        kG0 = kLoW;
        while (posMod(kG0 - gk0, gx) != 0) kG0++;
        nGridW = (kHiW - kG0) / gx + 1;
        qOff = posMod((kG0 - gk0) / gx, gy);
        jLo = std::max(0, ceilDiv(kEqLo - kG0, gx));
        jHi = std::min(nGridW - 1, floorDiv(kEqHi - kG0, gx));
        // kernels (8 taps, windowed sinc, cut-off matched to the delays the guard interval allows)
        const int J = 4;
        const double kappa = std::min(1.0, std::max(0.5, 1.3 * G * gx / (double)N));
        kern.assign((size_t)gx, std::vector<float>((size_t)(2 * J), 0.f));
        const double beta = 4.5;
        for (int i = 0; i < gx; i++) {
            const double f = (double)i / gx;
            double sum = 0;
            std::vector<double> h((size_t)(2 * J));
            for (int m = -J + 1; m <= J; m++) {
                const double u = m - f;
                const double w = std::fabs(u) < J ? besselI0(beta * std::sqrt(1 - (u / J) * (u / J))) / besselI0(beta) : 0;
                const double a = kappa * u;
                const double sc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(kPi * a) / (kPi * a);
                h[(size_t)(m + J - 1)] = kappa * sc * w;
                sum += h[(size_t)(m + J - 1)];
            }
            for (int m = 0; m < 2 * J; m++) kern[(size_t)i][(size_t)m] = (float)(h[(size_t)m] / sum);
        }
        // reference values of the lattice pilots, per symbol of the frame
        const int oc = L ? L->occ : -1;
        occRef = oc;
        refCache.assign((size_t)ns, std::vector<cf32>((size_t)nGridW, cf32(0, 0)));
        for (int s = 0; s < ns; s++)
            for (int j = 0; j < nGridW; j++) {
                const int k = kG0 + j * gx;
                if (posMod(j + qOff, gy) != s % gy) continue;
                cf32 r;
                if (pilotRef(mode, oc, s, k, r)) refCache[(size_t)s][(size_t)j] = r;
            }
        grid.resize(ns, kEqLo, kEqHi);
        gridOcc = oc;
    }

    SymRec& rec(int64_t idx) { return ring[(size_t)(idx % kRing)]; }
    const SymRec* recIf(int64_t idx) const {
        const SymRec& r = ring[(size_t)(idx % kRing)];
        return (r.idx == idx && r.haveHr) ? &r : nullptr;
    }
    int sOf(int64_t idx) const { return posMod((int)((idx - h0) % ns), ns); }

    // ---------------------------------------------------------------- one symbol
    // returns false when the samples of the next symbol have not all arrived
    bool trackStep() {
        const int64_t c0 = nS + Dacc;                 // start of the cyclic prefix as the window sees it
        const int64_t ws = c0 + G - back;
        if ((uint64_t)(ws + N + 4) > total || (uint64_t)(c0 + G + N) > total) return false;
        if ((uint64_t)ws < base || (uint64_t)c0 < base) { restartSearch("the samples of a symbol were already discarded"); return false; }
        const cf32* x = buf.data() + (ws - (int64_t)base);
        // guard interval correlation of this symbol (how much of an OFDM signal it still is)
        {
            const cf32* c = buf.data() + (c0 - (int64_t)base);
            std::complex<double> acc = 0; double e1 = 0, e2 = 0;
            for (int i = 0; i < G; i++) { acc += std::complex<double>(c[i]) * std::conj(std::complex<double>(c[i + N])); e1 += std::norm(c[i]); e2 += std::norm(c[i + N]); }
            const double rho = std::abs(acc) / (std::sqrt(e1 * e2) + 1e-30);
            cpRho += (rho - cpRho) * 0.2;
            cpBadRun = rho < 0.10 ? cpBadRun + 1 : 0;
        }
        // rotate by the carrier offset and transform
        work.resize((size_t)N);
        {
            const double ph0 = phRef + 2 * kPi * fHz * ((double)ws - nRef) / fs;
            cf32 r = std::polar(1.0f, (float)(-std::fmod(ph0, 2 * kPi)));
            const cf32 st = std::polar(1.0f, (float)(-2 * kPi * fHz / fs));
            for (int i = 0; i < N; i++) { work[(size_t)i] = x[i] * r; r *= st; }
        }
        fftN.forward(work.data());
        SymRec& R = rec(si);
        R.idx = si; R.haveHr = false;
        const int W = kHiW - kLoW + 1;
        R.Y.resize((size_t)W);
        // refer the window to the nominal grid: the window starts Dacc samples late
        const double ramp = -2 * kPi * (double)Dacc / (double)N;
        for (int k = kLoW; k <= kHiW; k++) {
            const cf32 v = work[(size_t)posMod(k, N)];
            R.Y[(size_t)(k - kLoW)] = Dacc ? v * std::polar(1.0f, (float)(ramp * k)) : v;
        }
        R.cp = (float)cpRho;
        const int64_t t = si;
        si++;
        nS += Ts;
        // Without the OFDM structure for a while: a false lock is given up at once; a lock whose FAC decoded holds through the fade (the FAC decides,
        // see frameDone(), and the acquisition looks for the signal in the background)
        if (cpBadRun >= 6 && facOkSince == 0) { restartSearch("the signal lost its OFDM structure"); return true; }
        if (!frameSync) {
            frameSyncStep(t);
        } else {
            lattice(t);
            holdDetect(t);
            freqLoop(t);
            drainEqualize(t);
        }
        return true;
    }

    // equalise the symbols that have all their pilots; a restart of the search (eqNext < 0) ends the loop
    void drainEqualize(int64_t t) {
        while (phase == kTrack && eqNext >= 0 && eqNext + lag <= t) {
            const int64_t e = eqNext;
            eqNext++;
            equalize(e);
        }
    }

    // time reference correlation of one symbol; decides the frame start
    void frameSyncStep(int64_t t) {
        const SymRec& R = rec(t);
        cf32 acc(0, 0); double nrm = 0;
        for (const Pair& p : timePairs) {
            const cf32 ya = R.Y[(size_t)(p.a - kLoW)], yb = R.Y[(size_t)(p.b - kLoW)];
            acc += yb * std::conj(ya) * std::conj(p.rb) * p.ra * p.comp;
            nrm += (double)std::abs(ya) * std::abs(yb);
        }
        const int h = (int)(t % ns);
        syncAcc[(size_t)h] += acc / (float)(nrm + 1e-30);
        syncCnt[(size_t)h]++;
        if (t < 2 * ns) return;
        // best and second best position
        int b = 0; double bv = -1, sv = -1;
        for (int i = 0; i < ns; i++) { const double v = std::abs(syncAcc[(size_t)i]); if (v > bv) { bv = v; b = i; } }
        for (int i = 0; i < ns; i++) if (i != b) sv = std::max(sv, (double)std::abs(syncAcc[(size_t)i]));
        const double mean = bv / std::max(1, syncCnt[(size_t)b]);
        if (bv > 2.4 * sv && mean > 0.12 && syncCnt[(size_t)b] >= 2) {
            h0 = b;
            frameSync = true;
            // everything stored so far gets its lattice values; the equalisation starts at the oldest frame start still stored
            int64_t oldest = std::max<int64_t>(0, t - kRing + 8);
            for (int64_t i = oldest; i <= t; i++) lattice(i);
            int64_t first = oldest;
            while (sOf(first) != 0) first++;
            eqNext = first;
            framesSync = 0;
            char b2[120];
            snprintf(b2, sizeof b2, "DRM: frame start found (time reference correlation %.2f)", mean);
            log(b2);
            drainEqualize(t);
        } else if (t > 10 * ns) {
            restartSearch("no frame start");
        }
    }

    // raw channel estimates at the lattice pilots of a symbol
    void lattice(int64_t t) {
        SymRec& R = rec(t);
        if (R.idx != t || R.haveHr) return;
        const int s = sOf(t);
        R.Hr.assign((size_t)nGridW, cf32(0, 0));
        const std::vector<cf32>& refs = refCache[(size_t)s];
        for (int j = 0; j < nGridW; j++) {
            const cf32 r = refs[(size_t)j];
            if (r == cf32(0, 0)) continue;
            const int k = kG0 + j * gx;
            R.Hr[(size_t)j] = R.Y[(size_t)(k - kLoW)] / r;
        }
        R.haveHr = true;
    }

    // Weights of the Wiener filter in time for every offset d (0 .. gy - 1) of a symbol from the last pilot of a carrier: taps at the pilots
    // t - d + m gy (m = -2 .. 2), correlation exp(-(pi fd dt)^2) (the Gaussian Doppler spectrum the Doppler estimate assumes), noise to signal sigma2 / P.
    // fd is taken a little wider than measured: smoothing too little costs less than smoothing a fast channel away.
    // Below 1 Hz of Doppler spread the Wiener filter gains about 1 dB (16-QAM at 8 dB: 11 % of the audio frames lost instead of 74 %); in faster
    // channels the noise estimate also holds the change of the channel, the filter would smooth too much: the linear interpolation is used there
    // Above 1 Hz the second-difference noise estimate (sigma2) is mostly the channel's own change; the Wiener weights then take the noise from the
    // pilot power (accP = P + noise) minus the signal power implied by the measured correlation (pilotP), never more than sigma2.
    static constexpr double kWienerMaxDopplerHz = 4.0;
    bool wienerOn() const { return dopplerSm < kWienerMaxDopplerHz; }
    double sigmaW() const {
        if (dopplerSm < 1.0 || pilotP <= 0) return sigma2;
        return std::min(sigma2, std::max(accP - pilotP, 1e-3 * pilotP));
    }
    void wienerWeights() {
        wienOk = false;
        if (statN < 30 || pilotP <= 0 || sigma2 <= 0) return;
        wien.assign((size_t)gy * kWT, 0.f);
        const double symSec = (double)Ts / fs;
        const double fd = std::max(0.3, 1.5 * dopplerSm + 0.2);
        const double sg = sigmaW();
        const double beta = sg / pilotP;
        auto R = [&](double dt) { const double a = kPi * fd * dt; return std::exp(-a * a); };
        double mseSum = 0;
        for (int d = 0; d < gy; d++) {
            double tau[kWT], A[kWT][kWT + 1];
            for (int m = 0; m < kWT; m++) tau[m] = (double)((m - kWT / 2) * gy - d) * symSec;
            for (int i = 0; i < kWT; i++) {
                for (int j = 0; j < kWT; j++) A[i][j] = R(tau[i] - tau[j]) + (i == j ? beta : 0.0);
                A[i][kWT] = R(tau[i]);
            }
            for (int c = 0; c < kWT; c++) {            // Gauss-Jordan with partial pivoting (the matrix is positive definite)
                int p = c;
                for (int r = c + 1; r < kWT; r++) if (std::fabs(A[r][c]) > std::fabs(A[p][c])) p = r;
                if (p != c) for (int k = 0; k <= kWT; k++) std::swap(A[c][k], A[p][k]);
                const double piv = A[c][c];
                if (std::fabs(piv) < 1e-12) return;
                for (int k = c; k <= kWT; k++) A[c][k] /= piv;
                for (int r = 0; r < kWT; r++) if (r != c) { const double f = A[r][c]; if (f != 0) for (int k = c; k <= kWT; k++) A[r][k] -= f * A[c][k]; }
            }
            // the weights are scaled to a sum of 1: the estimate of a steady channel keeps its level (the MMSE weights shrink it towards 0 by the
            // noise to signal ratio, which would scale every equalised cell of a QAM constellation)
            double wsum = 0;
            for (int m = 0; m < kWT; m++) wsum += A[m][kWT];
            if (wsum < 0.2) return;
            double wr = 0, w2 = 0;
            for (int m = 0; m < kWT; m++) { const double w = A[m][kWT] / wsum; wien[(size_t)(d * kWT + m)] = (float)w; wr += w * R(tau[m]); w2 += w * w; }
            // the noise the filter leaves in the estimate, sigma2 |w|^2 (the part of the error that comes from the channel's change depends on the
            // assumed Doppler spread, which is on the wide side: not counted, so that the weights of the cells keep their scale when the filter is
            // switched on)
            (void)wr;
            mseSum += sg * w2;
        }
        wienMse = mseSum / gy;
        wienOk = true;
    }

    // carrier offset from the phase of the frequency reference tones from symbol to symbol (mode E: of the gain references from one symbol to the
    // one 4 later, which has them on the same carriers)
    void freqLoop(int64_t t) {
        if (mode == kModeE) {
            if (t < gy) return;
            const SymRec* A = recIf(t - gy);
            const SymRec* B = recIf(t);
            if (!A || !B) return;
            cf32 acc(0, 0);
            for (int j = 0; j < nGridW; j++) {
                const cf32 a = A->Hr[(size_t)j], b = B->Hr[(size_t)j];
                if (a == cf32(0, 0) || b == cf32(0, 0)) continue;
                acc += b * std::conj(a);
            }
            if (std::abs(acc) < 1e-12) return;
            steerFreq(t, std::arg(acc) / (2 * kPi * (double)gy * Ts / fs));
            return;
        }
        if (t < 1) return;
        const SymRec& A = rec(t - 1);
        const SymRec& B = rec(t);
        if (A.idx != t - 1 || B.idx != t) return;
        cf32 acc(0, 0);
        const RefTable& ft = kFreqRefs[mode];
        const int sa = sOf(t - 1), sb = sOf(t);
        for (int i = 0; i < ft.n; i++) {
            const int k = ft.data[i][0];
            cf32 ra, rb;
            if (!pilotRef(mode, -1, sa, k, ra) || !pilotRef(mode, -1, sb, k, rb)) continue;
            const cf32 ya = A.Y[(size_t)(k - kLoW)] * std::conj(ra), yb = B.Y[(size_t)(k - kLoW)] * std::conj(rb);
            acc += yb * std::conj(ya);
        }
        if (std::abs(acc) < 1e-12) return;
        steerFreq(t, std::arg(acc) / (2 * kPi * (double)Ts / fs));   // Hz
    }
    void steerFreq(int64_t t, double eps) {
        if (hold) return;                              // a fade: the phase of noise says nothing; the frequency holds
        freqErr += (eps - freqErr) * 0.15;
        // move the correction in smoothly; the rotation phase stays continuous
        if (t % 4 == 0 && std::fabs(freqErr) > 0.005) {
            const double nNow = (double)nS;
            phRef += 2 * kPi * fHz * (nNow - nRef) / fs;
            nRef = nNow;
            fHz += 0.5 * freqErr;
            freqErr *= 0.5;
        }
    }

    // Fade detection on the newest symbol (2 gy ahead of the one being equalised): the SNR of its pilots against the noise of a raw estimate,
    // smoothed over about three symbols. In a fade the loops (frequency, timing, clock) hold and the noise and Doppler averages are not fed.
    void holdDetect(int64_t t) {
        const SymRec* R = recIf(t);
        if (!R || sigma2 <= 0 || statN < 30) return;
        double sum = 0; int cnt = 0;
        for (int j = jLo; j <= jHi; j++) { const cf32 h = R->Hr[(size_t)j]; if (h != cf32(0, 0)) { sum += std::norm(h); cnt++; } }
        if (cnt < 3) return;
        const double inst = (sum / cnt - sigma2) / (2 * sigma2);
        holdSnrDb += (10 * std::log10(std::max(inst, 1e-3)) - holdSnrDb) * 0.35;
        if (!hold && holdSnrDb < kHoldInDb) { hold = true; holdStart = t; holdEvents++; }
        else if (hold && holdSnrDb > kHoldOutDb) {
            hold = false; holdEnd = t;
            sroRefValid = false; sroRefSi = t + 20;     // the drift of the delay is measured again from here
        }
        if (hold) holdSyms++;
    }

    // channel estimate and equalised cells of symbol t (the pilots of symbols t - y .. t + y are there)
    void equalize(int64_t t) {
        const SymRec& R = rec(t);
        if (R.idx != t) return;
        const int s = sOf(t);
        auto L = be.layout();
        if ((L ? L->occ : -1) != gridOcc) setupLattice();
        const int nJ = jHi - jLo + 1;
        if (nJ < 4) return;
        G1.assign((size_t)nJ, cf32(0, 0));
        Gvalid.assign((size_t)nJ, 0);
        double accN = 0; int cntN = 0;
        double sumRe = 0, sumIm = 0, sumP = 0, sum2Re = 0, sum2Im = 0; int cntD = 0;
        if (s == 0 || wien.empty()) wienerWeights();
        for (int j = jLo; j <= jHi; j++) {
            const int qj = posMod(j + qOff, gy);
            const int d = posMod(s % gy - qj, gy);
            const SymRec* a = recIf(t - d);
            const SymRec* b = recIf(t - d + gy);
            cf32 v(0, 0);
            bool ok = false;
            // Wiener filter over the five pilots of this carrier nearest in time (t - d - 2 gy .. t - d + 2 gy), fitted to the measured Doppler spread
            // and noise; the linear interpolation between the two pilots around t when not all of them are there (the first symbols)
            const bool useW = wienOk && wienerOn();
            if (useW) {
                const SymRec* tap[kWT];
                bool all = true;
                for (int m = 0; m < kWT && all; m++) { tap[m] = recIf(t - d + (m - kWT / 2) * gy); all = tap[m] != nullptr; }
                if (all) {
                    const float* wv = &wien[(size_t)d * kWT];
                    for (int m = 0; m < kWT; m++) v += tap[m]->Hr[(size_t)j] * wv[m];
                    ok = true;
                }
            }
            if (d == 0) {
                if (a && !ok) { v = a->Hr[(size_t)j]; ok = true; }
                const SymRec* p = recIf(t - gy);
                if (a && p && b) {
                    const cf32 pred = 0.5f * (p->Hr[(size_t)j] + b->Hr[(size_t)j]);
                    accN += std::norm(a->Hr[(size_t)j] - pred); cntN++;
                    const cf32 c = a->Hr[(size_t)j] * std::conj(p->Hr[(size_t)j]); sumRe += c.real(); sumIm += c.imag(); sumP += std::norm(a->Hr[(size_t)j]); cntD++;
                    if (const SymRec* pp = recIf(t - 2 * gy)) { const cf32 c2 = a->Hr[(size_t)j] * std::conj(pp->Hr[(size_t)j]); sum2Re += c2.real(); sum2Im += c2.imag(); }
                }
            } else if (ok) {
            } else if (a && b) {
                const float f = (float)d / gy;
                v = a->Hr[(size_t)j] * (1 - f) + b->Hr[(size_t)j] * f; ok = true;
            } else if (a) { v = a->Hr[(size_t)j]; ok = true; }
            else if (b) { v = b->Hr[(size_t)j]; ok = true; }
            G1[(size_t)(j - jLo)] = v;
            Gvalid[(size_t)(j - jLo)] = ok ? 1 : 0;
        }
        // fill holes from the nearest valid neighbour
        for (int j = 0; j < nJ; j++) if (!Gvalid[(size_t)j]) {
            int best = -1;
            for (int o = 1; o < nJ && best < 0; o++) { if (j - o >= 0 && Gvalid[(size_t)(j - o)]) best = j - o; else if (j + o < nJ && Gvalid[(size_t)(j + o)]) best = j + o; }
            if (best >= 0) G1[(size_t)j] = G1[(size_t)best];
        }
        // Noise of a raw estimate from the second difference in time of the pilots (in a fast fading channel the change of the channel counts too: the
        // estimate is on the safe side), the Doppler spread from their correlation gy symbols apart. Not updated from pilots in or next to a fade
        // (holdDetect() runs 2 gy symbols ahead of this), except in a fade that lasts (a level that changed for good is learnt).
        nearFade = hold ? t + gy >= holdStart - 3 : (holdEnd >= 0 && t - gy <= holdEnd + 1);
        if (!nearFade || (hold && si - holdStart > kHoldFreezeSyms)) {
            if (cntN >= 3) {
                const double s2 = accN / (1.5 * cntN);
                sigma2 = sigma2 > 0 ? sigma2 + (s2 - sigma2) * 0.12 : s2;
                statN++;
            }
            if (cntD >= 3) {
                // The Doppler spread from the correlation of the pilots gy and 2 gy symbols apart: the noise is in neither, and with a Gaussian
                // Doppler spectrum (correlation exp(-(pi fd dt)^2)) their ratio is x^3, x the correlation at gy symbols
                accRr += (sumRe / cntD - accRr) * 0.05; accRi += (sumIm / cntD - accRi) * 0.05; accP += (sumP / cntD - accP) * 0.05;
                acc2r += (sum2Re / cntD - acc2r) * 0.05; acc2i += (sum2Im / cntD - acc2i) * 0.05;
                const double r1 = std::hypot(accRr, accRi), r2 = std::hypot(acc2r, acc2i);
                const double x = r1 > 1e-30 ? std::max(0.02, std::min(1.0, std::cbrt(r2 / r1))) : 1.0;
                pilotP = std::max(std::min(r1 / x, accP), 1e-12);
                const double tauY = (double)gy * Ts / fs;
                dopplerSm = x < 0.9999 ? std::min(20.0, std::sqrt(-std::log(x)) / (kPi * tauY)) : 0.0;
            }
        }
        // Delay of the channel from the phase slope over the lattice. The estimates are referred to the nominal grid, so the slope is taken relative to the
        // window (the part the window has been moved by is added back): it stays small however far a clock offset has carried the signal from the grid.
        cf32 sl(0, 0);
        for (int j = 0; j + 1 < nJ; j++) sl += G1[(size_t)(j + 1)] * std::conj(G1[(size_t)j]);
        double tauNow = tauRef;
        if (std::abs(sl) > 0) {
            sl *= std::polar(1.0f, (float)(2 * kPi * (double)gx * (double)Dacc / (double)N));
            tauNow = (double)Dacc - std::arg(sl) * (double)N / (2 * kPi * gx);
        }
        if (!nearFade) tauRef += (tauNow - tauRef) * 0.15;   // in a fade the slope is that of noise: the delay holds
        // Timing: the window sits in the middle of the range of starts that see no neighbouring symbol, which is half a guard interval before the useful
        // part for one path and moves with the middle of the delay support for several (whole samples, with hysteresis).
        const double tauC = tauRef + tauMidRel;
        const double err = (tauC - back) - Dacc;
        if (!nearFade) { if (err > 1.0) Dacc++; else if (err < -1.0) Dacc--; }
        // frequency interpolation, with the delay removed
        const double ramp = 2 * kPi * tauC / (double)N;
        for (int j = 0; j < nJ; j++) { const int k = kG0 + (jLo + j) * gx; G1[(size_t)j] *= std::polar(1.0f, (float)(ramp * k)); }
        const int W = kEqHi - kEqLo + 1;
        Hhat.assign((size_t)W, cf32(0, 0));
        const int J = 4;
        double hp = 0;
        for (int k = kEqLo; k <= kEqHi; k++) {
            const int j0 = floorDiv(k - kG0, gx);
            const int i = (k - kG0) - j0 * gx;
            cf32 acc(0, 0);
            const std::vector<float>& kk = kern[(size_t)i];
            for (int m = -J + 1; m <= J; m++) {
                int jj = j0 + m;
                jj = std::max(jLo, std::min(jHi, jj));
                acc += G1[(size_t)(jj - jLo)] * kk[(size_t)(m + J - 1)];
            }
            acc *= std::polar(1.0f, (float)(-ramp * k));
            Hhat[(size_t)(k - kEqLo)] = acc;
            hp += std::norm(acc);
        }
        hp /= W;
        hPow += (hp - hPow) * 0.1;
        // noise power of a cell: pilot noise times the pilot power gain (2), and the error of the channel estimate (what the Wiener filter leaves)
        const double n0 = std::max(2.0 * sigma2 + (wienOk && wienerOn() ? wienMse : 0.0), 1e-9 * std::max(hPow, 1e-30));
        if (sigma2 > 0) {
            const double snr = hPow / std::max(2.0 * sigma2, 1e-30);   // the SNR of the cells (without the error of the channel estimate)
            snrSm += (10 * std::log10(std::max(snr, 1e-3)) - snrSm) * 0.1;
        }
        // equalised cells into the frame
        const int rows = s;
        for (int k = kEqLo; k <= kEqHi; k++) {
            const cf32 h = Hhat[(size_t)(k - kEqLo)];
            const float p = std::norm(h);
            const size_t idx = (size_t)rows * (size_t)grid.width() + (size_t)(k - kEqLo);
            if (p > 1e-20f) { grid.z[idx] = R.Y[(size_t)(k - kLoW)] / h; grid.w[idx] = (float)(p / n0); }
            else { grid.z[idx] = cf32(0, 0); grid.w[idx] = 0.f; }
        }
        if (t % 6 == 0) plots(t);
        // Sample rate offset from the drift of the delay. The clock corrector takes the offset out of the 48 kHz stream; what drifts in the window is what is
        // left, so the offset is the correction plus the drift that is measured. The reference of the drift is taken a little after each change of the
        // correction (the delay estimate lags by a few symbols), and the first one after 150 symbols (the lock may start off-centre).
        if (nearFade) {
            // nothing to measure in a fade; the corrector keeps the last offset
        } else if (!sroRefValid) {
            if (t >= sroRefSi) { sroRefValid = true; sroRefSi = t; sroRefTau = tauRef; }
        } else {
            const double dSym = (double)(t - sroRefSi);
            if (dSym >= 40) {
                const double drift = (tauRef - sroRefTau) / (dSym * Ts) * 1e6;
                const double est = corrPpm + drift;
                // In multipath fading the centroid of the delay wanders by up to the delay spread within a window (channel 5: +-2 ms in 3.5 s, several
                // hundred ppm). A clock offset is the same from window to window: a window is taken when it agrees with the one before within 15 ppm
                // (or its drift is small), so that the corrector is not steered by the channel; the offset shown is that of the windows taken.
                if (dSym >= 130) {
                    const bool consistent = sroWinHave && std::fabs(est - sroWinEst) < 15.0;
                    sroWinEst = est; sroWinHave = true;
                    if (consistent) { sroPpm = sroHave ? sroPpm + 0.5 * (est - sroPpm) : est; sroHave = true; }
                    if (consistent || std::fabs(drift) < 15.0) {
                        corrPpm = std::max(-400.0, std::min(400.0, corrPpm + 0.8 * drift));
                        corr.setPpm(corrPpm);
                    } else sroRejected++;
                    sroRefValid = false; sroRefSi = t + 20;
                }
            }
        }
        if (s == ns - 1) frameDone();
    }

    void frameDone() {
        const uint64_t okBefore = be.facOk, badBefore = be.facBad, rejBefore = be.facRejected;
        be.frame(grid);
        framesSync++;
        // a frame that was mostly in a fade
        const bool faded = holdSyms - holdSymsAtFrame > (uint64_t)(ns / 2);
        holdSymsAtFrame = holdSyms;
        fadedRun = faded ? fadedRun + 1 : fadedRun;
        if (be.facOk != okBefore) { facOkSince++; facBadRun = 0; fadedRun = 0; lastOkSamples = total; }
        else if (be.facBad != badBefore || be.facRejected != rejBefore) facBadRun++;
        auto L = be.layout();
        if ((L ? L->occ : -1) != gridOcc) setupLattice();
        const int k = mode == kModeE ? 3 : 1;           // mode E frames are 100 ms
        // The FAC fails: 12 frames (4.8 s) of it with the signal there (the frames in a fade do not count) and the lock is wrong; through a fade the
        // tracker holds on for up to 60 frames (24 s), so that the signal is decoded again the moment it comes back (the time interleaver still holds
        // what came before the fade).
        if (facOkSince == 0 && framesSync >= 14 * k) restartSearch("the FAC cannot be decoded");
        else if (facBadRun - fadedRun >= 12 * k) restartSearch("the FAC is lost");
        else if (facBadRun >= 60 * k) restartSearch("no signal for 24 s");
    }

    // No frequency pilots, or a lock whose FAC never decodes: the next search looks at the mirrored spectrum (I and Q swapped by the radio or
    // the file format). The stream is conjugated from here on until the next toggle.
    void toggleMirror() {
        mirror = !mirror;
        for (auto& v : buf) v = std::conj(v);
        for (auto& v : other.buf) v = std::conj(v);
    }

    void restartSearch(const char* why) {
        char b[200];
        snprintf(b, sizeof b, "DRM: signal lost (%s)", why);
        log(b);
        if (facOkSince == 0) { blockedMode = mode; blockedShift = trackShift; blockedUntil = inCount + (uint64_t)(inRate * 6); toggleMirror(); }
        phase = kSearch;
        frameSync = false;
        be.frameLost();
        eqNext = -1;
        pendHave = false;
        // both lanes search again, the 48 kHz one active; the lane that was idle has old samples
        if (eLane) swapLanes();
        clearOther();
        lastSearch = total; lastSearchE = other.total;
    }

    // ---------------------------------------------------------------- audio and messages
    // the audio service that plays: the one selected by the user, else the first one of the multiplex
    const SdcAudio* chooseAudio(const SdcInfo& sd) const {
        const int sel = selected.load();
        if (sel >= 0 && sel < 4 && sd.audio[sel].present) return &sd.audio[sel];
        for (int i = 0; i < 4; i++) if (sd.audio[i].present) return &sd.audio[i];
        return nullptr;
    }
    static size_t audioKey(const SdcAudio& a) {
        size_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix((uint64_t)a.shortId); mix((uint64_t)a.streamId); mix((uint64_t)a.coding); mix((uint64_t)a.sbr); mix((uint64_t)a.mode); mix((uint64_t)a.rateCode); mix((uint64_t)a.mps);
        for (uint8_t c : a.config) mix(c);
        return h | 1;
    }

    void onLogical(const LogicalFrame& f) {
        const SdcInfo& sd = be.sdc();
        if (streamCb) streamCb(f.stream, f.data, f.lenA + f.lenB, f.lenA, tel);
        journaline(f, sd);
        dataServices(f, sd);
        const SdcAudio* au = chooseAudio(sd);
        if (!au || au->streamId != f.stream) return;
        const int len = f.lenA + f.lenB;
        int audioLen = len;
        if (au->text && len > 4) { audioLen = len - 4; if (textDec.feed(f.data + len - 4)) textMsg = textDec.text(); }
        const bool E = be.mode() == kModeE;
        // the decoder follows the audio description of the SDC
        const size_t key = audioKey(*au) ^ (E ? (size_t)0x5bd1e995u : (size_t)0);
        if (key != adecKey) {
            adecKey = key; adecShort = au->shortId;
            adec.configure(*au, E);
            adecOkSeen = adecBadSeen = 0;
            audioInfoText = adec.info();
            char b[200];
            snprintf(b, sizeof b, "DRM: audio: %s", audioInfoText.c_str());
            log(b);
        }
        if (f.afterGap) { adec.reset(); pendHave = false; }
        pcm.clear(); gaps.clear();
        playout.setSuperFrameSecs(E ? 0.2 : 0.4);
        if (E) {
            // DRM+: the audio super frame is the first half (the logical frame at an even position of the super frame) and the second
            if (f.position % 2 == 0) { pendE.assign(f.data, f.data + audioLen); pendIdx = f.index; pendHave = true; return; }
            if (!pendHave || f.index != pendIdx + 1) { pendHave = false; return; }
            pendHave = false;
            joinE = pendE;
            joinE.insert(joinE.end(), f.data, f.data + audioLen);
            adec.superFrame(joinE.data(), (int)joinE.size(), 2 * f.lenA, pcm, &gaps);
        } else {
            adec.superFrame(f.data, audioLen, f.lenA, pcm, &gaps);
        }
        audioOk += adec.framesOk() - adecOkSeen; audioBad += adec.framesBad() - adecBadSeen;
        adecOkSeen = adec.framesOk(); adecBadSeen = adec.framesBad();
        audioInfoText = adec.info();
        audioStateNow = adec.state();
        if (!pcm.empty()) playout.decoded(pcm.data(), pcm.size() / 2, gaps);
    }

    // The playout hands over the sound for the signal time that has passed (a steady stream: one super frame of it waits in the playout's queue,
    // the sound device holds 200 ms); the playout's resampler follows the device's queue (the sound card's clock).
    void playAudio() {
        if (!playout.started()) return;
        const bool dev = !silent.load();
        if (dev && !audioOut) {
            audioOut = std::make_unique<AudioOut>();
            audioOut->start(48000);
            audioOut->setStartThreshold(drm::Playout::kDeviceStartFrames);
            audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        }
        outPcm.clear();
        playout.advance((double)inCount / inRate, dev && audioOut ? audioOut->bufferedFrames() : -1, outPcm);
        const size_t n = outPcm.size() / 2;
        if (n == 0) return;
        if (tap) {
            tapL.resize(n); tapR.resize(n);
            for (size_t i = 0; i < n; i++) { tapL[i] = outPcm[2 * i]; tapR[i] = outPcm[2 * i + 1]; }
            tap(tapL.data(), tapR.data(), n);
        }
        if (!dev || !audioOut) return;
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > drm::Playout::kDeviceFlushFrames) audioOut->flush();   // cannot happen while the resampler follows the device
        audioOut->write(outPcm.data(), (int)n);
    }

    // ---------------------------------------------------------------- plots
    void plots(int64_t t) {
        // channel magnitude over the carriers
        const int W = kEqHi - kEqLo + 1;
        const int step = std::max(1, W / 400);
        chanDb.clear();
        for (int k = kEqLo; k <= kEqHi; k += step) chanDb.push_back((float)(10 * std::log10(std::max((double)std::norm(Hhat[(size_t)(k - kEqLo)]), 1e-20))));
        // impulse response from the lattice (the delay of the middle of its support is taken out: the first half of the points is the past, the second the future)
        const int nJ = jHi - jLo + 1;
        const int M = 256;
        const double tauC = tauRef + tauMidRel;
        std::vector<cf32> g((size_t)M, cf32(0, 0));
        for (int j = 0; j < nJ && j < M; j++) {
            const double wnd = 0.5 - 0.5 * std::cos(2 * kPi * (j + 0.5) / nJ);
            g[(size_t)j] = G1[(size_t)j] * (float)wnd;
        }
        DrmFft f(M);
        f.inverse(g.data());
        cirDb.assign((size_t)M, 0.f);
        std::vector<double> pw((size_t)M);
        double mx = 1e-30;
        for (int i = 0; i < M; i++) { const int ii = (i + M / 2) % M; pw[(size_t)i] = std::norm(g[(size_t)ii]); mx = std::max(mx, pw[(size_t)i]); }
        for (int i = 0; i < M; i++) cirDb[(size_t)i] = (float)(10 * std::log10(std::max(pw[(size_t)i] / mx, 1e-9)));
        const double spanSamples = (double)N / gx;
        const double dStep = spanSamples / M;
        cirStepMs = (float)(dStep / fs * 1000.0);
        cirStartMs = (float)((-0.5 * spanSamples + tauC - back) / fs * 1000.0);
        // the support: the points more than 20 dB below the peak or near the noise floor do not count; its middle is where the window belongs
        std::vector<double> sorted(pw);
        std::nth_element(sorted.begin(), sorted.begin() + M / 2, sorted.end());
        const double med = sorted[(size_t)M / 2] + 1e-30;
        const double thr = std::max(0.01 * mx, 14.0 * med);
        int lo = -1, hi = -1;
        for (int i = 0; i < M; i++) if (pw[(size_t)i] > thr) { if (lo < 0) lo = i; hi = i; }
        if (lo >= 0 && mx > 30.0 * med && !nearFade) {
            const double mid = (0.5 * (lo + hi) - M / 2) * dStep;
            tauMidRel = std::max(-(double)G, std::min((double)G, tauMidRel + 0.4 * mid));
        }
        // rms delay spread over the points that stand clearly above the noise
        double noise = 0; int nn = 0;
        for (int i = 0; i < M / 8; i++) { noise += pw[(size_t)i]; nn++; }
        noise = noise / std::max(nn, 1) * 3;
        double sw = 0, st = 0, s2 = 0;
        for (int i = 0; i < M; i++) { const double p = pw[(size_t)i] > noise ? pw[(size_t)i] : 0; sw += p; st += p * i; s2 += p * i * i; }
        if (sw > 0) { const double m = st / sw; delaySpreadMs = (float)(std::sqrt(std::max(0.0, s2 / sw - m * m)) * cirStepMs); }
        (void)t;
    }

    // ---------------------------------------------------------------- feed and telemetry
    // one lane: front end, clock corrector, the mirror, into the lane's buffer
    void push(DrmFront& fr, SroCorrector& co, const cf32* x, size_t n, std::vector<cf32>& b, uint64_t& tot) {
        tmp.clear();
        fr.process(x, n, tmp);
        tmp2.clear();
        co.process(tmp.data(), tmp.size(), tmp2);
        if (mirror) for (auto& v : tmp2) v = std::conj(v);
        b.insert(b.end(), tmp2.begin(), tmp2.end());
        tot += tmp2.size();
    }

    void feed(const cf32* x, size_t n) {
        if (!ready || n == 0) return;
        if (!eLane) {
            // a channel away from the middle of the sample band (beyond the +-24 kHz the front end keeps) is moved there first
            cin.assign(x, x + n);
            cc.process(cin.data(), n, phase == kTrack && be.sdcOk > 0 && be.mscOk > 0);   // FAC alone also comes through an alias of a strong channel far out
            if (cc.takeChanged()) resetAll(false);
            push(front, corr, cin.data(), n, buf, total);
            // the DRM+ lane takes the input as it is (a 100 kHz channel is not moved), while nothing has been found
            if (eOk && phase == kSearch) push(other.front, other.corr, x, n, other.buf, other.total);
        } else {
            push(front, corr, x, n, buf, total);
        }
        inSamples += n; sincePub += n; inCount += n;
        run();
        playAudio();
        // keep the stream bounded
        const double sc = fs / kFs;
        const uint64_t keepSearch = (uint64_t)(120000 * sc);
        uint64_t keepFrom = total > keepSearch ? total - keepSearch : 0;
        if (phase == kTrack) {
            const int64_t need = nS + Dacc - 2 * Ts;
            keepFrom = std::min<uint64_t>(keepFrom, need > 0 ? (uint64_t)need : 0);
            if (total > (uint64_t)(400000 * sc)) keepFrom = std::max<uint64_t>(keepFrom, total - (uint64_t)(400000 * sc));
        }
        if (keepFrom > base + (uint64_t)(65536 * sc)) {
            buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)(keepFrom - base));
            base = keepFrom;
        }
        if (!eLane) {
            const uint64_t keepE = 2 * kWindowE;
            if (other.total - other.base > keepE + 65536) {
                const uint64_t from = other.total - keepE;
                other.buf.erase(other.buf.begin(), other.buf.begin() + (ptrdiff_t)(from - other.base));
                other.base = from;
            }
        }
        if (sincePub >= (uint64_t)(inRate * 0.25)) publish();
    }

    static constexpr uint64_t kWindowE = 96000;   // 0.5 s of the DRM+ lane: 200 symbols

    void run() {
        if (phase == kSearch && !eLane) {
            if (total - lastSearch >= 19200 && total - base >= 72000) {
                lastSearch = total;
                statSearchRuns++;
                const Acq a = acquire();
                if (a.ok) startTracking(a);
            }
            // DRM+ every 0.4 s
            if (phase == kSearch && eOk && other.total - lastSearchE >= 76800 && other.total - other.base >= kWindowE) {
                lastSearchE = other.total;
                const uint64_t absStart = other.total - kWindowE;
                const Acq a = acquireE(other.buf.data() + (absStart - other.base), kWindowE, absStart);
                if (a.ok) {
                    swapLanes();
                    clearOther();
                    startTracking(a);
                }
            }
        }
        if (phase == kTrack) { int guard = 0; while (phase == kTrack && trackStep() && ++guard < 4000) {} }
        if (phase == kTrack && !eLane && frameSync && facOkSince > 0) backgroundAcquire();
    }

    double wrapTs(double x) const { return x - (double)Ts * std::round(x / (double)Ts); }

    // While the FAC fails, the acquisition runs every 0.4 s on the last 1.5 s. A signal at another symbol timing (the radio lost samples) or another
    // frequency, found twice in a row, is taken at once. While the FAC decodes, the same search now and then measures where the acquisition puts the
    // symbol start relative to the tracker's grid (multipath moves it a little), the reference for "another timing".
    void backgroundAcquire() {
        const bool trouble = facBadRun >= 2;
        const bool calibrate = facBadRun == 0 && facOkSince >= 3 && !hold && (!offCal || total - lastCal > (uint64_t)(30 * fs));
        if (!trouble && !calibrate) { reacqVotes = 0; return; }
        if (total - lastReacq < 19200 || total - base < 72000) return;
        lastReacq = total;
        const Acq a = acquire(false);
        if (!a.ok || a.mode != mode) { if (trouble) reacqVotes = 0; return; }
        const double off = wrapTs((double)a.symStart - (double)(nS + Dacc));
        if (calibrate) { off0 = off; offCal = true; lastCal = total; return; }
        if (!offCal) return;
        const double sp = modeParams(mode).spacingHz();
        const double dOff = wrapTs(off - off0);
        const bool moved = std::fabs(dOff) > std::max(0.5 * G, 0.2 * Ts) || std::fabs(a.fTotal - fHz) > 0.3 * sp;
        if (!moved) { reacqVotes = 0; return; }
        if (reacqVotes > 0 && std::fabs(wrapTs(off - reacqOffPrev)) < 0.1 * Ts && std::fabs(a.fTotal - reacqFPrev) < 0.1 * sp) {
            char b[200];
            snprintf(b, sizeof b, "DRM: the signal moved (timing %+.1f ms, frequency %+.1f Hz): synchronising again", dOff / fs * 1000.0, a.fTotal - fHz);
            log(b);
            resyncs++;
            Acq n = a;
            // start at a symbol near the end of the buffer: the start of the search window may still be before the jump
            const double from = (double)total - 0.3 * fs;
            if ((double)n.symStart < from) n.symStart += (uint64_t)(std::floor((from - (double)n.symStart) / Ts)) * (uint64_t)Ts;
            startTracking(n);
            return;
        }
        reacqVotes = 1; reacqOffPrev = off; reacqFPrev = a.fTotal;
    }

    void publish() {
        sincePub = 0;
        DrmTelemetry t;
        t.levelDbfs = (float)front.levelDbfs();
        front.resetLevel();
        t.mode = phase == kTrack ? mode : (be.facValid() ? be.mode() : -1);
        if (t.mode >= 0) { t.modeName = modeParams(t.mode).name; }
        const bool tracking = phase == kTrack && frameSync;
        t.cfoHz = fHz + cc.offsetHz();
        t.snrDb = tracking && sigma2 > 0 ? (float)std::max(0.0, std::min(60.0, snrSm)) : 0.f;
        t.correlation = (float)cpRho;
        t.sroPpm = (float)sroPpm;
        t.dopplerHz = (float)dopplerSm;
        t.delaySpreadMs = (float)delaySpreadMs;
        t.timingMs = (float)((tauRef + tauMidRel - back) / fs * 1000.0);
        // multiplex description
        if (be.facValid()) {
            const FacInfo& f = be.fac();
            t.occupancy = f.occupancy;
            t.bandwidthKhz = (float)occupancyKhz(be.mode(), f.occupancy);
            t.longInterleave = f.interleaver == 0 ? 1 : 0;
            if (be.mode() == kModeE) { t.mscQam = f.mscMode == 0 ? 16 : f.mscMode == 3 ? 4 : 0; t.sdcQam = 4; }
            else { t.mscQam = f.mscMode == 0 ? 64 : f.mscMode == 3 ? 16 : 0; t.sdcQam = f.sdcMode == 0 ? 16 : 4; }
            t.identity = f.identity;
            t.reconfiguration = f.reconfig;
            t.hierarchicalUnsupported = be.hierarchical();
        }
        const SdcInfo& sd = be.sdc();
        if (be.sdcValid()) {
            t.protA = sd.mux.protA; t.protB = sd.mux.protB;
            t.numStreams = sd.mux.nStreams;
            for (int i = 0; i < sd.mux.nStreams && i < 4; i++) { t.streamLenA[i] = sd.mux.stream[i].lenA; t.streamLenB[i] = sd.mux.stream[i].lenB; }
        }
        // services
        t.services.clear();
        for (int i = 0; i < 4; i++) {
            const FacService* fs = be.facService(i);
            if (!fs && !(sd.audio[i].present || sd.label[i].present || sd.app[i].present)) continue;
            DrmService s;
            s.shortId = i;
            if (fs) {
                s.id = fs->id; s.audio = !fs->data; s.language = fs->language; s.languageName = languageName(fs->language);
                s.programmeType = fs->descriptor; s.programmeName = fs->data ? "" : (fs->descriptor < 30 ? programmeTypeName(fs->descriptor) : "");
                s.conditionalAccess = fs->audioCa || fs->dataCa;
            }
            if (sd.app[i].present && !sd.audio[i].present) {
                const SdcApp& a = sd.app[i];
                s.audio = false;
                s.streamId = a.streamId;
                s.appName = drmAppName(a.domain, a.dabUserApp());
                char mb[120];
                if (a.packet) snprintf(mb, sizeof mb, "packet mode, %s, packet id %d", a.dataUnit ? "data units" : "single packets", a.packetId);
                else snprintf(mb, sizeof mb, "stream mode data, not decoded");
                s.appMode = mb;
                const int app = a.dabUserApp();
                s.appDecoded = a.packet && a.dataUnit && a.packetLen > 0 && a.domain == 0 && (app == 0x002 || app == 0x003 || app == 0x004 || app == 0x007 || app == 0x44A);
                if (a.streamId < sd.mux.nStreams)
                    s.bitrateBps = (int)((sd.mux.stream[a.streamId].lenA + sd.mux.stream[a.streamId].lenB) * 8 / (modeParams(be.mode() >= 0 ? be.mode() : 1).frameMs() / 1000.0));
            }
            if (sd.label[i].present) s.label = sd.label[i].text;
            if (sd.lang[i].present) { s.country = sd.lang[i].country; if (!sd.lang[i].language.empty() && sd.lang[i].language[0] != '-') s.languageName = sd.lang[i].language; }
            if (sd.audio[i].present) {
                const SdcAudio& a = sd.audio[i];
                s.audio = true;
                s.audioCoding = a.coding; s.sbr = a.sbr != 0; s.audioMode = a.mode; s.audioRateHz = a.rateHz(); s.textMessage = a.text; s.streamId = a.streamId;
                if (a.streamId < sd.mux.nStreams)
                    s.bitrateBps = (int)((sd.mux.stream[a.streamId].lenA + sd.mux.stream[a.streamId].lenB) * 8 / (modeParams(be.mode() >= 0 ? be.mode() : 1).frameMs() / 1000.0));
                char cb[100];
                snprintf(cb, sizeof cb, "%s%s%s, %.0f kHz %s", a.coding == 3 ? "xHE-AAC" : "AAC", a.sbr ? " + SBR" : "", a.coding == 0 && a.mode == 1 ? " + PS" : "",
                         a.rateHz() / 1000.0, a.mode == 0 ? "mono" : a.mode == 2 ? "stereo" : "mono core");
                s.codecText = cb;
            }
            t.services.push_back(s);
        }
        if (sd.time.present) {
            t.timeValid = true;
            int y, m, d;
            mjdToDate(sd.time.mjd, y, m, d);
            t.year = y; t.month = m; t.day = d; t.hour = sd.time.hour; t.minute = sd.time.minute;
            t.hasLocalOffset = sd.time.hasOffset; t.localOffsetHalfHours = sd.time.offsetHalfHours;
        }
        t.facOk = be.facOk; t.facBad = be.facBad; t.sdcOk = be.sdcOk; t.sdcBad = be.sdcBad; t.mscFramesOk = be.mscOk; t.mscFramesBad = be.mscBad;
        t.blocksOk = audioOk; t.blocksBad = audioBad;
        t.textMessage = textMsg;
        t.textSegmentsOk = textDec.segmentsOk(); t.textSegmentsBad = textDec.segmentsBad();
        // state
        const uint64_t recent = (uint64_t)(fs * 2.0);
        const bool facRecent = tracking && be.facValid() && total - lastOkSamples < recent * 2;
        const bool mscFlowing = be.mscOk != lastMscOk;
        if (mscFlowing) { sinceMsc = 0; lastMscOk = be.mscOk; } else sinceMsc += 1;
        t.state = !tracking ? 0 : (facRecent ? ((be.sdcValid() && be.mscReady() && sinceMsc < 12) ? 2 : 1) : 0);
        t.dataValid = t.state == 2 && audioOk > 0;
        // quality: FAC and audio success weighted by the SNR
        {
            const double okRate = (be.facOk + 1.0) / (be.facOk + be.facBad + 1.0);
            const double aud = (audioOk + 0.5) / (audioOk + audioBad + 0.5);
            const double q = t.state == 0 ? 0 : std::min(1.0, 0.5 * okRate + 0.5 * (t.dataValid ? aud : 0.0)) * std::min(1.0, std::max(0.0, (t.snrDb - 3) / 15.0 + 0.3));
            qualitySm += (q - qualitySm) * 0.3;
            t.quality = (float)qualitySm;
        }
        t.chanDb = chanDb; t.chanFirstCarrier = kEqLo; t.chanCarrierStep = (float)std::max(1, (kEqHi - kEqLo + 1) / 400);
        t.chanSpacingHz = mode >= 0 ? (float)modeParams(mode).spacingHz() : 0.f;
        t.cirDb = cirDb; t.cirStartMs = cirStartMs; t.cirStepMs = cirStepMs;
        t.facConst = be.facConst; t.sdcConst = be.sdcConst; t.mscConst = be.mscConst;
        t.audioState = (!sd.audio[0].present && !sd.audio[1].present && !sd.audio[2].present && !sd.audio[3].present) ? 0 : (audioStateNow == 2 ? 2 : 1);
        t.audioInfo = t.audioState == 0 ? "no audio service" : audioInfoText.empty() ? "waiting for audio frames" : audioInfoText;
        t.audioSamples = adec.samples();
        t.serviceAudioSelected = adecShort;
        t.audioConcealedSecs = playout.concealedSecs();
        t.audioMutedSecs = playout.mutedSecs();
        t.audioRealSecs = playout.realSecs();
        t.audioConcealEvents = playout.events();
        t.audioQueueSecs = (float)playout.queueSecs();
        t.audioDriftPpm = (float)((playout.ratio() - 1.0) * 1e6);
        t.audioUnderruns = audioOut ? audioOut->underruns() : 0;
        {
            const double madeUp = t.audioConcealedSecs + t.audioMutedSecs;
            concHist.push_back({t.audioRealSecs, madeUp});
            if (concHist.size() > 41) concHist.erase(concHist.begin());
            const double dr = t.audioRealSecs - concHist.front().first, dm = madeUp - concHist.front().second;
            t.audioConcealedPct = dr + dm > 0 ? (float)(100.0 * dm / (dr + dm)) : 0.f;
        }
        t.holding = phase == kTrack && hold;
        t.fades = holdEvents;
        t.resyncs = resyncs;
        {
            char b[200];
            if (t.mode < 0 && eOk) snprintf(b, sizeof b, "searching (guard correlation A %.2f B %.2f C %.2f D %.2f E %.2f)", lastRho[0], lastRho[1], lastRho[2], lastRho[3], lastRho[4]);
            else if (t.mode < 0) snprintf(b, sizeof b, "searching (guard correlation A %.2f B %.2f C %.2f D %.2f)", lastRho[0], lastRho[1], lastRho[2], lastRho[3]);
            else if (!tracking) snprintf(b, sizeof b, "mode %c found, looking for the frame start", t.modeName);
            else if (t.holding) snprintf(b, sizeof b, "mode %c, fade: holding the synchronisation until the signal comes back", t.modeName);
            else snprintf(b, sizeof b, "mode %c, %s", t.modeName, t.state == 2 ? "decoding" : t.state == 1 ? "FAC read" : "no FAC yet");
            t.status = b;
        }
        std::lock_guard<std::mutex> lk(mu);
        t.seq = ++telSeq;
        tel = std::move(t);
    }
};

DrmReceiver::DrmReceiver() : p_(std::make_unique<Impl>()) {}
DrmReceiver::~DrmReceiver() = default;

void DrmReceiver::configure(double inputRateHz) { p_->configure(inputRateHz); }
bool DrmReceiver::ready() const { return p_->ready; }
void DrmReceiver::reset() { if (p_->ready) { p_->cc.reset(); p_->resetAll(true); } }
void DrmReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool DrmReceiver::telemetry(DrmTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq == lastSeq) return false;
    out = p_->tel;
    return true;
}
void DrmReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }
void DrmReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void DrmReceiver::setMuted(bool m) { p_->muted = m; }
void DrmReceiver::setSilent(bool s) { p_->silent = s; }
void DrmReceiver::setAudioTap(std::function<void(const float*, const float*, size_t)> cb) { p_->tap = std::move(cb); }
void DrmReceiver::selectService(int shortId) { p_->selected = shortId; }
JournalineDecoder& DrmReceiver::journaline() { return p_->jl; }
bool DrmReceiver::dataView(int shortId, DrmDataView& out) const {
    std::lock_guard<std::mutex> lk(p_->dataMu);
    auto it = p_->dsvc.find(shortId);
    if (it == p_->dsvc.end()) return false;
    it->second.fill(out);
    return true;
}
bool DrmReceiver::takeSlide(int& shortId, DrmSlide& out) {
    std::lock_guard<std::mutex> lk(p_->dataMu);
    for (auto& kv : p_->dsvc) if (kv.second.takeSlide(out)) { shortId = kv.first; return true; }
    return false;
}
bool DrmReceiver::hasJournaline() const { return p_->jlStream.load() >= 0; }
void DrmReceiver::setStreamCallback(std::function<void(int, const uint8_t*, int, int, const DrmTelemetry&)> cb) { p_->streamCb = std::move(cb); }

} // namespace dect2
