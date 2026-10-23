// Mercury MFSK floor (see mercury_mfsk.h).
#include "dect2/mercury/mercury_mfsk.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace dect2 {
namespace mercury {

namespace {

using dect2::math::kPi;
constexpr double kFs = 8000.0;
constexpr int kM = 32, kNc = 50, kOffset = 9, kHop = 13, kXfade = 32;
constexpr int kPreTones[4] = {4, 20, 12, 28};
constexpr int kPostShift = 8;
constexpr int kBits = 5;
constexpr double kTxAmp = 2200.0 * 0.5 / 16384.0;   // Mercury's MFSK_TXAMP, in the units of the OFDM modulator (peak 0.5)

MfskGeometry makeGeom(int mode) {
    MfskGeometry g;
    g.mode = mode;
    g.nfft = mode == kMfsk500 ? 512 : 256;
    g.ngi = g.nfft / 4;
    g.nofdm = g.nfft + g.ngi;
    g.spacingHz = kFs / g.nfft;
    g.fcHz = 1500.0 - 0.5 * g.spacingHz;
    g.preSyms = 4;
    g.paySyms = (1600 + kBits - 1) / kBits;
    g.burstSamples = (2 * g.preSyms + g.paySyms) * g.nofdm;
    return g;
}

int postTone(int s) { return (kPreTones[s] + kPostShift) % kM; }

std::vector<int> buildInterleaver(int n) {
    std::vector<int> p((size_t)n);
    for (int i = 0; i < n; i++) p[(size_t)i] = i;
    unsigned long long st = 0x9E3779B97F4A7C15ULL;   // xorshift64, as mfsk_interleave_init: the same table on every platform
    for (int i = n - 1; i > 0; i--) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        const int j = (int)(st % (unsigned long long)(i + 1));
        std::swap(p[(size_t)i], p[(size_t)j]);
    }
    return p;
}

float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

} // namespace

const MfskGeometry& mfskGeometry(int mode) {
    static const MfskGeometry g1 = makeGeom(kMfsk), g5 = makeGeom(kMfsk500);
    return mode == kMfsk500 ? g5 : g1;
}

double mfskToneHz(int mode, int a) {
    const MfskGeometry& g = mfskGeometry(mode);
    return g.fcHz - (a - 16) * g.spacingHz;   // carrier 9 + a is FFT bin a - 16, mirrored by the transmitter's real mixing
}

const std::vector<int>& mfskInterleaver() {
    static const std::vector<int> p = buildInterleaver(1600);
    return p;
}

std::vector<uint8_t> makeMfskFrame(const uint8_t* payload, size_t n, uint16_t seed) {
    std::vector<uint8_t> f(100, 0);
    if (payload) std::memcpy(f.data(), payload, std::min<size_t>(n, 98));
    const uint16_t c = (uint16_t)(frameCrc(f.data(), 98) ^ seed);
    f[98] = (uint8_t)(c >> 8); f[99] = (uint8_t)c;
    return f;
}

std::vector<cf> modulateMfskBurst(int mode, const std::vector<uint8_t>& frame) {
    const MfskGeometry& g = mfskGeometry(mode);
    const LdpcCode& code = ldpcCode(kMfsk1600);
    std::vector<uint8_t> cw(1600, 0);
    for (int i = 0; i < 800; i++) cw[(size_t)i] = (size_t)(i >> 3) < frame.size() ? (uint8_t)((frame[(size_t)(i >> 3)] >> (7 - (i & 7))) & 1) : 0;
    ldpcEncode(code, cw.data(), cw.data() + 800);
    const std::vector<int>& ilv = mfskInterleaver();
    // the tone of every symbol: preamble, data (slot t carries coded bit ilv[t]; 5 bits MSB first are a Gray code), postamble
    std::vector<int> tones;
    for (int s = 0; s < g.preSyms; s++) tones.push_back(kPreTones[s]);
    for (int s = 0; s < g.paySyms; s++) {
        int v = 0;
        for (int b = 0; b < kBits; b++) {
            const int t = s * kBits + b;
            const int bit = t < 1600 && ilv[(size_t)t] < 1600 ? cw[(size_t)ilv[(size_t)t]] : 0;
            v = (v << 1) | bit;
        }
        int bin = v;   // Gray -> binary
        for (int sh = 1; sh < kBits; sh++) bin ^= v >> sh;
        tones.push_back((bin + s * kHop) % kM);
    }
    for (int s = 0; s < g.preSyms; s++) tones.push_back(postTone(s));
    // synthesis as mfsk_synth: one IFFT per symbol, the cyclic prefix, raised cosine crossfades of 32 samples at the boundaries
    std::vector<double> rise(kXfade);
    for (int j = 0; j < kXfade; j++) rise[(size_t)j] = 0.5 * (1.0 - std::cos(kPi * (j + 0.5) / kXfade));
    std::vector<cf> out;
    out.reserve(tones.size() * (size_t)g.nofdm);
    std::vector<std::complex<double>> tail(kXfade), t((size_t)g.nfft), cp((size_t)g.nofdm);
    bool haveTail = false;
    int64_t n = 0;
    for (size_t s = 0; s < tones.size(); s++) {
        // a tone of amplitude sqrt(50) on carrier 9 + tone, which is FFT bin tone - 16
        const int bin = tones[s] + kOffset - kNc / 2;
        const double amp = std::sqrt((double)kNc);
        for (int m = 0; m < g.nfft; m++) t[(size_t)m] = std::polar(amp, 2 * kPi * bin * m / g.nfft);
        for (int j = 0; j < g.ngi; j++) cp[(size_t)j] = t[(size_t)(g.nfft - g.ngi + j)];
        for (int j = 0; j < g.nfft; j++) cp[(size_t)(g.ngi + j)] = t[(size_t)j];
        const bool last = s + 1 == tones.size();
        for (int j = 0; j < kXfade; j++) cp[(size_t)j] = cp[(size_t)j] * rise[(size_t)j] + (haveTail ? tail[(size_t)j] : 0.0);
        if (last) for (int j = 0; j < kXfade; j++) cp[(size_t)(g.nofdm - kXfade + j)] *= rise[(size_t)(kXfade - 1 - j)];
        for (int j = 0; j < kXfade; j++) tail[(size_t)j] = t[(size_t)j] * rise[(size_t)(kXfade - 1 - j)];
        haveTail = !last;
        // on the air: Re(cp * conj(carrier)), whose analytic signal is conj(cp) * carrier
        for (int i = 0; i < g.nofdm; i++, n++) {
            const std::complex<double> p = std::polar(1.0, std::fmod(2 * kPi * g.fcHz * (double)n / kFs, 2 * kPi));
            const std::complex<double> a = std::conj(cp[(size_t)i]) * p * kTxAmp;
            out.push_back(cf((float)a.real(), (float)a.imag()));
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ receiver

namespace {
constexpr int kRingBits = 19, kRingN = 1 << kRingBits, kRingMask = kRingN - 1;   // 65 s at 8 kHz
constexpr int kRowsN = 1 << 12, kRowsMask = kRowsN - 1;
}

struct MfskReceiver::Impl {
    const MfskGeometry& g;
    int R = 32;                  // spectrogram hop (a tenth of a symbol)
    int H = 6;                   // frequency hypotheses: -H .. H half bins
    int B = 40;                  // bins kept per row: -B .. B of the double-length FFT
    std::vector<cf> ring;
    int64_t n = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> rows;     // kRowsN x (2B + 1)
    int64_t nextRow = 0;
    std::vector<cf> tw;          // [a][m]: the DFT of tone a at no offset
    LdpcDecoder dec{kMfsk1600};
    struct Cl { bool on = false; int64_t row = 0; float m = 0, n = 0; int h = 0; };
    Cl clPre, clPost;
    struct Cand { int64_t t0; int h; float pre = 0, post = 0, preN = 0, postN = 0; int64_t tPre = -1, tPost = -1; };
    std::vector<Cand> cands;
    std::vector<OfdmFrame> out;
    std::vector<std::pair<int64_t, int64_t>> done;
    uint64_t nDet = 0, nFail = 0;

    explicit Impl(int mode) : g(mfskGeometry(mode)) {
        R = g.nofdm / 10;
        H = mode == kMfsk500 ? 12 : 6;
        B = 31 + H + 2;
        ring.assign(kRingN, cf(0, 0));
        fft = std::make_unique<Fft>(2 * g.nfft);
        rows.assign((size_t)kRowsN * (size_t)(2 * B + 1), 0.f);
        tw.resize((size_t)(kM * g.nfft));
        for (int a = 0; a < kM; a++) {
            const double f = mfskToneHz(g.mode, a) - 1500.0;
            for (int m = 0; m < g.nfft; m++) tw[(size_t)(a * g.nfft + m)] = std::polar(1.f, (float)(-2 * kPi * f * m / kFs));
        }
    }

    cf at(int64_t i) const { return ring[(size_t)(i & kRingMask)]; }
    bool have(int64_t a, int64_t b) const { return a >= 0 && b <= n && n - a <= kRingN; }
    float* row(int64_t r) { return &rows[(size_t)((r & kRowsMask) * (2 * B + 1))]; }
    // energy of bin k (half-bin units, -B .. B) of row r
    float E(int64_t r, int k) { return row(r)[k + B]; }

    void reset() {
        std::fill(ring.begin(), ring.end(), cf(0, 0));
        n = 0; nextRow = 0;
        clPre = clPost = Cl();
        cands.clear(); out.clear(); done.clear();
    }

    // How much the 4 symbols ending at row r are the known ones, at offset h (half bins). F: the share of their energy in the known
    // tones (noise alone: about 1/32, and above 0.18 about once in 10^8 tries). Fn: F over the share of the strongest tones, near 1
    // for the known sequence at any SNR, k/4 for a stretch of data that happens to agree in k of the 4 symbols (a data symbol is one
    // random tone): F rules out noise, Fn the data of a burst.
    float knownMetric(int64_t r, int h, bool post, float* fn = nullptr) {
        double on = 0, all = 0, top = 0;
        for (int s = 0; s < 4; s++) {
            const int64_t rr = r - (3 - s) * 10;
            const int tone = post ? postTone(s) : kPreTones[s];
            double mx = 0;
            for (int a = 0; a < kM; a++) { const double e = E(rr, 31 - 2 * a + h); all += e; mx = std::max(mx, e); }
            on += E(rr, 31 - 2 * tone + h);
            top += mx;
        }
        if (fn) *fn = top > 0 ? (float)(on / top) : 0.f;
        return all > 0 ? (float)(on / all) : 0.f;
    }

    void computeRows() {
        std::vector<cf> buf((size_t)(2 * g.nfft));
        while ((nextRow * R) + g.nfft <= n) {
            const int64_t r = nextRow++;
            if (n - r * R > kRingN) continue;
            std::fill(buf.begin(), buf.end(), cf(0, 0));
            for (int m = 0; m < g.nfft; m++) buf[(size_t)m] = at(r * R + m);
            fft->forward(buf.data());
            float* d = row(r);
            const int N2 = 2 * g.nfft;
            for (int k = -B; k <= B; k++) d[k + B] = std::norm(buf[(size_t)((k + N2) % N2)]);
            if (r < 30) continue;
            // preamble / postamble: the best offset of the 4 symbols ending here
            for (int w = 0; w < 2; w++) {
                float best = 0, bestN = 0;
                int bh = 0;
                for (int h = -H; h <= H; h++) {
                    float fn = 0;
                    const float m = knownMetric(r, h, w == 1, &fn);
                    if (fn >= 0.55f && m > best) { best = m; bestN = fn; bh = h; }
                }
                Cl& c = w ? clPost : clPre;
                if (c.on && r - c.row > 10) { anchor(c, w == 1); c.on = false; }
                if (best >= 0.08f && (!c.on || best > c.m * 1.02f)) { c.on = true; c.row = r; c.m = best; c.n = bestN; c.h = bh; }
            }
        }
    }

    void anchor(const Cl& c, bool post) {
        // the first symbol's FFT window starts at row c.row - 30: the burst (its first cyclic prefix) a prefix earlier
        int64_t t0 = (c.row - 30) * R - g.ngi;
        if (post) t0 -= (int64_t)(g.preSyms + g.paySyms) * g.nofdm;
        if (t0 < 0) return;
        // the same burst: an anchor of the same end within a symbol; the other end within the sample clock error searched (2 %)
        const int64_t span = (int64_t)(g.preSyms + g.paySyms) * g.nofdm;
        Cand* best = nullptr;
        int64_t bestD = INT64_MAX;
        for (Cand& k : cands) {
            if (std::abs(k.h - c.h) > 2) continue;
            const int64_t ref = post ? (k.tPost >= 0 ? k.tPost : k.tPre) : (k.tPre >= 0 ? k.tPre : k.tPost);
            const bool sameEnd = post ? k.tPost >= 0 : k.tPre >= 0;
            const int64_t d = std::llabs(ref - t0);
            if (d > (sameEnd ? 3 * R + 64 : span / 50 + 3 * R) || d >= bestD) continue;
            best = &k; bestD = d;
        }
        if (best) {
            Cand& k = *best;
            if (post) { if (c.m > k.post) { k.post = c.m; k.postN = c.n; k.tPost = t0; } } else { if (c.m > k.pre) { k.pre = c.m; k.preN = c.n; k.tPre = t0; } }
            return;
        }
        Cand k;
        k.t0 = t0; k.h = c.h;
        if (post) { k.post = c.m; k.postN = c.n; k.tPost = t0; } else { k.pre = c.m; k.preN = c.n; k.tPre = t0; }
        cands.push_back(k);
    }

    // the energies of the 32 tones of the symbol whose window starts at w (samples), at offset hb half bins
    void tonesAt(double w, double hb, float* e) {
        const int64_t w0 = (int64_t)std::llround(w);
        std::vector<cf> v((size_t)g.nfft);
        const std::complex<double> step = std::polar(1.0, -2 * kPi * hb * 0.5 * g.spacingHz / kFs);
        std::complex<double> rot(1, 0);
        for (int m = 0; m < g.nfft; m++) { const cf x = at(w0 + m); v[(size_t)m] = cf((float)(x.real() * rot.real() - x.imag() * rot.imag()), (float)(x.real() * rot.imag() + x.imag() * rot.real())); rot *= step; }
        for (int a = 0; a < kM; a++) {
            cf acc(0, 0);
            const cf* t = &tw[(size_t)(a * g.nfft)];
            for (int m = 0; m < g.nfft; m++) acc += v[(size_t)m] * t[m];
            e[a] = std::norm(acc);
        }
    }

    // the share of energy in the known tones, the burst starting at t0 (one end of it)
    double anchorMetric(double t0, double hb, bool post) {
        double on = 0, all = 0;
        float e[kM];
        const double base = t0 + (post ? (double)(g.preSyms + g.paySyms) * g.nofdm : 0.0);
        for (int s = 0; s < 4; s++) {
            tonesAt(base + s * g.nofdm + g.ngi, hb, e);
            on += e[post ? postTone(s) : kPreTones[s]];
            for (int a = 0; a < kM; a++) all += e[a];
        }
        return all > 0 ? on / all : 0;
    }
    // the score of knownMetric at an exact position (for the pairing of the two ends)

    // the timing that puts the FFT window in the middle of where it sees the whole symbol (the plateau of the metric)
    double refine(double t0, double hb, bool post) {
        std::vector<double> m;
        std::vector<double> ts;
        for (int d = -2 * R; d <= 2 * R; d += 4) { ts.push_back(t0 + d); m.push_back(anchorMetric(t0 + d, hb, post)); }
        const double mx = *std::max_element(m.begin(), m.end());
        double s = 0;
        int k = 0;
        for (size_t i = 0; i < m.size(); i++) if (m[i] >= 0.9 * mx) { s += ts[i]; k++; }
        return k ? s / k : t0;
    }

    bool decode(const Cand& c, OfdmFrame& f) {
        const double tPre = c.tPre >= 0 ? refine((double)c.tPre, c.h, false) : -1, tPost = c.tPost >= 0 ? refine((double)c.tPost, c.h, true) : -1;
        const double span = (double)(g.preSyms + g.paySyms) * g.nofdm;
        // the readings to try: the sample clock the two ends give, then each end alone at the nominal clock
        if (tPre >= 0 && tPost >= 0 && decodeAt(c, tPre, 1.0 + (tPost - tPre) / span, f)) return true;
        if (tPre >= 0 && decodeAt(c, tPre, 1.0, f)) return true;
        if (tPost >= 0 && decodeAt(c, tPost, 1.0, f)) return true;
        return false;
    }

    bool decodeAt(const Cand& c, double t0, double rate, OfdmFrame& f) {
        f = OfdmFrame();
        f.mode = g.mode;
        double hb = c.h;
        // the offset to a quarter bin: the one under which the data symbols' strongest tones hold the most energy
        double bestC = -1, bestH = hb;
        for (double dh = -0.5; dh <= 0.5 + 1e-9; dh += 0.125) {
            double conc = 0;
            float e[kM];
            for (int s = 0; s < g.paySyms; s += 6) {
                tonesAt(t0 + (double)(g.preSyms + s) * g.nofdm * rate + g.ngi, hb + dh, e);
                double mx = 0, sum = 0;
                for (int a = 0; a < kM; a++) { mx = std::max(mx, (double)e[a]); sum += e[a]; }
                conc += sum > 0 ? mx / sum : 0;
            }
            if (conc > bestC) { bestC = conc; bestH = hb + dh; }
        }
        hb = bestH;
        f.cfoHz = hb * 0.5 * g.spacingHz;
        f.startSample = (int64_t)std::llround(t0);
        f.endSample = f.startSample + (int64_t)std::llround((double)g.burstSamples * rate);
        f.metric = std::max(c.pre, c.post);
        f.viaPostamble = c.pre <= 0;
        // soft bits as mfsk_demod: noise from the bins but the strongest, max-log over the Gray labels, clamped to +-5
        std::vector<float> llrSlot((size_t)(g.paySyms * kBits));
        double snrS = 0;
        int snrN = 0;
        float e[kM];
        for (int s = 0; s < g.paySyms; s++) {
            tonesAt(t0 + (double)(g.preSyms + s) * g.nofdm * rate + g.ngi, hb, e);
            double sum = 0, mx = 0;
            for (int a = 0; a < kM; a++) { sum += e[a]; mx = std::max(mx, (double)e[a]); }
            const double nv = std::max(1e-30, (sum - mx) / (kM - 1));
            snrS += std::max(0.0, mx - nv) / nv; snrN++;
            const double scale = 1.0 / (2.0 * nv);
            const int hop = (s * kHop) % kM;
            double Eu[kM];
            for (int mm = 0; mm < kM; mm++) Eu[mm] = e[(mm + hop) % kM];
            for (int k = 0; k < kBits; k++) {
                const int mask = 1 << (kBits - 1 - k);
                double e1 = -1e30, e0 = -1e30;
                for (int mm = 0; mm < kM; mm++) { if ((mm ^ (mm >> 1)) & mask) e1 = std::max(e1, Eu[mm]); else e0 = std::max(e0, Eu[mm]); }
                llrSlot[(size_t)(s * kBits + k)] = clampf((float)((e0 - e1) * scale), -5.f, 5.f);
            }
        }
        const double snrBin = snrN ? snrS / snrN : 0;
        f.esNoDb = (float)(10 * std::log10(std::max(1e-3, snrBin)));
        f.snrDb = (float)(f.esNoDb + 10 * std::log10(g.spacingHz / 3000.0));
        const std::vector<int>& ilv = mfskInterleaver();
        std::vector<float> llr(1600, 0.f);
        for (int t = 0; t < 1600 && t < (int)llrSlot.size(); t++) if (ilv[(size_t)t] < 1600) llr[(size_t)ilv[(size_t)t]] = llrSlot[(size_t)t];
        std::vector<uint8_t> bits(1600);
        int it = 0;
        f.ldpcOk = dec.decode(llr.data(), 60, bits.data(), &it);
        f.ldpcIters = it;
        int raw = 0;
        for (int i = 0; i < 1600; i++) raw += (llr[(size_t)i] < 0) != (bits[(size_t)i] != 0);
        f.rawErrors = raw;
        std::vector<uint8_t> bytes(100, 0);
        for (int i = 0; i < 800; i++) if (bits[(size_t)i]) bytes[(size_t)(i >> 3)] |= (uint8_t)(0x80 >> (i & 7));
        f.payload.assign(bytes.begin(), bytes.begin() + 98);
        f.crcRx = (uint16_t)(bytes[98] << 8 | bytes[99]);
        f.crcCalc = frameCrc(bytes.data(), 98);
        f.crcPlain = f.crcRx == f.crcCalc;
        return f.ldpcOk;
    }

    static double overlap(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
        const int64_t lo = std::max(a0, b0), hi = std::min(a1, b1);
        return hi <= lo ? 0 : (double)(hi - lo) / (double)std::max<int64_t>(1, std::min(a1 - a0, b1 - b0));
    }

    void addSpan(int64_t a, int64_t b) { done.push_back({a, b}); }

    void service(bool all) {
        for (size_t i = 0; i < cands.size();) {
            const Cand c = cands[i];
            const int64_t end = c.t0 + g.burstSamples;
            const bool ready = all ? true : end + 4000 + 12 * R <= n;
            if (!ready) { i++; continue; }
            cands.erase(cands.begin() + (long)i);
            bool covered = false;
            for (const auto& d : done) covered |= overlap(c.t0, end, d.first, d.second) > 0.2;
            // one end found clearly (all four symbols), or both ends at lower levels
            const bool worth = (c.pre >= 0.18f && c.preN >= 0.75f) || (c.post >= 0.18f && c.postN >= 0.75f) ||
                               (c.pre >= 0.10f && c.post >= 0.10f && c.pre + c.post >= 0.24f && c.preN >= 0.6f && c.postN >= 0.6f);
            if (covered || !worth || !have(c.t0 - 2 * R, std::min(n, end + 2 * R))) continue;
            nDet++;
            OfdmFrame f;
            if (decode(c, f)) { done.push_back({f.startSample, f.endSample}); out.push_back(std::move(f)); }
            else {
                nFail++;
                if (c.pre >= 0.18f && c.post >= 0.18f && c.preN >= 0.75f && c.postN >= 0.75f) out.push_back(std::move(f));   // well acquired: worth a row (OfdmReceiver drops it if anything decoded there)
            }
        }
        while (done.size() > 64) done.erase(done.begin());
    }

    void push(const cf* x, size_t k) {
        for (size_t i = 0; i < k; i++) { ring[(size_t)(n & kRingMask)] = x[i]; n++; }
        computeRows();
        service(false);
    }
};

MfskReceiver::MfskReceiver(int mode) : p_(std::make_unique<Impl>(mode)) {}
MfskReceiver::~MfskReceiver() = default;
void MfskReceiver::reset() { p_->reset(); }
void MfskReceiver::push(const cf* x, size_t n) { p_->push(x, n); }
void MfskReceiver::flush() {
    if (p_->clPre.on) { p_->anchor(p_->clPre, false); p_->clPre.on = false; }
    if (p_->clPost.on) { p_->anchor(p_->clPost, true); p_->clPost.on = false; }
    p_->service(true);
}
std::vector<OfdmFrame> MfskReceiver::take() { std::vector<OfdmFrame> v; v.swap(p_->out); return v; }
void MfskReceiver::addDecodedSpan(int64_t a, int64_t b) { p_->addSpan(a, b); }
uint64_t MfskReceiver::detections() const { return p_->nDet; }
uint64_t MfskReceiver::failures() const { return p_->nFail; }

} // namespace mercury
} // namespace dect2
