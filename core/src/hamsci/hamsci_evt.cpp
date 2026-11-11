// Ham science event monitors (hamsci_evt.h): the signal processing of the Schumann, whistler / tweek, Moon-echo, Radio Jove, CALLISTO-style and
// aurora / sporadic-E / TEP monitors. Written for OnAir from the public descriptions listed in docs/W1B-3.9.md; no third-party code.
#include "dect2/hamsci/hamsci_evt.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <numeric>

namespace dect2 {
namespace hamsci {

namespace {
using detail::cd;
using detail::kPi;
using cf = std::complex<float>;

double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-30)); }

// Hann-windowed power spectrum of n complex samples (n a power of two); out has n values in FFT order.
struct Spec {
    size_t n = 0;
    std::vector<double> win;
    std::vector<cd> buf;
    explicit Spec(size_t N = 0) { init(N); }
    void init(size_t N) {
        n = N; win.assign(N, 1.0); buf.assign(N, cd(0, 0));
        for (size_t i = 0; i < N; i++) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)N);
    }
    void power(const cf* x, std::vector<double>& out) {
        for (size_t i = 0; i < n; i++) buf[i] = cd((double)x[i].real() * win[i], (double)x[i].imag() * win[i]);
        detail::fft(buf);
        out.resize(n);
        double wp = 0;
        for (double w : win) wp += w * w;
        for (size_t i = 0; i < n; i++) out[i] = std::norm(buf[i]) / wp;   // per-sample power units: white noise of variance s2 gives s2 per bin
    }
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)m, v.end());
    return v[m];
}
}  // namespace

const char* evtKindName(int kind) {
    static const char* nm[kEvtKinds] = {"Schumann resonance monitor", "Whistler and tweek detector", "Moon echo (EME) test", "Radio Jove", "e-CALLISTO solar bursts", "Aurora / sporadic-E / TEP beacon events"};
    return nm[std::clamp(kind, 0, (int)kEvtKinds - 1)];
}

// ===================================================================================================================== implementation
struct EvtMonitor::Impl {
    int kind;
    double rate = 0, offsetHz = 0, startUnix = 0, thrDb = -1;   // thrDb < 0: the kind's default
    bool autoLine = true;
    double beaconHz = 0;
    // front end: mix by -offset, integrate and dump by `dec`
    int dec = 1;
    double fs = 0, nco = 0;
    cf acc{0, 0};
    int accN = 0;
    bool realOnly = false;
    std::vector<cf> y;           // decimated samples not yet consumed
    uint64_t outN = 0;           // decimated samples consumed
    EvtState st;
    std::vector<EvtEvent> fresh;
    double nextTrend = 60;
    // counters feeding the trend
    int trendCnt1 = 0, trendCnt2 = 0;
    double trendMax = 0, trendSum = 0; int trendN = 0;

    explicit Impl(int k) : kind(k) { st.kind = k; }

    // ---- shared
    double thr(double dflt) const { return thrDb > 0 ? thrDb : dflt; }
    void emit(const EvtEvent& e) {
        fresh.push_back(e);
        st.eventsTotal++;
        if (st.hourly.size() != 24) st.hourly.assign(24, 0);
        const int64_t u = (int64_t)(startUnix + e.timeSec);
        st.hourly[(size_t)(((u / 3600) % 24 + 24) % 24)]++;
    }
    void pushTrend(float a, float b) {
        st.trend.push_back(a); st.trend2.push_back(b);
        if (st.trend.size() > 1440) { st.trend.erase(st.trend.begin()); st.trend2.erase(st.trend2.begin()); }
    }
    void pushRow(std::vector<float> r, size_t keep) {
        st.rows.push_back(std::move(r));
        while (st.rows.size() > keep) st.rows.erase(st.rows.begin());
    }

    // ---- per kind state
    Spec spec;
    std::vector<double> P;
    std::vector<cf> frame;          // samples waiting for a frame
    size_t N = 0, hop = 0;
    uint64_t rowNo = 0;
    double rowEnd = 0;           // signal time at the end of the frame being processed
    template <class F> void eachFrame(F fn) {
        for (size_t k = 0; k < y.size(); k++) frame.push_back(y[k]);
        outN += y.size(); y.clear();
        size_t pos = 0;
        while (frame.size() - pos >= N) {
            spec.power(frame.data() + pos, P);
            rowEnd = ((double)outN - (double)frame.size() + (double)pos + (double)N) / fs;
            pos += hop;
            fn();
        }
        frame.erase(frame.begin(), frame.begin() + (long)pos);
    }

    // Schumann
    std::vector<double> avg;                  // long average power spectrum
    int avgCount = 0;
    double hp = 0, lp1 = 0, lp2 = 0, prevX = 0;   // transient band filter (5 .. 40 Hz)
    double blkE = 0; int blkN = 0;
    std::vector<double> blkHist;
    bool tOn = false; double tStart = 0, tPeak = 0; int tRows = 0;
    // whistler
    std::vector<std::vector<float>> ring;     // dB over floor, one per hop
    std::vector<double> floorP;
    uint64_t ringBase = 0;                    // absolute row number of ring[0]
    double lastWhistlerT = -100, lastTweekT = -100;
    uint64_t nextCheck = 0;                   // next impulse row to examine
    std::vector<float> bbHist;
    int sfericCount = 0;
    int dispAccN = 0; std::vector<float> dispAcc;
    // EME
    std::vector<std::vector<float>> pring;    // linear power rows (complex spectrum bins, FFT order)
    std::vector<double> floorE;
    double gNoise = 0; int gNoiseN = 0;
    struct Pulse { uint64_t r0 = 0, r1 = 0; int k0 = 0; double txP = 0; bool done = false; };
    std::vector<Pulse> pulses;
    bool txOn = false; uint64_t txStart = 0; std::vector<int> txBins; double txPsum = 0; int txPn = 0; int offRows = 0;
    // Jove / CALLISTO
    std::vector<double> bg, var;
    std::vector<double> rowPow;
    int fr = 0;                    // frames in the current row
    int rowFrames = 0;
    bool evOn = false; int evInactive = 0; double evStart = 0, evPeak = 0, evBw = 0; std::vector<double> evT, evC; double evFc0 = 0;
    int bgRows = 0;
    double minuteLevelSum = 0; int minuteLevelN = 0; double level0 = 0; bool haveLevel0 = false;
    // aurora
    bool aOn = false; int aHits = 0, aMiss = 0; double aStart = 0, aPeak = 0;
    std::vector<double> aSnr, aWidth, aDop;

    void reconfigure() {
        // front end settings per kind
        realOnly = (kind == kEvtSchumann || kind == kEvtWhistler);
        double target = 2000;
        if (kind == kEvtSchumann) target = 200;
        else if (kind == kEvtWhistler) target = 24000;
        else if (kind == kEvtJove || kind == kEvtCallisto) target = rate;      // the whole capture
        dec = std::max(1, (int)std::floor(rate / target + 0.5));
        if (rate / target < 1.0) dec = 1;
        fs = rate / dec;
        nco = 0; acc = cf(0, 0); accN = 0; y.clear(); outN = 0;
        st = EvtState(); st.kind = kind; fresh.clear();
        st.hourly.assign(24, 0);
        nextTrend = 60; trendCnt1 = trendCnt2 = 0; trendMax = trendSum = 0; trendN = 0;
        frame.clear(); rowNo = 0;
        avg.clear(); avgCount = 0; hp = lp1 = lp2 = prevX = 0; blkE = 0; blkN = 0; blkHist.clear(); tOn = false; tRows = 0;
        ring.clear(); floorP.clear(); ringBase = 0; lastWhistlerT = lastTweekT = -100; nextCheck = 0; bbHist.clear(); sfericCount = 0; dispAccN = 0; dispAcc.clear();
        pring.clear(); floorE.clear(); gNoise = 0; gNoiseN = 0; pulses.clear(); txOn = false; txBins.clear(); offRows = 0;
        bg.clear(); var.clear(); fr = 0; rowPow.clear(); evOn = false; evInactive = 0; evT.clear(); evC.clear(); bgRows = 0; minuteLevelSum = 0; minuteLevelN = 0; haveLevel0 = false;
        aOn = false; aHits = aMiss = 0; aSnr.clear(); aWidth.clear(); aDop.clear();
        switch (kind) {
        case kEvtSchumann: N = 2048; hop = 1024; st.rowSec = (double)hop / fs; st.binHz = fs / (double)N; st.trendName = "fundamental peak, Hz"; st.trend2Name = "fundamental peak, dB over baseline"; break;
        case kEvtWhistler: N = 512; hop = 128; st.rowSec = 0.1; st.binHz = fs / (double)N * 2; st.f0Hz = 0.5e3; st.trendName = "events per minute"; st.trend2Name = "sferics per minute"; break;
        case kEvtEme: N = 512; hop = 128; st.rowSec = (double)hop * 4 / fs; st.binHz = fs / (double)N * 2; st.trendName = "echo level, dB (last in the minute)"; st.trend2Name = "echo delay, s"; break;
        case kEvtJove: case kEvtCallisto: {
            const double target_bin = kind == kEvtJove ? 31250.0 : 62500.0;
            size_t n = 8;
            while (n < 256 && fs / (double)(n * 2) >= target_bin * 0.7) n *= 2;
            N = n; hop = N;
            st.rowSec = kind == kEvtJove ? 0.1 : 0.25;
            st.binHz = fs / (double)N; st.f0Hz = -fs / 2;
            rowFrames = std::max(1, (int)std::lround(st.rowSec * fs / (double)N));
            st.trendName = "background level, dB re first minute"; st.trend2Name = "bursts per minute";
            break;
        }
        case kEvtAurora: N = 1024; hop = 1024; st.rowSec = (double)hop / fs; st.binHz = fs / (double)N * 4; st.f0Hz = -fs / 2; st.trendName = "peak S/N in the minute, dB"; st.trend2Name = "best width, Hz"; break;
        }
        spec.init(N);
        st.levelDb = -200;
    }

    // ---- front end
    void front(const cf* x, size_t n) {
        const double w = realOnly ? 0.0 : -2 * kPi * offsetHz / rate;
        for (size_t i = 0; i < n; i++) {
            cf v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf(0, 0);
            if (realOnly) v = cf(v.real(), 0.f);
            else if (w != 0.0) {
                nco += w;
                if (nco < -kPi) nco += 2 * kPi;
                v *= cf((float)std::cos(nco), (float)std::sin(nco));
            }
            acc += v;
            if (++accN >= dec) { y.push_back(acc / (float)accN); acc = cf(0, 0); accN = 0; }
        }
    }

    void feed(const cf* x, size_t n) {
        if (rate <= 0) return;
        front(x, n);
        run();
        st.timeSec = (double)outN / fs;
        while (st.timeSec >= nextTrend) { nextTrend += 60; minuteEnd(); }
    }

    void run() {
        switch (kind) {
        case kEvtSchumann: runSchumann(); break;
        case kEvtWhistler: runWhistler(); break;
        case kEvtEme: runEme(); break;
        case kEvtJove: case kEvtCallisto: runBursts(); break;
        case kEvtAurora: runAurora(); break;
        }
        // drop what has been consumed
    }

    void minuteEnd() {
        switch (kind) {
        case kEvtSchumann: pushTrend(st.peakHz.empty() ? 0.f : st.peakHz[0], st.peakDb.empty() ? 0.f : st.peakDb[0]); break;
        case kEvtWhistler: pushTrend((float)trendCnt1, (float)sfericCount); trendCnt1 = 0; sfericCount = 0; break;
        case kEvtEme: pushTrend(trendN ? (float)trendMax : 0.f, trendN ? (float)trendSum / (float)trendN : 0.f); trendMax = trendSum = 0; trendN = 0; break;
        case kEvtJove: case kEvtCallisto: {
            const double l = minuteLevelN ? minuteLevelSum / minuteLevelN : 0;
            if (!haveLevel0 && minuteLevelN) { level0 = l; haveLevel0 = true; }
            pushTrend((float)(l - level0), (float)trendCnt1); trendCnt1 = 0; minuteLevelSum = 0; minuteLevelN = 0; break;
        }
        case kEvtAurora: pushTrend(trendN ? (float)trendMax : 0.f, trendN ? (float)trendSum / (float)trendN : 0.f); trendMax = trendSum = 0; trendN = 0; break;
        }
    }

    // ================================================================================================================= Schumann
    // Decimated to about 200 Hz, 2048-point spectra (0.1 Hz) every 5 s, averaged with a 5 minute memory. The five lowest resonances are looked for around the
    // published frequencies (7.83, 14.1, 20.3, 26.3, 32.5 Hz: Wikipedia, "Schumann resonances"); a peak counts when it stands 2 dB above the median baseline.
    void runSchumann() {
        // transient detector: one-pole 5 Hz high-pass and two one-pole 40 Hz low-passes, energy in 0.5 s blocks against the median of the last 5 minutes
        const double a_hp = std::exp(-2 * kPi * 5.0 / fs), a_lp = 1 - std::exp(-2 * kPi * 40.0 / fs);
        const size_t blk = std::max<size_t>(1, (size_t)std::lround(fs * 0.5));
        size_t i = 0;
        for (; i < y.size(); i++) {
            const double xv = y[i].real();
            hp = a_hp * (hp + xv - prevX); prevX = xv;
            lp1 += a_lp * (hp - lp1); lp2 += a_lp * (lp1 - lp2);
            blkE += lp2 * lp2; blkN++;
            if ((size_t)blkN >= blk) {
                const double e = blkE / blkN; blkE = 0; blkN = 0;
                const double med = median(blkHist);
                const bool ready = blkHist.size() >= 60;
                const double dbv = ready ? db10(e / std::max(med, 1e-30)) : 0;
                const double t = (double)(outN + i) / fs;
                if (ready && dbv >= thr(8.0)) {
                    if (!tOn) { tOn = true; tStart = t - 0.5; tPeak = dbv; tRows = 0; }
                    tPeak = std::max(tPeak, dbv); tRows++;
                } else if (tOn) {
                    EvtEvent ev; ev.timeSec = tStart; ev.durSec = tRows * 0.5; ev.type = "transient"; ev.peakDb = tPeak; ev.note = "ELF transient in 5 - 40 Hz";
                    emit(ev); tOn = false;
                }
                if (!tOn) { blkHist.push_back(e); if (blkHist.size() > 600) blkHist.erase(blkHist.begin()); }   // bursts do not enter the reference
                st.levelDb = (float)db10(e);
            }
        }
        eachFrame([this] { schumannFrame(); });
    }
    void schumannFrame() {
        static const double kMode[5] = {7.83, 14.1, 20.3, 26.3, 32.5};
        const size_t H = N / 2;
        if (avg.empty()) avg.assign(H, 0.0);
        avgCount++;
        const double alpha = std::max(1.0 / avgCount, st.rowSec / 300.0);
        for (size_t k = 0; k < H; k++) avg[k] += alpha * (P[k] - avg[k]);
        const double bh = st.binHz;
        const size_t kLo = (size_t)(3.0 / bh), kHi = std::min(H - 1, (size_t)(60.0 / bh));
        // baseline: median over +- 5 Hz
        const size_t wHalf = (size_t)(5.0 / bh);
        std::vector<double> base(kHi + 1, 1.0), tmp;
        for (size_t k = kLo; k <= kHi; k++) {
            tmp.assign(avg.begin() + (long)(k > wHalf ? k - wHalf : 0), avg.begin() + (long)std::min(H, k + wHalf + 1));
            base[k] = std::max(median(tmp), 1e-30);
        }
        // spectrogram row 3 - 45 Hz grouped by 4 bins, instantaneous power over the baseline
        const size_t kEnd = std::min(kHi, (size_t)(45.0 / bh));
        std::vector<float> row;
        for (size_t k = kLo; k + 3 <= kEnd; k += 4) {
            double m = 0;
            for (size_t j = 0; j < 4; j++) m = std::max(m, P[k + j] / base[k + j]);
            row.push_back((float)db10(m));
        }
        st.f0Hz = (double)kLo * bh;
        pushRow(row, 120);
        st.binHz = bh;   // the grouped row has binHz * 4 per value: the UI multiplies
        // average spectrum over baseline for display
        st.avgSpec.clear();
        for (size_t k = kLo; k <= kEnd; k++) st.avgSpec.push_back((float)db10(avg[k] / base[k]));
        st.avgF0Hz = (double)kLo * bh; st.avgBinHz = bh;
        if (avgCount * st.rowSec < 60) return;
        st.peakHz.assign(5, 0.f); st.peakDb.assign(5, 0.f); st.peakQ.assign(5, 0.f);
        int found = 0;
        for (int m = 0; m < 5; m++) {
            const size_t a = (size_t)std::max(1.0, (kMode[m] - 1.6) / bh), b = std::min(kHi, (size_t)((kMode[m] + 1.6) / bh));
            size_t kb = a; double best = 0;
            for (size_t k = a; k <= b; k++) { const double r = avg[k] / base[k]; if (r > best) { best = r; kb = k; } }
            if (db10(best) < 2.0) continue;
            double f = (double)kb * bh;
            if (kb > 1 && kb + 1 < H) {   // parabola in the log domain
                const double l0 = std::log(avg[kb - 1] / base[kb - 1]), l1 = std::log(best), l2 = std::log(avg[kb + 1] / base[kb + 1]);
                const double d = l0 - 2 * l1 + l2;
                if (d < 0) f += std::clamp(0.5 * (l0 - l2) / d, -0.5, 0.5) * bh;
            }
            // width at half height of the excess over the baseline
            const double half = 1.0 + (best - 1.0) / 2.0;
            size_t lo = kb, hi = kb;
            while (lo > a && avg[lo] / base[lo] > half) lo--;
            while (hi < b && avg[hi] / base[hi] > half) hi++;
            const double w = std::max(bh, (double)(hi - lo) * bh);
            st.peakHz[(size_t)m] = (float)f; st.peakDb[(size_t)m] = (float)db10(best); st.peakQ[(size_t)m] = (float)(f / w);
            found++;
        }
        st.info.clear();
        char b[96];
        std::snprintf(b, sizeof b, "%d of 5 resonance peaks found", found); st.info.push_back(b);
        if (st.peakHz[0] > 0) { std::snprintf(b, sizeof b, "fundamental %.2f Hz, %.1f dB over the baseline", (double)st.peakHz[0], (double)st.peakDb[0]); st.info.push_back(b); }
    }

    // ================================================================================================================= whistler and tweek
    // Spectrogram of the audio (512 points, 5.3 ms hop), per-bin noise floor. Tweeks: after a broadband impulse, a narrow ridge near n x 1.8 kHz (the cut-off of
    // the Earth-ionosphere waveguide, 1.6 - 1.8 kHz in the literature, harmonics at multiples) that settles towards the cut-off. Whistlers: a descending tone
    // following t = t0 + D / sqrt(f) (Toyokawa; Wikipedia), searched by a path sum over D and t0.
    void runWhistler() {
        eachFrame([this] { whistlerRow(); });
    }
    void whistlerRow() {
        const size_t H = N / 2 + 1;
        const double hopSec = (double)hop / fs;
        if (floorP.empty()) floorP.assign(H, 0.0);
        std::vector<float> d(H);
        const uint64_t absRow = ringBase + ring.size();
        const bool init = absRow < 200;   // 1 s of rows to find the floor
        double tot = 0;
        for (size_t k = 0; k < H; k++) {
            if (init) floorP[k] += (P[k] - floorP[k]) / ((double)absRow + 1.0);
            else floorP[k] += (std::min(P[k], 3.0 * floorP[k]) - floorP[k]) * (hopSec / 20.0);
            d[k] = (float)std::clamp(db10(P[k] / std::max(floorP[k], 1e-30)), -10.0, 40.0);
        }
        // broadband measure over 1 - 10 kHz
        const size_t kA = (size_t)(1000.0 / (fs / N)), kB = std::min(H - 1, (size_t)(10000.0 / (fs / N)));
        for (size_t k = kA; k <= kB; k++) tot += std::max(0.f, d[k]);
        bbHist.push_back((float)(tot / (double)(kB - kA + 1)));
        ring.push_back(std::move(d));
        const size_t keep = (size_t)(4.0 / hopSec);
        while (ring.size() > keep) { ring.erase(ring.begin()); bbHist.erase(bbHist.begin()); ringBase++; }
        // display row: max over 19 hops, 0.5 - 10 kHz, bins grouped by 2
        if (dispAcc.empty()) dispAcc.assign(H, -10.f);
        for (size_t k = 0; k < H; k++) dispAcc[k] = std::max(dispAcc[k], ring.back()[k]);
        if (++dispAccN >= 19) {
            std::vector<float> row;
            const size_t k0 = (size_t)(500.0 / (fs / N));
            for (size_t k = k0; k + 1 <= kB; k += 2) row.push_back(std::max(dispAcc[k], dispAcc[k + 1]));
            pushRow(row, 120);
            dispAcc.assign(H, -10.f); dispAccN = 0;
            st.f0Hz = (double)k0 * (fs / N);
        }
        st.levelDb = (float)db10(P[kA]);
        if (absRow < 400) return;
        // ---- tweeks: examine the impulse 18 rows back
        const uint64_t cur = absRow;
        if (nextCheck < 250) nextCheck = 250;
        const int tail = 18;
        while (nextCheck + (uint64_t)tail <= cur) {
            const uint64_t r = nextCheck++;
            if (r < ringBase + 10 || r + 10 > ringBase + ring.size()) continue;
            tweekCheck(r);
        }
        // ---- whistler search every 0.5 s
        if (absRow % 94 == 0 && ring.size() >= (size_t)(3.0 / hopSec)) whistlerSearch();
    }
    float bbAt(uint64_t r) const { return bbHist[(size_t)(r - ringBase)]; }
    void tweekCheck(uint64_t r) {
        const double bh = fs / N, hopSec = (double)hop / fs;
        const float c = bbAt(r);
        if (c < 5.f) return;
        for (int j = -8; j <= 8; j++) if (bbAt(r + j) > c || (bbAt(r + j) == c && j < 0)) return;   // not the local maximum
        sfericCount++;
        const double t = (double)r * hopSec;
        if (t - lastTweekT < 0.12) return;
        const size_t H = N / 2 + 1;
        std::vector<double> m(H, 0.0), mEarly(H, 0.0), mLate(H, 0.0);
        for (int j = 3; j <= 9; j++) {
            const auto& row = ring[(size_t)(r + (uint64_t)j - ringBase)];
            for (size_t k = 0; k < H; k++) { m[k] += row[k] / 7.0; if (j <= 5) mEarly[k] += row[k] / 3.0; if (j >= 7) mLate[k] += row[k] / 3.0; }
        }
        // strongest ridge in 1.2 - 5 kHz
        const size_t kA = (size_t)(1200.0 / bh), kB = std::min(H - 3, (size_t)(5000.0 / bh));
        size_t kp = kA; double mp = -100;
        for (size_t k = kA; k <= kB; k++) { const double v = (m[k - 1] + m[k] + m[k + 1]) / 3.0; if (v > mp) { mp = v; kp = k; } }
        const double fp = (double)kp * bh;
        if (mp < 6.0) return;
        const bool n1 = fp >= 1400 && fp <= 2300, n2 = fp >= 3100 && fp <= 4600;
        if (!n1 && !n2) return;
        // narrow: well above the same rows 600 Hz either side, and the high band (6 - 9 kHz) quiet
        const size_t dk = (size_t)(600.0 / bh);
        const double side = std::max(kp > dk ? m[kp - dk] : 0.0, kp + dk < H ? m[kp + dk] : 0.0);
        double hi = 0; int hn = 0;
        for (size_t k = (size_t)(6000.0 / bh); k < std::min(H, (size_t)(9000.0 / bh)); k++) { hi += m[k]; hn++; }
        if (mp - side < 4.0 || (hn && hi / hn > 4.0)) return;
        // the ridge settles downwards: the early peak is not below the late one
        auto peakOf = [&](const std::vector<double>& v) { size_t kb = kA; double b = -100; for (size_t k = kA; k <= kB; k++) if (v[k] > b) { b = v[k]; kb = k; } return kb; };
        const size_t ke = peakOf(mEarly), kl = peakOf(mLate);
        (void)ke;
        lastTweekT = t;
        EvtEvent ev; ev.timeSec = t; ev.durSec = 0.04; ev.type = "tweek"; ev.freqHz = (double)kl * bh; ev.peakDb = mp; ev.aux1 = n1 ? 1 : 2;
        char b[96]; std::snprintf(b, sizeof b, "ridge settles near %.0f Hz (mode %d, cut-off about %d x 1.8 kHz)", ev.freqHz, n1 ? 1 : 2, n1 ? 1 : 2); ev.note = b;
        emit(ev); trendCnt1++;
    }
    void whistlerSearch() {
        const double bh = fs / N, hopSec = (double)hop / fs;
        const size_t H = N / 2 + 1;
        const uint64_t rows = ring.size();
        if (rows < 100) return;
        std::vector<size_t> kf;
        for (double f = 1500; f <= 9000; f += 3 * bh) { const size_t k = (size_t)(f / bh); if (k < H) kf.push_back(k); }
        const double refF = 4000.0, isr = 1.0 / std::sqrt(refF);
        double bestS = 0, bestD = 0, bestT = 0; double bestFr = 0;
        const int maxEnd = (int)rows - 1;
        for (double Ds = 10.0; Ds <= 300.0; Ds *= 1.07) {
            // path offsets in rows relative to the row of f = 4 kHz
            std::vector<int> off(kf.size());
            int offMin = 1 << 30, offMax = -(1 << 30);
            for (size_t i = 0; i < kf.size(); i++) {
                const double f = (double)kf[i] * bh;
                off[i] = (int)std::lround(Ds * (1.0 / std::sqrt(f) - isr) / hopSec);
                offMin = std::min(offMin, off[i]); offMax = std::max(offMax, off[i]);
            }
            for (int t4 = -offMin; t4 + offMax <= maxEnd - 4; t4++) {
                double s = 0; int hit = 0;
                for (size_t i = 0; i < kf.size(); i++) {
                    const float v = ring[(size_t)(t4 + off[i])][kf[i]];
                    s += std::clamp(v, 0.f, 15.f);
                    if (v > 4.f) hit++;
                }
                s /= (double)kf.size();
                if (hit < (int)(0.75 * (double)kf.size())) continue;
                if (s > bestS) { bestS = s; bestD = Ds; bestT = (double)(ringBase + (uint64_t)t4) * hopSec; bestFr = (double)hit / (double)kf.size(); }
            }
        }
        if (bestS < thr(6.0)) return;
        if (bestT - lastWhistlerT < 1.5) return;
        lastWhistlerT = bestT;
        EvtEvent ev; ev.timeSec = bestT; ev.durSec = bestD * (1.0 / std::sqrt(1500.0) - 1.0 / std::sqrt(9000.0)); ev.type = "whistler"; ev.peakDb = bestS; ev.aux1 = bestD; ev.freqHz = refF;
        char b[96]; std::snprintf(b, sizeof b, "D = %.0f s sqrt(Hz), %.0f %% of the path above 4 dB, time given at 4 kHz", bestD, bestFr * 100.0); ev.note = b;
        emit(ev); trendCnt1++;
    }

    // ================================================================================================================= Moon echo
    // The monitor finds the strong keyed carrier of your own transmission (or any strong carrier that is keyed), and looks 2.3 - 2.85 s after each pulse for a
    // weaker copy of the same length, anywhere within +-500 Hz of it (the Doppler shift of the Moon at 144 MHz reaches about +-440 Hz). The echo must stand
    // above the scores of the same search at delays where no echo can be (from 3.15 s plus the pulse length, 0.6 s wide).
    void runEme() {
        eachFrame([this] { emeRow(); });
    }
    void emeRow() {
        const double hopSec = (double)hop / fs;
        if (floorE.empty()) floorE.assign(N, 0.0);
        const uint64_t absRow = rowNo++;
        // per-row: the strongest bin and the median power
        size_t kmax = 0; double pmax = 0;
        for (size_t k = 0; k < N; k++) if (P[k] > pmax) { pmax = P[k]; kmax = k; }
        const double med = median(P) / 0.693;
        if (gNoiseN < 60) { gNoise += (med - gNoise) / (double)(gNoiseN + 1); gNoiseN++; }
        else if (!txOn) gNoise += (med - gNoise) * (hopSec / 30.0);
        std::vector<float> row(N);
        for (size_t k = 0; k < N; k++) row[k] = (float)P[k];
        pring.push_back(std::move(row));
        const size_t keep = (size_t)(24.0 / hopSec);
        while (pring.size() > keep) { pring.erase(pring.begin()); ringBase++; }
        levelUpdateEme(med);
        const bool on = gNoiseN >= 60 && pmax > gNoise * std::pow(10.0, 2.5);
        if (on) {
            if (!txOn) { txOn = true; txStart = absRow; txBins.clear(); txPsum = 0; txPn = 0; }
            txBins.push_back((int)kmax); txPsum += pmax; txPn++; offRows = 0;
        } else if (txOn) {
            if (++offRows >= 3) {
                txOn = false;
                const uint64_t r1 = absRow - 2;
                if ((double)(r1 - txStart) * hopSec >= 0.5) {
                    std::vector<int> b = txBins; std::sort(b.begin(), b.end());
                    Pulse p; p.r0 = txStart; p.r1 = r1; p.k0 = b[b.size() / 2]; p.txP = txPsum / std::max(1, txPn);
                    pulses.push_back(p); st.txPulses++;
                    st.txFreqHz = (p.k0 < (int)N / 2 ? p.k0 : p.k0 - (int)N) * (fs / N);
                }
            }
        }
        // search for each finished pulse once 3.9 s of rows have passed
        for (auto& p : pulses) {
            if (p.done) continue;
            if ((double)(absRow - p.r0) * hopSec < 3.9 + 2.0 * (double)(p.r1 - p.r0 + 1) * hopSec) continue;
            p.done = true;
            emeSearch(p);
        }
        while (!pulses.empty() && pulses.front().done && pulses.front().r0 < ringBase) pulses.erase(pulses.begin());
        st.info.clear();
        char b[120];
        if (st.txPulses == 0) st.info.push_back("no keyed strong carrier yet: send a pulse of 1 s or more");
        else { std::snprintf(b, sizeof b, "own carrier %+.1f Hz, %d pulses seen, %d echoes", st.txFreqHz, st.txPulses, st.eventsTotal); st.info.push_back(b); }
        // display row each 4 hops: around the carrier +-500 Hz
        if (++dispAccN >= 4) {
            dispAccN = 0;
            const int kc = pulses.empty() ? 0 : pulses.back().k0;
            const int half = (int)(500.0 / (fs / N));
            std::vector<float> r;
            for (int j = -half; j < half; j += 2) {
                const int k1 = ((kc + j) % (int)N + (int)N) % (int)N, k2 = ((kc + j + 1) % (int)N + (int)N) % (int)N;
                r.push_back((float)db10(std::max(P[(size_t)k1], P[(size_t)k2]) / std::max(gNoise, 1e-30)));
            }
            st.f0Hz = -500.0;
            pushRow(r, 120);
        }
    }
    void levelUpdateEme(double med) { st.levelDb = (float)db10(med); }
    double scoreAt(uint64_t rStart, int len, int kd) const {   // mean power in three bins over len rows from absolute row rStart
        double s = 0;
        for (int j = 0; j < len; j++) {
            const auto& row = pring[(size_t)(rStart + (uint64_t)j - ringBase)];
            for (int d = -1; d <= 1; d++) s += row[(size_t)(((kd + d) % (int)N + (int)N) % (int)N)];
        }
        return s / (3.0 * len);
    }
    bool overlapsTx(uint64_t a, int len) const {   // another transmission (or the running one) inside rows a .. a + len
        const uint64_t b = a + (uint64_t)len;
        for (const auto& q : pulses) if (q.r0 <= b && q.r1 + 1 >= a) return true;
        if (txOn && txStart <= b) return true;
        return false;
    }
    void emeSearch(const Pulse& p) {
        const double hopSec = (double)hop / fs;
        const int len = std::max(2, (int)(p.r1 - p.r0 + 1) - 2);
        const int half = (int)(500.0 / (fs / N));
        auto okRange = [&](double tau) { const uint64_t a = p.r0 + (uint64_t)std::lround(tau / hopSec); return a >= ringBase && a + (uint64_t)len < ringBase + pring.size(); };
        const double c0 = 3.15 + (double)len * hopSec;   // the control delays start after any echo of this pulse has ended
        if (!okRange(2.3) || !okRange(c0 + 0.6)) return;
        // control scores
        std::vector<double> ctrl;
        for (double tau = c0; tau <= c0 + 0.6; tau += hopSec) {
            if (!okRange(tau)) continue;
            const uint64_t a = p.r0 + (uint64_t)std::lround(tau / hopSec);
            if (overlapsTx(a, len)) continue;
            for (int kd = -half; kd <= half; kd += 3) {
                ctrl.push_back(scoreAt(a, len, p.k0 + kd));
            }
        }
        if (ctrl.size() < 20) return;
        double mean = std::accumulate(ctrl.begin(), ctrl.end(), 0.0) / (double)ctrl.size(), v = 0;
        for (double c : ctrl) v += (c - mean) * (c - mean);
        const double sd = std::sqrt(v / (double)ctrl.size());
        double best = 0, bestTau = 0; int bestKd = 0;
        for (double tau = 2.3; tau <= 2.85; tau += hopSec) {
            if (!okRange(tau)) continue;
            const uint64_t a = p.r0 + (uint64_t)std::lround(tau / hopSec);
            if (overlapsTx(a, len)) continue;
            for (int kd = -half; kd <= half; kd++) {
                const double sc = scoreAt(a, len, p.k0 + kd);
                if (sc > best) { best = sc; bestTau = tau; bestKd = kd; }
            }
        }
        const double z = (best - mean) / std::max(sd, 1e-30);
        if (z < thr(6.0)) return;
        // refine the delay: parabola through the scores one row either side at the best Doppler bin
        double tau = bestTau;
        {
            auto sAt = [&](double t) { return scoreAt(p.r0 + (uint64_t)std::lround(t / hopSec), len, p.k0 + bestKd); };
            const double s0 = sAt(bestTau - hopSec), s1 = best, s2 = sAt(bestTau + hopSec);
            const double dd = s0 - 2 * s1 + s2;
            if (dd < 0) tau += std::clamp(0.5 * (s0 - s2) / dd, -0.5, 0.5) * hopSec;
        }
        // spread: bins around the best one that hold at least half of the excess
        const uint64_t a = p.r0 + (uint64_t)std::lround(bestTau / hopSec);
        int lo = bestKd, hi = bestKd;
        while (lo > -half && scoreAt(a, len, p.k0 + lo - 1) - mean > 0.5 * (best - mean)) lo--;
        while (hi < half && scoreAt(a, len, p.k0 + hi + 1) - mean > 0.5 * (best - mean)) hi++;
        EvtEvent ev; ev.timeSec = (double)p.r0 * hopSec; ev.durSec = (double)len * hopSec; ev.type = "echo";
        ev.freqHz = bestKd * (fs / N); ev.peakDb = db10(best / std::max(mean, 1e-30)); ev.aux1 = tau;
        ev.aux2 = (double)(hi - lo + 1) * (fs / N);
        char b[200];
        std::snprintf(b, sizeof b, "delay %.3f s, Doppler %+.1f Hz, %.1f dB below your carrier, %.1f sigma over the control delays, width at half height %.0f Hz", tau, ev.freqHz, db10(p.txP / std::max(best, 1e-30)), z, ev.aux2);
        ev.note = b;
        emit(ev);
        trendMax = std::max(trendMax, ev.peakDb); trendSum += tau; trendN++;
    }

    // ================================================================================================================= Radio Jove and CALLISTO-style bursts
    // A fast spectrogram of the whole capture (31 kHz bins and 0.1 s rows for Jove, 62.5 kHz bins and 0.25 s rows for CALLISTO: the published spectrometer
    // resolution), per-bin background (20 s memory, quiet rows only) with its own spread, a burst is a run of rows with many bins above the background (half of the band for Jove: a
    // broadband burst; two bins for CALLISTO, where bursts drift).
    void runBursts() {
        eachFrame([this] {
            if (rowPow.empty()) rowPow.assign(N, 0.0);
            for (size_t k = 0; k < N; k++) rowPow[(k + N / 2) % N] += P[k];   // increasing frequency order
            if (++fr >= rowFrames) { burstRow(); fr = 0; std::fill(rowPow.begin(), rowPow.end(), 0.0); }
        });
    }
    void burstRow() {
        const double t = rowEnd - st.rowSec;
        std::vector<double> d(N);
        double tot = 0;
        for (size_t k = 0; k < N; k++) { d[k] = db10(rowPow[k] / fr); tot += rowPow[k] / fr; }
        st.levelDb = (float)db10(tot / (double)N);
        minuteLevelSum += db10(tot / (double)N); minuteLevelN++;
        if (bg.empty()) { bg = d; var.assign(N, 0.04); bgRows = 1; }
        const double aBg = std::max(1.0 / (bgRows + 1), st.rowSec / 20.0);
        const double thrBin = thr(1.0);
        std::vector<float> row(N);
        int cnt = 0; double sumE = 0, cw = 0, cs = 0;
        std::vector<char> ex(N, 0);
        for (size_t k = 0; k < N; k++) {
            const double e = d[k] - bg[k];
            const double sg = std::sqrt(var[k]);
            const bool x = bgRows >= 20 && e > std::max(thrBin, 5.0 * sg);
            ex[k] = x;
            row[k] = (float)std::clamp(e, -5.0, 30.0);
            if (x) { cnt++; sumE += e; cw += e * (double)k; cs += e; }
        }
        pushRow(row, 150);
        const bool active = kind == kEvtJove ? cnt >= (int)(N / 2) : cnt >= 2;
        if (!active && !evOn) {   // the background follows only quiet rows
            for (size_t k = 0; k < N; k++) {
                const double e = d[k] - bg[k];
                bg[k] += aBg * e;
                var[k] += aBg * (e * e - var[k]);
                var[k] = std::max(var[k], 0.0025);
            }
            bgRows++;
        }
        if (active) {
            if (!evOn) { evOn = true; evStart = t; evPeak = 0; evBw = 0; evT.clear(); evC.clear(); }
            evInactive = 0;
            evPeak = std::max(evPeak, sumE / cnt); evBw = std::max(evBw, (double)cnt * st.binHz);
            evT.push_back(t); evC.push_back(st.f0Hz + (cw / std::max(cs, 1e-9) + 0.5) * st.binHz);
        } else if (evOn && ++evInactive >= 2) {
            evOn = false;
            EvtEvent ev; ev.timeSec = evStart; ev.durSec = std::max(st.rowSec, evT.empty() ? 0.0 : evT.back() - evT.front() + st.rowSec);
            ev.peakDb = evPeak; ev.aux2 = evBw; ev.freqHz = evC.empty() ? 0 : evC.front();
            double slope = 0;
            if (evT.size() >= 4) {
                double mt = 0, mc = 0;
                for (size_t i = 0; i < evT.size(); i++) { mt += evT[i]; mc += evC[i]; }
                mt /= (double)evT.size(); mc /= (double)evT.size();
                double sxx = 0, sxy = 0;
                for (size_t i = 0; i < evT.size(); i++) { sxx += (evT[i] - mt) * (evT[i] - mt); sxy += (evT[i] - mt) * (evC[i] - mc); }
                if (sxx > 0) slope = sxy / sxx;
            }
            ev.aux1 = slope;
            char b[140];
            const bool drift = evT.size() >= 4 && std::fabs(slope) * ev.durSec >= 2 * st.binHz;
            if (kind == kEvtJove) { ev.type = "burst"; std::snprintf(b, sizeof b, "%s, %.1f MHz of the band, %.2f s", ev.durSec < 1.0 ? "short (under 1 s)" : "long (1 s or more)", evBw / 1e6, ev.durSec); }
            else { ev.type = drift ? "drifting burst" : "burst"; if (drift) std::snprintf(b, sizeof b, "drift %+.2f MHz/s, %.1f MHz wide", slope / 1e6, evBw / 1e6); else std::snprintf(b, sizeof b, "no resolved drift, %.2f MHz wide", evBw / 1e6); }
            ev.note = b;
            emit(ev); trendCnt1++;
        }
        st.info.clear();
        char b[100]; std::snprintf(b, sizeof b, "%zu bins of %.1f kHz, rows of %.2f s", N, st.binHz / 1e3, st.rowSec); st.info.push_back(b);
    }

    // ================================================================================================================= aurora / sporadic-E / TEP beacons
    // The beacon is a carrier near the middle of a +-1 kHz window. Every 0.5 s the spectrum is searched with sliding windows of 5, 13, 31, 77 and 155 bins for
    // the most significant excess over the median noise; the width that wins says whether the beacon is a clean carrier or Doppler-spread. An opening needs
    // four significant rows in a row and ends after ten seconds without. The classes are by signal character only (our thresholds, docs/W1B-3.9.md).
    void runAurora() {
        eachFrame([this] { auroraRow(); });
    }
    void auroraRow() {
        const double bh = fs / N, t = rowEnd - (double)N / fs;
        std::vector<double> q(N);
        for (size_t k = 0; k < N; k++) q[(k + N / 2) % N] = P[k];   // increasing frequency
        const double nm = std::max(median(q) / 0.693, 1e-30);
        st.levelDb = (float)db10(nm);
        std::vector<double> pre(N + 1, 0.0);
        for (size_t k = 0; k < N; k++) pre[k + 1] = pre[k] + q[k];
        static const int W[5] = {5, 13, 31, 77, 155};
        double bestZ = -1e9, bestEx = 0, bestC = 0; int bestW = 5;
        for (int wi = 0; wi < 5; wi++) {
            const int w = W[wi];
            for (int c = 0; c + w <= (int)N; c += std::max(1, w / 4)) {
                const double s = pre[(size_t)(c + w)] - pre[(size_t)c];
                const double z = (s - w * nm) / (nm * std::sqrt((double)w));
                // a wider window must be clearly better than a narrow one to win (a carrier also fills the wide windows)
                const double zz = z - 0.0 * wi;
                if (zz > bestZ) { bestZ = zz; bestEx = s - w * nm; bestC = c + w / 2.0; bestW = w; }
            }
        }
        // the narrowest window whose significance is within 85 % of the best one is the width
        {
            double zn = -1e9; int wn = bestW; double cn = bestC, en = bestEx;
            for (int wi = 0; wi < 5; wi++) {
                const int w = W[wi];
                double zb = -1e9, cb = 0, eb = 0;
                for (int c = 0; c + w <= (int)N; c += std::max(1, w / 4)) {
                    const double s = pre[(size_t)(c + w)] - pre[(size_t)c];
                    const double z = (s - w * nm) / (nm * std::sqrt((double)w));
                    if (z > zb) { zb = z; cb = c + w / 2.0; eb = s - w * nm; }
                }
                if (zb >= 0.85 * bestZ && zn < 0) { zn = zb; wn = w; cn = cb; en = eb; }
            }
            if (zn > 0) { bestW = wn; bestC = cn; bestEx = en; }
        }
        const double snr = bestEx > 0 ? db10(bestEx / nm) : -10.0;
        // spread: the rms width of the power more than 4x the noise within +-100 bins of the best window's centre (a Doppler-spread signal is a few
        // blobs, so the narrowest significant window would understate it), as a full width at half height of a Gaussian
        double widthHz = bestW * bh;
        {
            const int lo = std::max(0, (int)bestC - 100), hi = std::min((int)N - 1, (int)bestC + 100);
            double sw = 0, sx = 0, sxx = 0;
            for (int k = lo; k <= hi; k++) { const double wv = std::max(q[(size_t)k] - 4.0 * nm, 0.0); sw += wv; sx += wv * k; sxx += wv * (double)k * k; }
            if (sw > 0) { const double mu = sx / sw, var = std::max(0.0, sxx / sw - mu * mu); widthHz = std::max(widthHz, 2.355 * std::sqrt(var) * bh); }
        }
        const double dopHz = (bestC - (double)N / 2.0) * (fs / N);
        // display row (dB over the noise, 4 bins grouped)
        std::vector<float> row;
        for (size_t k = 0; k + 3 < N; k += 4) row.push_back((float)std::clamp(db10(std::max({q[k], q[k + 1], q[k + 2], q[k + 3]}) / nm), -5.0, 40.0));
        pushRow(row, 120);
        trendMax = std::max(trendMax, bestZ >= thr(12.0) ? snr : 0.0);
        if (bestZ >= thr(12.0)) { trendSum += widthHz; trendN++; }
        if (bestZ >= thr(12.0)) {
            aHits++; aMiss = 0;
            aSnr.push_back(snr); aWidth.push_back(widthHz); aDop.push_back(dopHz);
            if (!aOn && aHits >= 4) { aOn = true; aStart = t - 3 * st.rowSec; aPeak = snr; }
            aPeak = std::max(aPeak, snr);
        } else {
            aHits = 0;
            if (aOn) {
                aMiss++;
                if (aMiss * st.rowSec >= 10.0) {
                    aOn = false;
                    finishOpening(t - aMiss * st.rowSec);
                }
            } else { aSnr.clear(); aWidth.clear(); aDop.clear(); }
        }
        st.info.clear();
        char b[120];
        std::snprintf(b, sizeof b, "noise %.1f dB, now: %s, best width %.0f Hz, z %.1f, S/N %.1f dB", db10(nm), aOn ? "open" : "closed", widthHz, bestZ, snr); st.info.push_back(b);
    }
    void finishOpening(double tEnd) {
        if (aSnr.size() < 4) { aSnr.clear(); aWidth.clear(); aDop.clear(); return; }
        std::vector<double> w = aWidth, dp = aDop;
        const double medW = median(w), medD = median(dp);
        double m = 0, v = 0; const size_t a = 1, b = aSnr.size() - 1;
        for (size_t i = a; i < b; i++) m += aSnr[i];
        m /= (double)(b - a);
        for (size_t i = a; i < b; i++) v += (aSnr[i] - m) * (aSnr[i] - m);
        const double flutter = std::sqrt(v / (double)(b - a));
        EvtEvent ev; ev.timeSec = aStart; ev.durSec = tEnd - aStart; ev.type = "opening"; ev.peakDb = aPeak; ev.freqHz = medD; ev.aux1 = medW; ev.aux2 = flutter;
        const char* cls = medW >= 40 ? "Doppler-spread (aurora-like)" : flutter >= 3.0 ? "fluttering carrier (TEP / scatter-like)" : "steady carrier (sporadic-E-like)";
        char bf[160]; std::snprintf(bf, sizeof bf, "%s: width %.0f Hz, shift %+.0f Hz, flutter %.1f dB", cls, medW, medD, flutter); ev.note = bf;
        emit(ev);
        aSnr.clear(); aWidth.clear(); aDop.clear();
    }
};

// ===================================================================================================================== public
EvtMonitor::EvtMonitor(int kind) : p_(new Impl(std::clamp(kind, 0, (int)kEvtKinds - 1))), kind_(std::clamp(kind, 0, (int)kEvtKinds - 1)) {}
EvtMonitor::~EvtMonitor() { delete p_; }
void EvtMonitor::configure(double r) { p_->rate = r; p_->reconfigure(); }
void EvtMonitor::setSignalOffset(double hz) { p_->offsetHz = hz; }
void EvtMonitor::setStartUnix(double t) { p_->startUnix = t; }
void EvtMonitor::reset() { if (p_->rate > 0) p_->reconfigure(); }
void EvtMonitor::feed(const cf* x, size_t n) { p_->feed(x, n); }
std::vector<EvtEvent> EvtMonitor::take() { std::vector<EvtEvent> o; o.swap(p_->fresh); return o; }
const EvtState& EvtMonitor::state() const { return p_->st; }
void EvtMonitor::setThresholdDb(double db) { p_->thrDb = db; }
void EvtMonitor::setBeaconOffsetHz(double hz) { p_->beaconHz = hz; p_->autoLine = false; }
void EvtMonitor::setAutoLine(bool on) { p_->autoLine = on; }

static std::string utcStamp(int64_t u) {
    const std::time_t t = (std::time_t)u;
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char b[40]; std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return b;
}

std::string evtCsv(const std::vector<EvtEvent>& ev, int64_t startUtc) {
    std::string s = "utc,signal_time_s,type,frequency_hz,peak_db,duration_s,aux1,aux2,note\n";
    for (const auto& e : ev) {
        char b[160];
        std::snprintf(b, sizeof b, ",%.3f,%s,%.2f,%.2f,%.3f,%.4f,%.4f,\"", e.timeSec, e.type.c_str(), e.freqHz, e.peakDb, e.durSec, e.aux1, e.aux2);
        s += utcStamp(startUtc + (int64_t)e.timeSec) + b;
        for (char c : e.note) s += c == '"' ? '\'' : c;
        s += "\"\n";
    }
    return s;
}

std::string evtTrendCsv(const EvtState& st, int64_t startUtc) {
    std::string s = "utc,\"" + (st.trendName.empty() ? std::string("trend") : st.trendName) + "\",\"" + (st.trend2Name.empty() ? std::string("trend2") : st.trend2Name) + "\"\n";
    for (size_t i = 0; i < st.trend.size(); i++) {
        char b[80];
        std::snprintf(b, sizeof b, ",%.3f,%.3f\n", (double)st.trend[i], i < st.trend2.size() ? (double)st.trend2[i] : 0.0);
        s += utcStamp(startUtc + (int64_t)((double)(i + 1) * st.trendStepSec)) + b;
    }
    return s;
}

}  // namespace hamsci
}  // namespace dect2
