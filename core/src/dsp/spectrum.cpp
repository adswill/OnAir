#include "dect2/dsp/gpu_fft.h"
#include "dect2/dsp/gpu_policy.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/dsp/zoom_spectrum.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <mutex>

namespace dect2 {


// 10*log10(p) for p > 0 as a branch-free loop body the compiler vectorises (std::log10f is a library call per bin: with a 64k FFT that was
// two million calls a second, most of the cost at low sample rates). p = m * 2^e with m in [0.707, 1.414); log2(m) = 2/ln2 * atanh(z),
// z = (m-1)/(m+1), |z| < 0.172, so the series to z^7 is off by under 1e-7 in log2: below 0.00001 dB (checked in tests/dsp/test_spectrum_fastlog.cpp).
static inline float fastDb(float p) {
    uint32_t u; std::memcpy(&u, &p, 4);
    const int e0 = (int)((u >> 23) & 0xFF) - 127;
    u = (u & 0x007FFFFFu) | 0x3F800000u;           // mantissa as m0 in [1, 2)
    float m; std::memcpy(&m, &u, 4);
    const bool hi = m > 1.41421356f;               // fold to [0.707, 1.414)
    m = hi ? m * 0.5f : m;
    const float e = (float)(e0 + (hi ? 1 : 0));
    const float z = (m - 1.f) / (m + 1.f), z2 = z * z;
    const float l2 = 2.88539008f * z * (1.f + z2 * (1.f / 3 + z2 * (1.f / 5 + z2 * (1.f / 7))));
    return 3.01029996f * (e + l2);                 // 10*log10(2) * log2(p)
}

float spectrumDb(float p) { return fastDb(p); }

// DECT2_SPEC_LEGACY=1: the analyser as it was before 3.0 (16 blocks a frame, bins of 250 Hz up to 32k): for CPU comparisons only
static bool legacySpectrum() { static const bool l = std::getenv("DECT2_SPEC_LEGACY") != nullptr; return l; }

// Everything the feeding thread (the engine's spectrum thread) keeps to itself: its copy of the transform settings, the window, the scratch
// arrays and the partial block. It reads the shared settings under the mutex when a piece arrives, does the transforms WITHOUT the mutex (a
// 256k-point transform takes milliseconds, and the analysis thread that takes the frames must never wait for it), and adds the result to the
// shared sum under the mutex. A resize() or reset() bumps `gen`: blocks that were under way are dropped, not added to the new sum.
struct Worker {
    uint64_t gen = 0;
    size_t n = 0;
    int log2n = 0;
    std::vector<float> window, re, im;
    std::vector<cf32> pending;         // a partial block carried across feed() calls
    // Overlapped frames (large transforms): when two blocks do not fit in a frame period (2 n / fs > 1/30 s) the frame rate would fall to
    // fs / 2n (7 a second at 2.4 Msps with 131072 bins). Instead each frame is one transform of the latest n samples, started every fs / 35
    // samples (a little more often than the 30 a second the frames are taken), so the display keeps its 30 a second whatever the size.
    bool overlap = false;
    double fs = 0, hop = 0, nextAt = 0;
    uint64_t pos = 0;
    std::vector<cf32> ring;
    // fast rows
    uint64_t fastGen = 0;
    double rps = 0, stride = 0, nextEnd = 0, dt = 0;
    size_t fn = 0, fb = 1, fblocks = 0;
    int flog2 = 0;
    uint64_t fpos = 0;
    std::vector<cf32> fring;
    std::vector<float> fwin, fre, fim, facc, frow;
    int winType = 0;
    void reload(size_t nn, double sampleRate, int wt, double maxOverlap) {
        if (n != nn || window.size() != nn || winType != wt) {   // (a reset with the same size keeps the window)
            n = nn; log2n = (int)std::lround(std::log2((double)n)); winType = wt;
            window.resize(n); specwin::make(wt, window.data(), (int)n);
            re.assign(n, 0.f); im.assign(n, 0.f);
        }
        pending.clear();
        fs = sampleRate;
        overlap = fs > 0 && 2.0 * (double)n / fs > 1.0 / 30.0;
        hop = SpectrumAnalyzer::hopFor(fs, n, maxOverlap); pos = 0; nextAt = (double)n;   // (a little under the 33 ms tick: every tick finds a fresh block)
        if (overlap) ring.assign(n, cf32(0.f, 0.f)); else { ring.clear(); ring.shrink_to_fit(); }
    }
};

struct SpectrumAnalyzer::Impl {
    std::vector<float> accum;   // the sum of the powers of the blocks of the frame being made (n_ bins)
    size_t blocks = 0;
    // stats accumulators
    double sumSq = 0, sumI = 0, sumQ = 0;
    float peak = 0;
    uint64_t count = 0, clip = 0;
    uint32_t hist[64] = {};
    uint64_t seq = 0;
    uint64_t gen = 1;           // bumped by resize() and reset()
    double sampleRate = 0;      // setSampleRate(): 0 = unknown (blocks follow each other, no overlap)
    int window = 0;             // specwin::Type
    double maxOverlap = -1;     // setMaxOverlap (negative: automatic)
    std::atomic<bool> transform{true};
    std::atomic<size_t> maxBlocks{SpectrumAnalyzer::kDefaultBlocks};   // blocks averaged into one frame (the rest of the samples only count for the stats)
    // fast rows: the request (setFastRows), the queue the rows wait in (takeFastRows)
    double reqRps = 0, reqFs = 0;
    uint64_t reqGen = 0, fastDropped = 0;
    size_t fastBins = 0; double fastDt = 0;   // of the rows on the queue (set with them)
    std::vector<float> fastOut;
    std::mutex mu;   // the worker thread feeds, the analysis thread takes frames and resets
    Worker w;
};

SpectrumAnalyzer::SpectrumAnalyzer(size_t fftSize) : p_(new Impl), n_(fftSize) {
    p_->accum.assign(n_, 0.f);
    if (legacySpectrum()) p_->maxBlocks = 16;
}

SpectrumAnalyzer::~SpectrumAnalyzer() {
    delete p_;
}

// ---- fast rows -------------------------------------------------------------------------------------------------------------------------
size_t SpectrumAnalyzer::fastRowFft(double fs, double rps) {
    if (!(fs > 0) || !(rps > 0)) return 1024;
    double want = std::min(fs * 0.02, 8e6 / rps);   // ~20 ms of signal; rows a second x points <= 8 M (about a tenth of a core)
    size_t n = 1024;
    while (n < 65536 && (double)n * 2 <= want) n *= 2;
    return n;
}

void SpectrumAnalyzer::setFastRows(double rps, double fs) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    if (!(rps > 0) || !(fs > 0)) { rps = 0; fs = 0; }
    if (s.reqRps == rps && s.reqFs == fs) return;
    s.reqRps = rps; s.reqFs = fs; s.reqGen++;
    s.fastOut.clear(); s.fastDropped = 0;
}

size_t SpectrumAnalyzer::takeFastRows(std::vector<float>& rows, size_t& bins, double& dt, uint64_t& dropped) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& s = *p_;
    dropped = s.fastDropped; s.fastDropped = 0;
    if (s.reqRps <= 0 || s.fastOut.empty()) return 0;
    rows.clear();
    rows.swap(s.fastOut);   // the caller's old buffer becomes the next queue: no allocation in the steady state
    bins = s.fastBins; dt = s.fastDt;
    return s.fastBins ? rows.size() / s.fastBins : 0;
}

void SpectrumAnalyzer::setSampleRate(double fs) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->sampleRate == fs) return;
    p_->sampleRate = fs;
    p_->gen++;   // the feeding thread sets up again (overlap or not)
}

// ---- the transforms --------------------------------------------------------------------------------------------------------------------

// The latest n samples (a, then b: the two runs of a ring, or a alone) windowed into split arrays and transformed
static void windowFft(const cf32* a, size_t na, const cf32* b, size_t nb, const float* win, float* re, float* im, int log2n) {
    for (size_t i = 0; i < na; i++) { re[i] = a[i].real() * win[i]; im[i] = a[i].imag() * win[i]; }
    for (size_t i = 0; i < nb; i++) { re[na + i] = b[i].real() * win[na + i]; im[na + i] = b[i].imag() * win[na + i]; }
    fftSplit(re, im, log2n, false);
}

// One fast-row transform of the ring ending at w.fpos, added to the row being built; every fb blocks the row is finished (dB, DC-centred, as
// takeFrame) and put on the queue. Returns true when a row is ready in w.frow.
static bool fastBlock(Worker& w) {
    const size_t N = w.fn;
    const size_t at = (size_t)(w.fpos % N);   // the oldest sample sits where the next one will be written
    windowFft(w.fring.data() + at, N - at, w.fring.data(), at, w.fwin.data(), w.fre.data(), w.fim.data(), w.flog2);
    float* acc = w.facc.data(); const float* re = w.fre.data(); const float* im = w.fim.data();
    for (size_t i = 0; i < N; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
    if (++w.fblocks < w.fb) return false;
    const float inv = 1.0f / ((float)w.fb * (float)N * (float)N);
    const size_t h = N / 2;
    w.frow.resize(N);
    float* o = w.frow.data();
    for (size_t i = 0; i < h; i++) o[i] = fastDb(std::max(acc[i + h] * inv, 1e-14f));
    for (size_t i = h; i < N; i++) o[i] = fastDb(std::max(acc[i - h] * inv, 1e-14f));
    std::fill(w.facc.begin(), w.facc.end(), 0.f);
    w.fblocks = 0;
    return true;
}

void SpectrumAnalyzer::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    Worker& w = s.w;
    size_t maxBlocks, curN;
    uint64_t gen;
    bool doFft;
    double fsNow = 0, maxOv = -1;
    int winType = 0;
    uint64_t reqGen;
    double reqRps, reqFs;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        // Power, DC and peak over every sample, in eight float lanes per piece of 4096 samples (summed into doubles between pieces). The samples
        // are read as interleaved floats (I Q I Q ...) so the loop is plain float arithmetic the compiler vectorises: lanes 0,2,4,6 hold I and
        // 1,3,5,7 hold Q; power is the sum of squares of all lanes. (The complex<float> accessors made it a third slower.)
        // The ADC histogram and the clip count look at every fourth sample, each counted four times: they are display statistics. Four partial
        // histograms in turn: consecutive samples usually fall into the same low bin, and one array would wait on each previous increment.
        for (size_t p0 = 0; p0 < n; p0 += 4096) {
            const size_t pn = std::min<size_t>(4096, n - p0), p4 = pn & ~(size_t)3;   // whole groups of four samples = 8 floats
            const cf32* y = x + p0;
            const float* f = reinterpret_cast<const float*>(y);
            float sq[8] = {}, sm[8] = {}, pk[8] = {};
            for (size_t i = 0; i < p4 * 2; i += 8)
                for (int j = 0; j < 8; j++) {
                    const float v = f[i + j];
                    sq[j] += v * v; sm[j] += v;
                    pk[j] = std::max(pk[j], std::fabs(v));
                }
            for (int j = 0; j < 8; j++) {
                s.sumSq += sq[j]; (j & 1 ? s.sumQ : s.sumI) += sm[j];
                s.peak = std::max(s.peak, pk[j]);
            }
            for (size_t i = p4; i < pn; i++) {
                const float I = y[i].real(), Q = y[i].imag();
                s.sumSq += (double)I * I + (double)Q * Q; s.sumI += I; s.sumQ += Q;
                s.peak = std::max(s.peak, std::max(std::fabs(I), std::fabs(Q)));
            }
            uint32_t h4[4][64] = {};
            uint32_t clip = 0;
            // every fourth sample, in groups of 16 samples (64 input samples): the bins and clip flags are worked out in a plain loop the
            // compiler vectorises, then only the increments run one by one
            size_t i = 0;
            for (; i + 64 <= pn; i += 64) {
                uint8_t bin[32];
                for (int k = 0; k < 16; k++) {
                    const float I = std::fabs(y[i + 4 * k].real()), Q = std::fabs(y[i + 4 * k].imag());
                    clip += std::max(I, Q) >= 126.0f / 128.0f;
                    bin[2 * k] = (uint8_t)std::min(63.f, I * 64);   // clamp as a float first: converting a huge value is undefined (INT_MIN on x86)
                    bin[2 * k + 1] = (uint8_t)std::min(63.f, Q * 64);
                }
                for (int k = 0; k < 32; k += 4) { h4[0][bin[k]]++; h4[1][bin[k + 1]]++; h4[2][bin[k + 2]]++; h4[3][bin[k + 3]]++; }
            }
            for (; i < pn; i += 4) {
                const float I = std::fabs(y[i].real()), Q = std::fabs(y[i].imag());
                clip += std::max(I, Q) >= 126.0f / 128.0f;
                h4[0][(int)std::min(63.f, I * 64)]++; h4[0][(int)std::min(63.f, Q * 64)]++;
            }
            s.clip += 4 * (uint64_t)clip;
            for (int b = 0; b < 64; b++) s.hist[b] += 4 * (h4[0][b] + h4[1][b] + h4[2][b] + h4[3][b]);
        }
        s.count += n;

        maxBlocks = s.maxBlocks.load(std::memory_order_relaxed);
        doFft = s.transform.load();
        gen = s.gen; curN = n_;
        fsNow = s.sampleRate; winType = s.window; maxOv = s.maxOverlap;
        reqGen = s.reqGen; reqRps = s.reqRps; reqFs = s.reqFs;
    }
    if (!doFft) { w.pending.clear(); return; }
    if (w.gen != gen) { w.reload(curN, fsNow, winType, maxOv); w.gen = gen; }
    // fast rows: their own transforms; the frames below are not touched
    if (w.fastGen != reqGen) {
        w.fastGen = reqGen; w.rps = reqRps;
        if (reqRps > 0) {
            const size_t N = fastRowFft(reqFs, reqRps);
            const double hop = std::max(4.0, reqFs / reqRps);   // samples per row
            w.fn = N; w.flog2 = (int)std::lround(std::log2((double)N));
            w.fb = (size_t)std::max(1.0, std::min(4.0, std::floor(hop / (double)N)));
            w.stride = hop / (double)w.fb; w.dt = hop / reqFs;
            w.fring.assign(N, cf32(0.f, 0.f));
            w.fwin.resize(N); hannWindowNorm(w.fwin.data(), (int)N);
            w.fre.assign(N, 0.f); w.fim.assign(N, 0.f); w.facc.assign(N, 0.f);
            w.fpos = 0; w.fblocks = 0; w.nextEnd = (double)N;
        } else { w.fring.clear(); w.fring.shrink_to_fit(); }
    }
    if (w.rps > 0) {
        const size_t N = w.fn;
        size_t used = 0;
        while (used < n) {
            const uint64_t endAt = (uint64_t)std::llround(w.nextEnd);
            size_t take = n - used;
            if (endAt > w.fpos) take = (size_t)std::min<uint64_t>(take, endAt - w.fpos);
            for (size_t k = 0; k < take;) {   // into the ring (at most two runs)
                const size_t at = (size_t)((w.fpos + k) % N), run = std::min(take - k, N - at);
                std::copy(x + used + k, x + used + k + run, w.fring.begin() + (long)at);
                k += run;
            }
            w.fpos += take; used += take;
            if (w.fpos >= endAt) {
                if (w.fpos >= N && fastBlock(w)) {
                    std::lock_guard<std::mutex> lk(s.mu);   // (only to put the row on the queue)
                    if (s.reqGen == w.fastGen) {
                        s.fastBins = N; s.fastDt = w.dt;
                        const size_t maxRows = std::max<size_t>(16, (size_t)(8u << 20) / (4 * N));   // 8 MB of queue
                        if (s.fastOut.size() / N >= maxRows) { s.fastOut.erase(s.fastOut.begin(), s.fastOut.begin() + (long)N); s.fastDropped++; }
                        s.fastOut.insert(s.fastOut.end(), w.frow.begin(), w.frow.end());
                    }
                }
                w.nextEnd += w.stride;
                if (w.nextEnd < (double)w.fpos) w.nextEnd = (double)w.fpos + w.stride;   // far behind (a stall): catch up without a burst
            }
        }
    }

    const size_t N = w.n;
    // adds the power of the block in w.re / w.im to the frame being made, unless the settings changed meanwhile or the frame is full
    auto add = [&]() {
        std::lock_guard<std::mutex> lk(s.mu);
        if (w.gen != s.gen || s.blocks >= maxBlocks) return;
        float* acc = s.accum.data(); const float* re = w.re.data(); const float* im = w.im.data();
        for (size_t i = 0; i < N; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
        s.blocks++;
    };
    auto full = [&]() {   // display doesn't need every block: when the frame has its blocks, the rest of the samples only count for the stats
        std::lock_guard<std::mutex> lk(s.mu);
        return w.gen != s.gen || s.blocks >= maxBlocks;
    };
    if (w.overlap) {
        // every fs / 35 samples one transform of the latest N samples
        size_t used = 0;
        while (used < n) {
            const uint64_t endAt = (uint64_t)std::llround(w.nextAt);
            size_t take = n - used;
            if (endAt > w.pos) take = (size_t)std::min<uint64_t>(take, endAt - w.pos);
            for (size_t k = 0; k < take;) {
                const size_t at = (size_t)((w.pos + k) % N), run = std::min(take - k, N - at);
                std::copy(x + used + k, x + used + k + run, w.ring.begin() + (long)at);
                k += run;
            }
            w.pos += take; used += take;
            if (w.pos >= endAt) {
                if (w.pos >= N && !full()) {
                    const size_t at = (size_t)(w.pos % N);
                    windowFft(w.ring.data() + at, N - at, w.ring.data(), at, w.window.data(), w.re.data(), w.im.data(), w.log2n);
                    add();
                }
                w.nextAt += w.hop;
                if (w.nextAt < (double)w.pos) w.nextAt = (double)w.pos + w.hop;
            }
        }
        return;
    }
    // One block: window into split re/im, FFT, add the power. Whole blocks are read straight from the caller's samples; only a partial
    // block at the end is copied (and completed by the next call).
    auto block = [&](const cf32* b) {
        windowFft(b, N, nullptr, 0, w.window.data(), w.re.data(), w.im.data(), w.log2n);
        add();
    };
    if (full()) { w.pending.clear(); return; }
    const cf32* src = x; size_t left = n;
    if (!w.pending.empty()) {   // finish the block left over from the last call
        const size_t take = std::min(left, N - w.pending.size());
        w.pending.insert(w.pending.end(), src, src + take);
        src += take; left -= take;
        if (w.pending.size() < N) return;
        block(w.pending.data());
        w.pending.clear();
    }
    // GPU (gpu_fft.h): whole blocks go to the GPU in one batch (window, FFT and power sum on it) when the "Spectrum" GPU tick and compute mode allow
    // it. Auto only from `kGpuAutoMin` points: below that the CPU transform finishes before a GPU round trip does (docs/GPU-AUDIT-3.7.md).
    {
        const int gm = gpupol::liveMode(gpupol::Dsp);
        static constexpr size_t kGpuAutoMin = 1u << 30;   // set from the measurement in docs/GPU-AUDIT-3.7.md: no size beat the CPU, so Auto stays on the CPU
        const bool want = gm != 0 && (gm == 1 || N >= kGpuAutoMin) && GpuFft::instance().available();
        if (want && left >= N && !full()) {
            size_t room; { std::lock_guard<std::mutex> lk(s.mu); room = w.gen != s.gen || s.blocks >= maxBlocks ? 0 : maxBlocks - s.blocks; }
            const size_t k = std::min(left / N, room);
            if (k > 0) {
                std::vector<float> part(N, 0.f);
                if (GpuFft::instance().powerAccumulate(src, (int)k, N, w.window.data(), part.data())) {
                    std::lock_guard<std::mutex> lk(s.mu);
                    if (w.gen == s.gen && s.blocks < maxBlocks) {
                        float* acc = s.accum.data();
                        for (size_t i = 0; i < N; i++) acc[i] += part[i];
                        s.blocks += k;
                    }
                    gpupol::reportGpu(gpupol::Dsp, true);
                    src += k * N; left -= k * N;
                }
            }
        } else gpupol::reportGpu(gpupol::Dsp, false);
    }
    while (left >= N && !full()) { block(src); src += N; left -= N; }
    if (left && left < N && !full()) w.pending.assign(src, src + left);
}

void SpectrumAnalyzer::setTransform(bool on) { p_->transform = on; }

void SpectrumAnalyzer::setWindow(int type) {
    type = std::max(0, std::min((int)specwin::NumTypes - 1, type));
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->window == type) return;
    p_->window = type;
    std::fill(p_->accum.begin(), p_->accum.end(), 0.f);
    p_->blocks = 0; p_->gen++;
}
int SpectrumAnalyzer::window() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->window; }

void SpectrumAnalyzer::setMaxOverlap(double f) {
    if (f >= 0) f = std::min(0.95, f);
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->maxOverlap == f) return;
    p_->maxOverlap = f; p_->gen++;
}

double SpectrumAnalyzer::hopFor(double fs, size_t n, double maxOverlap) {
    double hop = fs / 35.0;
    if (maxOverlap >= 0) hop = std::max(hop, (double)n * (1.0 - std::min(0.95, maxOverlap)));
    else if (n > (size_t(1) << 18)) hop = std::max(hop, (double)n * 0.25);
    return std::max(1.0, hop);
}
void SpectrumAnalyzer::setBlocksPerFrame(size_t n) { p_->maxBlocks = std::max<size_t>(1, std::min<size_t>(64, n)); }
size_t SpectrumAnalyzer::blocksPerFrame() const { return p_->maxBlocks.load(); }

size_t SpectrumAnalyzer::autoSize(double fs) {
    // bins of about 125 Hz or finer, 8192..65536: a zoomed-in view keeps its detail (with two blocks a frame a 64k transform costs a
    // quarter of what 16 blocks of 32k did)
    size_t n = 8192;
    if (legacySpectrum()) { while (n < 32768 && fs / (double)n > 250.0) n *= 2; return n; }
    while (n < 65536 && fs / (double)n > 125.0) n *= 2;
    return n;
}

void SpectrumAnalyzer::resize(size_t fftSize) {
    fftSize = std::min<size_t>(SpectrumAnalyzer::kMaxFft, std::max<size_t>(1024, fftSize));
    size_t pw = 1024; while (pw < fftSize) pw *= 2;
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (pw == n_) return;
    n_ = pw;
    s.accum.assign(n_, 0.f);
    s.blocks = 0; s.gen++;   // the feeding thread sets up for the new size (its partial block is dropped)
}

void SpectrumAnalyzer::reset() {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.gen++;   // (the partial block of the feeding thread goes too)
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
}

bool SpectrumAnalyzer::takeFrame(SpectrumFrame& out) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.count == 0 || (s.blocks == 0 && s.transform)) return false;
    out.dbfs.resize(n_);
    // normalise so the sum over bins equals total power (dBFS/bin)
    float inv = 1.0f / ((float)std::max<size_t>(s.blocks, 1) * (float)n_ * (float)n_);
    // DC-centre: the upper half of the FFT output goes first, then the lower half (n_ is a power of two, so the halves are exact)
    const size_t h = n_ / 2;
    float* o = out.dbfs.data(); const float* ac = s.accum.data();
    for (size_t i = 0; i < h; i++) o[i] = fastDb(std::max(ac[i + h] * inv, 1e-14f));
    for (size_t i = h; i < n_; i++) o[i] = fastDb(std::max(ac[i - h] * inv, 1e-14f));
    out.stats.peak = s.peak;
    out.stats.rmsDbfs = 10.0f * std::log10(std::max(s.sumSq / (double)s.count, 1e-12));
    out.stats.clipFraction = (float)s.clip / (float)s.count;
    out.stats.dcI = (float)(s.sumI / s.count);
    out.stats.dcQ = (float)(s.sumQ / s.count);
    std::copy(std::begin(s.hist), std::end(s.hist), out.stats.hist);
    out.seq = ++s.seq;

    std::fill(s.accum.begin(), s.accum.end(), 0.f);
    s.blocks = 0; s.sumSq = s.sumI = s.sumQ = 0; s.peak = 0; s.count = s.clip = 0;
    std::fill(std::begin(s.hist), std::end(s.hist), 0u);
    return true;
}

} // namespace dect2
