#include "dect2/t2plp.h"
#include "dect2/gpu_ldpc.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "dect2/platform.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

namespace {
template <class F>
void parallelFor(int n, int threads, F fn) {
    if (threads <= 1 || n <= 1) { for (int i = 0; i < n; i++) fn(i); return; }
    std::atomic<int> next{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < std::min(threads, n); t++)
        ts.emplace_back([&] { setThreadPriority(ThreadPriority::Background); for (int i; (i = next++) < n;) fn(i); });
    for (auto& t : ts) t.join();
}
} // namespace

PlpResult PlpDecoder::decodeNow(const PlpJob& job, int threads, bool useGpu, const std::atomic<int>* backlog) {
    auto t0 = std::chrono::steady_clock::now();
    PlpResult res;
    res.frameNo = job.frameNo; res.t2Frame = job.t2Frame; res.frameSec = job.frameSec; res.plpId = job.plpId; res.fec = job.fec;
    FecDims d = fecDims(job.fec);
    if (!d.ok || job.numBlocks <= 0 || (int)job.cells.size() < job.numBlocks * d.cellsPerBlock) return res;
    const int nb = job.numBlocks, cells = d.cellsPerBlock, bps = d.bitsPerCell;
    res.blocks = nb;
    std::vector<cf32> rx((size_t)nb * cells);
    std::vector<float> n0((size_t)nb * cells);
    cellDeinterleave(job.fec, nb, job.tiBlocks, job.cells.data(), rx.data());
    cellDeinterleaveF(job.fec, nb, job.tiBlocks, job.n0.data(), n0.data());
    if (getenv("DECT2_PLPDUMP")) {
        double m = 0, nn = 0; size_t cnt = job.cells.size();
        for (auto& c : job.cells) m += std::abs(c);
        for (float v : job.n0) nn += v;
        fprintf(stderr, "[plp] frame %d cells %zu mean|c| %.3f mean n0 %.4f blocks %d ti %d mod %d rate %d rot %d\n", job.t2Frame, cnt, m / std::max<size_t>(1, cnt), nn / std::max<size_t>(1, job.n0.size()), nb, job.tiBlocks, job.fec.mod, job.fec.rate, (int)job.fec.rotation);
    }
    const LdpcCode& ldpc = ldpcFor(job.fec);
    const BchCode& bch = bchFor(job.fec);
    const auto& map = bitInterleaverMap(job.fec);

    struct BlockOut {
        bool ok = false;
        int iters = 0, corrected = 0;
        long errBits = 0;
        bool retried = false;
        double sigP = 0, errP = 0;
        BbFrame frame;
        std::vector<cf32> pts;
        std::vector<uint8_t> perr;
        std::vector<uint16_t> ptx;
    };
    std::vector<BlockOut> outs(nb);
    // GPU path: demap every block, decode all LDPC blocks in one batch on the GPU, then finish (BCH etc.) on the CPU
    std::vector<float> gLlr;
    std::vector<uint8_t> gHard, gOk;
    std::vector<int> gIt;
    if (useGpu && GpuLdpc::instance().available()) {
        gLlr.resize((size_t)nb * d.nLdpc);
        parallelFor(nb, threads, [&](int b) {
            std::vector<float> llrLab((size_t)cells * bps);
            qamDemapBlock(job.fec, rx.data() + (size_t)b * cells, n0.data() + (size_t)b * cells, cells, llrLab.data());
            float* llr = gLlr.data() + (size_t)b * d.nLdpc;
            for (int p = 0; p < d.nLdpc; p++) llr[map[p]] = llrLab[p];
        });
        gHard.resize((size_t)nb * d.nLdpc); gOk.resize(nb); gIt.resize(nb);
        if (GpuLdpc::instance().decode(ldpc, gLlr.data(), nb, 50, gHard.data(), gOk.data(), gIt.data())) res.usedGpu = true;
        else { gLlr.clear(); }
    }
    const bool gpuDone = res.usedGpu;
    std::atomic<int> tried{0}, failedFirst{0};
    std::atomic<bool> recovered{false};
    parallelFor(nb, threads, [&](int b) {
        BlockOut& o = outs[b];
        // A frame whose first blocks all fail to decode may be garbage (lost sync) or just in a fade that only hits part of the
        // frame. Do not burn CPU on the rest, but keep probing: every 5th block gets a plain first-pass decode, and as soon as one
        // decodes the fade is over and the remaining blocks are decoded in full.
        const bool lost = tried.load() >= 16 && failedFirst.load() >= 16 && !recovered.load();
        if (lost && (b % 5) != 0) { o.frame.blockIndex = b; return; }
        const cf32* c = rx.data() + (size_t)b * cells;
        const float* nn = n0.data() + (size_t)b * cells;
        std::vector<float> llr(d.nLdpc);
        std::vector<uint8_t> hard;
        if (gpuDone) {
            std::copy(gLlr.begin() + (size_t)b * d.nLdpc, gLlr.begin() + (size_t)(b + 1) * d.nLdpc, llr.begin());
            hard.assign(gHard.begin() + (size_t)b * d.nLdpc, gHard.begin() + (size_t)(b + 1) * d.nLdpc);
            o.iters = gIt[b];
        } else {
            std::vector<float> llrLab((size_t)cells * bps);
            qamDemapBlock(job.fec, c, nn, cells, llrLab.data());
            for (int p = 0; p < d.nLdpc; p++) llr[map[p]] = llrLab[p];
            ldpc.decodeFast(llr, 50, hard, &o.iters);
        }
        // hard decisions before LDPC, for the pre-FEC bit error rate
        long pre = 0;
        std::vector<uint8_t> bb(hard.begin(), hard.begin() + d.kLdpc);
        int corr = bch.decode(bb);
        // second chance for blocks that did not converge: more iterations and other min-sum normalisations
        // The retries are what a marginal signal costs the most time (up to 3 x 150 iterations per failed block), and a frame that takes
        // longer than its own duration makes the decoder fall behind and drop whole frames, which is far worse than a block that stays
        // lost. So they run only while the frame is on schedule and nothing is queued behind it.
        static const bool bench = getenv("DECT2_NOPACE") != nullptr;   // benchmarks stay deterministic: always retry
        auto onSchedule = [&] {
            if (bench) return true;
            if (backlog && backlog->load() > 0) return false;
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < 0.6 * job.frameSec * 1000.0;
        };
        if (corr < 0 && !lost && onSchedule()) {
            static const float kAlpha[3] = {0.78f, 0.66f, 0.9f};
            std::vector<uint8_t> h2;
            for (int a = 0; a < 3 && corr < 0 && onSchedule(); a++) {
                int it2 = 0;
                ldpc.decodeFast(llr, 150, h2, &it2, kAlpha[a]);
                std::vector<uint8_t> b2(h2.begin(), h2.begin() + d.kLdpc);
                int c2 = bch.decode(b2);
                if (c2 >= 0) { corr = c2; bb = std::move(b2); hard = std::move(h2); o.iters = it2; o.retried = true; }
            }
        }
        o.frame.blockIndex = b;
        if (tried.load() < 16) { tried++; if (corr < 0) failedFirst++; }
        if (corr >= 0 && lost) recovered = true;
        if (corr < 0) return;
        o.ok = true;
        o.corrected = corr;
        std::vector<uint8_t> cw0;
        const bool measure = (b % 8) == 0;
        o.frame.bits.assign(bb.begin(), bb.begin() + d.kBch);
        const uint8_t* rnd0 = bbRandomiser();
        for (int i = 0; i < d.kBch; i++) o.frame.bits[i] ^= rnd0[i];
        parseBbHeader(o.frame.bits.data(), o.frame.header);
        if (!measure) return;
        // reconstruct the codeword to measure the pre-LDPC error rate and the MER
        std::vector<uint8_t> cw(bb.begin(), bb.begin() + d.kBch);
        bch.encode(cw, d.kBch);
        ldpc.encode(cw);
        for (int p = 0; p < d.nLdpc; p++) pre += (llr[map[p]] < 0) != (cw[map[p]] != 0);
        o.errBits = pre;
        std::vector<uint16_t> lab(cells);
        for (int k = 0; k < cells; k++) { unsigned l = 0; for (int j = 0; j < bps; j++) l = (l << 1) | cw[map[k * bps + j]]; lab[k] = (uint16_t)l; }
        std::vector<cf32> ideal;
        qamMapBlock(job.fec, lab, ideal);
        const size_t step = std::max<size_t>(1, (size_t)cells / 600);
        // display points: the derotated observation (I of this cell, Q of the next) against the unrotated transmitted point
        static const float kDmin[4] = {1.4142f, 0.6325f, 0.3086f, 0.1534f};
        float cr = 1.f, sr = 0.f;
        if (job.fec.rotation) { static const double deg[4] = {29.0, 16.8, 8.6, 3.576}; double ang = deg[job.fec.mod] * M_PI / 180.0; cr = (float)std::cos(ang); sr = (float)std::sin(ang); }
        for (int k = 0; k < cells; k++) {
            cf32 e = c[k] - ideal[k];
            o.sigP += std::norm(ideal[k]);
            o.errP += std::norm(e);
            if ((size_t)k % step == 0) {
                cf32 obs = c[k];
                if (job.fec.rotation) {
                    float aI = c[k].real(), aQ = c[(k + 1) % cells].imag();
                    obs = cf32(aI * cr + aQ * sr, aQ * cr - aI * sr);
                }
                cf32 tx = qamPoint(job.fec.mod, false, lab[k]);
                cf32 de = obs - tx;
                const float h = 0.5f * kDmin[job.fec.mod];
                o.pts.push_back(obs);
                o.ptx.push_back(lab[k]);
                o.perr.push_back(std::fabs(de.real()) > h || std::fabs(de.imag()) > h);
            }
        }
    });
    double sig = 0, err = 0;
    long preErr = 0, preBits = 0;
    int measured = 0;
    long itSum = 0;
    for (auto& o : outs) {
        itSum += o.iters;
        if (o.ok) {
            res.blocksOk++;
            if (o.retried) res.retryRecovered++;
            res.bchCorrected += o.corrected;
            if (o.sigP > 0) { sig += o.sigP; err += o.errP; preErr += o.errBits; preBits += d.nLdpc; measured++; }
            if (o.frame.header.crcOk) res.headerOk++;
            res.constellation.insert(res.constellation.end(), o.pts.begin(), o.pts.end());
            res.constErr.insert(res.constErr.end(), o.perr.begin(), o.perr.end());
            res.constTx.insert(res.constTx.end(), o.ptx.begin(), o.ptx.end());
        } else res.bchFailed++;
        res.frames.push_back(std::move(o.frame));
    }
    res.avgLdpcIters = (double)itSum / nb;
    res.preBer = preBits ? (double)preErr / preBits : 0;
    res.merDb = err > 0 ? 10 * std::log10(sig / err) : 99;
    // thin the constellation sample
    if (res.constellation.size() > 6000) {
        size_t st = res.constellation.size() / 6000 + 1;
        std::vector<cf32> c2; std::vector<uint8_t> e2; std::vector<uint16_t> t2;
        for (size_t i = 0; i < res.constellation.size(); i += st) { c2.push_back(res.constellation[i]); e2.push_back(res.constErr[i]); t2.push_back(res.constTx[i]); }
        res.constellation = std::move(c2); res.constErr = std::move(e2); res.constTx = std::move(t2);
    }
    res.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

PlpDecoder::PlpDecoder(int threads) {
    gpuOk_ = GpuLdpc::instance().available();
    autoGpu_ = gpuOk_;   // Auto starts on the GPU
    threads_ = threads > 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
    th_ = std::thread([this] { loop(); });
}

PlpDecoder::~PlpDecoder() {
    stop_ = true;
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool PlpDecoder::submit(PlpJob&& job) {
    std::unique_lock<std::mutex> lk(mu_);
    static const bool bench = getenv("DECT2_NOPACE") != nullptr;   // benchmarks never drop a frame: wait for the decoder instead
    if (bench) idle_.wait(lk, [&] { return jobs_.empty() && !busy_; });
    if (jobs_.size() >= 3) { dropped_++; return false; }   // a slow stretch is absorbed by the queue; only a real backlog drops a frame
    backlog_ = (int)jobs_.size() + 1;
    jobs_.push_back(std::move(job));
    cv_.notify_one();
    return true;
}

bool PlpDecoder::poll(PlpResult& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

int PlpDecoder::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (int)jobs_.size() + (busy_ ? 1 : 0);
}

void PlpDecoder::loop() {
    setThreadPriority(ThreadPriority::Background);
    for (;;) {
        PlpJob job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            backlog_ = (int)jobs_.size();
            busy_ = true;
        }
        const int mode = mode_.load();
        const bool wantGpu = gpuOk_ && (mode == 1 || (mode == 2 && autoGpu_));
        PlpResult r = decodeNow(job, threads_, wantGpu, &backlog_);
        if (mode == 2 && gpuOk_) {
            // Auto: run wherever the decoder keeps up. Both paths are timed; a path that takes more than 70 % of a frame's duration for
            // three frames in a row (or that makes frames get dropped) is left for the other one, unless the other one is known to be slower.
            // (A slow built-in graphics chip can be slower than the CPU; the CPU can be slower than the GPU on a weak signal.)
            const uint64_t dr = dropped_.load();
            const double frameMs = r.frameSec * 1000.0;
            double& ema = r.usedGpu ? emaGpuMs_ : emaCpuMs_;
            ema = ema <= 0 ? r.decodeMs : 0.8 * ema + 0.2 * r.decodeMs;
            const bool dropping = dr > lastDropped_;
            lastDropped_ = dr;
            pathFrames_++;
            // the first frames of a path include its start-up (shader compile, buffer allocation): they do not count
            // after a switch the new path gets a long trial (about 15 s) before it can be left again, so two struggling paths do not take turns
            const bool struggling = pathFrames_ > (switched_ ? 60 : 4) && (r.decodeMs > 0.7 * frameMs || dropping);
            slowRun_ = struggling ? slowRun_ + 1 : 0;
            if (slowRun_ >= (dropping ? 2 : 3)) {   // (one slow frame is usually a one-off, such as a new code being set up)
                const double other = r.usedGpu ? emaCpuMs_ : emaGpuMs_;   // 0 = not measured yet
                if (other <= 0 || other < ema * 0.9) { if (getenv("DECT2_AUTOLOG")) fprintf(stderr, "[auto] %s -> %s after %.0f ms (frame %.0f ms); other path %.0f ms\n", r.usedGpu ? "GPU" : "CPU", r.usedGpu ? "CPU" : "GPU", r.decodeMs, frameMs, other); autoGpu_ = !r.usedGpu; slowRun_ = 0; pathFrames_ = 0; switched_ = true; }
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            results_.push_back(std::move(r));
            if (results_.size() > 8) results_.pop_front();
            busy_ = false;
        }
        idle_.notify_all();
    }
}

} // namespace dect2
