// TETRA TEDS identity and signalling (tetra_teds.h).
#include "dect2/tetra/tetra_teds.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2::tetra {

// ---------------------------------------------------------------- extended services broadcast

bool parseExtServices(int optFlag, uint32_t v, ExtServices& e) {
    if (optFlag != 3) return false;
    e = ExtServices();
    e.security = (int)((v >> 12) & 0xFF);
    e.sdsTl = (int)((v >> 10) & 3);
    e.gck = (v >> 9) & 1;
    e.section = (int)((v >> 7) & 3);
    e.bits = (int)(v & 0x7F);
    if (e.section == 0) {
        e.dataPriority = (e.bits >> 6) & 1; e.extAdvLinks = (e.bits >> 5) & 1; e.qos = (e.bits >> 4) & 1; e.d8psk = (e.bits >> 3) & 1;
        e.sec2 = (e.bits >> 2) & 1; e.sec3 = (e.bits >> 1) & 1; e.sec4 = e.bits & 1;
    } else if (e.section == 1) {
        e.qamMask = ((e.bits >> 6) & 1) | (((e.bits >> 5) & 1) << 1) | (((e.bits >> 4) & 1) << 2) | (((e.bits >> 3) & 1) << 3);
    }
    return true;
}

uint32_t encodeExtServices(const ExtServices& e) {
    int bits = e.bits;
    if (e.section == 0)
        bits = (e.dataPriority << 6) | (e.extAdvLinks << 5) | (e.qos << 4) | (e.d8psk << 3) | (e.sec2 << 2) | (e.sec3 << 1) | (int)e.sec4;
    else if (e.section == 1)
        bits = ((e.qamMask & 1) << 6) | (((e.qamMask >> 1) & 1) << 5) | (((e.qamMask >> 2) & 1) << 4) | (((e.qamMask >> 3) & 1) << 3);
    return ((uint32_t)(e.security & 0xFF) << 12) | ((uint32_t)(e.sdsTl & 3) << 10) | ((uint32_t)e.gck << 9) | ((uint32_t)(e.section & 3) << 7) |
           (uint32_t)(bits & 0x7F);
}

void TedsCaps::add(const ExtServices& e) {
    if (e.section == 0) {
        d8psk = e.d8psk ? 1 : 0;
        if (!e.sec2 && qamMask < 0) qamMask = 0;   // "no information in section 2": no QAM announced
    } else if (e.section == 1) qamMask = e.qamMask;
}

const char* carrierKindText(CarrierKind k) {
    switch (k) {
        case CarrierKind::Dqpsk: return "pi/4-DQPSK";
        case CarrierKind::D8psk: return "pi/8-D8PSK";
        case CarrierKind::Qam: return "QAM";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------- carrier identification

namespace {

int log2Floor(size_t v) { int k = 0; while ((size_t(2) << k) <= v) k++; return k; }

// root raised cosine magnitude response at f (Hz) for symbol rate rs and roll-off a
double rrcMag(double f, double rs, double a) {
    const double af = std::fabs(f), f1 = rs * (1 - a) / 2, f2 = rs * (1 + a) / 2;
    if (af <= f1) return 1;
    if (af >= f2) return 0;
    return std::sqrt(0.5 * (1 + std::cos(M_PI / (a * rs) * (af - f1))));
}

// One band of the spectrum X (N bins at fs, natural order) around fc, shaped by a root raised cosine (rs, a), brought back to the time
// domain with M2 points (rate M2 * fs / N)
std::vector<cf32> band(const std::vector<float>& re, const std::vector<float>& im, int N, double fs, double fc, double rs, double a, int M2) {
    std::vector<float> br((size_t)M2, 0.f), bi((size_t)M2, 0.f);
    const double bin = fs / N;
    const int half = (int)std::ceil(rs * (1 + a) / 2 / bin) + 1;
    const int c = (int)std::lround(fc / bin);
    for (int k = -half; k <= half; k++) {
        if (std::abs(k) >= M2 / 2) continue;
        const int src = ((c + k) % N + N) % N, dst = (k % M2 + M2) % M2;
        const float g = (float)rrcMag((c + k) * bin - fc, rs, a);
        br[(size_t)dst] = re[(size_t)src] * g; bi[(size_t)dst] = im[(size_t)src] * g;
    }
    fftSplit(br.data(), bi.data(), log2Floor((size_t)M2), true);
    std::vector<cf32> out((size_t)M2);
    for (int i = 0; i < M2; i++) out[(size_t)i] = cf32(br[(size_t)i], bi[(size_t)i]);
    return out;
}

cf32 lerp(const std::vector<cf32>& v, double p) {
    const long i = (long)std::floor(p);
    if (i < 0 || i + 1 >= (long)v.size()) return cf32(0, 0);
    const float f = (float)(p - (double)i);
    return v[(size_t)i] * (1 - f) + v[(size_t)i + 1] * f;
}

} // namespace

TedsIdentifier::TedsIdentifier(const cf32* x, size_t n, double fs) : fs_(fs) {
    // up to 0.35 s (and 2^21 samples), a power of two
    const size_t use = std::min(std::min(n, (size_t)(0.35 * fs)), size_t(1) << 21);
    if (!x || fs <= 0 || use < 4096) return;
    const int lg = log2Floor(use);
    N_ = 1 << lg;
    re_.resize((size_t)N_); im_.resize((size_t)N_);
    for (int i = 0; i < N_; i++) { re_[(size_t)i] = x[i].real(); im_[(size_t)i] = x[i].imag(); }
    fftSplit(re_.data(), im_.data(), lg, false);
}

TedsCarrierClass classifyCarrier(const cf32* x, size_t n, double fs, double offHz, double widthHz) { return TedsIdentifier(x, n, fs).classify(offHz, widthHz); }

TedsCarrierClass TedsIdentifier::classify(double offHz, double widthHz) const {
    TedsCarrierClass r;
    if (N_ == 0) return r;
    const int N = N_;
    const double fs = fs_;
    const std::vector<float>& re = re_;
    const std::vector<float>& im = im_;
    const double bin = fs / N;
    // the spectrum around the candidate, smoothed to about 300 Hz
    const int sm = std::max(1, (int)std::lround(300.0 / bin));
    auto pAt = [&](double f) {
        const int c = (int)std::lround(f / bin);
        double s = 0;
        for (int k = -sm; k <= sm; k++) { const size_t q = (size_t)(((c + k) % N + N) % N); s += (double)re[q] * re[q] + (double)im[q] * im[q]; }
        return s / (2 * sm + 1);
    };
    const double look = std::max(200e3, 2 * widthHz);
    std::vector<double> ps;
    for (double f = -look / 2; f <= look / 2; f += 300) ps.push_back(pAt(offHz + f));
    std::vector<double> sorted = ps;
    std::sort(sorted.begin(), sorted.end());
    const double floorP = sorted[sorted.size() / 10];
    // in-band level: the median over the middle 8 kHz
    std::vector<double> mid;
    for (double f = -4e3; f <= 4e3; f += 300) mid.push_back(pAt(offHz + f));
    std::sort(mid.begin(), mid.end());
    const double inband = mid[mid.size() / 2];
    if (inband < floorP * 2) return r;   // less than 3 dB over the floor
    r.snrDb = (float)(10 * std::log10(std::max(1e-12, inband / floorP - 1)));
    // occupied width: contiguous from the centre while above floor + half of the rise
    const double thr = floorP + 0.5 * (inband - floorP);
    double lo = 0, hi = 0;
    for (double f = 0; f < look / 2; f += 150) { if (pAt(offHz + f) < thr && pAt(offHz + f + 450) < thr) break; hi = f; }
    for (double f = 0; f < look / 2; f += 150) { if (pAt(offHz - f) < thr && pAt(offHz - f - 450) < thr) break; lo = f; }
    const double width = lo + hi;
    const double centre = offHz + (hi - lo) / 2;

    // phase modulation at 18 ksym/s: the fourth (pi/4-DQPSK) or eighth (pi/8-D8PSK) power of the one-symbol differential
    float r4 = 0, r8 = 0;
    {
        int M2 = 1;
        while (M2 * bin < 4 * 18000.0) M2 <<= 1;
        const std::vector<cf32> s = band(re, im, N, fs, centre, kSymRate, kRolloff, M2);
        const double sps = M2 * bin / kSymRate;
        for (int ph = 0; ph < 8; ph++) {
            std::complex<double> a4(0, 0), a8(0, 0);
            double m4 = 0;
            cf32 prev(0, 0);
            for (double p = ph * sps / 8; p + 1 < (double)s.size(); p += sps) {
                const cf32 z = lerp(s, p), d = z * std::conj(prev);
                prev = z;
                const std::complex<double> d2 = std::complex<double>(d) * std::complex<double>(d), d4 = d2 * d2;
                a4 += d4; a8 += d4 * d4 / (std::abs(d4) + 1e-30); m4 += std::abs(d4);
            }
            if (m4 > 0) { r4 = std::max(r4, (float)(std::abs(a4) / m4)); r8 = std::max(r8, (float)(std::abs(a8) / m4 * 1.0)); }
        }
    }
    // QAM: 8 sub-carriers per 25 kHz at 2 400 symbols/s. Each sub-carrier's matched filter output is sampled on a grid of 16 timing phases;
    // the energy peaks at the symbol instants, and all the sub-carriers of a carrier share one symbol clock.
    float qamScore = 0;
    int qamM = 0;
    double qamKurt = 0, qamSnr = 0;
    for (int M : {8, 16, 32, 48}) {
        const double occ = M * kQamSpacingHz;
        if (width < 0.7 * occ || width > 1.3 * occ) continue;
        int M2 = 1;
        while (M2 * bin < 8 * kQamSymRate) M2 <<= 1;
        const double sps = M2 * bin / kQamSymRate;
        std::vector<std::vector<double>> e(M, std::vector<double>(16, 0.0));
        std::vector<std::vector<cf32>> sub(M);
        for (int m = 0; m < M; m++) {
            const double fm = (0.5625 - (M / 2.0 - m) * 1.125) * kQamSymRate;
            sub[m] = band(re, im, N, fs, centre + fm, kQamSymRate, kQamRolloff, M2);
            for (int ph = 0; ph < 16; ph++)
                for (double p = ph * sps / 16 + 8 * sps; p + 8 * sps < (double)sub[m].size(); p += sps) e[m][ph] += std::norm(lerp(sub[m], p));
        }
        // the common clock: the sum of the energy curves; the score is the depth of its modulation and how many sub-carriers agree
        std::vector<double> tot(16, 0.0);
        for (int m = 0; m < M; m++) for (int ph = 0; ph < 16; ph++) tot[ph] += e[m][ph];
        const int best = (int)(std::max_element(tot.begin(), tot.end()) - tot.begin());
        const double mx = tot[best], mn = *std::min_element(tot.begin(), tot.end());
        int agree = 0;
        for (int m = 0; m < M; m++) {
            const int bm = (int)(std::max_element(e[m].begin(), e[m].end()) - e[m].begin());
            const int d = std::abs(bm - best);
            if (std::min(d, 16 - d) <= 2) agree++;
        }
        const float depth = mx > 0 ? (float)((mx - mn) / mx) : 0.f;
        const float score = agree * 4 >= M * 3 ? depth : 0.f;   // three quarters of the sub-carriers on the common clock
        if (score > qamScore) {
            qamScore = score; qamM = M;
            // the constellation: the normalised fourth moment of the symbols at the common clock, the noise taken out (from the floor)
            double s2 = 0, s4 = 0, cnt = 0;
            for (int m = 0; m < M; m++)
                for (double p = best * sps / 16 + 8 * sps; p + 8 * sps < (double)sub[m].size(); p += sps) {
                    const double v = std::norm(lerp(sub[m], p));
                    s2 += v; s4 += v * v; cnt++;
                }
            s2 /= cnt; s4 /= cnt;
            const double snrLin = std::max(1e-3, inband / floorP - 1);
            const double Nn = s2 / (1 + snrLin), S = s2 - Nn;
            qamKurt = S > 0 ? (s4 - 4 * S * Nn - 2 * Nn * Nn) / (S * S) : 0;
            qamSnr = 10 * std::log10(snrLin);
        }
    }
    r.score = std::max(qamScore, std::max(r4, r8));
    // QAM: the symbol clock shows (an RRC 0.2 pulse modulates the matched filter energy by about 10 %) and the sub-carrier symbols are not
    // Gaussian (a pi/4-DQPSK carrier cut into 2.7 kHz slices gives a fourth moment of about 2)
    if (qamScore > 0.04f && qamKurt > 0.8 && qamKurt < 1.6 && r4 < 0.5f && r8 < 0.4f) {
        r.kind = CarrierKind::Qam;
        r.subcarriers = qamM;
        r.bwHz = qamM / 8 * 25e3;
        // 4-QAM (and the unit-circle pilots): 1.0; 16-QAM 1.32, 64-QAM 1.38 (less with the pilots mixed in)
        if (qamKurt > 0 && qamKurt < 1.13) { r.qamOrder = 4; r.orderSure = qamSnr > 12; }
        else if (qamKurt >= 1.13) {
            r.qamOrder = 16;
            if (qamSnr > 28) { r.orderSure = true; if (qamKurt > 1.31) r.qamOrder = 64; }
        }
    } else if (r4 > 0.5f) {
        r.kind = CarrierKind::Dqpsk; r.bwHz = 25e3;
    } else if (r8 > 0.4f) {
        r.kind = CarrierKind::D8psk; r.bwHz = 25e3;
    }
    return r;
}

// ---------------------------------------------------------------- QAM test signal

namespace { constexpr int kQSps = 96, kQSpan = 8; }

QamGen::QamGen(int bwKHz, int order, uint32_t seed) : M_(std::max(1, bwKHz / 25) * 8), order_(order), rng_(seed ? seed : 1) {
    if (M_ != 8 && M_ != 16 && M_ != 32 && M_ != 48) M_ = 8;
    // root raised cosine 0.2 at 96 samples per symbol, +-kQSpan symbols
    const int L = 2 * kQSpan * kQSps + 1;
    h_.resize((size_t)L);
    const double a = kQamRolloff;
    double en = 0;
    for (int i = 0; i < L; i++) {
        const double t = (double)(i - kQSpan * kQSps) / kQSps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - a + 4 * a / M_PI;
        else if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9)
            v = a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        else v = (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - (4 * a * t) * (4 * a * t)));
        h_[(size_t)i] = (float)v;
        en += v * v;
    }
    const float g = (float)(1.0 / std::sqrt(en / kQSps * M_));   // M unit-power sub-carriers: unit mean power in all
    for (auto& v : h_) v *= g;
    hist_.assign((size_t)M_, std::vector<cf32>((size_t)(2 * kQSpan + 1), cf32(0, 0)));
    ph_.assign((size_t)M_, 0.0);
}

cf32 QamGen::dataSymbol() {
    auto rnd = [&]() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return rng_ >> 16; };   // xorshift32
    const int lv = order_ == 64 ? 8 : order_ == 16 ? 4 : 2;
    const float sc = order_ == 64 ? 1.f / std::sqrt(42.f) : order_ == 16 ? 1.f / std::sqrt(10.f) : 1.f / std::sqrt(2.f);
    const int a = (int)(rnd() % (unsigned)lv), b = (int)(rnd() % (unsigned)lv);
    return cf32((float)(2 * a - lv + 1) * sc, (float)(2 * b - lv + 1) * sc);
}

void QamGen::push(int symbols, std::vector<cf32>& out) {
    const int L = 2 * kQSpan + 1;
    for (int s = 0; s < symbols; s++, sym_++) {
        for (int m = 0; m < M_; m++) {
            auto& h = hist_[(size_t)m];
            std::rotate(h.begin(), h.begin() + 1, h.end());
            cf32 v;
            if (sym_ % 8 == 0) { const float ang = (float)(((sym_ / 8) * 7 + m * 3) % 16) * (float)M_PI / 8; v = cf32(std::cos(ang), std::sin(ang)); }   // pilot
            else v = dataSymbol();
            h[(size_t)(L - 1)] = v;
        }
        for (int j = 0; j < kQSps; j++) {
            cf32 y(0, 0);
            for (int m = 0; m < M_; m++) {
                const auto& h = hist_[(size_t)m];
                cf32 acc(0, 0);
                for (int k = 0; k < L; k++) {
                    const int idx = -kQSps * (k - kQSpan) + j + kQSpan * kQSps;
                    if (idx < 0 || idx >= (int)h_.size()) continue;
                    acc += h[(size_t)k] * h_[(size_t)idx];
                }
                const double fm = (0.5625 - (M_ / 2.0 - m) * 1.125) * kQamSymRate;
                double& ph = ph_[(size_t)m];
                y += acc * cf32((float)std::cos(ph), (float)std::sin(ph));
                ph = std::fmod(ph + 2 * M_PI * fm / kQamGenRate, 2 * M_PI);
            }
            out.push_back(y);
        }
    }
}

} // namespace dect2::tetra
