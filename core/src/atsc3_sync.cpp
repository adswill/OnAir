#include "dect2/atsc3_sync.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <map>
#include <thread>
#include <memory>
#include <mutex>

namespace dect2 {
namespace atsc3 {

namespace {
const double kFs = kBootstrapRate;

cf32 expj(double ph) { return cf32((float)std::cos(ph), (float)std::sin(ph)); }
}

namespace {
struct Bank {
    int half = 16, taps = 32, phases = 2048, stride = 32;   // stride: taps rounded up to a multiple of four, the extra coefficients are zero
    double fc = 0.5;
    std::vector<float> h;   // (phases + 1) x taps
};

double besselI0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}

Bank makeBank(double ratio) {
    Bank b;
    const double down = std::min(1.0, ratio);
    b.fc = 0.5 * down * 0.97;
    b.half = (int)std::ceil(16.0 / down);
    b.taps = 2 * b.half;
    b.stride = (b.taps + 3) & ~3;
    b.h.assign((size_t)(b.phases + 1) * b.stride, 0.f);
    const double beta = 8.0, i0b = besselI0(beta);
    for (int p = 0; p <= b.phases; p++) {
        double tau = (double)p / b.phases, sum = 0;
        std::vector<double> t(b.taps);
        for (int k = 0; k < b.taps; k++) {
            double x = (k - (b.half - 1)) - tau;   // distance of input sample (floor + k - half + 1) from the output position
            double w = std::fabs(x) < b.half ? besselI0(beta * std::sqrt(1.0 - (x / b.half) * (x / b.half))) / i0b : 0.0;
            double sc = x == 0 ? 1.0 : std::sin(2 * M_PI * b.fc * x) / (2 * M_PI * b.fc * x);
            t[k] = 2 * b.fc * sc * w;
            sum += t[k];
        }
        for (int k = 0; k < b.taps; k++) b.h[(size_t)p * b.stride + k] = (float)(t[k] / sum);
    }
    return b;
}
}

void resampleExact(const cf32* in, size_t n, double inRate, double outRate, std::vector<cf32>& out, double startOffset) {
    out.clear();
    if (n == 0) return;
    const double ratio = outRate / inRate;
    // the filter bank depends on the ratio only and is costly to design: keep the last few
    static std::mutex bmu;
    static std::vector<std::pair<double, std::shared_ptr<const Bank>>> bankCache;
    std::shared_ptr<const Bank> bp;
    {
        std::lock_guard<std::mutex> lk(bmu);
        for (auto& e : bankCache) if (e.first == ratio) bp = e.second;
        if (!bp) {
            bp = std::make_shared<const Bank>(makeBank(ratio));
            if (bankCache.size() >= 8) bankCache.erase(bankCache.begin());
            bankCache.push_back({ratio, bp});
        }
    }
    const Bank& b = *bp;
    const double step = inRate / outRate;
    const long count = (long)std::floor(((double)n - 1 - startOffset) / step) + 1;
    if (count <= 0) return;
    // split input with zero padding on both sides, so that the inner loop has no bounds checks
    const long pad = b.taps + 2;
    std::vector<float> re((size_t)n + 2 * pad, 0.f), im((size_t)n + 2 * pad, 0.f);
    for (size_t i = 0; i < n; i++) { re[i + pad] = in[i].real(); im[i + pad] = in[i].imag(); }
    out.resize((size_t)count);
    const int taps = b.stride;
    auto part = [&](long k0, long k1) {
        for (long k = k0; k < k1; k++) {
            const double pos = startOffset + (double)k * step;
            const long i = (long)std::floor(pos);
            int p0 = (int)std::lround((pos - (double)i) * b.phases);   // nearest phase of the fine bank: a timing error below 1/4000 of a sample
            const float* h = &b.h[(size_t)p0 * taps];
            const long base = i - (b.half - 1) + pad;
            const float* r = &re[(size_t)base];
            const float* m = &im[(size_t)base];
            float ar[4] = {0, 0, 0, 0}, ai[4] = {0, 0, 0, 0};
            for (int t = 0; t < taps; t += 4)
                for (int j = 0; j < 4; j++) { ar[j] += r[t + j] * h[t + j]; ai[j] += m[t + j] * h[t + j]; }
            out[(size_t)k] = cf32((ar[0] + ar[1]) + (ar[2] + ar[3]), (ai[0] + ai[1]) + (ai[2] + ai[3]));
        }
    };
    // long blocks are split over a few threads (the outputs do not depend on each other)
    const int hw = (int)std::thread::hardware_concurrency();
    const int nt = (count >= 200000 && hw >= 4) ? std::min(4, hw / 2) : 1;
    if (nt <= 1) { part(0, count); return; }
    std::vector<std::thread> th;
    for (int t = 1; t < nt; t++) th.emplace_back(part, count * t / nt, count * (t + 1) / nt);
    part(0, count / nt);
    for (auto& t : th) t.join();
}

void derotate(cf32* x, size_t n, double f, double rate, double start) {
    // advance the phase in steps (a long cos/sin per sample would be slow); renormalise now and then
    const double w = -2.0 * M_PI * f / rate;
    std::complex<double> ph = std::polar(1.0, w * start), step = std::polar(1.0, w);
    for (size_t i = 0; i < n; i++) {
        x[i] *= cf32((float)ph.real(), (float)ph.imag());
        ph *= step;
        if ((i & 1023) == 1023) ph /= std::abs(ph);
    }
}

double estimateCfoGuard(const cf32* x, size_t n, int fft, int guard, int symbols, double rate) {
    std::complex<double> acc(0, 0);
    const size_t sl = (size_t)fft + guard;
    for (int s = 0; s < symbols; s++) {
        size_t b = (size_t)s * sl;
        if (b + sl > n) break;
        for (int i = 0; i < guard; i++) acc += std::complex<double>(x[b + i]) * std::conj(std::complex<double>(x[b + fft + i]));
    }
    // x[n + fft] = x[n] e^{j 2 pi f fft / rate}, so the sum of x[n] conj(x[n + fft]) has the phase -2 pi f fft / rate
    return -std::arg(acc) * rate / (2.0 * M_PI * fft);
}

BootstrapFind findBootstrap(const cf32* x, size_t n, double maxCfo, double center, int seedOnly) {
    if (center != 0.0) {   // a known approximate offset: remove it first and search around zero
        std::vector<cf32> y(x, x + n);
        derotate(y.data(), y.size(), center, kFs, 0.0);
        BootstrapFind r = findBootstrap(y.data(), y.size(), maxCfo, 0.0, seedOnly);
        r.cfoHz += center;
        return r;
    }
    BootstrapFind res;
    const int L = kBootstrapSymbolLen;
    if (n < (size_t)5 * L) return res;
    // 1. autocorrelation at lag 2048 over 520 samples: the part C repeats the end of part A, whatever the carrier offset is
    const int lag = 2048, win = 520;
    const size_t m = n - lag;
    std::vector<std::complex<double>> pre(m + 1);
    std::vector<double> e1(m + 1), e2(m + 1);
    pre[0] = 0; e1[0] = e2[0] = 0;
    for (size_t i = 0; i < m; i++) {
        pre[i + 1] = pre[i] + std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i + lag]));
        e1[i + 1] = e1[i] + std::norm(std::complex<double>(x[i]));
        e2[i + 1] = e2[i] + std::norm(std::complex<double>(x[i + lag]));
    }
    std::vector<double> met(m - win + 1);
    for (size_t p = 0; p + win <= m; p++) {
        double a = e1[p + win] - e1[p], b = e2[p + win] - e2[p];
        met[p] = (a > 0 && b > 0) ? std::abs(pre[p + win] - pre[p]) / std::sqrt(a * b) : 0.0;
    }
    // 2. the strongest peaks, at least one symbol apart
    std::vector<std::pair<double, size_t>> cand;
    {
        std::vector<std::pair<double, size_t>> all;
        for (size_t p = 0; p < met.size(); p++) if (met[p] > 0.35 && (p == 0 || met[p] >= met[p - 1]) && (p + 1 == met.size() || met[p] > met[p + 1])) all.push_back({met[p], p});
        std::sort(all.rbegin(), all.rend());
        for (auto& c : all) {
            bool near = false;
            for (auto& k : cand) if (std::llabs((long long)k.second - (long long)c.second) < 1500) near = true;
            if (!near) cand.push_back(c);
            if (cand.size() >= 5) break;
        }
    }
    // 3. for each peak: carrier offsets (fractional from the phase, plus whole carrier spacings), then the template correlation
    double bestMetric = 0;
    for (auto& cd : cand) {
        const size_t pa = cd.second;
        std::complex<double> z = pre[pa + win] - pre[pa];
        const double fFrac = -std::arg(z) * kFs / (2.0 * M_PI * lag);
        const int mMax = (int)std::ceil(maxCfo / 3000.0);
        // the window: 4 symbols plus margins on both sides
        const long w0 = (long)pa - 40;
        if (w0 < 0 || (size_t)(w0 + 4 * L + 100) > n) continue;
        for (int mi = -mMax; mi <= mMax; mi++) {
            const double f = fFrac + mi * 3000.0;
            if (std::fabs(f) > maxCfo + 1500) continue;
            std::vector<cf32> seg(x + w0, x + std::min(n, (size_t)(w0 + 8 * L + 100)));
            derotate(seg.data(), seg.size(), f, kFs, (double)w0);
            // quick test of the hypothesis: the correlation with the first symbol over a few lags and the eight seeds
            Detection det = detectBootstrap(seg.data(), seg.size(), seedOnly);
            if (!det.found || det.metric < 0.30f) continue;
            if (det.metric > bestMetric) {
                bestMetric = det.metric;
                res.found = true;
                res.start = (double)w0 + (double)det.start;
                res.cfoHz = f;
                res.metric = det.metric;
                res.det = det;
            }
        }
        if (res.found && res.det.valid && res.metric > 0.6f) break;
    }
    if (!res.found) return res;
    // 4. refine the offset from the residual phase slope over the first symbol: compare the two halves
    {
        const long b = (long)std::llround(res.start);
        if (b >= 0 && (size_t)(b + 4 * L) <= n) {
            std::vector<cf32> seg(x + b, x + b + 4 * L);
            derotate(seg.data(), seg.size(), res.cfoHz, kFs, (double)b);
            auto sym = generateBootstrap(res.det.info);
            std::complex<double> c1(0, 0), c2(0, 0);
            const int half = 2 * L;   // the first two symbols against the first two of the template
            for (int i = 0; i < half; i++) c1 += std::complex<double>(seg[i]) * std::conj(std::complex<double>(sym[i]));
            for (int i = half; i < 4 * L && i < (int)sym.size(); i++) c2 += std::complex<double>(seg[i]) * std::conj(std::complex<double>(sym[i]));
            // symbols 2 and 3 carry the signalling shifts and are not identical to the template when the template is wrong, but with the right fields they are
            if (res.det.valid && std::abs(c1) > 0 && std::abs(c2) > 0) {
                double dphi = std::arg(c2 * std::conj(c1));
                res.cfoHz += dphi * kFs / (2.0 * M_PI * half);
            }
        }
    }
    return res;
}

// ---- streaming


// ---- frame decoding on worker threads

struct Atsc3Sync::Pool {
    struct Job { long seq; std::vector<cf32> y; Bootstrap bs; };
    struct Done { FrameResult fr; Bootstrap bs; };
    Atsc3Receiver* rx;
    std::mutex m;
    std::condition_variable cv, cvSpace;
    std::deque<Job> jobs;
    std::map<long, Done> done;
    long nextSeq = 0, nextCommit = 0;
    int inflight = 0, maxInflight = 2;
    bool committing = false, stop = false;
    double secDecode = 0;
    std::vector<std::thread> th;

    Pool(Atsc3Receiver* r, int n) : rx(r), maxInflight(2 * n) {
        for (int i = 0; i < n; i++) th.emplace_back([this] { run(); });
    }
    ~Pool() {
        flush();
        { std::lock_guard<std::mutex> lk(m); stop = true; }
        cv.notify_all();
        for (auto& t : th) t.join();
    }
    void submit(std::vector<cf32>&& y, const Bootstrap& bs) {
        std::unique_lock<std::mutex> lk(m);
        cvSpace.wait(lk, [&] { return inflight < maxInflight; });
        jobs.push_back({nextSeq++, std::move(y), bs});
        inflight++;
        cv.notify_one();
    }
    void flush() {
        std::unique_lock<std::mutex> lk(m);
        cvSpace.wait(lk, [&] { return inflight == 0; });
    }
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) return;
                j = std::move(jobs.front());
                jobs.pop_front();
            }
            const auto t0 = std::chrono::steady_clock::now();
            Done d{decodeFrame(j.y.data(), j.y.size(), j.bs), j.bs};
            std::unique_lock<std::mutex> lk(m);
            secDecode += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            done.emplace(j.seq, std::move(d));
            if (committing) continue;   // the thread that is committing carries on with this one
            committing = true;
            while (!done.empty() && done.begin()->first == nextCommit) {
                Done c = std::move(done.begin()->second);
                done.erase(done.begin());
                nextCommit++;
                lk.unlock();
                rx->commitFrame(c.fr, c.bs);
                lk.lock();
                inflight--;
                cvSpace.notify_all();
            }
            committing = false;
        }
    }
};

Atsc3Sync::Atsc3Sync(double rate, Atsc3Receiver* rx) : rate_(rate), rx_(rx) {}
Atsc3Sync::~Atsc3Sync() = default;

void Atsc3Sync::setThreads(int n) {
    pool_.reset();
    if (n > 1) pool_.reset(new Pool(rx_, n));
}

void Atsc3Sync::flush() { if (pool_) pool_->flush(); }

SyncStats Atsc3Sync::stats() const {
    SyncStats s = st_;
    if (pool_) { std::lock_guard<std::mutex> lk(pool_->m); s.secDecode = pool_->secDecode; }
    return s;
}

void Atsc3Sync::push(const cf32* x, size_t n) {
    raw_.insert(raw_.end(), x, x + n);
    work();
}

namespace {
double secondsToNext(const Bootstrap& b) { return minTimeToNextMs(b.minTimeToNext) / 1000.0; }
}

bool Atsc3Sync::search() {
    const auto t0 = std::chrono::steady_clock::now();
    struct Timer { double& d; std::chrono::steady_clock::time_point t; ~Timer() { d += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); } } tm{st_.secSearch, t0};
    // look at everything available beyond the search start (a few frames long at most), converted to the bootstrap rate
    const long from = std::max(searchFrom_, rawStart_);
    const long avail = rawStart_ + (long)raw_.size() - from;
    const double rate = rateEff();
    const long need = (long)(rate * (8.0 * 3072 / kBootstrapRate + 0.003));
    if (avail < need * 2) return false;
    // with a hint (the previous frame) a short window is enough; without one, look at a quarter of a second
    const long take = std::min<long>(avail, (long)(rate * (haveHint_ ? 0.01 : 0.25)));
    // the expected offset is removed at the radio's rate, before the band is cut to the bootstrap's: a channel recorded off centre would
    // otherwise lose an edge in the resampler. Without a hint the centre comes from the spectrum.
    std::vector<cf32> seg(raw_.begin() + (from - rawStart_), raw_.begin() + (from - rawStart_) + take);
    if (!haveHint_) coarse_ = centreOfChannel(seg.data(), seg.size());
    const double pre = haveHint_ ? cfo_ : coarse_;
    derotate(seg.data(), seg.size(), pre, rate, (double)from);
    std::vector<cf32> y;
    resampleExact(seg.data(), seg.size(), rate, kBootstrapRate, y);
    st_.searches++;
    auto bf = haveHint_ ? findBootstrap(y.data(), y.size(), 2500.0, 0.0, bs_.minorVersion) : findBootstrap(y.data(), y.size(), 20000.0);
    bf.cfoHz += pre;
    if (haveHint_ && !bf.found && ++hintMisses_ > 100) { haveHint_ = false; hintMisses_ = 0; sroPpm_ = 0; sroCount_ = 0; predNext_ = -1; }   // lost: search widely again
    if (!bf.found || !bf.det.valid) {
        // the bootstrap may straddle the end: keep an overlap of five symbols (a detection needs four after its start)
        searchFrom_ = from + take - (long)(rate * 5.0 * 3072 / kBootstrapRate);
        return take == avail ? false : true;
    }
    bs_ = bf.det.info;
    cfo_ = bf.cfoHz;
    const double bsExact = (double)from + bf.start * rate / kBootstrapRate;
    bsStart_ = (long)std::llround(bsExact);
    // the clock: the previous frame said where this bootstrap starts; the difference over the frame's length is the clock error left
    if (predNext_ > 0 && predFrom_ >= 0 && predNext_ > predFrom_) {
        const double ppm = (bsExact - predNext_) / (predNext_ - predFrom_) * 1e6;
        if (std::fabs(ppm) < 400.0) {   // more: not the frame that was predicted (a gap, lost samples, a frame not read)
            sroCount_++;
            sroPpm_ += (sroCount_ < 4 ? 0.9 : 0.3) * ppm;
            sroPpm_ = std::max(-300.0, std::min(300.0, sroPpm_));
        }
    }
    predNext_ = -1;
    predFrom_ = bsExact;
    bsExact_ = bsExact;
    const double bsLen = bs_.numSymbols * 3072 / kBootstrapRate;
    frameStart_ = bsStart_ + (long)std::llround(bsLen * rateEff());
    minToNextSec_ = secondsToNext(bs_);
    locked_ = true;
    haveHint_ = true;
    frameSamples_ = 0;
    st_.bootstraps++;
    st_.locked = true;
    st_.cfoHz = cfo_;
    st_.sroPpm = sroPpm_;
    return true;
}

// The centre of the strongest 5.83 MHz wide block of the spectrum (an ATSC 3.0 channel of 6 MHz, the bootstrap's band and more), in Hz
// from the middle of the sample band; 0 when nothing stands out.
double Atsc3Sync::centreOfChannel(const cf32* x, size_t n) const {
    const int lg = 10, N = 1 << lg;
    if (n < (size_t)N * 8) return 0.0;
    std::vector<double> P((size_t)N, 0.0);
    std::vector<float> re((size_t)N), im((size_t)N);
    const size_t blocks = std::min<size_t>(n / N, 256), stride = n / blocks;
    for (size_t b = 0; b < blocks; b++) {
        const cf32* p = x + b * stride;
        bool finite = true;
        for (int i = 0; i < N; i++) {
            const float w = 0.5f - 0.5f * std::cos(2.0f * (float)M_PI * (i + 0.5f) / N);
            re[(size_t)i] = p[i].real() * w; im[(size_t)i] = p[i].imag() * w;
            if (!std::isfinite(re[(size_t)i]) || !std::isfinite(im[(size_t)i])) finite = false;
        }
        if (!finite) continue;
        fftSplit(re.data(), im.data(), lg, false);
        for (int k = 0; k < N; k++) P[(size_t)((k + N / 2) % N)] += (double)re[(size_t)k] * re[(size_t)k] + (double)im[(size_t)k] * im[(size_t)k];
    }
    std::vector<double> cum((size_t)N + 1, 0.0);
    for (int i = 0; i < N; i++) cum[(size_t)i + 1] = cum[(size_t)i] + P[(size_t)i];
    const double bin = rate_ / N;
    // the two edges of the occupied band (4.5 MHz for the bootstrap, 5.83 MHz for a frame in a 6 MHz channel, up to about 7.8 MHz in an 8 MHz one, depending on the bootstrap's
    // sample rate coefficient and the carriers), in decibels so that the ripple of an echo does not move them: the left edge where the
    // power rises most, the right one where it falls most
    const int S = std::max(2, (int)std::lround(0.06e6 / bin)), W0 = (int)std::lround(4.4e6 / bin), W1 = (int)std::lround(8.0e6 / bin);
    if (W0 + 2 * S + 2 >= N) return 0.0;
    const double floorP = std::max(1e-3 * cum[(size_t)N] / N, 1e-30);   // no edge counts for more than 30 dB (an empty, noise-free stopband)
    auto dB = [&](int a, int b) { return std::log(std::max((cum[(size_t)b] - cum[(size_t)a]) / (b - a), floorP)); };
    std::vector<double> rise((size_t)N, -1e30), fall((size_t)N, -1e30);
    for (int i = S; i + S <= N; i++) { rise[(size_t)i] = dB(i, i + S) - dB(i - S, i); fall[(size_t)i] = -rise[(size_t)i]; }
    double best = -1e30;
    int bi = -1;
    for (int a = S; a + W0 + S <= N; a++)
        for (int b = a + W0; b <= std::min(N - S, a + W1); b++) {
            const double score = rise[(size_t)a] + fall[(size_t)b];
            if (score > best) { best = score; bi = a + b; }   // twice the centre, in bins
        }
    if (bi < 0 || best < std::log(10.0)) return 0.0;   // edges of less than 5 dB each: no channel shape, leave the search at the centre
    return (0.5 * bi - N / 2) * bin;
}

bool Atsc3Sync::cutFrame() {
    struct Timer { double& d; std::chrono::steady_clock::time_point t; ~Timer() { d += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); } } tm{st_.secCut, std::chrono::steady_clock::now()};
    const double frameRate = postBootstrapRate(bs_);
    const double rate = rateEff();
    PreambleParams pp;
    if (!preambleParams(bs_.preambleStructure, pp)) { locked_ = false; searchFrom_ = bsStart_ + (long)(rate * 0.01); return true; }
    const long availEnd = rawStart_ + (long)raw_.size();
    // 1. the Preamble tells the length of the frame (it can be longer than the signalled minimum time to the next frame)
    if (frameSamples_ == 0) {
        const long headSamples = (long)std::ceil((double)(pp.fftSize + pp.guard) * 6 * rate / frameRate) + 64;   // up to six Preamble symbols
        if (availEnd < frameStart_ + headSamples) return false;
        std::vector<cf32> head(raw_.begin() + (frameStart_ - rawStart_), raw_.begin() + (frameStart_ - rawStart_) + headSamples);
        derotate(head.data(), head.size(), cfo_, rate, (double)frameStart_);
        std::vector<cf32> y;
        resampleExact(head.data(), head.size(), rate, frameRate, y);
        if (y.size() > (size_t)(pp.fftSize + pp.guard)) {
            double res = estimateCfoGuard(y.data(), y.size(), pp.fftSize, pp.guard, 1, frameRate);
            if (std::fabs(res) < 0.4 * frameRate / pp.fftSize) { cfo_ += res; st_.cfoHz = cfo_; }
        }
        // read L1 from the head, with a copy that is not modified
        PreambleResult pre = decodePreamble(y.data(), y.size(), bs_);
        if (pre.basicOk) {
            size_t len = pre.detailOk ? frameLengthSamples(bs_, pre.basic, pre.detail) : 0;
            if (len == 0) len = (size_t)(minToNextSec_ * frameRate);   // L1-Detail not readable: take the signalled minimum
            frameExact_ = (double)len * rate / frameRate;
            frameSamples_ = (long)std::ceil(frameExact_);
        } else {
            // not a frame we can read: look for the next bootstrap
            missed_++;
            searchFrom_ = bsStart_ + (long)std::llround((minToNextSec_ - 0.002) * rate);
            locked_ = false;
            return true;
        }
    }
    if (availEnd < frameStart_ + frameSamples_ + 16) return false;
    // 2. the whole frame: carrier offset removed at the radio's rate, then the rate converted to the frame's
    std::vector<cf32> seg(raw_.begin() + (frameStart_ - rawStart_), raw_.begin() + (frameStart_ - rawStart_) + frameSamples_ + 16);
    derotate(seg.data(), seg.size(), cfo_, rate, (double)frameStart_);
    std::vector<cf32> y;
    resampleExact(seg.data(), seg.size(), rate, frameRate, y);
    if (pool_) {
        pool_->submit(std::move(y), bs_);
        st_.frames++;
    } else {
        const auto td = std::chrono::steady_clock::now();
        bool ok = rx_->pushFrame(y.data(), y.size(), bs_);
        st_.secDecode += std::chrono::duration<double>(std::chrono::steady_clock::now() - td).count();
        if (ok) { st_.frames++; missed_ = 0; } else missed_++;
    }
    // the next bootstrap comes no earlier than the signalled time, and not before this frame is over
    long next = std::max<long>(bsStart_ + (long)std::llround((minToNextSec_ - 0.002) * rate), frameStart_ + frameSamples_ - (long)(rate * 0.0005));
    searchFrom_ = next;
    predNext_ = bsExact_ + bs_.numSymbols * 3072 / kBootstrapRate * rate + frameExact_;   // frames follow each other without a gap: the next bootstrap starts here
    locked_ = false;
    frameSamples_ = 0;
    return true;
}

void Atsc3Sync::work() {
    for (int guard = 0; guard < 64; guard++) {
        bool progress;
        if (!locked_) progress = search();
        else progress = cutFrame();
        // drop what is no longer needed
        long keepFrom = locked_ ? bsStart_ : std::max(searchFrom_, rawStart_);
        keepFrom -= (long)(rate_ * 0.002);
        if (keepFrom > rawStart_ && keepFrom - rawStart_ < (long)raw_.size()) {
            raw_.erase(raw_.begin(), raw_.begin() + (keepFrom - rawStart_));
            rawStart_ = keepFrom;
        }
        if (!progress) break;
    }
}

} // namespace atsc3
} // namespace dect2
