// OpenSky identifier: mix to 0 Hz, resample to 96 kHz, channel filter, then one-second blocks of the discriminator. The symbol rate is a line in the spectrum of the squared transitions of the discriminator; the levels are read from a histogram at the symbol centres; the burst pattern from the autocorrelation of the envelope.
#include "dect2/opensky/opensky_rx.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/fm_front.h"
#include "dect2/opensky/opensky_tel.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <vector>

namespace dect2 {

ModeTuning openSkyTuning() {
    ModeTuning t;
    t.stdMode = 324;
    t.id = "opensky";
    t.name = "OpenSky identifier";
    t.minMhz = 30.0;
    t.maxMhz = 1000.0;
    t.defMhz = 851.0125;
    t.sampleRate = 240000;
    t.basebandHz = 200000;
    t.bandwidthMhz = 0.025;
    t.minSampleRate = 96000;
    t.tuneOffsetHz = 50000;
    return t;
}

namespace {
constexpr double kWork = 96000.0;
constexpr size_t kBlock = 1 << 17;   // about 1.4 s
constexpr double kBaudLo = 2000.0, kBaudHi = 25000.0;
}

struct OpenSkyReceiver::Impl {
    std::mutex mu;
    OpenSkyTelemetry tel;
    std::function<void(const std::string&)> log;
    double fs = 0, offHz = 0;
    bool rateSet = false;
    ExactResampler rs;
    std::vector<cf32> mix, rsb, buf;
    FmBiquad lr[2], li[2];
    cf32 prev{0, 0};
    double phRe = 1, phIm = 0, stepW = 0, stRe = 1, stIm = 0;
    std::unique_ptr<Fft> fft;
    double power = 0;

    void clearState() {
        const uint64_t s = tel.seq;
        tel = OpenSkyTelemetry();
        tel.seq = s + 1; tel.inputRate = fs; tel.state = rateSet ? 1 : 0;
        buf.clear(); prev = cf32(0, 0); phRe = 1; phIm = 0; power = 0; rs.reset();
        for (int i = 0; i < 2; i++) { lr[i].clear(); li[i].clear(); }
    }

    void analyse() {
        const size_t N = buf.size();
        // envelope, discriminator
        std::vector<float> disc(N), env(N);
        cf32 p = prev;
        for (size_t i = 0; i < N; i++) {
            disc[i] = (float)(std::arg(buf[i] * std::conj(p)) * kWork / (2 * M_PI));
            env[i] = std::norm(buf[i]);
            p = buf[i];
        }
        prev = p;
        double envMean = 0, envMax = 0;
        for (float e : env) { envMean += e; envMax = std::max(envMax, (double)e); }
        envMean /= (double)N;
        tel.levelDb = (float)(10 * std::log10(envMean + 1e-30));
        const double on = 0.25 * envMax;
        size_t onCount = 0;
        for (float e : env) onCount += (double)e > on;
        tel.burstDuty = (float)((double)onCount / (double)N);
        // keep only the samples where the carrier is on for the frequency statistics (the discriminator on noise is meaningless)
        std::vector<float> d2;
        d2.reserve(N);
        double disM = 0;
        size_t cnt = 0;
        for (size_t i = 1; i < N; i++) if ((double)env[i] > on && (double)env[i - 1] > on) { const float df = disc[i] - disc[i - 1]; d2.push_back(df * df); disM += disc[i]; cnt++; }
        if (cnt < 4096) { tel.dataValid = false; tel.seq++; return; }
        disM /= (double)cnt;
        // the symbol rate: the line in the spectrum of the squared transitions
        size_t L = 1;
        while (L * 2 <= d2.size() && L < (size_t)1 << 17) L <<= 1;
        d2.resize(L);
        double m = 0;
        for (float v : d2) m += v;
        m /= (double)L;
        std::vector<cf32> s(L);
        for (size_t i = 0; i < L; i++) s[i] = cf32((float)(d2[i] - m), 0);
        if (!fft || fft->size() != L) fft = std::make_unique<Fft>((int)L);
        fft->forward(s.data());
        const double bin = kWork / (double)L;
        const size_t lo = (size_t)(kBaudLo / bin), hi = std::min((size_t)(kBaudHi / bin), L / 2 - 1);
        std::vector<float> mag(hi + 2);
        for (size_t i = lo; i <= hi; i++) mag[i] = std::norm(s[i]);
        size_t bestI = lo;
        for (size_t i = lo; i <= hi; i++) if (mag[i] > mag[bestI]) bestI = i;
        std::vector<float> sorted(mag.begin() + (long)lo, mag.begin() + (long)hi + 1);
        std::nth_element(sorted.begin(), sorted.begin() + (long)sorted.size() / 2, sorted.end());
        const float median = sorted[sorted.size() / 2] + 1e-20f;
        tel.lineDb = (float)(10 * std::log10((double)mag[bestI] / median));
        // a parabolic refinement of the peak
        double frac = 0;
        if (bestI > lo && bestI < hi) {
            const double a = mag[bestI - 1], b = mag[bestI], c = mag[bestI + 1], d = (2 * b - a - c);
            frac = d > 0 ? 0.5 * (c - a) / d : 0;
        }
        tel.baud = tel.lineDb > 8 ? (bestI + frac) * bin : 0;
        // the levels: histogram of the discriminator (where the carrier is on and near the symbol centres), peaks counted
        std::vector<float> values;
        for (size_t i = 0; i < N; i++) if ((double)env[i] > on) values.push_back(disc[i] - (float)disM);
        std::sort(values.begin(), values.end());
        const float span = std::max(fabsf(values.front()), fabsf(values.back()));
        constexpr int kBins = 121;
        std::vector<float> h(kBins, 0.f);
        const float scale = (float)(kBins / 2) / (span * 1.05f + 1e-3f);
        for (float v : values) {
            const int b = std::clamp((int)std::lround(v * scale) + kBins / 2, 0, kBins - 1);
            h[(size_t)b] += 1;
        }
        std::vector<float> smooth(kBins, 0.f);
        for (int i = 2; i < kBins - 2; i++) smooth[(size_t)i] = (h[(size_t)i - 2] + 2 * h[(size_t)i - 1] + 3 * h[(size_t)i] + 2 * h[(size_t)i + 1] + h[(size_t)i + 2]) / 9.f;
        const float top = *std::max_element(smooth.begin(), smooth.end());
        int peaks = 0;
        std::vector<int> at;
        for (int i = 4; i < kBins - 4; i++) if (smooth[(size_t)i] > 0.20f * top && smooth[(size_t)i] >= smooth[(size_t)i - 1] && smooth[(size_t)i] > smooth[(size_t)i + 1] && smooth[(size_t)i] >= smooth[(size_t)i - 2]) { peaks++; at.push_back(i); }
        tel.levels = peaks == 2 || peaks == 3 ? 2 : peaks >= 4 ? 4 : 0;
        if (at.size() >= 2) tel.deviationHz = (float)((double)(at.back() - at.front()) / scale * 0.5);
        // occupied width: the spectrum of the baseband (block of 2^15)
        {
            const int M = 1 << 15;
            Fft f2(M);
            std::vector<cf32> w((size_t)M);
            for (int i = 0; i < M; i++) { const float h2 = (float)(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / M)); w[(size_t)i] = buf[(size_t)i] * h2; }
            f2.forward(w.data());
            std::vector<double> ps((size_t)M);
            double total = 0;
            for (int i = 0; i < M; i++) { ps[(size_t)i] = std::norm(w[(size_t)i]); total += ps[(size_t)i]; }
            // grow from the middle bin until 90 % of the power is inside
            double inside = ps[0];
            int r = 0;
            while (inside < 0.9 * total && r < M / 2 - 1) { r++; inside += ps[(size_t)r] + ps[(size_t)(M - r)]; }
            tel.occupiedHz = (float)(2.0 * r * kWork / M);
        }
        // the envelope period: autocorrelation of the on/off envelope (lags 5 ms to 400 ms) at 1 ms resolution
        {
            const int dec = (int)(kWork / 1000.0);
            std::vector<float> e2;
            for (size_t i = 0; i + (size_t)dec <= N; i += (size_t)dec) { float sum = 0; for (int k = 0; k < dec; k++) sum += env[i + (size_t)k]; e2.push_back(sum / dec); }
            double mean = 0; for (float v : e2) mean += v; mean /= (double)e2.size();
            for (float& v : e2) v -= (float)mean;
            double e0 = 0; for (float v : e2) e0 += (double)v * v;
            int bestLag = 0; double bestC = 0;
            for (int lag = 5; lag < (int)std::min<size_t>(e2.size() / 2, 400); lag++) {
                double c = 0;
                for (size_t i = 0; i + (size_t)lag < e2.size(); i++) c += (double)e2[i] * e2[i + (size_t)lag];
                c /= (e0 + 1e-20);
                if (c > bestC) { bestC = c; bestLag = lag; }
            }
            tel.burstPeriodMs = tel.burstDuty < 0.97f && bestC > 0.5 ? (double)bestLag : 0;
        }
        tel.looksLikeOpenSky = tel.baud > 0 && std::fabs(tel.baud - 9600.0) < 300.0 && tel.levels == 4;
        char b[220];
        if (tel.looksLikeOpenSky) snprintf(b, sizeof b, "Consistent with OpenSky: %.0f baud, four frequency levels, %.1f kHz wide%s. The traffic is encrypted and the framing is not public, so nothing is decoded.", tel.baud, tel.occupiedHz / 1e3, tel.burstPeriodMs > 0 ? ", TDMA bursts" : "");
        else if (tel.baud > 0) snprintf(b, sizeof b, "Measured %.0f baud, %d level(s), %.1f kHz wide: not the OpenSky pattern (9600 baud, four levels, 25 kHz channel).", tel.baud, tel.levels, tel.occupiedHz / 1e3);
        else snprintf(b, sizeof b, "No symbol rate found in the signal.");
        tel.verdict = b;
        tel.blocksOk++; tel.dataValid = true; tel.locked = tel.looksLikeOpenSky; tel.state = 2;
        tel.seq++;
    }
};

OpenSkyReceiver::OpenSkyReceiver() : p_(std::make_unique<Impl>()) {}
OpenSkyReceiver::~OpenSkyReceiver() = default;
void OpenSkyReceiver::configure(double r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->fs = r;
    p_->rateSet = r >= openSkyTuning().minSampleRate - 1 && p_->rs.configure(r, kWork, 8);
    for (int i = 0; i < 2; i++) { p_->lr[i].lowpass(12500.0, kWork, i ? 1.3066 : 0.5412); p_->li[i].lowpass(12500.0, kWork, i ? 1.3066 : 0.5412); }
    p_->clearState();
}
void OpenSkyReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; }
bool OpenSkyReceiver::ready() const { return p_->rateSet; }
void OpenSkyReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearState(); }
void OpenSkyReceiver::feed(const cf32* x, size_t n) {
    if (!x || !n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.rateSet) { m.tel.state = 0; return; }
    const double step = -2 * M_PI * m.offHz / m.fs;
    if (step != m.stepW) { m.stepW = step; m.stRe = std::cos(step); m.stIm = std::sin(step); }
    m.mix.resize(n);
    double pr = m.phRe, pi = m.phIm;
    for (size_t i = 0; i < n; i++) {
        const float lr = (float)pr, li = (float)pi, xr = x[i].real(), xi = x[i].imag();
        m.mix[i] = cf32(xr * lr - xi * li, xr * li + xi * lr);
        const double nr = pr * m.stRe - pi * m.stIm;
        pi = pr * m.stIm + pi * m.stRe; pr = nr;
        if ((i & 1023) == 1023) { const double g = 1.0 / std::sqrt(pr * pr + pi * pi); pr *= g; pi *= g; }
    }
    const double g = 1.0 / std::sqrt(pr * pr + pi * pi); m.phRe = pr * g; m.phIm = pi * g;
    m.rsb.clear();
    m.rs.process(m.mix.data(), n, m.rsb);
    for (const cf32& v : m.rsb) {
        double re = m.lr[0].run(v.real()), im = m.li[0].run(v.imag());
        re = m.lr[1].run(re); im = m.li[1].run(im);
        m.buf.push_back(cf32((float)re, (float)im));
        if (m.buf.size() >= kBlock) { m.analyse(); m.buf.clear(); }
    }
    m.tel.timeSec += (double)n / m.fs;
}
bool OpenSkyReceiver::telemetry(OpenSkyTelemetry& out, uint64_t lastSeq) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->tel.seq <= lastSeq) return false; out = p_->tel; return true; }
void OpenSkyReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

} // namespace dect2
