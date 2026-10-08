#include "dect2/ais_phy.h"
#include "dect2/dmr_dsp.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;

// phase (unwrapped, in radians) at a fractional sample position
struct PhaseTrack {
    std::vector<double> ph;      // ph[i]: sum of the phase steps up to sample i
    double at(double t) const {
        if (t <= 0) return ph.front();
        const double last = (double)ph.size() - 1;
        if (t >= last) return ph.back();
        const size_t i = (size_t)t;
        const double f = t - (double)i;
        return ph[i] * (1 - f) + ph[i + 1] * f;
    }
};

struct Slice {
    std::vector<double> m;       // per bit: frequency integral minus the offset, radians (positive = frequency above the centre)
    int first = 0;               // index of the bit that m[0] belongs to (the bit time is tau + 5 * (first + i))
};

// Integrate and dump: the frequency over one bit as the phase change across a window of 'w' samples centred on the bit
Slice sliceBits(const PhaseTrack& pt, double tau, double f0, double w, size_t n) {
    Slice s;
    const double half = w / 2;
    const int k0 = (int)std::ceil((half - tau) / kAisSps);
    const int k1 = (int)std::floor(((double)n - 1 - half - tau) / kAisSps);
    s.first = k0;
    for (int k = k0; k <= k1; k++) {
        const double t = tau + kAisSps * k;
        s.m.push_back(pt.at(t + half) - pt.at(t - half) - f0 * w);
    }
    return s;
}

// The frequency pulse of GMSK (BT 0.4) is three bits wide: the integral over one bit is alpha * a[k] + beta * (a[k-1] + a[k+1]) with a = +-1
// (alpha 0.74 and beta 0.13 of the full swing before the receiver's filters). A four state Viterbi search over the line bits finds the sequence that
// fits the integrals best, so alternating bits (the training sequence, many flags) no longer lose half of their eye opening.
void mlseBits(const std::vector<double>& y, double al, double be, ais::Bits& out) {
    const size_t K = y.size();
    out.assign(K, 0);
    if (K < 3) return;
    std::vector<std::array<uint8_t, 4>> back(K + 1);
    double cost[4] = {0, 0, 0, 0}, nc[4];
    for (size_t k = 0; k < K; k++) {
        for (int ns = 0; ns < 4; ns++) nc[ns] = 1e300;
        for (int st = 0; st < 4; st++) {                       // st = (a[k-1] << 1) | a[k]
            const double pm = (st >> 1) ? 1.0 : -1.0, cu = (st & 1) ? 1.0 : -1.0;
            for (int nb = 0; nb < 2; nb++) {
                const double nx = nb ? 1.0 : -1.0;
                const double e = y[k] - (al * cu + be * (pm + nx));
                const double c = cost[st] + e * e;
                const int to = ((st & 1) << 1) | nb;
                if (c < nc[to]) { nc[to] = c; back[k + 1][to] = (uint8_t)st; }
            }
        }
        for (int i = 0; i < 4; i++) cost[i] = nc[i];
    }
    int st = 0;
    for (int i = 1; i < 4; i++) if (cost[i] < cost[st]) st = i;
    for (size_t k = K; k > 0; k--) {
        st = back[k][st];
        out[k - 1] = (uint8_t)(st & 1);                       // state before step k-1 is (a[k-2], a[k-1])
    }
}

} // namespace

static AisBurstResult decodeOnce(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg) {
    AisBurstResult res;
    if (n < 100) return res;
    PhaseTrack pt;
    pt.ph.assign(n, 0.0);
    std::vector<double> step(n, 0.0);
    for (size_t i = 1; i < n; i++) {
        const cf32 p = x[i] * std::conj(x[i - 1]);
        step[i] = std::atan2((double)p.imag(), (double)p.real());
        pt.ph[i] = pt.ph[i - 1] + step[i];
    }
    // mean frequency inside the burst, away from its edges
    const size_t margin = 24;
    size_t a = std::min(core0 + margin, n - 1), b = core1 > margin ? std::min(core1 - margin, n) : 0;
    if (b <= a + 20) { a = std::min(core0, n - 1); b = std::min(core1, n); }
    if (b <= a + 20) return res;
    double f0 = 0;
    for (size_t i = a; i < b; i++) f0 += step[i];
    f0 /= (double)(b - a);

    // eye opening for each timing phase: the mean of |integral| inside the burst
    struct Hyp { double tau, metric; };
    std::vector<Hyp> hyp;
    for (int ti = 0; ti < 10; ti++) {
        const double tau = 0.5 * ti;
        const Slice s = sliceBits(pt, tau, f0, cfg.intWidth, n);
        double sum = 0; int cnt = 0;
        for (size_t i = 0; i < s.m.size(); i++) {
            const double t = tau + kAisSps * (s.first + (int)i);
            if (t < (double)a || t > (double)b) continue;
            sum += std::fabs(s.m[i]); cnt++;
        }
        hyp.push_back({tau, cnt ? sum / cnt : 0.0});
    }
    std::sort(hyp.begin(), hyp.end(), [](const Hyp& p, const Hyp& q) { return p.metric > q.metric; });

    const int tries = std::min<int>(cfg.hypotheses, (int)hyp.size());
    for (int h = 0; h < tries; h++) {
        res.tauTried++;
        const double tau = hyp[h].tau;
        double off = f0;
        Slice s = sliceBits(pt, tau, off, cfg.intWidth, n);
        for (int it = 0; it < cfg.refine; it++) {
            // the two levels of the frequency (marks and spaces), their middle is the slicer
            double hi = 0, lo = 0; int nh = 0, nl = 0;
            for (size_t i = 0; i < s.m.size(); i++) {
                const double t = tau + kAisSps * (s.first + (int)i);
                if (t < (double)a || t > (double)b) continue;
                if (s.m[i] > 0) { hi += s.m[i]; nh++; } else { lo += s.m[i]; nl++; }
            }
            if (!nh || !nl) break;
            const double mid = 0.5 * (hi / nh + lo / nl) / cfg.intWidth;
            if (std::fabs(mid) < 1e-6) break;
            off += mid;
            s = sliceBits(pt, tau, off, cfg.intWidth, n);
        }
        ais::Bits line(s.m.size());
        for (size_t i = 0; i < s.m.size(); i++) line[i] = s.m[i] > 0;
        const ais::Bits dec = ais::nrziDecode(line, line.empty() ? 1 : line[0]);
        ais::HdlcResult r = ais::hdlcFrames(dec);
        if (r.good.empty() && cfg.mlse && s.m.size() > 40) {
            // fit alpha and beta to the hard decisions inside the burst, then search for the best sequence
            std::vector<double> y(s.m.size());
            for (size_t i = 0; i < y.size(); i++) y[i] = s.m[i] / (kPi / 2);
            ais::Bits cur = line;
            for (int pass = 0; pass < 2; pass++) {
                double saa = 0, sab = 0, sbb = 0, sya = 0, syb = 0;
                for (size_t i = 1; i + 1 < cur.size(); i++) {
                    const double t = tau + kAisSps * (s.first + (int)i);
                    if (t < (double)a || t > (double)b) continue;
                    const double ak = cur[i] ? 1 : -1, nb = (cur[i - 1] ? 1 : -1) + (cur[i + 1] ? 1 : -1);
                    saa += ak * ak; sab += ak * nb; sbb += nb * nb; sya += y[i] * ak; syb += y[i] * nb;
                }
                const double det = saa * sbb - sab * sab;
                if (std::fabs(det) < 1e-9) break;
                const double al = (sya * sbb - syb * sab) / det, be = (syb * saa - sya * sab) / det;
                if (al < 0.2) break;
                ais::Bits ml;
                mlseBits(y, al, be, ml);
                const ais::Bits d2 = ais::nrziDecode(ml, ml.empty() ? 1 : ml[0]);
                ais::HdlcResult r2 = ais::hdlcFrames(d2);
                if (!r2.good.empty()) { r = std::move(r2); break; }
                if (ml == cur) break;
                cur = ml;
            }
        }
        if (h == 0) res.bad = r.bad;
        if (!r.good.empty()) {
            res.frames = std::move(r.good);
            res.bad = 0;
            res.tauOk = (int)std::lround(tau * 10);
            res.cfoHz = off * kAisWorkRate / (2 * kPi);
            return res;
        }
    }
    res.cfoHz = f0 * kAisWorkRate / (2 * kPi);
    return res;
}

// Two passes: the channel filter of the receiver has to pass every carrier offset, so it is wide. Once the burst's own frequency is known, the burst is
// shifted to zero and filtered again with a narrower filter, which takes away noise (about 1.2 dB at 90 % of the bursts) whatever the offset was.
AisBurstResult aisDecodeBurst(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg) {
    if (cfg.narrowPassHz > 0 && n > 200 && core1 > core0 + 40) {
        // frequency of the burst: the mean phase step inside it
        const size_t margin = 24;
        const size_t a = std::min(core0 + margin, n - 2), b = core1 > margin ? std::min(core1 - margin, n) : 0;
        if (b > a + 20) {
            double acc = 0;
            cf32 sum(0, 0);
            for (size_t i = a; i < b; i++) sum += x[i] * std::conj(x[i - 1]);
            acc = std::atan2((double)sum.imag(), (double)sum.real());
            const std::vector<float> h = dmr::lowpassTaps(cfg.narrowPassHz, cfg.narrowStopHz, kAisWorkRate, 60, 9);
            const int m = (int)h.size() / 2;
            std::vector<cf32> rot(n), y(n);
            for (size_t i = 0; i < n; i++) rot[i] = x[i] * std::polar(1.0f, (float)(-acc * (double)i));
            for (size_t i = 0; i < n; i++) {
                float re = 0, im = 0;
                const long lo = std::max<long>(0, (long)i - m), hi = std::min<long>((long)n - 1, (long)i + m);
                for (long k = lo; k <= hi; k++) { const float hv = h[(size_t)(k - (long)i + m)]; re += hv * rot[(size_t)k].real(); im += hv * rot[(size_t)k].imag(); }
                y[i] = cf32(re, im);
            }
            AisBurstResult r = decodeOnce(y.data(), n, core0, core1, cfg);
            r.cfoHz += acc * kAisWorkRate / (2 * kPi);
            r.narrow = true;
            if (!r.frames.empty()) return r;
            AisBurstResult w = decodeOnce(x, n, core0, core1, cfg);     // the wide pass as the second chance
            if (!w.frames.empty() || r.bad <= w.bad) return w;
            r.narrow = false;
            return r;
        }
    }
    return decodeOnce(x, n, core0, core1, cfg);
}

} // namespace dect2
