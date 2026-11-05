// FT8 / FT4 / FT2 / WSPR decoder (see dect2/hfdig/hfdig_ftx.h and hfdig_ftx_int.h for the protocols and their sources).
// Pipeline of one slot: the audio at a rate where a symbol is a power of two samples (FT8 6400 Hz, FT4 / FT2 10667 Hz, WSPR 12000 Hz)
// -> a spectrogram (one symbol per frame, frames every quarter symbol, bins of half the tone spacing) -> the sync score of every
// time / frequency (Costas arrays, WSPR's sync vector) -> the best candidates -> each cut out of one large FFT of the slot into a narrow
// complex baseband (WSJT-X's way) -> a fine fit of time and frequency on the sync symbols -> per-symbol tone amplitudes -> soft bits
// (non-coherent likelihoods) -> LDPC / CRC (FT) or deinterleaving and Fano (WSPR) -> the message; each decoded signal is rebuilt and
// subtracted from the slot, and the search runs again on what is left.
#include "dect2/common/thread_name.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/net/pskreporter.h"
#include "dect2/net/wsprnet.h"
#include "dect2/dsp/fftutil.h"
#include "hfdig_ftx_int.h"
#include "hfdig_wsjt_int.h"
#include "hfdig_usb.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace dect2 {

const char* ftxModeName(int m) {
    static const char* n[kFtxModes] = {"FT8", "FT4", "FT2", "WSPR", "JT65", "JT9", "Q65", "FST4", "MSK144", "FST4W", "JT4"};
    return m >= 0 && m < kFtxModes ? n[m] : "?";
}
double ftxPeriod(int m) { return ftx::spec(m).period; }

namespace ftx {

const Spec& spec(int m) {
    static const Spec s[kModes] = {
        {"FT8", 15.0, 0.5, 0.160, 6.25, 79, 8, 2.0},
        {"FT4", 7.5, 0.5, 0.048, 12000.0 / 576, 105, 4, 1.0},
        {"FT2", 3.75, 0.5, 0.024, 12000.0 / 288, 105, 4, 1.0},
        {"WSPR", 120.0, 1.0, 8192.0 / 12000, 12000.0 / 8192, 162, 4, 0.0},
        {"JT65", 60.0, 1.0, 4096.0 / 11025, 11025.0 / 4096, 126, 66, 0.0},
        {"JT9", 60.0, 1.0, 6912.0 / 12000, 12000.0 / 6912, 85, 9, 0.0},
        {"Q65", 60.0, 1.0, 7200.0 / 12000, 12000.0 / 7200, 85, 65, 0.0},
        {"FST4", 60.0, 1.0, 3888.0 / 12000, 12000.0 / 3888, 160, 4, 0.0},
        {"MSK144", 15.0, 0.0, 0.072, 2000.0, 208, 2, 0.0},
        {"FST4W", 120.0, 1.0, 8200.0 / 12000, 12000.0 / 8200, 160, 4, 0.0},
        {"JT4", 60.0, 1.0, 2520.0 / 11025, 11025.0 / 2520, 206, 4, 0.0},
    };
    return s[std::max(0, std::min(kModes - 1, m))];
}

// the timing of a mode for its sub-mode and period
Spec specCfg(int mode, int sub, double period) {
    Spec s = spec(mode);
    switch (mode) {
    case kJt65: s.spacing *= (double)(1 << std::max(0, std::min(2, sub))); break;
    case kJt4: s.spacing *= (double)(1 << std::max(0, std::min(6, sub))); break;
    case kQ65: {
        static const double per[5] = {15, 30, 60, 120, 300};
        static const int nsps[5] = {1800, 3600, 7200, 16000, 41472};
        int i = 2;
        for (int k = 0; k < 5; k++) if (std::fabs(per[k] - period) < 0.5) i = k;
        s.period = per[i]; s.symSec = nsps[i] / 12000.0; s.spacing = 12000.0 / nsps[i] * (double)(1 << std::max(0, std::min(4, sub)));
        s.t0 = i == 0 ? 0.5 : 1.0;
        break;
    }
    case kFst4: case kFst4w: {
        static const double per[7] = {15, 30, 60, 120, 300, 900, 1800};
        static const int nsps[7] = {720, 1680, 3888, 8200, 21504, 66560, 134400};
        int i = mode == kFst4 ? 2 : 3;
        for (int k = 0; k < 7; k++) if (std::fabs(per[k] - period) < 0.5) i = k;
        s.period = per[i]; s.symSec = nsps[i] / 12000.0; s.spacing = 12000.0 / nsps[i];
        s.t0 = i == 0 ? 0.5 : 1.0;
        break;
    }
    case kMsk144:
        s.period = period > 20 ? 30.0 : 15.0;
        s.nn = (int)std::lround(s.period / s.symSec);
        break;
    default: break;
    }
    return s;
}

double dtRange(int mode, double period) {
    switch (mode) {
    case kJt65: return 8.0;           // WSJT-X searches -3 .. +8 s for JT65 (a slow start, EME delays)
    case kJt9: return 4.0;
    case kJt4: return 5.5;            // WSJT-X sync4 searches -0.6 .. +6.7 s from the start of the buffer; the WSJT-X sample DF2ZC starts at 5.1 s
    case kQ65: return std::min(3.0, 0.1 * period);
    case kFst4: case kFst4w: return std::min(3.0, 0.1 * period);
    case kMsk144: return 0.0;
    default: return 2.5;
    }
}

double dtLow(int mode, double period) {   // the earliest start offset searched (as a positive number of seconds early)
    return mode == kJt65 || mode == kJt9 || mode == kJt4 ? 3.0 : dtRange(mode, period);
}

std::vector<int> ftxTones(int mode, const uint8_t* b77) {
    uint8_t m[77], cw[kN];
    for (int i = 0; i < 77; i++) m[i] = b77[i] & 1;
    if (mode == kFt8) {
        encode174(m, cw);
        std::vector<int> t(79);
        for (int k = 0; k < 7; k++) t[(size_t)k] = t[(size_t)36 + k] = t[(size_t)72 + k] = kFt8Costas[k];
        for (int j = 0; j < 58; j++) {
            const int v = cw[3 * j] * 4 + cw[3 * j + 1] * 2 + cw[3 * j + 2];
            t[(size_t)(j < 29 ? 7 + j : 43 + j - 29)] = kFt8Gray[v];
        }
        return t;
    }
    for (int i = 0; i < 77; i++) m[i] ^= kFt4Rvec[i];
    encode174(m, cw);
    std::vector<int> t(103);
    for (int k = 0; k < 4; k++) {
        t[(size_t)k] = kFt4Costas[0][k]; t[(size_t)33 + k] = kFt4Costas[1][k];
        t[(size_t)66 + k] = kFt4Costas[2][k]; t[(size_t)99 + k] = kFt4Costas[3][k];
    }
    for (int j = 0; j < 87; j++) {
        const int v = cw[2 * j] * 2 + cw[2 * j + 1];
        t[(size_t)(j < 29 ? 4 + j : j < 58 ? 37 + j - 29 : 70 + j - 58)] = kFt4Gray[v];
    }
    return t;
}

void addWave(int mode, const std::vector<int>& tones, double f0, double amp, double rate, double start, bool mirrored, double phase,
             std::vector<float>& out) {
    const Spec& s = spec(mode);
    const int lead = (mode == kFt4 || mode == kFt2) ? 1 : 0;
    const int nsym = (int)tones.size(), total = nsym + 2 * lead;
    const double T = s.symSec, sg = mirrored ? -1.0 : 1.0;
    auto ext = [&](int k) {   // the tone of transmission symbol k, the ends held
        int i = k - lead;
        i = std::max(0, std::min(nsym - 1, i));
        return (double)tones[(size_t)i];
    };
    const double c = hfdig::kPi * std::sqrt(2.0 / std::log(2.0));
    auto pulse = [&](double x) { return 0.5 * (std::erf(c * s.bt * (x + 0.5)) - std::erf(c * s.bt * (x - 0.5))); };
    const long n0 = std::max(0L, (long)std::ceil(start)), n1 = std::min((long)out.size(), (long)std::floor(start + total * T * rate));
    double ph = phase;
    // the phase from the transmission start to n0 is not needed (an arbitrary starting phase)
    for (long n = n0; n < n1; n++) {
        const double u = (n - start) / (T * rate);   // time in symbols since the start
        double dev;
        if (s.bt <= 0) dev = ext((int)std::floor(u));
        else {
            dev = 0;
            const int k0 = (int)std::floor(u);
            for (int k = k0 - 2; k <= k0 + 2; k++) dev += ext(k) * pulse(u - k - 0.5);
        }
        double env = 1;
        if (mode == kFt8) {
            const double r = 1.0 / 8;
            if (u < r) env = 0.5 * (1 - std::cos(hfdig::kPi * u / r));
            else if (u > total - r) env = 0.5 * (1 - std::cos(hfdig::kPi * (total - u) / r));
        } else if (lead) {
            if (u < 1) env = 0.5 * (1 - std::cos(hfdig::kPi * u));
            else if (u > total - 1) env = 0.5 * (1 - std::cos(hfdig::kPi * (total - u)));
        }
        out[(size_t)n] += (float)(amp * env * std::cos(ph));
        ph += 2 * hfdig::kPi * (f0 + sg * s.spacing * dev) / rate;
        if (ph > 1e4) ph = std::fmod(ph, 2 * hfdig::kPi);
    }
}

// The same waveform as addWave, as cos and sin of the phase in one pass (the subtraction of a decoded signal): the Gaussian pulse from a
// table with linear interpolation, one sincos a sample.
void addWaveIQ(int mode, const std::vector<int>& tones, double f0, double rate, double start, bool mirrored, std::vector<float>& re, std::vector<float>& im) {
    const Spec& s = spec(mode);
    const int lead = (mode == kFt4 || mode == kFt2) ? 1 : 0;
    const int nsym = (int)tones.size(), total = nsym + 2 * lead;
    const double T = s.symSec, sg = mirrored ? -1.0 : 1.0;
    constexpr int kTab = 1 << 13;
    constexpr double kLim = 3.0;
    static std::mutex mu;
    static std::map<double, std::vector<float>> tabs;
    const float* tab = nullptr;
    if (s.bt > 0) {
        std::lock_guard<std::mutex> lk(mu);
        auto& t = tabs[s.bt];
        if (t.empty()) {
            const double c = hfdig::kPi * std::sqrt(2.0 / std::log(2.0));
            t.resize(kTab + 2);
            for (int i = 0; i < kTab + 2; i++) {
                const double x = -kLim + 2 * kLim * i / kTab;
                t[(size_t)i] = (float)(0.5 * (std::erf(c * s.bt * (x + 0.5)) - std::erf(c * s.bt * (x - 0.5))));
            }
        }
        tab = t.data();
    }
    auto pulse = [&](double x) {
        const double q = (x + kLim) * (kTab / (2 * kLim));
        const int i = (int)q;
        const float f = (float)(q - i);
        return tab[i] + f * (tab[i + 1] - tab[i]);
    };
    std::vector<double> tv((size_t)nsym + 8);   // tone of transmission symbol k, the ends held, at index k + 2
    for (int k = -2; k < nsym + 6; k++) tv[(size_t)(k + 2)] = (double)tones[(size_t)std::max(0, std::min(nsym - 1, k - lead))];
    const long n0 = std::max(0L, (long)std::ceil(start)), n1 = std::min((long)re.size(), (long)std::floor(start + total * T * rate));
    double ph = 0;
    const double invTr = 1.0 / (T * rate);
    for (long n = n0; n < n1; n++) {
        const double u = (n - start) * invTr;
        const int k0 = (int)std::floor(u);
        double dev;
        if (s.bt <= 0) dev = tv[(size_t)(k0 + 2)];
        else {
            dev = 0;
            for (int k = k0 - 2; k <= k0 + 2; k++) dev += tv[(size_t)(k + 2)] * pulse(u - k - 0.5);
        }
        double env = 1;
        if (mode == kFt8) {
            const double r = 1.0 / 8;
            if (u < r) env = 0.5 * (1 - std::cos(hfdig::kPi * u / r));
            else if (u > total - r) env = 0.5 * (1 - std::cos(hfdig::kPi * (total - u) / r));
        } else if (lead) {
            if (u < 1) env = 0.5 * (1 - std::cos(hfdig::kPi * u));
            else if (u > total - 1) env = 0.5 * (1 - std::cos(hfdig::kPi * (total - u)));
        }
        const double sn = std::sin(ph), cs = std::cos(ph);
        re[(size_t)n] += (float)(env * cs);
        im[(size_t)n] += (float)(env * sn);
        ph += 2 * hfdig::kPi * (f0 + sg * s.spacing * dev) / rate;
        if (ph > 1e4) ph = std::fmod(ph, 2 * hfdig::kPi);
    }
}

} // namespace ftx

bool ftxGridToLatLon(const std::string& grid, double& lat, double& lon) { return ftx::gridToLatLon(grid, lat, lon); }

bool ftxAddTransmission(int mode, const std::string& msg, double hz, double amp, double rate, double startSec, bool mirrored,
                        std::vector<float>& out) {
    return ftxAddTransmissionEx(mode, msg, hz, amp, rate, startSec, mirrored, 0, 0, out);
}

bool ftxAddTransmissionEx(int mode, const std::string& msg, double hz, double amp, double rate, double startSec, bool mirrored, int sub,
                          double period, std::vector<float>& out) {
    using namespace ftx;
    if (mode >= kJt65) {
        const Spec sp = specCfg(mode, sub, period);
        const double t = startSec;
        switch (mode) {
        case kJt65: { const auto tn = wsjt::jt65Tones(msg); if (tn.empty()) return false; wsjt::addJt65(tn, sub, hz, amp, rate, t, out); return true; }
        case kJt9: { const auto tn = wsjt::jt9Tones(msg); if (tn.empty()) return false; wsjt::addJt9(tn, hz, amp, rate, t, out); return true; }
        case kJt4: { const auto tn = wsjt::jt4Tones(msg); if (tn.empty()) return false; wsjt::addJt4(tn, sub, hz, amp, rate, t, out); return true; }
        case kQ65: { const auto tn = wsjt::q65Tones(msg); if (tn.empty()) return false; wsjt::addQ65(tn, sub, sp.period, hz, amp, rate, t, out); return true; }
        case kFst4: case kFst4w: { const auto tn = wsjt::fst4Tones(msg, mode == kFst4w); if (tn.empty()) return false; wsjt::addFst4(tn, sp.period, hz, amp, rate, t, out); return true; }
        case kMsk144: {
            uint8_t b[144];
            if (!wsjt::msk144Frame(msg, b)) return false;
            wsjt::addMsk144(b, hz, amp, rate, t, sp.period, out);
            return true;
        }
        default: return false;
        }
    }
    (void)mirrored;
    std::vector<int> tones;
    if (mode == kWspr) {
        if (!wsprSymbols(msg, tones)) return false;
    } else {
        uint8_t b[77];
        if (!pack77(msg, b)) return false;
        tones = ftxTones(mode, b);
    }
    addWave(mode, tones, hz, amp, rate, startSec * rate, mirrored, 0.0, out);
    return true;
}

namespace {

using namespace ftx;
using cd = std::complex<double>;

double nowSystemUtc() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- 8 kHz -> the mode rates (polyphase, L / M)
class Resampler {
public:
    Resampler(int L, int M, double cutHz) : L_(L), M_(M) {
        const auto h = hfdig::lowpass(L * kP, cutHz / (L * 8000.0), 8.0);
        h_.resize((size_t)L * kP);
        for (int i = 0; i < L * kP; i++) h_[(size_t)i] = (float)(h[(size_t)i] * L);
        hist_.assign(2 * kP, 0.f);
    }
    void reset() { std::fill(hist_.begin(), hist_.end(), 0.f); pos_ = 0; in_ = 0; out_ = 0; }
    // the input sample (8 kHz, fractional) at the centre of output sample j
    double inputPos(double j) const { return (j * M_ - (L_ * kP - 1) * 0.5) / L_; }
    double outputPos(double k) const { return (k * L_ + (L_ * kP - 1) * 0.5) / M_; }
    int64_t produced() const { return out_; }
    void process(const float* x, size_t n, std::vector<float>& out) {
        for (size_t i = 0; i < n; i++) {
            hist_[(size_t)pos_] = x[i]; hist_[(size_t)(pos_ + kP)] = x[i];
            if (++pos_ >= kP) pos_ = 0;
            in_++;
            // outputs whose newest input is this one
            for (;;) {
                const int64_t u = out_ * M_;
                const int64_t last = u / L_;
                if (last > in_ - 1) break;
                const int ph = (int)(u % L_);
                const float* w = &hist_[(size_t)pos_];   // oldest first; w[kP - 1] = x[in_ - 1]
                const int64_t back = (in_ - 1) - last;    // 0 normally
                float acc = 0;
                for (int k = 0; k < kP - back; k++) acc += h_[(size_t)(ph + k * L_)] * w[kP - 1 - back - k];
                out.push_back(acc);
                out_++;
            }
        }
    }
private:
    static constexpr int kP = 48;
    int L_, M_;
    std::vector<float> h_, hist_;
    int pos_ = 0;
    int64_t in_ = 0, out_ = 0;
};

// the streams: 0 FT8 (6400 Hz), 1 FT4 and FT2 (10667 Hz), 2 WSPR (12000 Hz), 3 the others (8000 Hz, resampled to their own rates when decoding)
constexpr int kStreams = 4;
const int kStreamL[kStreams] = {4, 4, 3, 1}, kStreamM[kStreams] = {5, 3, 2, 1};
const double kStreamCut[kStreams] = {3080, 3900, 3900, 3900};
int streamOf(int mode) { return mode == kFt8 ? 0 : mode == kWspr ? 2 : mode >= kJt65 ? 3 : 1; }   // stream 3: the 8 kHz audio for the modes of hfdig_wsjt_int.h
double streamRate(int s) { return 8000.0 * kStreamL[s] / kStreamM[s]; }

struct Layout {
    int nsps = 0, ndown = 0, nt = 0, nsym = 0, bits = 0;   // nsym: symbols in the transmission (with ramps)
    double rate = 0, sp = 0;
    std::vector<std::pair<int, int>> sync;   // (transmission symbol, tone)
    std::vector<int> data;                    // transmission symbols carrying data, in order
    int toneOff = 0;                          // transmission symbol of tone index 0
    double dtMax = 2.5;
    int maxCand = 100, passes = 2;
};

// FT8 / FT4 / FT2 decoding depth (the HFDIG_FTX_* variables are for measuring only)
static double envOr(const char* n, double v) { const char* e = std::getenv(n); return e ? std::atof(e) : v; }
static const bool kFtxCoherent = envOr("HFDIG_FTX_COH", 1) != 0;
static const bool kFtxOsd = envOr("HFDIG_FTX_OSD", 1) != 0;
static const float kSyncMin = (float)envOr("HFDIG_FTX_SYNCMIN", 1.45);
static const double kCandMul = envOr("HFDIG_FTX_CANDMUL", 1);
static const long kWsprFanoLimit = (long)envOr("HFDIG_WSPR_FANO", 20000.0 * 81);
static const int kFtxNudges = (int)envOr("HFDIG_FTX_NUDGES", 5);
static const double kFtxNudgeStep = envOr("HFDIG_FTX_NUDGESTEP", 1.0 / 48);
static const bool kOsdNudged = envOr("HFDIG_FTX_OSDNUDGE", 0) != 0;
static const bool kFtxApCq = envOr("HFDIG_FTX_APCQ", 1) != 0;
static const bool kNudgeDbg = envOr("HFDIG_FTX_NUDDBG", 0) != 0;
static const bool kOsdDebug = envOr("HFDIG_FTX_OSDDBG", 0) != 0;
static const int kOsdPairs = (int)envOr("HFDIG_FTX_OSDPAIRS", 40);
static const float kOsdMinScore = (float)envOr("HFDIG_FTX_OSDSCORE", 2.5);
static const double kOsdMaxDist = envOr("HFDIG_FTX_OSDDIST", 1.0), kOsdMinMargin = envOr("HFDIG_FTX_OSDMARGIN", 0.0);

Layout layoutOf(int mode) {
    Layout l;
    const Spec& s = spec(mode);
    if (mode >= kJt65) { l.rate = 8000; return l; }
    l.rate = streamRate(streamOf(mode));
    l.sp = s.spacing;
    l.nt = s.tones;
    l.nsym = s.nn;
    if (mode == kFt8) {
        l.nsps = 1024; l.ndown = 32; l.bits = 3; l.maxCand = 150; l.passes = (int)envOr("HFDIG_FTX_PASSES", 3);
        for (int g : {0, 36, 72}) for (int k = 0; k < 7; k++) l.sync.push_back({g + k, kFt8Costas[k]});
        for (int k = 7; k < 36; k++) l.data.push_back(k);
        for (int k = 43; k < 72; k++) l.data.push_back(k);
    } else if (mode == kFt4 || mode == kFt2) {
        l.nsps = mode == kFt4 ? 512 : 256; l.ndown = mode == kFt4 ? 32 : 16; l.bits = 2; l.toneOff = 1;
        l.maxCand = mode == kFt4 ? 100 : 60; l.passes = 2;
        if (mode == kFt2) l.dtMax = 1.0;
        const int g[4] = {0, 33, 66, 99};
        for (int a = 0; a < 4; a++) for (int k = 0; k < 4; k++) l.sync.push_back({g[a] + k + 1, kFt4Costas[a][k]});
        for (int k = 4; k < 33; k++) l.data.push_back(k + 1);
        for (int k = 37; k < 66; k++) l.data.push_back(k + 1);
        for (int k = 70; k < 99; k++) l.data.push_back(k + 1);
    } else {
        l.nsps = 8192; l.ndown = 256; l.bits = 1; l.maxCand = 40; l.passes = 2;
        for (int k = 0; k < 162; k++) { l.sync.push_back({k, kWsprSync[k]}); l.data.push_back(k); }
    }
    return l;
}

double logI0(double x) {   // log of the modified Bessel function I0
    x = std::fabs(x);
    if (x < 3.75) {
        const double t = (x / 3.75) * (x / 3.75);
        return std::log(1 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    const double t = 3.75 / x;
    const double p = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706 +
                     t * (0.02635537 + t * (-0.01647633 + t * 0.00392377)))))));
    return x - 0.5 * std::log(x) + std::log(p);
}

// "K1ABC FN42 37", "PJ4/K1ABC 30", "<K1ABC> FN42AX 37" -> the call and the grid
void wsprCallGrid(const std::string& msg, std::string& call, std::string& grid) {
    call.clear(); grid.clear();
    const size_t a = msg.find(' '), b = msg.rfind(' ');
    if (a == std::string::npos) return;
    call = msg.substr(0, a);
    if (call.size() > 2 && call.front() == '<') call = call.substr(1, call.size() - 2);
    if (call == "...") call.clear();
    if (b > a) grid = msg.substr(a + 1, b - a - 1);
}

struct Job {
    int mode = 0;
    double slotUtc = 0;
    double x0 = 0;            // time of x[0] against the start of the slot, s
    std::vector<float> x;
    double dtMin = -2.5, dtMax = 2.5;
    bool search = false;
    int sub = 0;
    double period = 60;
};

struct Result {
    std::vector<FtxDecode> dec;
    double ms = 0;
};

// ---------------------------------------------------------------- one slot of one mode
class SlotDecoder {
public:
    SlotDecoder(const Job& j, CallHash& hash, std::map<uint32_t, std::string>& wsprHash, bool mirrored)
        : j_(j), L_(layoutOf(j.mode)), hash_(hash), wsprHash_(wsprHash), mirrored_(mirrored) {}

    std::vector<FtxDecode> run() {
        x_ = j_.x;
        std::vector<FtxDecode> out;
        std::set<std::string> seen;
        // the plain decoder's passes first, then (FT8 / FT4 / FT2) the same passes again with the deeper metrics (coherent symbols, OSD,
        // a-priori) on what is left: so the deep stage never changes what the plain one decodes and subtracts
        for (int stage = 0; stage < (deepOn() ? 2 : 1); stage++) {
            deep_ = stage == 1;
            for (int pass = 0; pass < L_.passes; pass++) {
                bigFft();
                spectrogram();
                toneSums();
                int found = 0;
                for (int mir = 0; mir < (mirrored_ ? 2 : 1); mir++) {
                    const auto cands = candidates(mir != 0);
                    for (const auto& c : cands) {
                        if (deep_ && nearDecoded(c, mir != 0, out)) continue;   // what is left of a decoded signal: not worth the deep search
                        FtxDecode d;
                        std::vector<int> tones;
                        double f0, start;
                        bool ok = false;
                        // deep stage: the fitted frequency first, then nudged by fractions of the spacing (the sync fit is noisy at the
                        // threshold, and the two- to four-symbol metrics lose much to a small frequency error)
                        for (int k = 0; k < (deep_ && L_.nt == 4 ? kFtxNudges : 1) && !ok; k++) {   // FT4 / FT2: 16 sync symbols only
                            nudge_ = k == 0 ? 0.0 : ((k + 1) / 2) * kFtxNudgeStep * (k % 2 ? 1 : -1);
                            ok = tryCandidate(c, mir != 0, d, tones, f0, start);
                            if (kNudgeDbg) fprintf(stderr, "NUD k=%d err=%d score=%.2f ok=%d\n", k, lastErr_, c.score, (int)ok);
                        }
                        nudge_ = 0;
                        if (!ok) continue;
                        bool same = seen.count(d.msg) > 0;
                        for (const auto& o : out)   // the rest of a signal already decoded (its subtraction is not perfect)
                            same |= o.mirrored == d.mirrored && std::fabs(o.hz - d.hz) < 1.5 * L_.sp && std::fabs(o.dt - d.dt) < 2 * spec(j_.mode).symSec;
                        if (same) { subtract(tones, f0, start, mir != 0); continue; }
                        seen.insert(d.msg);
                        d.pass = pass + stage * L_.passes;
                        out.push_back(d);
                        raw_.push_back(lastRaw_);
                        found++;
                        subtract(tones, f0, start, mir != 0);
                    }
                }
                if (!found && (stage == 0 || pass > 0)) break;   // the deep stage always looks at least once
            }
        }
        // hashed calls heard later in the slot
        if (j_.mode == kWspr)
            for (size_t i = 0; i < out.size(); i++)
                if (out[i].msg.compare(0, 5, "<...>") == 0) {
                    const std::string m = wsprUnpack(raw_[i].data(), wsprHash_, nullptr);
                    if (!m.empty()) { out[i].msg = m; wsprCallGrid(m, out[i].call, out[i].grid); }
                }
        return out;
    }

private:
    struct Cand { int frame; int bin; float score; };
    bool deep_ = false;
    double nudge_ = 0;    // frequency offset of a deep retry, in tone spacings   // the second stage: the coherent metrics, OSD and a-priori decoding
    bool nearDecoded(const Cand& c, bool mir, const std::vector<FtxDecode>& out) const {
        const double hz = c.bin * binHz_, dt = j_.x0 + (double)c.frame * (L_.nsps / 4) / L_.rate - spec(j_.mode).t0;
        for (const auto& o : out)
            if (o.mirrored == mir && std::fabs(o.hz - hz) < 0.75 * L_.sp && std::fabs(o.dt - dt) < 1.5 * spec(j_.mode).symSec) return true;
        return false;
    }
    bool deepOn() const { return j_.mode != kWspr && (kFtxCoherent || kFtxOsd || kFtxApCq); }

    // the large FFT of the whole slot, kept for cutting out the candidates
    void bigFft() {
        size_t n = 1;
        while (n < x_.size()) n <<= 1;
        nBig_ = (int)n;
        X_.assign(n, cf32(0.f, 0.f));
        for (size_t i = 0; i < x_.size(); i++) X_[i] = cf32(x_[i], 0.f);
        Fft f((int)n);
        f.forward(X_.data());
        df_ = L_.rate / nBig_;
    }

    void spectrogram() {
        const int nfft = 2 * L_.nsps, hop = L_.nsps / 4;
        binHz_ = L_.rate / nfft;   // half the tone spacing
        bLo_ = (int)std::floor(150.0 / binHz_);
        bHi_ = (int)std::ceil((3000.0 + L_.nt * L_.sp) / binHz_);
        nb_ = bHi_ - bLo_ + 1;
        nf_ = x_.size() >= (size_t)L_.nsps ? (int)((x_.size() - (size_t)L_.nsps) / (size_t)hop) + 1 : 0;
        P_.assign((size_t)nf_ * nb_, 0.f);
        Fft f(nfft);
        std::vector<cf32> buf((size_t)nfft);
        for (int fr = 0; fr < nf_; fr++) {
            std::fill(buf.begin(), buf.end(), cf32(0.f, 0.f));
            for (int i = 0; i < L_.nsps; i++) buf[(size_t)i] = cf32(x_[(size_t)fr * hop + i], 0.f);
            f.forward(buf.data());
            for (int b = 0; b < nb_; b++) P_[(size_t)fr * nb_ + b] = std::norm(buf[(size_t)(bLo_ + b)]);
        }
    }
    void toneSums() {   // A_[m][f][b] = sum over the tones t of P(f, b + s t), s = +-2 (upright / mirrored)
        for (int m = 0; m < 2; m++) {
            A_[m].assign((size_t)nf_ * nb_, 0.f);
            if (j_.mode == kWspr) continue;
            const int st = m ? -2 : 2;
            for (int f = 0; f < nf_; f++) {
                const float* row = &P_[(size_t)f * nb_];
                float* out = &A_[m][(size_t)f * nb_];
                for (int b = 0; b < nb_; b++) {
                    float a = 0;
                    for (int t = 0; t < L_.nt; t++) { const int bb = b + st * t; if (bb >= 0 && bb < nb_) a += row[bb]; }
                    out[b] = a;
                }
            }
        }
    }
    float P(int fr, int b) const { return (fr < 0 || fr >= nf_ || b < 0 || b >= nb_) ? 0.f : P_[(size_t)fr * nb_ + b]; }

    // sync score of a transmission starting at frame fr with tone 0 at bin b
    float score(int fr, int b, bool mir) const {
        const int s = mir ? -2 : 2;
        if (j_.mode == kWspr) {
            double good = 0, all = 0;
            for (const auto& sy : L_.sync) {
                const int f = fr + 4 * sy.first;
                if (f >= nf_) break;
                const double p0 = P(f, b), p1 = P(f, b + s), p2 = P(f, b + 2 * s), p3 = P(f, b + 3 * s);
                const double on = sy.second ? p1 + p3 : p0 + p2, off = sy.second ? p0 + p2 : p1 + p3;
                good += on - off; all += on + off;
            }
            return all > 0 ? (float)(good / all) : 0.f;
        }
        double syn = 0, all = 0;
        int n = 0;
        const std::vector<float>& A = A_[mir ? 1 : 0];   // the sum over the tones, per frame and bin (filled in spectrogram())
        for (const auto& sy : L_.sync) {
            const int f = fr + 4 * sy.first;
            if (f >= nf_) continue;
            all += (b < 0 || b >= nb_) ? 0.f : A[(size_t)f * nb_ + b];
            syn += P(f, b + s * sy.second);
            n++;
        }
        if (n < (int)L_.sync.size() / 2 || all <= syn) return 0.f;
        return (float)(syn / ((all - syn) / (L_.nt - 1)));
    }

    std::vector<Cand> candidates(bool mir) const {
        const int hop = L_.nsps / 4;
        const Spec& sp = spec(j_.mode);
        const double tNom = sp.t0 - j_.x0;   // nominal start, seconds into x
        const int f0 = std::max(0, (int)std::floor((tNom + j_.dtMin) * L_.rate / hop));
        const int f1 = std::min(nf_ - 1, (int)std::ceil((tNom + j_.dtMax) * L_.rate / hop));
        const double width = (L_.nt - 1) * L_.sp;
        const double lowHz = mir ? 200 + width : 200, highHz = 3000;   // tone 0 anywhere in 200 .. 3000 Hz (mirrored: all tones above 200)
        const int b0 = std::max(0, (int)std::ceil(lowHz / binHz_) - bLo_), b1 = std::min(nb_ - 1, (int)std::floor(highHz / binHz_) - bLo_);
        if (f1 < f0 || b1 < b0) return {};
        std::vector<float> best((size_t)(b1 - b0 + 1), 0.f);
        std::vector<int> bestF((size_t)(b1 - b0 + 1), f0);
        for (int b = b0; b <= b1; b++)
            for (int f = f0; f <= f1; f++) {
                const float s = score(f, b, mir);
                if (s > best[(size_t)(b - b0)]) { best[(size_t)(b - b0)] = s; bestF[(size_t)(b - b0)] = f; }
            }
        std::vector<float> srt = best;
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        const float med = srt[srt.size() / 2];
        const float thr = j_.mode == kWspr ? std::max(0.12f, med + 0.08f) : std::max(kSyncMin, med * 1.15f);
        std::vector<Cand> c;
        for (int b = b0; b <= b1; b++) {
            const float s = best[(size_t)(b - b0)];
            if (s < thr) continue;
            bool peak = true;
            for (int d = -2; d <= 2 && peak; d++) {
                if (!d || b + d < b0 || b + d > b1) continue;
                const float o = best[(size_t)(b + d - b0)];
                if (o > s || (o == s && d < 0)) peak = false;
            }
            if (peak) c.push_back({bestF[(size_t)(b - b0)], b + bLo_, s});
        }
        std::sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        if ((int)c.size() > L_.maxCand * kCandMul) c.resize((size_t)(L_.maxCand * kCandMul));
        return c;
    }

    // the complex baseband around tone 0 at f0 (mirrored: conjugated, so that the tones go up), at rate / ndown
    void baseband(double f0, bool mir, std::vector<cf32>& y, int& kc) const {
        const double s = mir ? -1 : 1;
        const double fc = f0 + s * (L_.nt - 1) * 0.5 * L_.sp;
        kc = (int)std::lround(fc / df_);
        const int n2 = nBig_ / L_.ndown;
        y.assign((size_t)n2, cf32(0.f, 0.f));
        const double w1 = (L_.nt * 0.5 + 1.0) * L_.sp, w2 = w1 + 1.5 * L_.sp;
        for (int j = -n2 / 2; j < n2 / 2; j++) {
            const int k = kc + j;
            if (k < 0 || k >= nBig_ / 2) continue;
            const double f = std::fabs(j * df_);
            if (f >= w2) continue;
            const double tap = f <= w1 ? 1.0 : 0.5 * (1 + std::cos(hfdig::kPi * (f - w1) / (w2 - w1)));
            y[(size_t)((j + n2) % n2)] = X_[(size_t)k] * (float)tap;
        }
        Fft f(n2);
        f.inverse(y.data());
        const float g = 1.f / (float)nBig_;
        for (auto& v : y) v = (mir ? std::conj(v) : v) * g;
    }

    // tone correlations of one symbol starting at baseband sample i
    void symbol(const std::vector<cf32>& y, int i, const std::vector<cd>& w, int nspsb, cd* z) const {
        for (int c = 0; c < L_.nt; c++) z[c] = 0;
        for (int n = 0; n < nspsb; n++) {
            const int k = i + n;
            if (k < 0 || k >= (int)y.size()) continue;
            const cd v(y[(size_t)k].real(), y[(size_t)k].imag());
            for (int c = 0; c < L_.nt; c++) z[c] += v * w[(size_t)(c * nspsb + n)];
        }
    }

    void twiddles(double base, double dfr, int nspsb, double fsb, std::vector<cd>& w) const {
        w.resize((size_t)(L_.nt * nspsb));
        for (int c = 0; c < L_.nt; c++)
            for (int n = 0; n < nspsb; n++) {
                const double ph = -2 * hfdig::kPi * (c * L_.sp + base + dfr) * n / fsb;
                w[(size_t)(c * nspsb + n)] = cd(std::cos(ph), std::sin(ph));
            }
    }

    double syncMetric(const std::vector<cf32>& y, int tt, const std::vector<cd>& w, int nspsb) const {
        cd z[8];
        double m = 0;
        for (const auto& sy : L_.sync) {
            symbol(y, tt + sy.first * nspsb, w, nspsb, z);
            if (j_.mode == kWspr) {
                const int on0 = sy.second, on1 = sy.second + 2, of0 = 1 - sy.second, of1 = 3 - sy.second;
                m += std::norm(z[on0]) + std::norm(z[on1]) - std::norm(z[of0]) - std::norm(z[of1]);
            } else m += std::norm(z[sy.second]);
        }
        return m;
    }

    // LLRs of the data bits from groups of n neighbouring data symbols whose tone correlations are added as complex numbers (WSJT-X's nsym 2 / 3
    // metrics: max |sum| over the tone sequences with the bit 1 minus that with the bit 0), scaled to WSJT-X's spread
    void coherentLlr(int n, float* out) const {
        const uint8_t* gray = L_.nt == 8 ? kFt8Gray : kFt4Gray;
        const int nd = (int)L_.data.size();
        int bi = 0;
        double sum = 0, sum2 = 0;
        for (int i = 0; i < nd;) {
            int g = 1;
            while (g < n && i + g < nd && L_.data[(size_t)(i + g)] == L_.data[(size_t)(i + g - 1)] + 1) g++;
            const int nb = g * L_.bits, combos = 1 << nb;
            double m1[12], m0[12];
            for (int b = 0; b < nb; b++) m1[b] = m0[b] = -1e300;
            for (int cmb = 0; cmb < combos; cmb++) {
                cd acc = 0;
                for (int q = 0; q < g; q++) {
                    const int v = (cmb >> ((g - 1 - q) * L_.bits)) & (L_.nt - 1);
                    acc += zc_[(size_t)L_.data[(size_t)(i + q)] * L_.nt + gray[v]];
                }
                const double m = std::sqrt(std::norm(acc));
                for (int b = 0; b < nb; b++) { double& r = ((cmb >> (nb - 1 - b)) & 1) ? m1[b] : m0[b]; if (m > r) r = m; }
            }
            for (int b = 0; b < nb; b++) { const double l = m1[b] - m0[b]; out[bi++] = (float)l; sum += l; sum2 += l * l; }
            i += g;
        }
        const double mean = sum / bi, sd = std::sqrt(std::max(1e-30, sum2 / bi - mean * mean));
        for (int i = 0; i < bi; i++) out[i] = (float)std::max(-25.0, std::min(25.0, 2.83 * out[i] / sd));
    }

    bool tryCandidate(const Cand& c, bool mir, FtxDecode& d, std::vector<int>& tones, double& f0out, double& startOut) {
        const double s = mir ? -1 : 1;
        const double f0 = c.bin * binHz_;
        const int nspsb = L_.nsps / L_.ndown;
        const double fsb = L_.rate / L_.ndown;
        // the fit of a candidate (baseband, time, frequency) is kept for the deep stage's frequency retries (nudge_ != 0)
        if (nudge_ == 0 || !fit_.valid) {
            baseband(f0, mir, fit_.y, fit_.kc);
            fit_.base = s * (f0 - fit_.kc * df_);
            const std::vector<cf32>& y = fit_.y;
            const double base = fit_.base;
            const int tc = c.frame * (L_.nsps / 4) / L_.ndown;
            // fine search by coordinate descent: time over +- half a symbol at the middle frequency, frequency in steps of a sixteenth of the
            // spacing over +- half of it at that time, then the time again around the best frequency
            const int nfq = 9;
            std::vector<std::vector<cd>> W((size_t)nfq);
            auto Wq = [&](int q) -> std::vector<cd>& {
                if (W[(size_t)q].empty()) twiddles(base, (q - nfq / 2) * L_.sp / 16.0, nspsb, fsb, W[(size_t)q]);
                return W[(size_t)q];
            };
            double bestM = -1e300;
            int bestT = tc, bestF = nfq / 2;
            for (int dt = -nspsb / 2; dt <= nspsb / 2; dt++) {
                const double m = syncMetric(y, tc + dt, Wq(nfq / 2), nspsb);
                if (m > bestM) { bestM = m; bestT = tc + dt; }
            }
            for (int q = 0; q < nfq; q++) {
                if (q == nfq / 2) continue;
                const double m = syncMetric(y, bestT, Wq(q), nspsb);
                if (m > bestM) { bestM = m; bestF = q; }
            }
            if (bestF != nfq / 2) {
                const int t0 = bestT;
                for (int dt = -3; dt <= 3; dt++) {
                    if (!dt) continue;
                    const double m = syncMetric(y, t0 + dt, Wq(bestF), nspsb);
                    if (m > bestM) { bestM = m; bestT = t0 + dt; }
                }
            }
            // finer frequency around the best (twice as fine), then a parabola in time and frequency
            double dfr = (bestF - nfq / 2) * L_.sp / 16.0;
            {
                std::vector<cd> w;
                double mm[3];
                for (int k = -1; k <= 1; k++) { twiddles(base, dfr + k * L_.sp / 32.0, nspsb, fsb, w); mm[k + 1] = syncMetric(y, bestT, w, nspsb); }
                int kb = 1;
                if (mm[0] > mm[kb]) kb = 0;
                if (mm[2] > mm[kb]) kb = 2;
                dfr += (kb - 1) * L_.sp / 32.0;
                if (kb == 1) {
                    const double den = mm[0] - 2 * mm[1] + mm[2];
                    if (den < 0) dfr += 0.5 * (mm[0] - mm[2]) / den * L_.sp / 32.0;
                }
            }
            std::vector<cd> w;
            twiddles(base, dfr, nspsb, fsb, w);
            double tfrac = 0;
            {
                const double a = syncMetric(y, bestT - 1, w, nspsb), b = syncMetric(y, bestT, w, nspsb), cc = syncMetric(y, bestT + 1, w, nspsb);
                const double den = a - 2 * b + cc;
                if (den < 0 && b >= a && b >= cc) tfrac = std::max(-0.5, std::min(0.5, 0.5 * (a - cc) / den));
            }
            fit_.valid = true; fit_.bestT = bestT; fit_.dfr = dfr; fit_.tfrac = tfrac;
        }
        const std::vector<cf32>& y = fit_.y;
        const double base = fit_.base;
        const int bestT = fit_.bestT;
        const double tfrac = fit_.tfrac;
        const double dfr = fit_.dfr + nudge_ * L_.sp;   // the deep stage's retries a little off the fitted frequency (the coherent metrics need it close)
        std::vector<cd> w;
        twiddles(base, dfr, nspsb, fsb, w);
        // tone amplitudes of every symbol
        const int ns = L_.nsym;
        std::vector<double> amp((size_t)ns * L_.nt);
        zc_.assign((size_t)ns * L_.nt, cd(0, 0));
        cd z[8];
        // the phase of a continuous-phase transmission runs on by 2 pi (base + dfr) T per symbol whatever the tone: taken out, so that the tone
        // correlations of neighbouring symbols add up coherently
        const cd rot = std::polar(1.0, -2 * hfdig::kPi * (base + dfr) * nspsb / fsb);
        cd ph(1, 0);
        for (int k = 0; k < ns; k++) {
            symbol(y, bestT + k * nspsb, w, nspsb, z);
            for (int t = 0; t < L_.nt; t++) { amp[(size_t)k * L_.nt + t] = std::sqrt(std::norm(z[t])); zc_[(size_t)k * L_.nt + t] = z[t] * ph; }
            ph *= rot;
        }
        // noise and signal per bin from the data symbols: the strongest tone of each against the rest
        double sMax = 0, sRest = 0;
        for (int k : L_.data) {
            double mx = 0, sum = 0;
            for (int t = 0; t < L_.nt; t++) { const double p = amp[(size_t)k * L_.nt + t] * amp[(size_t)k * L_.nt + t]; mx = std::max(mx, p); sum += p; }
            sMax += mx; sRest += sum - mx;
        }
        const int nd = (int)L_.data.size();
        double sigma2 = std::max(1e-30, sRest / ((double)nd * (L_.nt - 1)));
        if (j_.mode == kWspr) {   // the two tones of the other sync parity carry no signal: the noise
            double nn = 0;
            for (const auto& sy : L_.sync) {
                const int k = sy.first;
                const double a0 = amp[(size_t)k * 4 + (1 - sy.second)], a1 = amp[(size_t)k * 4 + (3 - sy.second)];
                nn += a0 * a0 + a1 * a1;
            }
            sigma2 = std::max(1e-30, nn / (2.0 * 162));
        }
        const double A2 = std::max(0.05 * sigma2, sMax / nd - sigma2);
        const double A = std::sqrt(A2);
        const double kx = 2 * A / sigma2;
        d = FtxDecode();
        d.mode = j_.mode;
        d.slotUtc = j_.slotUtc;
        d.mirrored = mir;
        const double startSec = j_.x0 + (bestT + tfrac) * L_.ndown / L_.rate;
        d.dt = (float)(startSec - spec(j_.mode).t0);
        const double f0fine = f0 + s * dfr;
        d.hz = (float)f0fine;
        if (j_.mode == kWspr) {
            float llr[162], dl[162];
            for (int k = 0; k < 162; k++) {
                const int sy = kWsprSync[k];
                const double a0 = amp[(size_t)k * 4 + sy], a1 = amp[(size_t)k * 4 + sy + 2];
                llr[k] = (float)(logI0(kx * a1) - logI0(kx * a0));
            }
            wsprDeinterleave(llr, dl);
            uint8_t data[7];
            if (!wsprFano(dl, data, kWsprFanoLimit)) return false;
            int dbm = -1;
            const std::string msg = wsprUnpack(data, wsprHash_, &dbm);
            if (msg.empty()) return false;
            if (!wsprSymbols(msg, tones)) {   // a type 3 message with an unknown hash: rebuild from the bits
                uint8_t coded[162], ch[162];
                wsprConvEncode(data, coded);
                wsprInterleave(coded, ch);
                tones.resize(162);
                for (int k = 0; k < 162; k++) tones[(size_t)k] = kWsprSync[k] + 2 * ch[k];
            }
            d.msg = msg;
            d.dbm = dbm;
            std::copy(data, data + 7, lastRaw_.begin());
        } else {
            float llr[kN];
            int bi = 0;
            const uint8_t* gray = L_.nt == 8 ? kFt8Gray : kFt4Gray;
            for (int k : L_.data) {
                double li[8];
                for (int t = 0; t < L_.nt; t++) li[t] = logI0(kx * amp[(size_t)k * L_.nt + t]);
                for (int b = 0; b < L_.bits; b++) {
                    double m1 = -1e300, m0 = -1e300;
                    for (int v = 0; v < L_.nt; v++) {
                        const double l = li[gray[v]];
                        double& m = ((v >> (L_.bits - 1 - b)) & 1) ? m1 : m0;
                        m = m > l ? m + std::log1p(std::exp(l - m)) : l + std::log1p(std::exp(m - l));
                    }
                    llr[bi++] = (float)std::max(-25.0, std::min(25.0, m1 - m0));
                }
            }
            uint8_t cw[kN];
            const int bp0 = ldpcDecode(llr, 40, cw);
            lastErr_ = bp0;
            bool got = bp0 == 0 && checkCrc(cw);
            if (!got && !deep_) return false;
            // WSJT-X's other metrics: two and three (FT4 / FT2: two and four) neighbouring symbols taken together coherently
            float llrC[2][kN];
            const int grp[2] = {2, L_.nt == 8 ? 3 : 4};
            for (int q = 0; q < 2 && !got && kFtxCoherent; q++) {
                coherentLlr(grp[q], llrC[q]);
                got = ldpcDecode(llrC[q], 40, cw) == 0 && checkCrc(cw);
            }
            // a codeword found by OSD or with a-priori bits: OSD finds some codeword in noise now and then, so keep only the plain QSO
            // message types with no unknown hashed call (and with the CQ bits assumed, a CQ)
            auto plausible = [&](const uint8_t* w, bool cq) {
                uint8_t b[77];
                for (int i = 0; i < 77; i++) b[i] = (uint8_t)(w[i] ^ (j_.mode == kFt8 ? 0 : kFt4Rvec[i]));
                const int i3 = b[74] << 2 | b[75] << 1 | b[76];
                if (!(i3 == 1 || i3 == 2 || i3 == 4)) return false;
                const std::string m = unpack77(b, &hash_);
                return !m.empty() && m.find("<...>") == std::string::npos && (!cq || m.compare(0, 3, "CQ ") == 0);
            };
            bool viaOsd = false;
            if (!got && !kFtxCoherent) { std::copy(llr, llr + kN, llrC[0]); std::copy(llr, llr + kN, llrC[1]); }
            const float* sets[3] = {llr, llrC[0], llrC[1]};
            // ordered statistics on the strongest candidates only
            if (!got && kFtxOsd && c.score >= kOsdMinScore && (nudge_ == 0 || kOsdNudged))   // AP (below) only at the fitted frequency: each try is a chance of a false decode
                for (int q = 0; q < 3 && !got; q++) {
                    OsdStats os;
                    const bool crc = osdDecode(sets[q], kOsdPairs, cw, &os);
                    if (kOsdDebug && crc) fprintf(stderr, "osd set %d score %.2f hard %d dist %.3f margin %.3f\n", q, c.score, os.hardErrors, os.dist, os.margin);
                    got = viaOsd = crc && os.dist <= kOsdMaxDist && os.margin >= kOsdMinMargin && plausible(cw, false);
                }
            // a-priori decoding (WSJT-X's AP type 1, "CQ ??? ???"): the 29 bits of CQ and the message type taken as known; needs nobody's call
            if (!got && kFtxApCq && nudge_ == 0) {
                static const std::array<uint8_t, 77> cqBits = [] { std::array<uint8_t, 77> b{}; pack77("CQ K1ABC FN42", b.data()); return b; }();
                for (int q = 0; q < 3 && !got; q++) {
                    float a[kN];
                    std::copy(sets[q], sets[q] + kN, a);
                    float mx = 0;
                    for (int i = 0; i < kN; i++) mx = std::max(mx, std::fabs(a[i]));
                    const float mag = 1.01f * mx;
                    auto put = [&](int i) { const int v = cqBits[(size_t)i] ^ (j_.mode == kFt8 ? 0 : kFt4Rvec[i]); a[i] = v ? mag : -mag; };
                    for (int i = 0; i < 29; i++) put(i);
                    for (int i = 74; i < 77; i++) put(i);
                    got = ldpcDecode(a, 40, cw) == 0 && checkCrc(cw);
                    if (!got && kFtxOsd && c.score >= kOsdMinScore) { OsdStats os; got = osdDecode(a, kOsdPairs, cw, &os); }
                    got = got && plausible(cw, true);
                    viaOsd |= got;
                    if (got && kOsdDebug) fprintf(stderr, "ap set %d score %.2f\n", q, c.score);
                }
            }
            if (!got) return false;
            uint8_t b77[77];
            for (int i = 0; i < 77; i++) b77[i] = (uint8_t)(cw[i] ^ (j_.mode == kFt8 ? 0 : kFt4Rvec[i]));
            const std::string msg = unpack77(b77, &hash_);
            if (msg.empty()) return false;
            tones = ftxTones(j_.mode, b77);
            d.msg = msg;
            if (kOsdDebug && viaOsd) fprintf(stderr, "osd decode \"%s\"\n", msg.c_str());
        }
        // SNR from the decoded tones
        {
            double ps = 0, pn = 0;
            int cs = 0, cn = 0;
            for (int k = 0; k < (int)tones.size(); k++) {
                const int ks = k + L_.toneOff;
                for (int t = 0; t < L_.nt; t++) {
                    const double p = amp[(size_t)ks * L_.nt + t] * amp[(size_t)ks * L_.nt + t];
                    if (t == tones[(size_t)k]) { ps += p; cs++; } else { pn += p; cn++; }
                }
            }
            ps /= std::max(1, cs); pn /= std::max(1, cn);
            const double r = std::max(1e-3, ps / std::max(1e-30, pn) - 1);
            d.snrDb = (float)std::max(-35.0, std::round(10 * std::log10(r) + 10 * std::log10(L_.sp / 2500.0)));
        }
        if (j_.mode == kWspr) wsprCallGrid(d.msg, d.call, d.grid);
        else callAndGrid(d.msg, d.call, d.grid);
        d.cq = d.msg.compare(0, 3, "CQ ") == 0 || j_.mode == kWspr;
        f0out = f0fine;
        startOut = (bestT + tfrac) * L_.ndown;   // samples into x
        return true;
    }

    // rebuilds a decoded signal and takes it out of the slot: its slowly varying complex amplitude is measured against a clean copy
    void subtract(const std::vector<int>& tones, double f0, double start, bool mir) {
        const int n = (int)x_.size();
        std::vector<float> re((size_t)n, 0.f), im((size_t)n, 0.f);
        ftx::addWaveIQ(j_.mode, tones, f0, L_.rate, start, mir, re, im);   // cos and sin of the phase
        // z = x * conj(c), c = re + j im; box filter of one symbol over z and over |c|^2
        const int half = L_.nsps / 2;
        std::vector<cd> cz((size_t)n + 1);
        std::vector<double> ce((size_t)n + 1);
        cz[0] = 0; ce[0] = 0;
        for (int i = 0; i < n; i++) {
            const cd c(re[(size_t)i], im[(size_t)i]);
            cz[(size_t)i + 1] = cz[(size_t)i] + (double)x_[(size_t)i] * std::conj(c);
            ce[(size_t)i + 1] = ce[(size_t)i] + std::norm(c);
        }
        for (int i = 0; i < n; i++) {
            const double e2 = (double)re[(size_t)i] * re[(size_t)i] + (double)im[(size_t)i] * im[(size_t)i];
            if (e2 <= 0) continue;
            const int a = std::max(0, i - half), b = std::min(n, i + half);
            const double den = ce[(size_t)b] - ce[(size_t)a];
            if (den <= 1e-9) continue;
            const cd A = (cz[(size_t)b] - cz[(size_t)a]) / den;   // = a/2 of x = Re(a c)
            const cd c(re[(size_t)i], im[(size_t)i]);
            x_[(size_t)i] -= (float)(2.0 * (A * c).real());
        }
    }

    const Job& j_;
    Layout L_;
    CallHash& hash_;
    std::map<uint32_t, std::string>& wsprHash_;
    bool mirrored_;
    std::vector<float> x_;
    std::vector<cf32> X_;
    int nBig_ = 0;
    double df_ = 0, binHz_ = 0;
    int bLo_ = 0, bHi_ = 0, nb_ = 0, nf_ = 0;
    std::vector<float> P_;
    std::vector<float> A_[2];
    std::vector<cd> zc_;   // complex tone correlations of the candidate being tried, the symbol-to-symbol phase step taken out
    struct Fit { bool valid = false; std::vector<cf32> y; int kc = 0, bestT = 0; double base = 0, dfr = 0, tfrac = 0; } fit_;
    int lastErr_ = 0;
    std::array<uint8_t, 7> lastRaw_{};
    std::vector<std::array<uint8_t, 7>> raw_;
};

} // namespace

// the modes of hfdig_wsjt_int.h (JT65, JT9, Q65, FST4, MSK144, FST4W)
std::vector<FtxDecode> decodeOther(const Job& j) {
    wsjt::Slot s;
    s.x = j.x.data(); s.n = j.x.size(); s.x0 = j.x0; s.dtMin = j.dtMin; s.dtMax = j.dtMax; s.sub = j.sub; s.period = j.period; s.slotUtc = j.slotUtc;
    std::vector<FtxDecode> d;
    switch (j.mode) {
    case kJt65: d = wsjt::decodeJt65(s); break;
    case kJt9: d = wsjt::decodeJt9(s); break;
    case kJt4: d = wsjt::decodeJt4(s); break;
    case kQ65: d = wsjt::decodeQ65(s); break;
    case kFst4: d = wsjt::decodeFst4(s, false); break;
    case kFst4w: d = wsjt::decodeFst4(s, true); break;
    case kMsk144: d = wsjt::decodeMsk144(s); break;
    default: break;
    }
    for (auto& x : d) x.slotUtc = j.slotUtc;
    return d;
}

// ---------------------------------------------------------------- the decoder: slots, clock, worker thread

struct HfdigFtx::Impl {
    // settings (any thread)
    std::atomic<bool> enabled[kFtxModes];
    std::atomic<bool> mirrored{true};
    std::atomic<bool> clearReq{false};
    std::atomic<int> sub[kFtxModes];
    std::atomic<double> per[kFtxModes];   // 0: the default of the mode
    std::mutex setMu;
    double startTime = std::numeric_limits<double>::quiet_NaN();
    bool slotSearch = false;
    std::function<double()> wall;
    // receiver thread
    int64_t nAudio = 0;
    bool first = true;
    int timeMode = 0;
    double anchorUtc = 0;
    int64_t anchorK = 0;
    double errEma = 0;
    std::unique_ptr<Resampler> rs[kStreams];
    std::vector<float> buf[kStreams];
    int64_t bufBase[kStreams] = {};
    double nextSlot[kFtxModes];
    std::vector<float> tmp;
    // worker
    std::mutex mu;                    // guards everything below
    std::condition_variable cv, idleCv;
    std::deque<Job> jobs;
    bool busy = false, quit = false;
    double searchShift = std::numeric_limits<double>::quiet_NaN();   // from a slot search: the DT that becomes 0
    HfdigFtxTelemetry tel;
    CallHash hash;
    std::map<uint32_t, std::string> wsprHash;
    std::thread th;
    std::atomic<double> dialHz{0}, spotShift{0};
    std::atomic<HfdigFtx*> fwd{nullptr};

    Impl() {
        for (auto& e : enabled) e = true;
        for (auto& e : sub) e = 0;
        for (auto& e : per) e = 0.0;
        for (int s = 0; s < kStreams; s++) rs[s] = std::make_unique<Resampler>(kStreamL[s], kStreamM[s], kStreamCut[s]);
        for (double& v : nextSlot) v = std::numeric_limits<double>::quiet_NaN();
        th = dect2::namedThread("onair-ftx", [this] { worker(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(mu); quit = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    double clockNow() {
        std::function<double()> f;
        { std::lock_guard<std::mutex> lk(setMu); f = wall; }
        return f ? f() : nowSystemUtc();
    }

    void resetState() {
        nAudio = 0; first = true; errEma = 0;
        for (int s = 0; s < kStreams; s++) { rs[s]->reset(); buf[s].clear(); bufBase[s] = 0; }
        for (double& v : nextSlot) v = std::numeric_limits<double>::quiet_NaN();
        std::lock_guard<std::mutex> lk(mu);
        jobs.clear();
        searchShift = std::numeric_limits<double>::quiet_NaN();
        tel.audioSamples = 0;
        tel.pending = 0;
    }

    Spec sp(int m) const { return specCfg(m, sub[m].load(), per[m].load()); }

    double utcOfAudio(double k) const { return anchorUtc + (k - (double)anchorK) / kHfdigAudioRate; }
    double audioOfUtc(double u) const { return (double)anchorK + (u - anchorUtc) * kHfdigAudioRate; }

    // the window of a slot, against the slot start
    void window(int m, bool search, double& a, double& b, double& dtMin, double& dtMax) const {
        const Spec s = sp(m);
        const double len = s.nn * s.symSec;
        // a slot search: windows every half period, each looking for starts over that half period and a little beyond (so that
        // two windows cover every offset, and WSPR needs 3 minutes of audio for the first decode instead of 4)
        dtMax = search ? s.period * 0.55 : m >= kJt65 ? dtRange(m, s.period) : layoutOf(m).dtMax;
        dtMin = search ? -s.period * 0.05 : m >= kJt65 ? -dtLow(m, s.period) : -dtMax;
        const double guard = 2 * s.symSec;
        a = s.t0 + dtMin - guard;
        b = s.t0 + len + dtMax + guard;
    }

    void schedule(bool flushAll) {
        const bool search = timeMode == 2;
        for (int m = 0; m < kFtxModes; m++) {
            const int st = streamOf(m);
            const Resampler& r = *rs[st];
            const double rate = streamRate(st);
            if (buf[st].empty()) continue;
            const double P = sp(m).period;
            double a, b, dtMin, dtMax;
            window(m, search, a, b, dtMin, dtMax);
            const double uStart = utcOfAudio(r.inputPos((double)bufBase[st]));
            const double uEnd = utcOfAudio(r.inputPos((double)(bufBase[st] + (int64_t)buf[st].size())));
            if (std::isnan(nextSlot[m]) && search) nextSlot[m] = uStart;   // no clock yet: the first window starts with the audio
            if (std::isnan(nextSlot[m])) {
                double S = std::floor(uStart / P) * P;
                const double late = search ? 0.1 : 1.0;   // start mid-slot: a transmission missing more than its first second waits for the next slot
                while (uStart > S + (search ? a : sp(m).t0) + late) S += P;
                nextSlot[m] = S;
            }
            for (;;) {
                const double S = nextSlot[m];
                const bool complete = uEnd >= S + b;
                const bool partial = flushAll && uEnd > S + sp(m).t0 + 0.5 * sp(m).nn * sp(m).symSec;
                if (!complete && !partial) break;
                nextSlot[m] = S + (search ? P / 2 : P);
                if (!enabled[m].load()) continue;
                Job j;
                j.mode = m; j.slotUtc = S; j.search = search; j.dtMin = dtMin; j.dtMax = dtMax; j.sub = sub[m].load(); j.period = P;
                const double ja = r.outputPos(audioOfUtc(S + a));
                const int64_t i0 = (int64_t)std::floor(ja);
                const int64_t nwin = (int64_t)std::ceil((b - a) * rate);
                j.x0 = a + (double)(i0 - ja) / rate;
                j.x.assign((size_t)nwin, 0.f);
                for (int64_t i = 0; i < nwin; i++) {
                    const int64_t k = i0 + i - bufBase[st];
                    if (k >= 0 && k < (int64_t)buf[st].size()) j.x[(size_t)i] = buf[st][(size_t)k];
                }
                std::lock_guard<std::mutex> lk(mu);
                // a slow computer: never more than a few slots waiting; the oldest are given up
                while (jobs.size() >= 6) { jobs.pop_front(); tel.slotsDropped++; }
                jobs.push_back(std::move(j));
                cv.notify_one();
            }
        }
        // keep what the next slots need: from the earliest window start
        for (int st = 0; st < kStreams; st++) {
            double keepUtc = std::numeric_limits<double>::infinity();
            for (int m = 0; m < kFtxModes; m++) {
                if (streamOf(m) != st || std::isnan(nextSlot[m])) continue;
                double a, b, dtMin, dtMax;
                window(m, search, a, b, dtMin, dtMax);
                keepUtc = std::min(keepUtc, nextSlot[m] + a - 1.0);
            }
            if (!std::isfinite(keepUtc)) continue;
            const int64_t keep = (int64_t)std::floor(rs[st]->outputPos(audioOfUtc(keepUtc)));
            const int64_t drop = std::min<int64_t>((int64_t)buf[st].size(), keep - bufBase[st]);
            if (drop > 0) { buf[st].erase(buf[st].begin(), buf[st].begin() + drop); bufBase[st] += drop; }
            // never more than five minutes (the long periods of the modes of stream 3: their longest enabled period and a minute)
            double capSec = 300;
            if (st == 3) for (int m = kJt65; m < kFtxModes; m++) if (enabled[m].load()) capSec = std::max(capSec, sp(m).period + 60);
            const int64_t cap = (int64_t)(capSec * streamRate(st));
            if ((int64_t)buf[st].size() > cap) {
                const int64_t d = (int64_t)buf[st].size() - cap;
                buf[st].erase(buf[st].begin(), buf[st].begin() + d);
                bufBase[st] += d;
            }
        }
    }

    void feed(const float* x, size_t n) {
        if (first) {
            first = false;
            double st;
            bool search;
            { std::lock_guard<std::mutex> lk(setMu); st = startTime; search = slotSearch; }
            anchorK = 0;
            if (!std::isnan(st)) { timeMode = 1; anchorUtc = st; }
            else if (search) { timeMode = 2; anchorUtc = 0; }
            else { timeMode = 0; anchorUtc = clockNow(); }
        }
        // a slot search that found the slots: the clock moves by the DT measured
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!std::isnan(searchShift) && timeMode == 2) {
                anchorUtc -= searchShift;
                timeMode = 3;
                for (double& v : nextSlot) v = std::numeric_limits<double>::quiet_NaN();
            }
            searchShift = std::numeric_limits<double>::quiet_NaN();
        }
        // NaN or infinite samples (a broken file, a driver fault) become silence instead of spreading through every FFT
        bool clean = true;
        for (size_t i = 0; i < n && clean; i++) clean = std::isfinite(x[i]);
        if (!clean) {
            tmp.assign(x, x + n);
            for (float& v : tmp) if (!std::isfinite(v)) v = 0.f;
            x = tmp.data();
        }
        for (int s = 0; s < kStreams; s++) rs[s]->process(x, n, buf[s]);
        nAudio += (int64_t)n;
        if (timeMode == 0) {   // follow the system clock; a jump (a stall, the clock set) moves the anchor
            const double err = clockNow() - utcOfAudio((double)nAudio);
            errEma += 0.05 * (err - errEma);
            if (std::fabs(err) > 2.0) { anchorUtc += err; errEma = 0; }
            else if (std::fabs(errEma) > 0.2) { anchorUtc += errEma; errEma = 0; }
        }
        schedule(false);
        std::lock_guard<std::mutex> lk(mu);
        tel.audioSamples = (uint64_t)nAudio;
        tel.timeMode = timeMode;
        tel.nowUtc = utcOfAudio((double)nAudio);
        tel.pending = (int)jobs.size() + (busy ? 1 : 0);
    }

    void worker() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu);
                busy = false;
                idleCv.notify_all();
                cv.wait(lk, [&] { return quit || !jobs.empty(); });
                if (quit) return;
                j = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<FtxDecode> dec;
            {
                // the hash tables are only used here (and cleared under the lock): no lock needed while decoding
                if (j.mode >= kJt65) dec = decodeOther(j);
                else {
                    SlotDecoder sd(j, hash, wsprHash, mirrored.load());
                    dec = sd.run();
                }
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::unique_lock<std::mutex> lk(mu);
            const int m = j.mode;
            tel.slots[m]++;
            tel.lastSlotCount[m] = (int)dec.size();
            tel.lastSlotUtc[m] = j.slotUtc;
            tel.decodeMs[m] = (float)ms;
            std::sort(dec.begin(), dec.end(), [](const FtxDecode& a, const FtxDecode& b) { return a.snrDb > b.snrDb; });
            for (auto& d : dec) {
                d.dialHz = dialHz.load();
                tel.decodes.push_back(d);
                if (!d.call.empty() && PskReporter::get().want()) PskReporter::get().spot(ftxModeName(m), d.hz, d.snrDb, d.call, d.grid, d.dialHz > 0 ? d.dialHz : 0.0);
                if (m == kWspr && !d.call.empty() && WsprNet::get().want())
                    WsprNet::get().spot(d.hz, d.snrDb, d.dt, d.call, d.grid, d.dbm, d.slotUtc, d.dialHz > 0 ? d.dialHz : 0.0);
                tel.total++;
                tel.perMode[m]++;
            }
            if (tel.decodes.size() > kFtxKeep) tel.decodes.erase(tel.decodes.begin(), tel.decodes.end() - (long)kFtxKeep);
            if (j.search && !dec.empty()) {
                std::vector<float> dts;
                for (const auto& d : dec) dts.push_back(d.dt);
                std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end());
                // the transmissions start at slotUtc + t0 + DT; the clock moves so that they start at t0 into a slot of the mode's grid
                const double P = j.period, at = j.slotUtc + dts[dts.size() / 2];
                searchShift = at - std::round(at / P) * P;
            }
            clockStats();
            if (HfdigFtx* f = fwd.load()) { lk.unlock(); f->addForeign(dec, m, j.slotUtc, (float)ms); }
        }
    }

    // the clock error: the DT of the decodes of the last minute (of the newest slot times)
    void clockStats() {
        double newest = 0;
        for (const auto& d : tel.decodes) newest = std::max(newest, d.slotUtc);
        std::vector<float> dts;
        for (const auto& d : tel.decodes) if (d.slotUtc >= newest - 60 && d.slotUtc <= newest) dts.push_back(d.dt);
        tel.clockN = (int)dts.size();
        if (dts.empty()) { tel.clockErr = 0; tel.dtSpread = 0; tel.clockWarn = false; return; }
        std::sort(dts.begin(), dts.end());
        const size_t n = dts.size();
        tel.clockErr = n % 2 ? dts[n / 2] : 0.5f * (dts[n / 2 - 1] + dts[n / 2]);
        tel.dtSpread = 0.5f * (dts[(size_t)std::min<double>((double)n - 1, std::floor(0.84 * (double)n))] - dts[(size_t)std::floor(0.16 * (double)n)]);
        tel.clockWarn = n >= 2 && std::fabs(tel.clockErr) > 1.0f;
    }
};

HfdigFtx::HfdigFtx() : p_(std::make_unique<Impl>()) {}
HfdigFtx::~HfdigFtx() = default;

void HfdigFtx::reset() { p_->resetState(); }
void HfdigFtx::feedAudio(const float* x, size_t n) {
    if (p_->clearReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(p_->mu);
        HfdigFtxTelemetry t;
        for (int m = 0; m < kFtxModes; m++) t.enabled[m] = p_->enabled[m].load();
        p_->tel = t;
    }
    p_->feed(x, n);
}
void HfdigFtx::telemetry(HfdigFtxTelemetry& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    out = p_->tel;
    for (int m = 0; m < kFtxModes; m++) { out.enabled[m] = p_->enabled[m].load(); out.sub[m] = p_->sub[m].load(); out.period[m] = p_->sp(m).period; }
    out.pending = (int)p_->jobs.size() + (p_->busy ? 1 : 0);
}
void HfdigFtx::setEnabled(int m, bool on) { if (m >= 0 && m < kFtxModes) p_->enabled[m] = on; }
bool HfdigFtx::enabled(int m) const { return m >= 0 && m < kFtxModes && p_->enabled[m].load(); }
void HfdigFtx::setModeOption(int m, int sub, double periodSec) {
    if (m < 0 || m >= kFtxModes) return;
    if (sub >= 0) p_->sub[m] = sub;
    if (periodSec > 0) p_->per[m] = periodSec;
}
void HfdigFtx::setMirrored(bool on) { p_->mirrored = on; }
bool HfdigFtx::mirrored() const { return p_->mirrored.load(); }
void HfdigFtx::clearDecodes() {
    std::lock_guard<std::mutex> lk(p_->mu);
    const HfdigFtxTelemetry old = p_->tel;
    HfdigFtxTelemetry t;
    t.audioSamples = old.audioSamples; t.timeMode = old.timeMode; t.nowUtc = old.nowUtc;
    p_->tel = t;
}
void HfdigFtx::setDecodes(std::vector<FtxDecode> d) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (d.size() > kFtxKeep) d.erase(d.begin(), d.end() - (long)kFtxKeep);
    p_->tel.decodes = std::move(d);
}
void HfdigFtx::setStartTime(double utc) { std::lock_guard<std::mutex> lk(p_->setMu); p_->startTime = utc; }
void HfdigFtx::setSlotSearch(bool on) { std::lock_guard<std::mutex> lk(p_->setMu); p_->slotSearch = on; }
void HfdigFtx::setWallClock(std::function<double()> fn) { std::lock_guard<std::mutex> lk(p_->setMu); p_->wall = std::move(fn); }
void HfdigFtx::setDialHz(double hz) { p_->dialHz = hz; }
void HfdigFtx::setSpotShiftHz(double hz) { p_->spotShift = hz; }
void HfdigFtx::setForwardTo(HfdigFtx* main) { p_->fwd = main; }
void HfdigFtx::addForeign(const std::vector<FtxDecode>& d, int m, double slotUtc, float ms) {
    if (m < 0 || m >= kFtxModes) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.slots[m]++;
    p_->tel.lastSlotCount[m] = (int)d.size();
    p_->tel.lastSlotUtc[m] = slotUtc;
    p_->tel.decodeMs[m] = ms;
    for (const auto& x : d) { p_->tel.decodes.push_back(x); p_->tel.total++; p_->tel.perMode[m]++; }
    if (p_->tel.decodes.size() > kFtxKeep) p_->tel.decodes.erase(p_->tel.decodes.begin(), p_->tel.decodes.end() - (long)kFtxKeep);
}
void HfdigFtx::copyClockFrom(HfdigFtx& o) {
    double st; bool ss; std::function<double()> w;
    { std::lock_guard<std::mutex> lk(o.p_->setMu); st = o.p_->startTime; ss = o.p_->slotSearch; w = o.p_->wall; }
    std::lock_guard<std::mutex> lk(p_->setMu);
    p_->startTime = st; p_->slotSearch = ss; p_->wall = std::move(w);
}
void HfdigFtx::waitIdle() {
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->idleCv.wait(lk, [&] { return p_->jobs.empty() && !p_->busy; });
}
void HfdigFtx::flushSlots() { p_->schedule(true); }

std::unique_ptr<HfdigFtx> makeFtxDecoder() { return std::make_unique<HfdigFtx>(); }

} // namespace dect2
