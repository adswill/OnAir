// JT4 (sub-modes A .. G) of the HF digital receiver: the EME / weak-signal mode of K1JT. 206 symbols of 2520 / 11025 s (4.375 baud, 47.1 s), 4-FSK:
// tone = sync bit + 2 * data bit, the tones spaced by 4.375 Hz * 2^k (A: 4.375 Hz .. G: 280 Hz). The message is the 72 bits of JT65 / JT9
// (hfdig_wsjt.cpp) + 31 tail bits, the K = 32 rate 1/2 convolutional code of JT9 (206 bits), bit-reversal interleaving, decoded by Fano sequential
// decoding of soft bits. Decoder: a spectrogram with one symbol per FFT (the tone spacing is a whole number of bins), a search over frequency and time
// for the parity pattern of the sync vector, a fine fit, soft bits from the two tones of the sync bit, Fano. Clean-room from the public description
// (WSJT-X user guide, the K1JT WSJT paper); the structure follows hfdig_jt9.cpp, itself after WSJT-X lib/gen9.f90, interleave9.f90, jt9fano.f90
// (K1JT et al., GPL-3).
//
// Taken from WSJT-X (read via the public source, GPL-3): the sync vector npr (lib/jt4.f90), tone = 2 * data bit + sync bit (lib/gen4.f90), the
// bit-reversal interleave (lib/interleave4.f90). Checked: jt9 -4 decodes our encoder's audio, and this receiver decodes the real WSJT-X sample
// DF2ZC_070926_040700.WAV (JT4A, "K1JT DF2ZC JO30", about -22 dB). That recording sends the sync bit inverted and starts 5.1 s into the minute, so
// both polarities are searched and the start window is -3 .. +5.5 s. Sub-modes B .. G are checked on synthetic audio only.
// Not decoded: Doppler spread / drift, the 2-bit message averaging of JT4's "average" mode, the JT4 "0 .. 9" shorthand messages.
#include "hfdig_wsjt_int.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>

namespace dect2 {
namespace wsjt {

namespace {

constexpr int kNsym = 206, kN = 4096, kHop = 1024;
constexpr double kSym = 2520.0 / 11025.0, kBaud = 11025.0 / 2520.0;
constexpr double kRate = kN * kBaud;   // 17920 Hz: a symbol is 4096 samples

const std::array<uint8_t, kNsym>& syncVec() {   // WSJT-X lib/jt4.f90 npr(2:207) (npr(1) is not sent)
    static const std::array<uint8_t, kNsym> v = {
    0,0,0,1,1,0,0,0,1,1,0,1,1,0,0,1,0,1,0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,1,
    0,1,1,0,1,1,0,1,0,1,1,1,1,1,0,1,0,0,0,1,0,0,1,0,0,1,1,1,1,1,0,0,0,1,0,1,0,0,0,1,
    1,1,1,0,1,1,0,0,1,0,0,0,1,1,0,1,0,1,0,1,0,1,0,1,1,1,1,1,0,1,0,1,0,1,1,0,1,0,1,0,
    1,1,1,0,0,1,0,1,1,0,1,1,1,1,0,0,0,0,1,1,0,1,1,0,0,0,1,1,1,0,1,1,1,0,1,1,1,0,0,1,
    0,0,0,1,1,0,1,1,0,0,1,0,0,0,1,1,1,1,1,1,0,0,1,1,0,0,0,0,1,1,0,0,0,1,0,1,1,0,1,1,
    1,1,0,1,0,1};
    return v;
}

const std::vector<int>& interleaveTable() {   // j0[i]: the position of coded bit i after interleaving (as JT9)
    static const std::vector<int> j0 = [] {
        std::vector<int> t;
        for (int i = 0; i < 256; i++) {
            int n = 0;
            for (int b = 0; b < 8; b++) n = 2 * n + ((i >> b) & 1);
            if (n <= 205) t.push_back(n);
        }
        return t;
    }();
    return j0;
}

inline int mult(int sub) { return 1 << std::max(0, std::min(6, sub)); }

} // namespace

std::vector<int> jt4Tones(const std::string& msg) {
    uint8_t sym[12];
    if (!packJt72(msg, sym)) return {};
    uint8_t bytes[13] = {};
    for (int i = 0; i < 72; i++) if ((sym[i / 6] >> (5 - i % 6)) & 1) bytes[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    uint8_t coded[206];
    convEncode32(bytes, 72, coded);
    const auto& j0 = interleaveTable();
    std::vector<int> tones(kNsym, 0);
    for (int i = 0; i < 206; i++) tones[(size_t)j0[(size_t)i]] = 2 * coded[i];
    for (int i = 0; i < kNsym; i++) tones[(size_t)i] += syncVec()[(size_t)i];
    return tones;
}

void addJt4(const std::vector<int>& tones, int sub, double f0, double amp, double rate, double start, std::vector<float>& out) {
    std::vector<double> fr(tones.size());
    for (size_t i = 0; i < tones.size(); i++) fr[i] = f0 + tones[i] * kBaud * mult(sub);
    addFsk(fr, kSym, amp, rate, start, out);
}

namespace {

class Jt4 {
public:
    explicit Jt4(const Slot& s) : s_(s), m_(mult(s.sub)) {}

    std::vector<FtxDecode> run() {
        std::vector<FtxDecode> res;
        y_ = resample8k(s_.x, s_.n, kRate);
        const double t0 = 1.0, df = kRate / kN;
        const int binLo = std::max(2, (int)std::floor(s_.fLo / df));
        const int fMaxBin = (int)std::ceil(s_.fHi / df);
        const int binHi = std::min(kN / 2 - 2, fMaxBin + 3 * m_ + 3);
        const Spectrogram sg = spectrogram(y_, kN, kHop, binLo, binHi);
        if (sg.frames < 4 * (kNsym - 1) + 1) return res;
        const int lagLo = std::max(0, (int)std::floor((t0 + s_.dtMin - s_.x0) * kRate / kHop));
        const int lagHi = std::min(sg.frames - 1 - 4 * (kNsym - 1), (int)std::ceil((t0 + s_.dtMax - s_.x0) * kRate / kHop));
        if (lagHi < lagLo) return res;
        // a robust noise level of the spectrogram (the mean of the lowest half of an exponential distribution is 0.6137 of its mean)
        std::vector<float> sample;
        for (int f = 0; f < sg.frames; f += 3) for (int b = 0; b < sg.nBins; b += 2) sample.push_back(sg.row(f)[b]);
        const double nz = std::max(1e-30, (double)lowMean(sample, 0.5) / 0.6137);
        const int bMax = std::min(fMaxBin, binHi - 3 * m_ - 1);
        struct C { int bin, lag; double z; int inv; };   // inv: the sync bit sent inverted (WSJT 7 sent some messages so; JT4 receivers try both)
        std::vector<C> cands;
        const auto& sv = syncVec();
        for (int b = binLo; b <= bMax; b++) {
            double best = -1e30, worst = 1e30; int bl = 0, wl = 0;
            for (int lag = lagLo; lag <= lagHi; lag++) {
                double sum = 0;
                for (int i = 0; i < kNsym; i++) {
                    const float* r = sg.row(lag + 4 * i) + (b - binLo);
                    const int pr = sv[(size_t)i];
                    sum += (double)r[pr * m_] + r[(pr + 2) * m_] - r[(1 - pr) * m_] - r[(3 - pr) * m_];
                }
                if (sum > best) { best = sum; bl = lag; }
                if (sum < worst) { worst = sum; wl = lag; }
            }
            cands.push_back({b, bl, best / (nz * std::sqrt(4.0 * kNsym)), 0});
            cands.push_back({b, wl, -worst / (nz * std::sqrt(4.0 * kNsym)), 1});
        }
        std::sort(cands.begin(), cands.end(), [](const C& a, const C& b) { return a.z > b.z; });
        std::vector<C> pick;
        for (const C& c : cands) {
            // a strong signal also scores at the bins a tone or two away (and inverted there): those echoes are dropped when they are clearly weaker
            // than a picked candidate whose tones they share, so that they do not use up the places of real signals (six signals in a slot lost one
            // or two of them to the old limit of 12)
            if (c.z < 3.5 || pick.size() >= 32) break;
            bool near = false;
            for (const C& p : pick) {
                if (std::abs(p.bin - c.bin) <= 1 && p.inv == c.inv) near = true;   // 2 bins is a tone pair away: another candidate
                if (std::abs(p.bin - c.bin) <= 3 * m_ + 1 && c.z < 0.6 * p.z) near = true;
            }
            if (!near) pick.push_back(c);
        }
        std::set<std::string> seen;
        for (const C& c : pick) {
            FtxDecode d;
            if (!tryCandidate(c.bin, c.lag, c.inv, t0, df, d)) continue;
            if (!seen.insert(d.msg).second) continue;
            res.push_back(d);
        }
        return res;
    }

private:
    // powers of the four tones (index t * m + 1 of nb) of every symbol, for a start (samples) and a frequency
    void powers(long bs, double bf, double df, std::vector<std::array<float, 4>>& pw) const {
        const int b0 = (int)std::lround(bf / df);
        const double shift = bf - b0 * df;
        const int nb = 3 * m_ + 3;
        std::vector<float> tmp((size_t)nb);
        pw.resize(kNsym);
        for (int i = 0; i < kNsym; i++) {
            symbolPowers(y_, kRate, bs + (long)i * kN, kN, shift, b0 - 1, nb, tmp.data());
            for (int t = 0; t < 4; t++) pw[(size_t)i][(size_t)t] = tmp[(size_t)(t * m_ + 1)];
        }
    }
    double syncScore(const std::vector<std::array<float, 4>>& pw, int inv) const {
        const auto& sv = syncVec();
        double s = 0;
        for (int i = 0; i < kNsym; i++) {
            const int pr = sv[(size_t)i] ^ inv;
            const auto& p = pw[(size_t)i];
            s += (double)p[(size_t)pr] + p[(size_t)pr + 2] - p[(size_t)(1 - pr)] - p[(size_t)(3 - pr)];
        }
        return s;
    }

    bool tryCandidate(int bin, int lag, int inv, double t0, double df, FtxDecode& out) {
        long bs = (long)lag * kHop;
        double bf = bin * df;
        std::vector<std::array<float, 4>> pw;
        {   // fine fit of the start (+-1/8 symbol) and the frequency (+-0.4 bin)
            double best = -1e30; long b1 = bs; double f1 = bf;
            for (int dt = -1; dt <= 1; dt++)
                for (int k = -2; k <= 2; k++) {
                    const long s = bs + (long)dt * (kN / 8);
                    const double f = bin * df + 0.2 * k * df;
                    powers(s, f, df, pw);
                    const double sc = syncScore(pw, inv);
                    if (sc > best) { best = sc; b1 = s; f1 = f; }
                }
            bs = b1; bf = f1;
        }
        powers(bs, bf, df, pw);
        const auto& sv = syncVec();
        double right = 0, wrong = 0;
        for (int i = 0; i < kNsym; i++) {
            const int pr = sv[(size_t)i] ^ inv;
            const auto& p = pw[(size_t)i];
            right += (double)p[(size_t)pr] + p[(size_t)pr + 2];
            wrong += (double)p[(size_t)(1 - pr)] + p[(size_t)(3 - pr)];
        }
        const double nn = wrong / (2.0 * kNsym);
        if (nn <= 0) return false;
        const double sig = std::max(right / kNsym - 2 * nn, 0.1 * nn);
        // the soft bits: the two tones of the sync bit, the data bit 1 against 0
        float soft[kNsym];
        for (int i = 0; i < kNsym; i++) {
            const int pr = sv[(size_t)i] ^ inv;
            const double l0 = logI0(2 * std::sqrt(sig * pw[(size_t)i][(size_t)pr]) / nn);
            const double l1 = logI0(2 * std::sqrt(sig * pw[(size_t)i][(size_t)pr + 2]) / nn);
            soft[i] = (float)(l1 - l0);
        }
        float coded[206];
        const auto& j0 = interleaveTable();
        for (int i = 0; i < 206; i++) coded[i] = soft[j0[(size_t)i]];
        uint8_t data[13];
        if (!fano32(coded, 72, data, 8000L * 103)) return false;
        {   // 4.0: the all-zero word ("000AAA 000AAA RA90") is what the Fano decoder returned for a few candidates beside strong signals; no station sends it
            bool zero = true;
            for (int i = 0; i < 9; i++) zero = zero && data[i] == 0;
            if (zero) return false;
        }
        uint8_t sym[12];
        for (int i = 0; i < 12; i++) {
            int v = 0;
            for (int b = 0; b < 6; b++) { const int k = i * 6 + b; v = v * 2 + ((data[k / 8] >> (7 - k % 8)) & 1); }
            sym[i] = (uint8_t)v;
        }
        bool freeText = false;
        const std::string msg = unpackJt72(sym, &freeText);
        if (msg.empty()) return false;
        uint8_t re[206];
        convEncode32(data, 72, re);
        double corr = 0, tot = 0;
        for (int i = 0; i < 206; i++) { corr += (re[i] ? 1 : -1) * coded[i]; tot += std::fabs(coded[i]); }
        const double q = corr / (tot + 1e-30);
        if (q <= (freeText ? 0.30 : 0.12)) return false;
        out.mode = ftx::kJt4;
        out.msg = msg;
        out.hz = (float)bf;
        out.dt = (float)((double)bs / kRate + s_.x0 - t0);
        out.snrDb = (float)(10 * std::log10(sig / nn * df / 2500.0));
        fillCallGrid(out);
        return true;
    }

    Slot s_;
    int m_;
    std::vector<float> y_;
};

} // namespace

std::vector<FtxDecode> decodeJt4(const Slot& s) { Jt4 d(s); return d.run(); }

} // namespace wsjt
} // namespace dect2
