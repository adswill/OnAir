// DRM receiver (ETSI ES 201 980): acquisition, OFDM tracking, channel estimation, and the hand-over to the back end (FAC, SDC, MSC) in drm_dec.cpp.
//
//   input -> channel filter, 48 kHz (drm_front) -> guard interval correlation (mode, symbol timing, carrier offset modulo the carrier spacing)
//         -> frequency reference triplet (whole carrier spacings) -> OFDM demodulation, one FFT per symbol
//         -> frame start from the time reference cells -> gain reference cells: channel estimate (time, then frequency interpolation)
//         -> equalised cells of a frame -> DrmBackend
//
// The window of every FFT starts half a guard interval into the guard, on whole samples; the window moves by whole samples to follow the channel's
// delay, and the symbols are referred to one fixed grid again by a phase ramp over the carriers, so channel estimates of different symbols stay comparable.
#include "dect2/drm_rx.h"
#include "dect2/audioout.h"
#include "dect2/drm_audio.h"
#include "dect2/drm_dec.h"
#include "dect2/drm_fft.h"
#include "drm_front.h"
#include "drm_internal.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {

using namespace drm;

namespace {
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// part of the band that every spectrum occupancy of the mode shares and where no gain reference is boosted: used until the FAC says which occupancy it is
const int kSafe[4][2] = {{8, 96}, {5, 87}, {-63, 63}, {-40, 40}};
// widest carrier range of the mode (union over the occupancies)
const int kWide[4][2] = {{-114, 350}, {-103, 311}, {-69, 213}, {-44, 135}};

int posMod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
int floorDiv(int a, int b) { int q = a / b; return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q; }
int ceilDiv(int a, int b) { return -floorDiv(-a, b); }
double besselI0(double x) { double s = 1, t = 1; for (int k = 1; k < 50; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; } return s; }
}

ModeTuning drmTuning() {
    ModeTuning t;
    t.stdMode = 12; t.id = "drm"; t.name = "DRM";
    t.minMhz = 1; t.maxMhz = 110; t.defMhz = 9.5;
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

    enum Phase { kSearch, kTrack } phase = kSearch;
    uint64_t lastSearch = 0;
    uint64_t blockedUntil = 0;       // after a false alarm: no new lock on the same (mode, shift) before this sample
    int blockedMode = -1, blockedShift = 0, trackShift = 0;
    bool mirror = false;                      // the input is conjugated (a mirrored spectrum)
    double lastRho[4] = {};

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
    double accRr = 0, accRi = 0;
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
    uint64_t statSearchRuns = 0;
    float levelDb = -120;
    double lastCorr = 0;

    Impl() : fftN(1) {
        ring.resize(kRing);
        be.log = [this](const std::string& s) { log(s); };
        be.onLogical = [this](const LogicalFrame& f) { onLogical(f); };
    }

    void log(const std::string& s) { if (logCb) logCb(s); }

    // ---------------------------------------------------------------- setup
    void configure(double rate) {
        inRate = rate;
        ready = front.configure(rate);
        corr.reset(); corrPpm = 0; corr.setPpm(0);
        resetAll(false);
    }

    void resetAll(bool bumpSeq) {
        front.reset();
        buf.clear(); base = 0; total = 0; lastSearch = 0;
        phase = kSearch;
        mirror = false;
        blockedMode = -1; blockedUntil = 0;
        be.reset();
        be.log = [this](const std::string& s) { log(s); };
        be.onLogical = [this](const LogicalFrame& f) { onLogical(f); };
        mode = -1;
        audioOk = audioBad = 0;
        textDec.reset(); textMsg.clear();
        adec.reset(); adecKey = 0; adecShort = -1; adecOkSeen = adecBadSeen = 0; audioInfoText.clear(); audioStateNow = 0;
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
    void guardCorr(const cf32* z, size_t W, int m, double& rho, size_t& tau, cf32& peak, int& nsym) {
        const ModeParams& mp = modeParams(m);
        const int Tu = mp.tu12 * 4, Tg = mp.tg12 * 4, Tsym = Tu + Tg;
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

    Acq acquire() {
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
        const bool blockedNow = mB == blockedMode && total < blockedUntil;
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
        if (bestScore < 0.5) { toggleMirror(); return res; }
        res.ok = true;
        res.mode = mB; res.rho = rhoB; res.score = bestScore;
        res.symStart = absStart + tauB;
        res.fTotal = fFrac + fErr + bestShift * kFs / (double)Tu;
        res.shift = bestShift;
        return res;
    }

    // ---------------------------------------------------------------- tracker set-up
    void startTracking(const Acq& a) {
        mode = a.mode;
        trackShift = a.shift;
        const ModeParams& mp = modeParams(mode);
        N = mp.tu12 * 4; G = mp.tg12 * 4; Ts = N + G; ns = mp.ns; gx = mp.gx; gy = mp.gy; gk0 = mp.gk0;
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
        lag = gy;
        framesSync = 0; facOkSince = 0; facBadRun = 0; cpBadRun = 0;
        tauRef = 0; tauMidRel = 0; tauSlope = 0; tauHist.clear();
        sigma2 = 0; snrSm = 0; hPow = 0; dopplerSm = 0; delaySpreadMs = 0; accR = accP = accRr = accRi = 0;
        sroPpm = corrPpm; sroRefSi = 150; sroRefTau = 0; sroRefValid = false; sroHave = false;
        occRef = -2;
        gridOcc = -3;
        be.setMode(mode);
        be.frameLost();
        setupLattice();
        phase = kTrack;
        char b[160];
        snprintf(b, sizeof b, "DRM: signal found: mode %c, carrier offset %+.1f Hz, guard correlation %.2f", mp.name, fHz, a.rho);
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
            const double ph0 = phRef + 2 * kPi * fHz * ((double)ws - nRef) / kFs;
            cf32 r = std::polar(1.0f, (float)(-std::fmod(ph0, 2 * kPi)));
            const cf32 st = std::polar(1.0f, (float)(-2 * kPi * fHz / kFs));
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
        if (cpBadRun >= 6) { restartSearch("the signal lost its OFDM structure"); return true; }
        if (!frameSync) {
            frameSyncStep(t);
        } else {
            lattice(t);
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

    // carrier offset from the phase of the frequency reference tones from symbol to symbol
    void freqLoop(int64_t t) {
        if (mode > 3 || t < 1) return;
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
        const double eps = std::arg(acc) / (2 * kPi * (double)Ts / kFs);   // Hz
        freqErr += (eps - freqErr) * 0.15;
        // move the correction in smoothly; the rotation phase stays continuous
        if (t % 4 == 0 && std::fabs(freqErr) > 0.005) {
            const double nNow = (double)nS;
            phRef += 2 * kPi * fHz * (nNow - nRef) / kFs;
            nRef = nNow;
            fHz += 0.5 * freqErr;
            freqErr *= 0.5;
        }
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
        double sumRe = 0, sumIm = 0, sumP = 0; int cntD = 0;
        for (int j = jLo; j <= jHi; j++) {
            const int qj = posMod(j + qOff, gy);
            const int d = posMod(s % gy - qj, gy);
            const SymRec* a = recIf(t - d);
            const SymRec* b = recIf(t - d + gy);
            cf32 v(0, 0);
            bool ok = false;
            if (d == 0) {
                if (a) { v = a->Hr[(size_t)j]; ok = true; }
                const SymRec* p = recIf(t - gy);
                if (a && p && b) {
                    const cf32 pred = 0.5f * (p->Hr[(size_t)j] + b->Hr[(size_t)j]);
                    accN += std::norm(a->Hr[(size_t)j] - pred); cntN++;
                }
                if (a && p) { const cf32 c = a->Hr[(size_t)j] * std::conj(p->Hr[(size_t)j]); sumRe += c.real(); sumIm += c.imag(); sumP += 0.5 * (std::norm(a->Hr[(size_t)j]) + std::norm(p->Hr[(size_t)j])); cntD++; }
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
        if (cntN >= 3) {
            const double s2 = accN / (1.5 * cntN);
            sigma2 = sigma2 > 0 ? sigma2 + (s2 - sigma2) * 0.12 : s2;
        }
        if (cntD >= 3) {
            accRr += (sumRe / cntD - accRr) * 0.05; accRi += (sumIm / cntD - accRi) * 0.05; accP += (sumP / cntD - accP) * 0.05;
            const double pw = std::max(accP - sigma2, 1e-12);
            const double rho = std::min(1.0, std::hypot(accRr, accRi) / pw);
            const double tauY = (double)gy * Ts / kFs;
            dopplerSm = rho < 0.999 ? std::sqrt(std::max(0.0, -std::log(std::max(rho, 1e-3)))) / (kPi * tauY) : 0;
            if (rho < 0.999) dopplerSm = std::min(dopplerSm, 20.0);
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
        tauRef += (tauNow - tauRef) * 0.15;
        // Timing: the window sits in the middle of the range of starts that see no neighbouring symbol, which is half a guard interval before the useful
        // part for one path and moves with the middle of the delay support for several (whole samples, with hysteresis).
        const double tauC = tauRef + tauMidRel;
        const double err = (tauC - back) - Dacc;
        if (err > 1.0) Dacc++; else if (err < -1.0) Dacc--;
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
        // noise power of a cell: pilot noise times the pilot power gain (2)
        const double n0 = std::max(2.0 * sigma2, 1e-9 * std::max(hPow, 1e-30));
        if (sigma2 > 0) {
            const double snr = hPow / n0;
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
        if (!sroRefValid) {
            if (t >= sroRefSi) { sroRefValid = true; sroRefSi = t; sroRefTau = tauRef; }
        } else {
            const double dSym = (double)(t - sroRefSi);
            if (dSym >= 40) {
                const double drift = (tauRef - sroRefTau) / (dSym * Ts) * 1e6;
                const double est = corrPpm + drift;
                sroPpm = sroHave ? sroPpm + 0.2 * (est - sroPpm) : est;
                sroHave = true;
                if (dSym >= 130) {
                    corrPpm = std::max(-400.0, std::min(400.0, corrPpm + 0.8 * drift));
                    corr.setPpm(corrPpm);
                    sroRefValid = false; sroRefSi = t + 20;
                }
            }
        }
        if (s == ns - 1) frameDone();
    }

    void frameDone() {
        const uint64_t okBefore = be.facOk, badBefore = be.facBad;
        be.frame(grid);
        framesSync++;
        if (be.facOk != okBefore) { facOkSince++; facBadRun = 0; lastOkSamples = total; }
        else if (be.facBad != badBefore) facBadRun++;
        auto L = be.layout();
        if ((L ? L->occ : -1) != gridOcc) setupLattice();
        if (facOkSince == 0 && framesSync >= 14) restartSearch("the FAC cannot be decoded");
        else if (facBadRun >= 12) restartSearch("the FAC is lost");
    }

    // No frequency pilots, or a lock whose FAC never decodes: the next search looks at the mirrored spectrum (I and Q swapped by the radio or
    // the file format). The stream is conjugated from here on until the next toggle.
    void toggleMirror() {
        mirror = !mirror;
        for (auto& v : buf) v = std::conj(v);
    }

    void restartSearch(const char* why) {
        char b[200];
        snprintf(b, sizeof b, "DRM: signal lost (%s)", why);
        log(b);
        if (facOkSince == 0) { blockedMode = mode; blockedShift = trackShift; blockedUntil = total + (uint64_t)(kFs * 6); toggleMirror(); }
        phase = kSearch;
        frameSync = false;
        be.frameLost();
        lastSearch = total;
        eqNext = -1;
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
        const SdcAudio* au = chooseAudio(sd);
        if (!au || au->streamId != f.stream) return;
        const int len = f.lenA + f.lenB;
        int audioLen = len;
        if (au->text && len > 4) { audioLen = len - 4; if (textDec.feed(f.data + len - 4)) textMsg = textDec.text(); }
        // the decoder follows the audio description of the SDC
        const size_t key = audioKey(*au);
        if (key != adecKey) {
            adecKey = key; adecShort = au->shortId;
            adec.configure(*au, false);
            adecOkSeen = adecBadSeen = 0;
            audioInfoText = adec.info();
            char b[200];
            snprintf(b, sizeof b, "DRM: audio: %s", audioInfoText.c_str());
            log(b);
        }
        if (f.afterGap) adec.reset();
        pcm.clear();
        adec.superFrame(f.data, audioLen, f.lenA, pcm);
        audioOk += adec.framesOk() - adecOkSeen; audioBad += adec.framesBad() - adecBadSeen;
        adecOkSeen = adec.framesOk(); adecBadSeen = adec.framesBad();
        audioInfoText = adec.info();
        audioStateNow = adec.state();
        if (!pcm.empty()) playAudio();
    }

    void playAudio() {
        const size_t n = pcm.size() / 2;
        if (tap) {
            tapL.resize(n); tapR.resize(n);
            for (size_t i = 0; i < n; i++) { tapL[i] = pcm[2 * i]; tapR[i] = pcm[2 * i + 1]; }
            tap(tapL.data(), tapR.data(), n);
        }
        if (silent.load()) return;
        if (!audioOut) {
            audioOut = std::make_unique<AudioOut>();
            audioOut->start(48000);
            audioOut->setStartThreshold(48000 / 4);
            audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();   // the radio's clock runs ahead of the sound card: catch up
        audioOut->write(pcm.data(), (int)n);
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
        cirStepMs = (float)(dStep / kFs * 1000.0);
        cirStartMs = (float)((-0.5 * spanSamples + tauC - back) / kFs * 1000.0);
        // the support: the points more than 20 dB below the peak or near the noise floor do not count; its middle is where the window belongs
        std::vector<double> sorted(pw);
        std::nth_element(sorted.begin(), sorted.begin() + M / 2, sorted.end());
        const double med = sorted[(size_t)M / 2] + 1e-30;
        const double thr = std::max(0.01 * mx, 14.0 * med);
        int lo = -1, hi = -1;
        for (int i = 0; i < M; i++) if (pw[(size_t)i] > thr) { if (lo < 0) lo = i; hi = i; }
        if (lo >= 0 && mx > 30.0 * med) {
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
    void feed(const cf32* x, size_t n) {
        if (!ready || n == 0) return;
        tmp.clear();
        front.process(x, n, tmp);
        tmp2.clear();
        corr.process(tmp.data(), tmp.size(), tmp2);
        if (mirror) for (auto& v : tmp2) v = std::conj(v);
        buf.insert(buf.end(), tmp2.begin(), tmp2.end());
        total += tmp2.size();
        inSamples += n; sincePub += n;
        run();
        // keep the stream bounded
        const uint64_t keepSearch = 120000;
        uint64_t keepFrom = total > keepSearch ? total - keepSearch : 0;
        if (phase == kTrack) {
            const int64_t need = nS + Dacc - 2 * Ts;
            keepFrom = std::min<uint64_t>(keepFrom, need > 0 ? (uint64_t)need : 0);
            if (total > 400000) keepFrom = std::max<uint64_t>(keepFrom, total - 400000);
        }
        if (keepFrom > base + 65536) {
            buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)(keepFrom - base));
            base = keepFrom;
        }
        if (sincePub >= (uint64_t)(inRate * 0.25)) publish();
    }

    void run() {
        if (phase == kSearch) {
            if (total - lastSearch >= 19200 && total - base >= 72000) {
                lastSearch = total;
                statSearchRuns++;
                const Acq a = acquire();
                if (a.ok) startTracking(a);
            }
        }
        if (phase == kTrack) { int guard = 0; while (phase == kTrack && trackStep() && ++guard < 4000) {} }
    }

    void publish() {
        sincePub = 0;
        DrmTelemetry t;
        t.levelDbfs = (float)front.levelDbfs();
        front.resetLevel();
        t.mode = phase == kTrack ? mode : (be.facValid() ? be.mode() : -1);
        if (t.mode >= 0) { t.modeName = modeParams(t.mode).name; }
        const bool tracking = phase == kTrack && frameSync;
        t.cfoHz = fHz;
        t.snrDb = tracking && sigma2 > 0 ? (float)std::max(0.0, std::min(60.0, snrSm)) : 0.f;
        t.correlation = (float)cpRho;
        t.sroPpm = (float)sroPpm;
        t.dopplerHz = (float)dopplerSm;
        t.delaySpreadMs = (float)delaySpreadMs;
        t.timingMs = (float)((tauRef + tauMidRel - back) / kFs * 1000.0);
        // multiplex description
        if (be.facValid()) {
            const FacInfo& f = be.fac();
            t.occupancy = f.occupancy;
            t.bandwidthKhz = (float)occupancyKhz(be.mode(), f.occupancy);
            t.longInterleave = f.interleaver == 0 ? 1 : 0;
            t.mscQam = f.mscMode == 0 ? 64 : f.mscMode == 3 ? 16 : 0;
            t.sdcQam = f.sdcMode == 0 ? 16 : 4;
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
            if (!fs && !(sd.audio[i].present || sd.label[i].present)) continue;
            DrmService s;
            s.shortId = i;
            if (fs) {
                s.id = fs->id; s.audio = !fs->data; s.language = fs->language; s.languageName = languageName(fs->language);
                s.programmeType = fs->descriptor; s.programmeName = fs->data ? "" : (fs->descriptor < 30 ? programmeTypeName(fs->descriptor) : "");
                s.conditionalAccess = fs->audioCa || fs->dataCa;
            }
            if (sd.label[i].present) s.label = sd.label[i].text;
            if (sd.lang[i].present) { s.country = sd.lang[i].country; if (!sd.lang[i].language.empty() && sd.lang[i].language[0] != '-') s.languageName = sd.lang[i].language; }
            if (sd.audio[i].present) {
                const SdcAudio& a = sd.audio[i];
                s.audio = true;
                s.audioCoding = a.coding; s.sbr = a.sbr != 0; s.audioMode = a.mode; s.audioRateHz = a.rateHz(); s.textMessage = a.text; s.streamId = a.streamId;
                if (a.streamId < sd.mux.nStreams) s.bitrateBps = (int)((sd.mux.stream[a.streamId].lenA + sd.mux.stream[a.streamId].lenB) * 8 / 0.4);
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
        const uint64_t recent = (uint64_t)(kFs * 2.0);
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
        {
            char b[200];
            if (t.mode < 0) snprintf(b, sizeof b, "searching (guard correlation A %.2f B %.2f C %.2f D %.2f)", lastRho[0], lastRho[1], lastRho[2], lastRho[3]);
            else if (!tracking) snprintf(b, sizeof b, "mode %c found, looking for the frame start", t.modeName);
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
void DrmReceiver::reset() { if (p_->ready) p_->resetAll(true); }
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
void DrmReceiver::setStreamCallback(std::function<void(int, const uint8_t*, int, int, const DrmTelemetry&)> cb) { p_->streamCb = std::move(cb); }

} // namespace dect2
