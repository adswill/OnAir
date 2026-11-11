// KAIT burst receiver (see kait_rx.h).
#include "dect2/kait/kait_rx.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <cmath>

namespace dect2::kait {

using kris::Mod;

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kChirpRate = 2.0 * kSweepHalfBandHz / ((double)kSweepSamples / kInternalRate);   // Hz per second
constexpr double kMaxCfoHz = 15000.0;

// |correlation|^2 of x with the template t at every start index n (x[n + k] against t[k]); zero where the template does not fit.
std::vector<float> corrPower(const std::vector<cf>& x, const std::vector<cf>& t) {
    const int N = 65536;
    const int Lt = (int)t.size();
    const int step = N - Lt + 1;
    std::vector<float> out(x.size(), 0.f);
    Fft fft(N);
    std::vector<cf32> T(N, cf32(0, 0));
    for (int i = 0; i < Lt; i++) T[(size_t)i] = t[(size_t)i];
    fft.forward(T.data());
    for (cf32& v : T) v = std::conj(v);
    std::vector<cf32> X(N);
    const float norm = 1.0f / (float)N;
    for (size_t b = 0; b + (size_t)Lt <= x.size(); b += (size_t)step) {
        const size_t have = std::min<size_t>(N, x.size() - b);
        std::copy(x.begin() + (long)b, x.begin() + (long)(b + have), X.begin());
        std::fill(X.begin() + (long)have, X.end(), cf32(0, 0));
        fft.forward(X.data());
        for (int i = 0; i < N; i++) X[(size_t)i] *= T[(size_t)i];
        fft.inverse(X.data());
        const size_t valid = std::min<size_t>((size_t)step, x.size() - b - (size_t)Lt + 1);
        for (size_t m = 0; m < valid; m++) out[b + m] = std::norm(X[m]) * norm * norm;
    }
    return out;
}

// parabolic refinement of a peak at index i of a power array (on the amplitude)
double refinePeak(const std::vector<float>& p, size_t i) {
    if (i == 0 || i + 1 >= p.size()) return (double)i;
    const double a = std::sqrt((double)p[i - 1]), b = std::sqrt((double)p[i]), c = std::sqrt((double)p[i + 1]);
    const double den = a - 2 * b + c;
    if (std::fabs(den) < 1e-12) return (double)i;
    return (double)i + std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
}

size_t argmaxIn(const std::vector<float>& p, long lo, long hi) {
    lo = std::max(0L, lo);
    hi = std::min<long>((long)p.size() - 1, hi);
    size_t best = (size_t)lo;
    for (long i = lo; i <= hi; i++) if (p[(size_t)i] > p[best]) best = (size_t)i;
    return best;
}

struct Sweep {
    bool found = false;
    double start = 0.0;    // burst (sweep pair) start in samples
    double cfoHz = 0.0;
    double peakRatio = 0.0;
};

// up peak near `expect` +- `win`, then the down peak near up + kSweepSamples +- the carrier range
Sweep locateSweep(const std::vector<float>& up, const std::vector<float>& dn, long expect, long win, double mean, float ratio) {
    Sweep s;
    const size_t iu = argmaxIn(up, expect - win, expect + win);
    if (up[iu] < ratio * mean) return s;
    const double nu = refinePeak(up, iu);
    const long cfoSamples = (long)(2.0 * kMaxCfoHz / kChirpRate * kInternalRate) + 8;   // the up and down peaks are 2 f / rate apart
    const size_t id = argmaxIn(dn, (long)iu + kSweepSamples - cfoSamples, (long)iu + kSweepSamples + cfoSamples);
    if (dn[id] < ratio * mean * 0.5f) return s;
    const double nd = refinePeak(dn, id);
    s.found = true;
    s.start = 0.5 * (nu + nd - kSweepSamples);
    s.cfoHz = kChirpRate * (nd - kSweepSamples - nu) / (2.0 * kInternalRate);
    s.peakRatio = up[iu] / mean;
    if (std::fabs(s.cfoHz) > kMaxCfoHz) s.found = false;
    return s;
}

// cubic Lagrange interpolation of x at fractional position pos
inline cf interp(const std::vector<cf>& x, double pos) {
    const long i = (long)std::floor(pos);
    const float f = (float)(pos - (double)i);
    if (i < 1 || i + 2 >= (long)x.size()) return cf{0.f, 0.f};
    const cf a = x[(size_t)i - 1], b = x[(size_t)i], c = x[(size_t)i + 1], d = x[(size_t)i + 2];
    const float w0 = -f * (f - 1) * (f - 2) / 6.f, w1 = (f + 1) * (f - 1) * (f - 2) / 2.f, w2 = -(f + 1) * f * (f - 2) / 2.f, w3 = (f + 1) * f * (f - 1) / 6.f;
    return a * w0 + b * w1 + c * w2 + d * w3;
}

struct Tracked {
    std::vector<cf> y;       // slots divided by the gain estimate
    double nvNorm = 1.0;     // complex noise variance per symbol after the division
    double esn0 = 0.0;
    double cfoResHz = 0.0;
};

// Pilot tracking over a run of slots (the first slot is slot 0 of the burst). Residual carrier offset from the phase between pilot segments, then a sliding
// window average of the pilots gives the complex gain (window length from the pilot SNR) and the noise variance.
Tracked trackPilots(std::vector<cf> r, int segPilots) {
    Tracked t;
    const long nSlots = (long)r.size();
    const long nP = nSlots / kPilotPeriod;   // pilots at slots 31, 63, ...
    if (nP < 8) { t.y = std::move(r); return t; }
    std::vector<cf> z((size_t)nP);
    auto refreshZ = [&] { for (long i = 0; i < nP; i++) z[(size_t)i] = r[(size_t)(i * kPilotPeriod + kPilotGroup)] * pilotSign(i); };
    refreshZ();
    for (int iter = 0; iter < 2; iter++) {
        const long S = std::max<long>(4, std::min<long>(segPilots >> iter, nP / 3));
        const long nSeg = nP / S;
        if (nSeg < 2) break;
        std::vector<cf> c((size_t)nSeg, cf{0, 0});
        for (long s = 0; s < nSeg; s++) for (long i = 0; i < S; i++) c[(size_t)s] += z[(size_t)(s * S + i)];
        cf acc{0, 0};
        for (long s = 0; s + 1 < nSeg; s++) acc += c[(size_t)s + 1] * std::conj(c[(size_t)s]);
        const double dphi = std::arg(acc);   // per S pilots
        const double fPerSlot = dphi / (2 * kPi * (double)(S * kPilotPeriod));   // cycles per slot
        t.cfoResHz += fPerSlot * kSymRate;
        for (long k = 0; k < nSlots; k++) {
            const double ph = -2 * kPi * fPerSlot * (double)k;
            r[(size_t)k] *= cf{(float)std::cos(ph), (float)std::sin(ph)};
        }
        refreshZ();
    }
    // first pass: wide window for the SNR
    auto smooth = [&](long W, std::vector<cf>& h) {
        std::vector<cf> pre((size_t)nP + 1, cf{0, 0});
        for (long i = 0; i < nP; i++) pre[(size_t)i + 1] = pre[(size_t)i] + z[(size_t)i];
        h.resize((size_t)nP);
        for (long i = 0; i < nP; i++) {
            const long lo = std::max<long>(0, i - W / 2), hi = std::min<long>(nP, i + W / 2 + 1);
            h[(size_t)i] = (pre[(size_t)hi] - pre[(size_t)lo]) / (float)(hi - lo);
        }
    };
    std::vector<cf> h;
    smooth(std::min<long>(257, nP), h);
    double pw = 0, nv = 0;
    for (long i = 0; i < nP; i++) { pw += std::norm(h[(size_t)i]); nv += std::norm(z[(size_t)i] - h[(size_t)i]); }
    pw /= (double)nP;
    nv /= (double)nP;
    double snrP = pw / std::max(nv, 1e-9);
    long W = (long)std::lround(30.0 / std::max(snrP, 1e-3));
    W = std::clamp<long>(W | 1, 5, 401);
    W = std::min<long>(W, nP | 1);
    smooth(W, h);
    pw = 0; nv = 0;
    for (long i = 0; i < nP; i++) { pw += std::norm(h[(size_t)i]); nv += std::norm(z[(size_t)i] - h[(size_t)i]); }
    pw /= (double)nP;
    nv = nv / (double)nP * ((double)W / std::max(1.0, (double)(W - 1)));
    t.esn0 = pw / std::max(nv, 1e-9);
    t.nvNorm = nv / std::max(pw, 1e-12);
    t.y.resize(r.size());
    for (long k = 0; k < nSlots; k++) {
        // gain at slot k: linear interpolation between the pilots around it (pilot i sits at slot i * 32 + 31)
        const double fi = ((double)k - (double)kPilotGroup) / (double)kPilotPeriod;
        long i0 = (long)std::floor(fi);
        double a = fi - (double)i0;
        long i1 = i0 + 1;
        if (i0 < 0) { i0 = 0; i1 = 0; a = 0; }
        if (i1 >= nP) { i1 = nP - 1; i0 = std::min(i0, i1); a = 0; }
        const cf g = h[(size_t)i0] * (float)(1 - a) + h[(size_t)i1] * (float)a;
        t.y[(size_t)k] = r[(size_t)k] / (std::abs(g) > 1e-9f ? g : cf{1e-9f, 0.f});
    }
    return t;
}

// the data symbols (pilots removed) of a slot run
std::vector<cf> dataOf(const std::vector<cf>& y) {
    std::vector<cf> d;
    d.reserve(y.size());
    for (size_t k = 0; k < y.size(); k++) if (!isPilotSlot((long)k)) d.push_back(y[k]);
    return d;
}

struct Frontend {
    std::vector<cf> x;        // derotated input
    std::vector<cf> mf;       // matched filter output, aligned with x
};

void matchedFilter(Frontend& f, long lo, long hi) {
    static const std::vector<float> taps = kris::rrcTaps(kRrcBeta, kSps, kRrcSpan);
    const long half = (long)(taps.size() - 1) / 2;
    f.mf.assign(f.x.size(), cf{0.f, 0.f});
    lo = std::max(half, lo);
    hi = std::min<long>((long)f.x.size() - half - 1, hi);
    for (long n = lo; n <= hi; n++) {
        float re = 0, im = 0;
        const cf* p = &f.x[(size_t)(n - half)];
        for (size_t t = 0; t < taps.size(); t++) { re += p[t].real() * taps[t]; im += p[t].imag() * taps[t]; }
        f.mf[(size_t)n] = cf{re, im};
    }
}

std::vector<cf> extractSlots(const Frontend& f, double s1, double ratio, long nSlots) {
    std::vector<cf> r((size_t)nSlots);
    const double base = s1 + (2.0 * kSweepSamples + kGuardSamples) * ratio;   // the sweeps and the guard take the receiver's clock too (50 ppm: 2.5 samples)
    for (long k = 0; k < nSlots; k++) r[(size_t)k] = interp(f.mf, base + (double)k * kSps * ratio);
    return r;
}

bool decodeHeader(const std::vector<cf>& y, double nvNorm, Header& h) {
    const std::vector<cf> d = dataOf(y);
    if ((long)d.size() < kHeaderDataSyms) return false;
    std::vector<float> acc((size_t)kHeaderCodedBits, 0.f), llr((size_t)kHeaderCodedBits);
    for (int c = 0; c < kHeaderRepeat; c++) {
        kris::qamDemapSoft(d.data() + (size_t)c * kHeaderCodedBits, (size_t)kHeaderCodedBits, Mod::Bpsk, (float)nvNorm, llr.data());
        const std::vector<uint8_t> pn = scramble(0x4000u + (uint32_t)c, kHeaderCodedBits);
        for (int i = 0; i < kHeaderCodedBits; i++) acc[(size_t)i] += pn[(size_t)i] ? -llr[(size_t)i] : llr[(size_t)i];
    }
    const std::vector<uint8_t> bits = kris::viterbiDecode(1, acc, true);
    if ((int)bits.size() < kHeaderInfoBits) return false;
    return parseHeaderBits(bits.data(), h);
}

}  // namespace

RxResult decodeBurst(const std::vector<cf>& inRaw, const RxConfig& cfg, const ProgressFn& progress) {
    RxResult res;
    std::vector<cf> in = inRaw;
    if (std::fabs(cfg.sampleRate - kInternalRate) > 0.5) in = kapp::resample(inRaw, cfg.sampleRate, kInternalRate);
    if ((long)in.size() < 4L * kSweepSamples) return res;

    // 1. sweeps
    const std::vector<cf> upT = sweepUp(), dnT = sweepDown();
    const std::vector<float> cu = corrPower(in, upT), cd = corrPower(in, dnT);
    double mean = 0;
    for (float v : cu) mean += v;
    mean /= (double)cu.size();
    if (mean <= 0) return res;
    size_t p1 = argmaxIn(cu, 0, (long)cu.size() - 1);
    if (cu[p1] < cfg.detectRatio * mean) return res;
    // the earlier of the two strongest, well separated peaks is the start (the end sweep is as strong)
    size_t startIdx = p1;
    {
        const long excl = (long)(0.5 * kInternalRate);
        std::vector<float> cut(cu);
        for (long i = std::max(0L, (long)p1 - excl); i < std::min<long>((long)cu.size(), (long)p1 + excl); i++) cut[(size_t)i] = 0.f;
        const size_t p2 = argmaxIn(cut, 0, (long)cut.size() - 1);
        if (cut[p2] > 0.3f * cu[p1] && p2 < p1 && (double)(p1 - p2) > 2.0 * kInternalRate) startIdx = p2;
    }
    Sweep sw = locateSweep(cu, cd, (long)startIdx, 2, mean, cfg.detectRatio);
    if (!sw.found) return res;
    res.synced = true;
    res.cfoHz = sw.cfoHz;

    // 2. derotate, matched filter over the span a burst can cover
    Frontend fe;
    fe.x = std::move(in);
    {
        const double w = -2 * kPi * sw.cfoHz / kInternalRate;
        const long s0 = std::max(0L, (long)sw.start - 1000);
        for (long n = s0; n < (long)fe.x.size(); n++) {
            const double ph = w * (double)n;   // phase continuous in absolute samples
            fe.x[(size_t)n] *= cf{(float)std::cos(ph), (float)std::sin(ph)};
        }
    }
    const long maxBurst = (long)(10.6 * kInternalRate);
    matchedFilter(fe, (long)sw.start + 2L * kSweepSamples - 200, (long)sw.start + maxBurst);

    // 3. header, with a few timing guesses
    const long hdrSlots = kHeaderDataSyms + kHeaderDataSyms / kPilotGroup;
    Tracked tr;
    double s1 = sw.start, hdrRatio = 1.0;
    const double tries[] = {0.0, 0.25, -0.25, 0.5, -0.5, 0.75, -0.75};
    // and a few sample clock errors: over the header's 78 000 samples a 50 ppm clock (a cheap radio's crystal) slips 4 samples, past the
    // timing guesses above; the end sweep measures the clock only after the header is read
    const double clocks[] = {0.0, 30e-6, -30e-6, 60e-6, -60e-6, 90e-6, -90e-6};
    for (double dc : clocks) {
        for (double dt : tries) {
            const std::vector<cf> r = extractSlots(fe, sw.start + dt, 1.0 + dc, hdrSlots);
            tr = trackPilots(r, 64);
            if (decodeHeader(tr.y, tr.nvNorm, res.hdr)) { res.headerOk = true; s1 = sw.start + dt; hdrRatio = 1.0 + dc; break; }
        }
        if (res.headerOk) break;
    }
    res.esn0Db = 10.0 * std::log10(std::max(tr.esn0, 1e-6));
    res.snrDb = res.esn0Db - 10.0 * std::log10(kBandwidthHz / kSymRate);
    if (!res.headerOk) return res;
    const Header& H = res.hdr;
    res.cfoHz = sw.cfoHz + tr.cfoResHz;

    // 4. end sweep: sample clock
    double ratio = hdrRatio;
    {
        const long e0 = endSweepOffset(H);
        const long win = (long)(3e-4 * (double)e0) + 40;
        const Sweep se = locateSweep(cu, cd, (long)std::llround(s1) + e0, win, mean, cfg.detectRatio * 0.5f);
        if (se.found) {
            const double rr = (se.start - s1) / (double)e0;
            if (std::fabs(rr - 1.0) < 3e-4) { ratio = rr; res.endSweep = true; res.clockPpm = (rr - 1.0) * 1e6; }
        }
    }

    // 4b. fine timing: the sweep gives the burst start to a fraction of a sample; the best symbol phase is the one with the highest pilot SNR
    {
        const long probe = std::min<long>(H.slots(), 60000);
        double bestEs = -1e9, bestDt = 0.0;
        double es[25];
        for (int i = 0; i < 25; i++) {   // +-1.5 samples: the header's timing guess is only as good as its clock guess (30 ppm steps)
            const double dt = -1.5 + 0.125 * i;
            es[i] = trackPilots(extractSlots(fe, s1 + dt, ratio, probe), 128).esn0;
            if (es[i] > bestEs) { bestEs = es[i]; bestDt = dt; }
        }
        s1 += bestDt;
    }

    // 5. all slots
    const long nSlots = H.slots();
    std::vector<cf> r = extractSlots(fe, s1, ratio, nSlots);
    fe.mf.clear(); fe.mf.shrink_to_fit(); fe.x.clear(); fe.x.shrink_to_fit();
    tr = trackPilots(std::move(r), 256);
    res.esn0Db = 10.0 * std::log10(std::max(tr.esn0, 1e-6));
    res.snrDb = res.esn0Db - 10.0 * std::log10(kBandwidthHz / kSymRate);
    res.cfoHz = sw.cfoHz + tr.cfoResHz;
    const std::vector<cf> data = dataOf(tr.y);

    // 6. layers
    res.stream.assign(H.streamBytes(), 0);
    size_t blockTotal = 0;
    for (int i = 0; i < H.nLayers; i++) blockTotal += (size_t)H.layers[(size_t)i].blocks;
    res.blockOk.assign(blockTotal, 0);
    res.layers.assign((size_t)H.nLayers, LayerResult{});
    long dpos = kHeaderDataSyms;
    size_t byteOff = 0, blockIdx = 0;
    uint32_t gb = 0;
    bool broken = false;
    size_t goodBlocksPrefix = 0;
    bool prefixOpen = true;
    std::vector<float> llr((size_t)kLdpcN), sum((size_t)kLdpcN);
    for (int li = 0; li < H.nLayers; li++) {
        const LayerSpec& L = H.layers[(size_t)li];
        const int K = infoBitsPerBlock(L.rate), pb = payloadBytesPerBlock(L.rate), spc = kLdpcN / (int)L.mod;
        LayerResult& lr = res.layers[(size_t)li];
        lr.blocks = L.blocks;
        std::vector<uint8_t> info((size_t)K);
        for (int b = 0; b < L.blocks; b++, gb++, blockIdx++, byteOff += (size_t)pb) {
            const long start = dpos;
            dpos += (long)spc * L.rep;
            if (broken) continue;
            if (dpos > (long)data.size()) { broken = true; continue; }
            std::fill(sum.begin(), sum.end(), 0.f);
            for (int c = 0; c < L.rep; c++) {
                kris::qamDemapSoft(data.data() + start + (long)c * spc, (size_t)spc, L.mod, (float)tr.nvNorm, llr.data());
                const std::vector<uint8_t> pn = scramble(0x10000u + gb * 8u + (uint32_t)c, kLdpcN);
                for (int i = 0; i < kLdpcN; i++) sum[(size_t)i] += pn[(size_t)i] ? -llr[(size_t)i] : llr[(size_t)i];
            }
            kris::ldpcDecode(kLdpcN, L.rate, sum.data(), info.data(), 50);
            std::vector<uint8_t> by((size_t)pb, 0);
            for (int i = 0; i < pb * 8; i++) by[(size_t)i / 8] = (uint8_t)(by[(size_t)i / 8] | (info[(size_t)i] << (7 - i % 8)));
            uint16_t crc = 0;
            for (int i = 0; i < 16; i++) crc = (uint16_t)((crc << 1) | info[(size_t)(pb * 8 + i)]);
            lr.attempted++;
            if (crc == kris::crc16(by.data(), by.size())) {
                lr.blocksOk++;
                res.blockOk[blockIdx] = 1;
                std::copy(by.begin(), by.end(), res.stream.begin() + (long)byteOff);
                if (prefixOpen) goodBlocksPrefix++;
            } else {
                prefixOpen = false;
                if (cfg.stopAtFirstFail) broken = true;
            }
        }
        // bytes of the good prefix so far
        if (prefixOpen || li == H.nLayers - 1 || true) {
            size_t bytes = 0, bi = 0, left = goodBlocksPrefix;
            for (int q = 0; q <= li && left > 0; q++)
                for (int b = 0; b < H.layers[(size_t)q].blocks && left > 0; b++, left--, bi++) bytes += (size_t)payloadBytesPerBlock(H.layers[(size_t)q].rate);
            res.bytesGood = bytes;
        }
        res.layersComplete = 0;
        for (int q = 0; q <= li; q++) {
            if (res.layers[(size_t)q].complete()) res.layersComplete++;
            else break;
        }
        if (progress) {
            RxResult part = res;
            if (cfg.progressiveImages && part.bytesGood > 0) part.image = decodeImage(part.stream.data(), part.bytesGood, H.streamInfo());
            progress(li, part);
        }
    }
    if (cfg.makeImage && res.bytesGood > 0) res.image = decodeImage(res.stream.data(), res.bytesGood, H.streamInfo());
    return res;
}

bool findStartSweep(const cf* x, size_t n, float ratio, double& pos, double& peakRatio) {
    if (n < 2U * (size_t)kSweepSamples) return false;
    const std::vector<cf> v(x, x + n);
    const std::vector<float> cu = corrPower(v, sweepUp());
    double mean = 0;
    for (float f : cu) mean += f;
    mean /= (double)cu.size();
    if (mean <= 0) return false;
    const size_t i = argmaxIn(cu, 0, (long)cu.size() - 1);
    peakRatio = cu[i] / mean;
    if (peakRatio < ratio) return false;
    pos = refinePeak(cu, i);
    return true;
}

void StreamReceiver::feed(const cf* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    const double need = (kBurstSeconds + 2.0) * cfg_.sampleRate;
    if ((double)buf_.size() >= need) {
        RxResult r = decodeBurst(buf_, cfg_, prog_);
        if (r.synced && cb_) cb_(r);
        buf_.clear();
    }
}

RxResult StreamReceiver::flush() {
    RxResult r = decodeBurst(buf_, cfg_, prog_);
    if (r.synced && cb_) cb_(r);
    buf_.clear();
    return r;
}

}  // namespace dect2::kait
