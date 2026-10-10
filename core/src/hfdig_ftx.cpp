// FT8 / FT4 / FT2 / WSPR decoder (see dect2/hfdig_ftx.h and hfdig_ftx_int.h for the protocols and their sources).
// Pipeline of one slot: the audio at a rate where a symbol is a power of two samples (FT8 6400 Hz, FT4 / FT2 10667 Hz, WSPR 12000 Hz)
// -> a spectrogram (one symbol per frame, frames every quarter symbol, bins of half the tone spacing) -> the sync score of every
// time / frequency (Costas arrays, WSPR's sync vector) -> the best candidates -> each cut out of one large FFT of the slot into a narrow
// complex baseband (WSJT-X's way) -> a fine fit of time and frequency on the sync symbols -> per-symbol tone amplitudes -> soft bits
// (non-coherent likelihoods) -> LDPC / CRC (FT) or deinterleaving and Fano (WSPR) -> the message; each decoded signal is rebuilt and
// subtracted from the slot, and the search runs again on what is left.
#include "dect2/hfdig_ftx.h"
#include "dect2/fftutil.h"
#include "hfdig_ftx_int.h"
#include "hfdig_usb.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace dect2 {

const char* ftxModeName(int m) {
    static const char* n[kFtxModes] = {"FT8", "FT4", "FT2", "WSPR"};
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
    };
    return s[std::max(0, std::min(kModes - 1, m))];
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

} // namespace ftx

bool ftxGridToLatLon(const std::string& grid, double& lat, double& lon) { return ftx::gridToLatLon(grid, lat, lon); }

bool ftxAddTransmission(int mode, const std::string& msg, double hz, double amp, double rate, double startSec, bool mirrored,
                        std::vector<float>& out) {
    using namespace ftx;
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

// the streams: 0 FT8 (6400 Hz), 1 FT4 and FT2 (10667 Hz), 2 WSPR (12000 Hz)
constexpr int kStreams = 3;
const int kStreamL[kStreams] = {4, 4, 3}, kStreamM[kStreams] = {5, 3, 2};
const double kStreamCut[kStreams] = {3080, 3900, 3900};
int streamOf(int mode) { return mode == kFt8 ? 0 : mode == kWspr ? 2 : 1; }
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

Layout layoutOf(int mode) {
    Layout l;
    const Spec& s = spec(mode);
    l.rate = streamRate(streamOf(mode));
    l.sp = s.spacing;
    l.nt = s.tones;
    l.nsym = s.nn;
    if (mode == kFt8) {
        l.nsps = 1024; l.ndown = 32; l.bits = 3; l.maxCand = 150; l.passes = 3;
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
        for (int pass = 0; pass < L_.passes; pass++) {
            bigFft();
            spectrogram();
            int found = 0;
            for (int mir = 0; mir < (mirrored_ ? 2 : 1); mir++) {
                const auto cands = candidates(mir != 0);
                for (const auto& c : cands) {
                    FtxDecode d;
                    std::vector<int> tones;
                    double f0, start;
                    if (!tryCandidate(c, mir != 0, d, tones, f0, start)) continue;
                    bool same = seen.count(d.msg) > 0;
                    for (const auto& o : out)   // the rest of a signal already decoded (its subtraction is not perfect)
                        same |= o.mirrored == d.mirrored && std::fabs(o.hz - d.hz) < 1.5 * L_.sp && std::fabs(o.dt - d.dt) < 2 * spec(j_.mode).symSec;
                    if (same) { subtract(tones, f0, start, mir != 0); continue; }
                    seen.insert(d.msg);
                    d.pass = pass;
                    out.push_back(d);
                    raw_.push_back(lastRaw_);
                    found++;
                    subtract(tones, f0, start, mir != 0);
                }
            }
            if (!found) break;
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
        for (const auto& sy : L_.sync) {
            const int f = fr + 4 * sy.first;
            if (f >= nf_) continue;
            double a = 0;
            for (int t = 0; t < L_.nt; t++) a += P(f, b + s * t);
            syn += P(f, b + s * sy.second);
            all += a;
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
        const float thr = j_.mode == kWspr ? std::max(0.12f, med + 0.08f) : std::max(1.45f, med * 1.15f);
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
        if ((int)c.size() > L_.maxCand) c.resize((size_t)L_.maxCand);
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

    bool tryCandidate(const Cand& c, bool mir, FtxDecode& d, std::vector<int>& tones, double& f0out, double& startOut) {
        const double s = mir ? -1 : 1;
        const double f0 = c.bin * binHz_;
        std::vector<cf32> y;
        int kc;
        baseband(f0, mir, y, kc);
        const int nspsb = L_.nsps / L_.ndown;
        const double fsb = L_.rate / L_.ndown;
        const double base = s * (f0 - kc * df_);
        const int tc = c.frame * (L_.nsps / 4) / L_.ndown;
        // fine search: frequency in steps of a sixteenth of the spacing over +- half of it, time over +- half a symbol
        const int nfq = 9;
        double bestM = -1e300;
        int bestT = tc, bestF = 0;
        std::vector<std::vector<cd>> W((size_t)nfq);
        for (int q = 0; q < nfq; q++) twiddles(base, (q - nfq / 2) * L_.sp / 16.0, nspsb, fsb, W[(size_t)q]);
        std::vector<double> mt((size_t)nfq * (size_t)(nspsb + 1));
        for (int q = 0; q < nfq; q++)
            for (int dt = -nspsb / 2; dt <= nspsb / 2; dt++) {
                const double m = syncMetric(y, tc + dt, W[(size_t)q], nspsb);
                if (m > bestM) { bestM = m; bestT = tc + dt; bestF = q; }
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
        // tone amplitudes of every symbol
        const int ns = L_.nsym;
        std::vector<double> amp((size_t)ns * L_.nt);
        cd z[8];
        for (int k = 0; k < ns; k++) {
            symbol(y, bestT + k * nspsb, w, nspsb, z);
            for (int t = 0; t < L_.nt; t++) amp[(size_t)k * L_.nt + t] = std::abs(z[t]);
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
            if (!wsprFano(dl, data, 20000L * 81)) return false;
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
            if (ldpcDecode(llr, 40, cw) != 0) return false;
            if (!checkCrc(cw)) return false;
            uint8_t b77[77];
            for (int i = 0; i < 77; i++) b77[i] = (uint8_t)(cw[i] ^ (j_.mode == kFt8 ? 0 : kFt4Rvec[i]));
            const std::string msg = unpack77(b77, &hash_);
            if (msg.empty()) return false;
            tones = ftxTones(j_.mode, b77);
            d.msg = msg;
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
        addWave(j_.mode, tones, f0, 1.0, L_.rate, start, mir, 0.0, re);
        addWave(j_.mode, tones, f0, 1.0, L_.rate, start, mir, -0.5 * hfdig::kPi, im);   // the quadrature copy: sin
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
    std::array<uint8_t, 7> lastRaw_{};
    std::vector<std::array<uint8_t, 7>> raw_;
};

} // namespace

// ---------------------------------------------------------------- the decoder: slots, clock, worker thread

struct HfdigFtx::Impl {
    // settings (any thread)
    std::atomic<bool> enabled[kFtxModes];
    std::atomic<bool> mirrored{true};
    std::atomic<bool> clearReq{false};
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

    Impl() {
        for (auto& e : enabled) e = true;
        for (int s = 0; s < kStreams; s++) rs[s] = std::make_unique<Resampler>(kStreamL[s], kStreamM[s], kStreamCut[s]);
        for (double& v : nextSlot) v = std::numeric_limits<double>::quiet_NaN();
        th = std::thread([this] { worker(); });
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

    double utcOfAudio(double k) const { return anchorUtc + (k - (double)anchorK) / kHfdigAudioRate; }
    double audioOfUtc(double u) const { return (double)anchorK + (u - anchorUtc) * kHfdigAudioRate; }

    // the window of a slot, against the slot start
    void window(int m, bool search, double& a, double& b, double& dtMin, double& dtMax) const {
        const Spec& s = spec(m);
        const Layout l = layoutOf(m);
        const double len = s.nn * s.symSec;
        dtMax = search ? s.period / 2 : l.dtMax;
        dtMin = -dtMax;
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
            const double P = spec(m).period;
            double a, b, dtMin, dtMax;
            window(m, search, a, b, dtMin, dtMax);
            const double uStart = utcOfAudio(r.inputPos((double)bufBase[st]));
            const double uEnd = utcOfAudio(r.inputPos((double)(bufBase[st] + (int64_t)buf[st].size())));
            if (std::isnan(nextSlot[m])) {
                double S = std::floor(uStart / P) * P;
                const double late = search ? 0.1 : 1.0;   // start mid-slot: a transmission missing more than its first second waits for the next slot
                while (uStart > S + (search ? a : spec(m).t0) + late) S += P;
                nextSlot[m] = S;
            }
            for (;;) {
                const double S = nextSlot[m];
                const bool complete = uEnd >= S + b;
                const bool partial = flushAll && uEnd > S + spec(m).t0 + 0.5 * spec(m).nn * spec(m).symSec;
                if (!complete && !partial) break;
                nextSlot[m] = S + P;
                if (!enabled[m].load()) continue;
                Job j;
                j.mode = m; j.slotUtc = S; j.search = search; j.dtMin = dtMin; j.dtMax = dtMax;
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
            // never more than five minutes
            const int64_t cap = (int64_t)(300 * streamRate(st));
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
                SlotDecoder sd(j, hash, wsprHash, mirrored.load());
                dec = sd.run();
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(mu);
            const int m = j.mode;
            tel.slots[m]++;
            tel.lastSlotCount[m] = (int)dec.size();
            tel.lastSlotUtc[m] = j.slotUtc;
            tel.decodeMs[m] = (float)ms;
            std::sort(dec.begin(), dec.end(), [](const FtxDecode& a, const FtxDecode& b) { return a.snrDb > b.snrDb; });
            for (const auto& d : dec) {
                tel.decodes.push_back(d);
                tel.total++;
                tel.perMode[m]++;
            }
            if (tel.decodes.size() > kFtxKeep) tel.decodes.erase(tel.decodes.begin(), tel.decodes.end() - (long)kFtxKeep);
            if (j.search && !dec.empty()) {
                std::vector<float> dts;
                for (const auto& d : dec) dts.push_back(d.dt);
                std::nth_element(dts.begin(), dts.begin() + dts.size() / 2, dts.end());
                searchShift = dts[dts.size() / 2];
            }
            clockStats();
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
    for (int m = 0; m < kFtxModes; m++) out.enabled[m] = p_->enabled[m].load();
    out.pending = (int)p_->jobs.size() + (p_->busy ? 1 : 0);
}
void HfdigFtx::setEnabled(int m, bool on) { if (m >= 0 && m < kFtxModes) p_->enabled[m] = on; }
bool HfdigFtx::enabled(int m) const { return m >= 0 && m < kFtxModes && p_->enabled[m].load(); }
void HfdigFtx::setMirrored(bool on) { p_->mirrored = on; }
bool HfdigFtx::mirrored() const { return p_->mirrored.load(); }
void HfdigFtx::clearDecodes() {
    std::lock_guard<std::mutex> lk(p_->mu);
    const HfdigFtxTelemetry old = p_->tel;
    HfdigFtxTelemetry t;
    t.audioSamples = old.audioSamples; t.timeMode = old.timeMode; t.nowUtc = old.nowUtc;
    p_->tel = t;
}
void HfdigFtx::setStartTime(double utc) { std::lock_guard<std::mutex> lk(p_->setMu); p_->startTime = utc; }
void HfdigFtx::setSlotSearch(bool on) { std::lock_guard<std::mutex> lk(p_->setMu); p_->slotSearch = on; }
void HfdigFtx::setWallClock(std::function<double()> fn) { std::lock_guard<std::mutex> lk(p_->setMu); p_->wall = std::move(fn); }
void HfdigFtx::waitIdle() {
    std::unique_lock<std::mutex> lk(p_->mu);
    p_->idleCv.wait(lk, [&] { return p_->jobs.empty() && !p_->busy; });
}
void HfdigFtx::flushSlots() { p_->schedule(true); }

std::unique_ptr<HfdigFtx> makeFtxDecoder() { return std::make_unique<HfdigFtx>(); }

} // namespace dect2
