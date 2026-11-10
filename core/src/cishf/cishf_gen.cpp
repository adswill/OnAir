// CIS HF test generators and test impairments (cishf_dsp.h). The test signals follow the published parameters; where a detail is not published
// (the BEE start word, the CIS-14 parity rule, the CIS-12 pulse shape, the CIS-128 carrier spacing and guard) the generator picks a value that fits
// the published figures, and docs/CIS-HF-3.9.md says which.
#include "dect2/cishf/cishf_dsp.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2::cishf {

namespace {
constexpr double kPi = 3.14159265358979323846;

// continuous-phase 2-FSK of a bit list at one rate, appended to out; phase carried in ph
void fskBits(std::vector<cf32>& out, const std::vector<uint8_t>& bits, double baud, const GenOpts& o, double& ph, double& tFrac) {
    const double sps = kRate / baud;
    double t = tFrac;   // samples into the current bit
    for (uint8_t b : bits) {
        const bool high = o.invert ? b != 0 : b == 0;
        const double f = o.centreHz + (high ? 0.5 : -0.5) * o.shiftHz;
        const double w = 2 * kPi * f / kRate;
        while (t < sps) { out.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.5f); ph += w; if (ph > kPi) ph -= 2 * kPi; t += 1; }
        t -= sps;
    }
    tFrac = t;
}
void silence(std::vector<cf32>& out, double sec) { out.insert(out.end(), (size_t)(sec * kRate), cf32(0, 0)); }
void push7(std::vector<uint8_t>& b, int c) { for (int i = 6; i >= 0; i--) b.push_back((uint8_t)((c >> i) & 1)); }   // most significant bit first
}  // namespace

const char* kindName(int k) {
    static const char* n[kKinds] = {"CIS-36-50 (BEE)", "CIS-14", "CIS-12", "CIS-36", "CIS OFDM HDR", "CIS-128", "CIS-3000"};
    return k >= 0 && k < kKinds ? n[k] : "CIS";
}

std::vector<cf32> genBee(const GenOpts& o, const std::vector<BeeMsg>& msgs) {
    std::vector<cf32> out;
    silence(out, o.leadSec);
    double ph = 0, tf = 0;
    std::vector<uint8_t> idle;
    for (int i = 0; i < (int)(o.idleSec * o.idleBaud); i++) idle.push_back((uint8_t)(i & 1));
    fskBits(out, idle, o.idleBaud, o, ph, tf);
    std::vector<uint8_t> b;
    for (int i = 0; i < (int)(0.6 * o.trafficBaud); i++) b.push_back((uint8_t)(i & 1));   // bit sync at the traffic rate
    for (const BeeMsg& m : msgs) {
        if (m.withStart) {
            uint64_t s = m.sync ? m.sync : 0xA5C3B1E4D21ull & ((1ull << 44) - 1);
            for (int i = 43; i >= 0; i--) b.push_back((uint8_t)((s >> i) & 1));
            for (int r = 0; r < 2; r++) for (int c : m.key) push7(b, c);
        }
        for (int c : m.chars) push7(b, c);
        for (int r = 0; r < 4; r++) push7(b, kBeeEom);
    }
    fskBits(out, b, o.trafficBaud, o, ph, tf);
    silence(out, o.tailSec);
    return out;
}

uint16_t cis14Frame(int state2, int a5, int b5, int parity2) {
    uint16_t f = (uint16_t)(state2 & 3);
    for (int i = 0; i < 5; i++) {
        f |= (uint16_t)(((a5 >> (4 - i)) & 1) << (2 + 2 * i));
        f |= (uint16_t)(((b5 >> (4 - i)) & 1) << (3 + 2 * i));
    }
    f |= (uint16_t)((parity2 & 3) << 12);
    return f;
}

std::vector<cf32> genCis14(const GenOpts& o, const std::vector<uint16_t>& frames) {
    std::vector<cf32> out;
    silence(out, o.leadSec);
    std::vector<uint8_t> b;
    for (uint16_t f : frames) for (int i = 0; i < 14; i++) b.push_back((uint8_t)((f >> i) & 1));
    double ph = 0, tf = 0;
    fskBits(out, b, 96.0, o, ph, tf);
    silence(out, o.tailSec);
    return out;
}

std::vector<cf32> genCis12(const GenOpts& o, std::vector<std::vector<uint8_t>>* steps) {
    const int order = o.variant == 1 ? 2 : 4;
    const int T = (int)std::lround(kRate / 120.0);
    std::mt19937 rng(o.seed);
    std::vector<cf32> out;
    silence(out, o.leadSec);
    const float a = 0.5f / std::sqrt(13.f);
    double phPil = 0;
    const double wp = 2 * kPi * 3300 / kRate;
    double ph[12];
    for (int k = 0; k < 12; k++) ph[k] = 2 * kPi * (double)(rng() % 1000) / 1000.0;
    size_t n0 = out.size();
    // 0.5 s pilot only (standby), then the symbols
    for (int i = 0; i < (int)(0.5 * kRate); i++) { out.push_back(cf32((float)std::cos(phPil), (float)std::sin(phPil)) * a); phPil += wp; }
    if (steps) steps->clear();
    for (int m = 0; m < o.symbols; m++) {
        std::vector<uint8_t> st(12);
        for (int k = 0; k < 12; k++) { st[k] = (uint8_t)(rng() % (unsigned)order); ph[k] += 2 * kPi * st[k] / order; }
        if (steps) steps->push_back(st);
        for (int i = 0; i < T; i++) {
            const double t = (double)(out.size() - n0);
            cf32 v = cf32((float)std::cos(phPil), (float)std::sin(phPil)) * a;
            phPil += wp;
            for (int k = 0; k < 12; k++) {
                const double f = 700 + 200 * k;
                const double p = 2 * kPi * f * t / kRate + ph[k];
                v += cf32((float)std::cos(p), (float)std::sin(p)) * a;
            }
            out.push_back(v);
        }
    }
    silence(out, o.tailSec);
    return out;
}

std::vector<cf32> genCis36(const GenOpts& o) {
    const double baud = o.trafficBaud == 10 || o.trafficBaud == 20 || o.trafficBaud == 40 ? o.trafficBaud : 40;
    std::mt19937 rng(o.seed);
    std::vector<cf32> out;
    silence(out, o.leadSec);
    // tones 1 .. 36, 40 Hz apart, centred on centreHz; control tones 1, 12, 24, 36 only rarely, so the groups 2-11, 13-23, 25-35 show
    const double f1 = o.centreHz - 17.5 * 40;
    std::vector<int> grp;
    for (int t = 1; t <= 36; t++) if (t != 1 && t != 12 && t != 24 && t != 36) grp.push_back(t);
    const double sps = kRate / baud;
    double ph = 0, t = 0;
    for (int m = 0; m < o.symbols; m++) {
        static const int ctl[4] = {1, 12, 24, 36};
        const int tone = rng() % 60 == 0 ? ctl[rng() % 4] : grp[rng() % grp.size()];
        const double w = 2 * kPi * (f1 + (tone - 1) * 40) / kRate;
        while (t < sps) { out.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.5f); ph += w; if (ph > kPi) ph -= 2 * kPi; t += 1; }
        t -= sps;
    }
    silence(out, o.tailSec);
    return out;
}

std::vector<cf32> genOfdm(int kind, const GenOpts& o) {
    // published: CIS-45 Tu 16 ms + Tg 9 ms (40 Bd) or 33.3 Bd, BPSK; CIS-60 Tu 22.5 ms + 5.625 ms, 8-PSK; CIS-93 Tu 32 ms + 13 ms, QPSK.
    // CIS-128: 128 carriers (centre one off) at about 21 Bd in 3.1 kHz; spacing and guard are not published: 24 Hz and 1/21 s are used here.
    int n = 45, order = 2, gap = -1;
    double tu = 0.016, ts = 0.025, centre = o.centreHz > 0 ? o.centreHz : 1650;
    bool pilot = true;
    if (kind == Hdr) {
        if (o.variant == 1) ts = 0.030;
        else if (o.variant == 2) { n = 60; order = 8; tu = 0.0225; ts = 0.028125; }
        else if (o.variant == 3) { n = 93; order = 4; tu = 0.032; ts = 0.045; }
    } else {
        n = 128; order = 4; gap = 64; pilot = false;
        tu = 1.0 / 24; ts = 1.0 / 21;
        if (o.variant == 1) { tu = 1.0 / 48; ts = 1.0 / 42; centre = 3300; }
    }
    const double df = 1.0 / tu;
    const int kLo = (int)std::lround((centre - (n - 1) / 2.0 * df) / df);   // carriers on whole multiples of df: the guard is then a cyclic prefix
    std::mt19937 rng(o.seed);
    std::vector<cf32> out;
    silence(out, o.leadSec);
    const float amp = 0.5f / std::sqrt((float)n);
    const size_t n0 = out.size();
    std::vector<cf32> sym(n);
    for (int m = 0; m < o.symbols; m++) {
        for (int k = 0; k < n; k++) sym[k] = k == gap ? cf32(0, 0) : std::polar(amp, (float)(2 * kPi * (rng() % (unsigned)order) / order + (order == 4 ? kPi / 4 : 0)));
        const size_t s0 = n0 + (size_t)std::llround(m * ts * kRate), s1 = n0 + (size_t)std::llround((m + 1) * ts * kRate);
        for (size_t i = s0; i < s1; i++) {
            const double t = (double)(i - n0) / kRate - m * ts - (ts - tu);   // time inside the useful part (negative in the guard)
            cf32 v(0, 0);
            for (int k = 0; k < n; k++) {
                if (k == gap) continue;
                const double p = 2 * kPi * (kLo + k) * df * t;
                v += sym[k] * cf32((float)std::cos(p), (float)std::sin(p));
            }
            if (pilot) { const double p = 2 * kPi * 3300 * (double)(i - n0) / kRate; v += cf32((float)std::cos(p), (float)std::sin(p)) * (amp * 2); }
            out.push_back(v);
        }
    }
    silence(out, o.tailSec);
    return out;
}

std::vector<cf32> genCis3000(const GenOpts& o) {
    const int sps = 4;   // 12 kHz / 3000 Bd
    const double alpha = 0.15;
    const int span = 8, nt = span * sps + 1;
    std::vector<double> h(nt);
    double e = 0;
    for (int i = 0; i < nt; i++) {
        const double t = (double)(i - nt / 2) / sps;
        double v;
        if (std::fabs(t) < 1e-9) v = 1 - alpha + 4 * alpha / kPi;
        else if (std::fabs(std::fabs(4 * alpha * t) - 1) < 1e-9) v = alpha / std::sqrt(2.0) * ((1 + 2 / kPi) * std::sin(kPi / (4 * alpha)) + (1 - 2 / kPi) * std::cos(kPi / (4 * alpha)));
        else v = (std::sin(kPi * t * (1 - alpha)) + 4 * alpha * t * std::cos(kPi * t * (1 + alpha))) / (kPi * t * (1 - 16 * alpha * alpha * t * t));
        h[i] = v; e += v * v;
    }
    for (auto& v : h) v /= std::sqrt(e);
    std::mt19937 rng(o.seed);
    std::vector<cf32> sy(o.symbols * sps + nt, cf32(0, 0));
    std::vector<cf32> up(o.symbols * sps, cf32(0, 0));
    for (int m = 0; m < o.symbols; m++) up[m * sps] = std::polar(1.f, (float)(2 * kPi * (rng() % 8) / 8));
    for (size_t i = 0; i < up.size(); i++) if (up[i] != cf32(0, 0)) for (int j = 0; j < nt; j++) sy[i + j] += up[i] * (float)h[j];
    std::vector<cf32> out;
    silence(out, o.leadSec);
    const double w = 2 * kPi * (o.centreHz > 0 ? o.centreHz : 1800) / kRate;
    for (size_t i = 0; i < sy.size(); i++) out.push_back(sy[i] * 0.35f * cf32((float)std::cos(w * i), (float)std::sin(w * i)));
    silence(out, o.tailSec);
    return out;
}

std::vector<cf32> impair(const std::vector<cf32>& x, double cfoHz, double snrDb3k, double ppm, uint32_t seed) {
    std::vector<cf32> y;
    const double r = 1 + ppm * 1e-6;
    y.reserve((size_t)(x.size() / r) + 1);
    for (double t = 0; t + 1 < (double)x.size(); t += r) {
        const size_t k = (size_t)t;
        const float f = (float)(t - (double)k);
        y.push_back(x[k] * (1 - f) + x[k + 1] * f);
    }
    double ps = 0; size_t nz = 0;
    for (auto& v : y) { const double p = std::norm(v); if (p > 1e-8) { ps += p; nz++; } }
    ps = nz ? ps / (double)nz : 0;
    const double sig = snrDb3k < 90 ? std::sqrt(4 * ps / std::pow(10.0, snrDb3k / 10) / 2) : 0;
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0, 1);
    for (size_t i = 0; i < y.size(); i++) {
        const double p = 2 * kPi * cfoHz * (double)i / kRate;
        y[i] *= cf32((float)std::cos(p), (float)std::sin(p));
        if (sig > 0) y[i] += cf32((float)(sig * d(rng)), (float)(sig * d(rng)));
    }
    return y;
}

}  // namespace dect2::cishf
