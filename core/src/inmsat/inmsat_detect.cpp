// Carrier detector of the Inmarsat all-carriers mode (inmsat_detect.h). Method as in the Aero receiver's band search (aero_rx.cpp): median floor,
// regions over a threshold, a new carrier must show in two searches in a row; here for any sample rate and with width classes.
#include "dect2/inmsat/inmsat_detect.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>

namespace dect2::inmsat {

const char* carrierClassName(CarrierClass c) {
    switch (c) {
        case CarrierClass::Narrow: return "narrow";
        case CarrierClass::Medium: return "medium";
        default: return "wide";
    }
}
CarrierClass classifyWidth(double w) { return w < 1500 ? CarrierClass::Narrow : w < 4500 ? CarrierClass::Medium : CarrierClass::Wide; }

struct CarrierDetector::Impl {
    DetectorConfig cfg;
    int nfft = 0;
    double bin = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fbuf;
    std::vector<float> acc, accPrev;
    int fpos = 0, slicesDone = 0;
    uint64_t periodLen = 0, periodPos = 0, sliceGap = 0, total = 0;
    uint64_t searches = 0;
    int nextId = 1;
    std::vector<DetectedCarrier> cars;
    std::vector<float> specDb;
    double specStart = 0, floorDbV = -200;

    void configure(const DetectorConfig& c) {
        cfg = c;
        nfft = 1024;
        while (c.rate / nfft > c.binHz && nfft < (1 << 18)) nfft *= 2;
        bin = c.rate / nfft;
        fft = std::make_unique<Fft>(nfft);
        win.resize((size_t)nfft);
        for (int i = 0; i < nfft; i++) win[(size_t)i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265f * (float)(i + 0.5) / (float)nfft);
        fbuf.assign((size_t)nfft, cf32());
        periodLen = (uint64_t)(c.searchSec * c.rate);
        const int sl = std::max(1, c.slices);
        sliceGap = std::max<uint64_t>((uint64_t)nfft, periodLen / (uint64_t)sl);
        reset();
    }
    void reset() {
        acc.assign((size_t)nfft, 0.f);
        accPrev.assign((size_t)nfft, 0.f);
        fpos = 0; slicesDone = 0; periodPos = 0; total = 0; searches = 0; nextId = 1;
        cars.clear(); specDb.clear();
    }
    double now() const { return cfg.rate > 0 ? (double)total / cfg.rate : 0; }
    int slicesPerPeriod() const { return (int)std::max<uint64_t>(1, std::min<uint64_t>((uint64_t)std::max(1, cfg.slices), periodLen / (uint64_t)nfft)); }

    void feed(const cf32* x, size_t n) {
        if (!fft) return;
        const int slices = slicesPerPeriod();
        size_t i = 0;
        while (i < n) {
            const uint64_t inSlice = periodPos % sliceGap;
            if (slicesDone < slices && inSlice < (uint64_t)nfft) {
                const size_t take = std::min((size_t)((uint64_t)nfft - inSlice), n - i);
                for (size_t k = 0; k < take; k++) fbuf[(size_t)fpos + k] = x[i + k] * win[(size_t)fpos + k];
                fpos += (int)take; i += take; periodPos += take; total += take;
                if (fpos == nfft) {
                    fft->forward(fbuf.data());
                    for (int k = 0; k < nfft; k++) acc[(size_t)k] += std::norm(fbuf[(size_t)k]);
                    fpos = 0; slicesDone++;
                }
            } else {
                const uint64_t toNext = slicesDone < slices ? sliceGap - inSlice : periodLen - periodPos;
                const size_t skip = (size_t)std::min<uint64_t>(toNext, n - i);
                i += skip; periodPos += skip; total += skip;
            }
            if (periodPos >= periodLen) {
                if (slicesDone == slices) evaluate();
                periodPos = 0; fpos = 0; slicesDone = 0;
            }
        }
    }

    struct Region { double freq, width; float peak; };

    void evaluate() {
        searches++;
        std::vector<float> p((size_t)nfft);
        for (int k = 0; k < nfft; k++) p[(size_t)k] = acc[(size_t)k] + accPrev[(size_t)k];
        accPrev = acc;
        std::fill(acc.begin(), acc.end(), 0.f);
        // frequency order (j = 0 is -fs/2), smoothed over 3 bins
        std::vector<float> s((size_t)nfft);
        for (int j = 0; j < nfft; j++) {
            const int k = (j + nfft / 2) % nfft;
            const int km = (k - 1 + nfft) % nfft, kp = (k + 1) % nfft;
            s[(size_t)j] = (p[(size_t)km] + p[(size_t)k] + p[(size_t)kp]) / 3;
        }
        auto fOf = [&](int j) { return (j - nfft / 2) * bin; };
        std::vector<char> ok((size_t)nfft, 0);
        std::vector<float> use;
        use.reserve((size_t)nfft);
        for (int j = 0; j < nfft; j++) {
            const double f = fOf(j);
            if (std::fabs(f) > cfg.edgeFraction * cfg.rate) continue;
            if (std::fabs(f) < cfg.dcExcludeHz) continue;
            ok[(size_t)j] = 1;
            use.push_back(s[(size_t)j]);
        }
        if (use.size() < 16) return;
        std::nth_element(use.begin(), use.begin() + (long)(use.size() / 2), use.end());
        const float floorP = std::max(use[use.size() / 2], 1e-30f);
        floorDbV = 10 * std::log10((double)floorP);
        const float thr = floorP * (float)std::pow(10.0, cfg.thresholdDb / 10);
        specStart = fOf(0);
        specDb.resize((size_t)nfft);
        for (int j = 0; j < nfft; j++) specDb[(size_t)j] = 10.f * std::log10(std::max(s[(size_t)j], 1e-30f) / floorP);
        const int gapMax = std::max(2, (int)std::lround(1500.0 / bin));   // a wide carrier's spectrum can break into pieces
        std::vector<Region> regs;
        int j = 0;
        while (j < nfft) {
            if (!ok[(size_t)j] || s[(size_t)j] <= thr) { j++; continue; }
            int a = j, b = j, gap = 0, k = j + 1;
            while (k < nfft && ok[(size_t)k] && (s[(size_t)k] > thr || gap < gapMax)) {
                if (s[(size_t)k] > thr) { b = k; gap = 0; } else gap++;
                k++;
            }
            // neighbouring carriers whose skirts meet above the threshold (Aero C channels sit 8 to 12 kHz apart) would make one region:
            // cut it at every valley at least 5 dB below the two peaks around it, peaks at least 2.5 kHz apart
            std::vector<int> cuts;
            {
                const int minSep = std::max(2, (int)std::lround(2500.0 / bin));
                auto dB = [&](int q) { float v = 0; int n = 0; for (int d = -1; d <= 1; d++) if (q + d >= a && q + d <= b) { v += specDb[(size_t)(q + d)]; n++; } return v / (float)n; };
                int lastPeak = -1;
                for (int q = a + 1; q < b; q++) {
                    if (!(dB(q) > dB(q - 1) && dB(q) >= dB(q + 1))) continue;        // a local maximum
                    if (lastPeak >= 0 && q - lastPeak >= minSep) {
                        int v = lastPeak; float vmin = dB(lastPeak);
                        for (int u = lastPeak + 1; u < q; u++) if (dB(u) < vmin) { vmin = dB(u); v = u; }
                        // a valley between two carriers is at least 1 kHz wide; a C channel's own spectrum has a narrow notch at its centre
                        const float lid = std::min(dB(lastPeak), dB(q)) - 5.f;
                        int lo = v, hi = v;
                        while (lo > lastPeak && dB(lo - 1) <= lid) lo--;
                        while (hi < q && dB(hi + 1) <= lid) hi++;
                        const bool valley = vmin <= lid && (hi - lo + 1) * bin >= 1000.0;
                        if (valley) { cuts.push_back(v); lastPeak = q; }
                        else if (dB(q) > dB(lastPeak)) lastPeak = q;      // the same carrier: keep its higher point
                    } else if (lastPeak < 0 || dB(q) > dB(lastPeak)) lastPeak = q;
                }
            }
            cuts.push_back(b + 1);
            int segA = a;
            for (int cut : cuts) {
                const int segB = cut - 1;
                double wsum = 0, fsum = 0, peak = 0;
                for (int q = segA; q <= segB; q++) {
                    const double w = std::max(0.0, (double)s[(size_t)q] - floorP);
                    wsum += w; fsum += w * fOf(q); peak = std::max(peak, (double)s[(size_t)q]);
                }
                const double width = (segB - segA + 1) * bin;
                if (wsum > 0 && width <= cfg.maxWidthHz && width >= bin) regs.push_back({fsum / wsum, width, (float)(10 * std::log10(peak / floorP))});
                segA = cut;
            }
            j = b + 1;
        }
        std::sort(regs.begin(), regs.end(), [](const Region& x, const Region& y) { return x.peak > y.peak; });
        std::vector<char> hit(cars.size(), 0);
        const double t = now();
        for (const Region& r : regs) {
            int m = -1;
            for (size_t q = 0; q < cars.size(); q++) {
                const double reach = std::max(1500.0, std::max(r.width, cars[q].widthHz) / 2);
                if (std::fabs(r.freq - cars[q].offsetHz) < reach) { m = (int)q; break; }
            }
            if (m >= 0 && hit[(size_t)m] && std::fabs(r.freq - cars[(size_t)m].offsetHz) > 2000) m = -1;   // a second region this far from the centre is a neighbour, not a piece
            if (m >= 0) {
                DetectedCarrier& c = cars[(size_t)m];
                if (hit[(size_t)m]) continue;            // two pieces of one carrier: the stronger came first
                hit[(size_t)m] = 1;
                c.offsetHz = 0.5 * c.offsetHz + 0.5 * r.freq;
                c.widthHz = 0.7 * c.widthHz + 0.3 * r.width;
                c.snrDb = r.peak;
                c.cls = classifyWidth(c.widthHz);
                c.lastSeen = t; c.seen++;
                if (c.seen >= 2) c.confirmed = true;
            } else {
                DetectedCarrier c;
                c.id = nextId++;
                c.offsetHz = r.freq; c.widthHz = r.width; c.snrDb = r.peak; c.cls = classifyWidth(r.width);
                c.firstSeen = c.lastSeen = t; c.seen = 1;
                cars.push_back(c);
                hit.push_back(1);
            }
        }
        // forget: a carrier seen once and not again (noise crosses the threshold now and then, never twice in one place), and the old ones
        std::vector<DetectedCarrier> keep;
        for (size_t q = 0; q < cars.size(); q++) {
            const DetectedCarrier& c = cars[q];
            if (!c.confirmed && !hit[q]) continue;
            if (c.confirmed && t - c.lastSeen > cfg.holdSec) continue;
            keep.push_back(c);
        }
        cars.swap(keep);
        std::sort(cars.begin(), cars.end(), [](const DetectedCarrier& x, const DetectedCarrier& y) { return x.snrDb > y.snrDb; });
    }
};

CarrierDetector::CarrierDetector() : p_(std::make_unique<Impl>()) {}
CarrierDetector::~CarrierDetector() = default;
void CarrierDetector::configure(const DetectorConfig& cfg) { p_->configure(cfg); }
void CarrierDetector::reset() { p_->reset(); }
void CarrierDetector::feed(const cf32* x, size_t n) { p_->feed(x, n); }
const std::vector<DetectedCarrier>& CarrierDetector::carriers() const { return p_->cars; }
uint64_t CarrierDetector::searches() const { return p_->searches; }
double CarrierDetector::floorDb() const { return p_->floorDbV; }
double CarrierDetector::nowSec() const { return p_->now(); }
void CarrierDetector::spectrum(std::vector<float>& dB, double& startHz, double& binHz) const { dB = p_->specDb; startHz = p_->specStart; binHz = p_->bin; }

} // namespace dect2::inmsat
