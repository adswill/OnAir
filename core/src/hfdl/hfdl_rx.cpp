// HFDL receiver (hfdl_rx.h)
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/rrc.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/automation/engine.h"
#include "dect2/common/threadpool.h"
#include "dect2/results/results_io.h"
#include "dect2/hfdl/hfdl_proto.h"
#include <algorithm>
#include <atomic>
#include <functional>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kFs = 7200.0;       // 4 samples per symbol
constexpr int kSps = 4;
constexpr int kSearchSyms = 2 * kHfdlALen;
constexpr int kChunk = 8;
constexpr double kMaxCfo = 250.0;
constexpr int kCoarseHyp = 4;          // trial offsets of the preamble search: -4 .. 4 steps
constexpr double kCoarseStep = 100.0;   // Hz (covers +-400 Hz in 8-symbol chunks; burst() refines +-250 Hz more)
constexpr double kCoarseThr = 0.50;     // blind preamble correlation (noise alone averages 1/sqrt(8) = 0.35 per hypothesis, more as the best of 9)
constexpr double kCoarseThrNearDc = 0.52; // dial channel sits on the LO: need a higher bar (DC spur false locks)
constexpr double kNearDcHz = 2500.0;
constexpr int kL = 12;               // feed-forward taps each side, half a symbol apart
constexpr int kNb = 6;               // feedback taps
constexpr double kDefCentre = 8.9e6;
// shared first stage (setup()): the output covers the channels' span with this margin, and a channel is this far (Hz) wide beyond its dial
constexpr double kDecimMargin = 1.2;
constexpr double kChanEdgeHz = 4000;
// Shared FFT channeliser (setup(), channelise()). Selection rule, by the capture rate and the channels in band: it is used when
// rate > kChanMinRate AND at least kChanMinChans channels are in band. The per-channel path costs about rate x channels (a mixer and a CIC
// integrator per channel on every input sample); up to 16 MHz that is what it always was and stays bit-identical, above it (32 MHz holds
// about the whole 19 MHz station table) the cost grows 4 to 5 times and the analysis thread falls behind, so one filter bank does the
// per-sample work once for all channels (cost ~ rate, whatever the number of channels).
constexpr double kChanMinRate = 16.5e6;
constexpr int kChanMinChans = 4;
constexpr double kChanMaxBinHz = 64000;   // sub-band spacing B = rate / N, N a power of two: B in (32, 64] kHz; the sub-band rate is 2 B
constexpr int kChanFold = 4;               // prototype length = kChanFold x N taps
using cd = std::complex<double>;

using dect2::dsp::rrc;

cf32 cubicAt(const std::vector<cf32>& x, double pos) {
    const int i = (int)std::floor(pos);
    const float f = (float)(pos - i);
    auto at = [&](int k) { return k >= 0 && k < (int)x.size() ? x[(size_t)k] : cf32(0, 0); };
    const cf32 a = at(i - 1), b = at(i), c = at(i + 1), d = at(i + 2);
    const cf32 c0 = b, c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

std::string hex6(uint32_t a) { char b[8]; snprintf(b, sizeof b, "%06X", a & 0xFFFFFF); return b; }

// the width of the station table's dials, Hz (lowest to highest dial)
double tableSpanHz() {
    double lo = 1e300, hi = -1e300;
    for (const auto& s : hfdlStations()) for (int k : s.khz) { lo = std::min(lo, k * 1e3); hi = std::max(hi, k * 1e3); }
    return hi >= lo ? hi - lo : 0;
}

} // namespace

std::vector<double> hfdlChannelsNear(double c, double half) {
    std::vector<double> o;
    for (const auto& s : hfdlStations())
        for (int k : s.khz) { const double f = k * 1e3; if (std::fabs(f + kHfdlCarrierHz - c) <= half && std::find(o.begin(), o.end(), f) == o.end()) o.push_back(f); }
    std::sort(o.begin(), o.end());
    return o;
}

struct HfdlReceiver::Impl {
    struct Chan {
        double dial = 0; double offHz = 0; int gsId = 0; bool inBand = false; int slot = -1;   // slot: the channeliser's bin slot
        cd ph{1, 0}, rot{1, 0}; int phN = 0;
        int D = 1; cd i1{0, 0}, i2{0, 0}, c1{0, 0}, c2{0, 0}; int dcnt = 0;
        std::vector<cf32> fh; size_t fpos = 0; int M = 1, mcnt = 0;
        double r2 = 0, tNext = 0; int64_t n2 = 0; cf32 h2[4] = {};
        std::vector<cf32> mh; size_t mpos = 0;
        std::vector<cf32> x; int64_t xBase = 0;          // matched filter output at 7200 Hz, x[0] is sample xBase
        std::vector<float> xa;                           // std::abs(x[i]), stored once when the sample is made (coarse() reads it)
        int64_t searchAt = 0, waitUntil = 0;
        double noisePow = 0; double blkPow = 0; int blkN = 0;
        HfdlChannelInfo info;
        std::vector<std::function<void()>> pend;        // what the channel found, applied to the shared lists after the parallel phase, in channel order (feed)
    };
    std::mutex mu;
    std::function<void(const std::string&)> logCb;
    double rate = 0, offset = 0, center = 0;
    std::vector<double> want;
    bool rebuild = true;
    bool whole = false;                  // Whole span: the station table in the capture plus the user's list
    std::vector<Chan> ch;
    std::vector<float> fir, mf;
    std::vector<cf32> firC;                   // the baseband filter with a centre offset (empty: use fir)
    BasebandFilterCfg bbf;
    uint64_t nIn = 0;
    uint64_t seq = 0, serial = 0;
    HfdlTelemetry tel;
    double lastGood = -1e9;
    std::atomic<double> lastBurst{-1e9};     // written by the channel jobs (all with the same value within one feed)
    bool parallel = true;                     // false: the channels run one after the other on the caller (tests)
    std::map<uint32_t, uint32_t> acIcao;      // gs << 8 | ac id -> ICAO
    std::map<int, bool> logged;
    // shared first stage (setup(), decimate()): F == 1 is the channel path as it was, nothing below is used
    int F = 1;
    double shiftHz = 0;                       // F > 1: the input is mixed by -shiftHz, so the channels sit around the middle of the band
    std::vector<float> dfir;                  // its anti-alias low-pass
    std::vector<cf32> dhist, dz, dout;
    cd mixPh{1, 0}, mixRot{1, 0};
    int mixN = 0, dcnt = 0;
    // shared FFT channeliser (cz): a weighted-overlap-add analysis filter bank, N bins of rate / N Hz, hop N / 2 (so each bin's stream runs at 2 rate / N,
    // twice its spacing: a channel is taken from the nearest bin, its residual offset is at most B / 2 and the 3.4 kHz channel stays inside the bin's pass-band)
    bool cz = false;
    int czForce = -1;                         // tests: 0 per-channel path, 1 channeliser, -1 by the rule
    int cN = 0, cH = 0, cL = 0;
    double cB = 0, cRate = 0;                 // bin spacing, sub-band rate
    std::vector<float> cw;                    // prototype low-pass, L = kChanFold x N taps, unit DC gain
    std::vector<cf32> cbuf;                   // input not yet consumed (sanitised)
    std::vector<Fft> cfft;
    std::vector<std::vector<cf32>> ctmp;
    std::vector<int> cbinOf;                  // per bin: its slot in cband, -1 if unused
    std::vector<int> cbins;                   // the bins used, by slot
    std::vector<std::vector<cf32>> cband;      // per slot, the frames of this feed() (already de-rotated by the hop phase)
    int64_t cm = 0;                           // frames made so far

    double nowSec() const { return rate > 0 ? (double)nIn / rate : 0; }
    // mu held: the clock starts again (configure), the kept lists move back with it
    void shiftTimes(double dt) {
        for (auto& m : tel.messages) m.timeSec += dt;
        for (auto& a : tel.aircraft) { a.lastHeardSec += dt; if (a.hasPos) a.posTime += dt; for (auto& p : a.track) p.time += dt; }
        for (auto& g : tel.stations) if (g.lastHeardSec != 0) g.lastHeardSec += dt;
        tel.nowSec += dt;
    }

    void setup() {
        rebuild = false;
        ch.clear();
        F = 1; shiftHz = 0; dfir.clear();
        cz = false; cbuf.clear(); cfft.clear(); ctmp.clear(); cbinOf.clear(); cbins.clear(); cband.clear(); cm = 0;
        if (rate < 1) return;
        const double c = center > 0 ? center : kDefCentre;
        const double half = rate / 2 - 3000;
        std::vector<double> list = want.empty() || whole ? hfdlChannelsNear(c, std::max(0.0, half * 0.95)) : want;
        if (whole && !want.empty()) { list.insert(list.end(), want.begin(), want.end()); std::sort(list.begin(), list.end()); list.erase(std::unique(list.begin(), list.end()), list.end()); }
        // Shared first stage, wide captures only: the capture is low-passed and decimated once, before the channels, and the channels run at rate / F.
        // Decided by the output rate: rate / F must cover the span of the channels (the station table or the list, whichever is wider) with
        // kDecimMargin, so F >= 2 needs a capture of about 2.4 times that span (64 MHz, not 32 MHz). Up to 16 MHz and at 32 MHz F is 1: no change.
        {   // the channeliser rule (see kChanMinRate): decided by the rate and by how many channels are in band
            int nIn = 0;
            for (double f : list) if (std::fabs(f + kHfdlCarrierHz - c + offset) < half) nIn++;
            cz = rate >= 1e6 && (czForce < 0 ? (rate > kChanMinRate && nIn >= kChanMinChans) : czForce > 0);
        }
        double passHz = 0;
        if (!list.empty() && !cz) {
            double lo = 1e300, hi = -1e300;
            for (double f : list) { const double o = f + kHfdlCarrierHz - c + offset; lo = std::min(lo, o); hi = std::max(hi, o); }
            const double span = std::max(tableSpanHz(), hi - lo);
            passHz = kDecimMargin * (span + 2 * kChanEdgeHz);
            const int Fd = (int)std::floor(rate / passHz);
            if (Fd >= 2) { F = Fd; shiftHz = 0.5 * (lo + hi); }
        }
        if (cz) {
            cN = 1; while (rate / cN > kChanMaxBinHz) cN <<= 1;
            cH = cN / 2; cL = kChanFold * cN; cB = rate / cN; cRate = 2 * cB;
            // Kaiser-windowed sinc, cut-off B (half way between the pass edge B / 2 + 4 kHz and the first alias, 2 B minus that edge), beta 6 (about 63 dB)
            cw.assign((size_t)cL, 0.f);
            auto i0 = [](double x) { double t = 1, sum = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); sum += t; } return sum; };
            const double beta = 6.0, fc = cB / rate;           // cycles per input sample
            double acc = 0;
            for (int i = 0; i < cL; i++) {
                const double m = i - (cL - 1) / 2.0, r = 2.0 * i / (cL - 1) - 1.0;
                const double sinc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
                const double v = sinc * i0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / i0(beta);
                cw[(size_t)i] = (float)v; acc += v;
            }
            for (auto& v : cw) v = (float)(v / acc);
            for (int t = 0; t < 8; t++) { cfft.emplace_back(cN); ctmp.emplace_back((size_t)cN); }
            cbinOf.assign((size_t)cN, -1);
        }
        const double rW = cz ? cRate : rate / F;      // the rate of the channel chains (rate itself when F == 1; the sub-band rate with the channeliser)
        const double halfW = rW / 2 - 3000;
        if (F > 1) {
            // cut-off at rW / 2 (the output's Nyquist) with the transition centred on it: flat to passHz / 2, down by the stop band from rW - passHz / 2
            dfir = bbf::design(BasebandFilterCfg(), rW, rW - passHz, rate).h;
            dhist.assign(dfir.size() - 1, cf32(0, 0));
            mixPh = cd(1, 0); mixRot = std::polar(1.0, -2 * M_PI * shiftHz / rate); mixN = 0; dcnt = 0;
        }
        const int D = std::max(1, (int)std::floor(rW / 28800));
        const double r1 = rW / D;
        const int M = std::max(1, (int)std::floor(r1 / kFs));
        const BbDesign bd = bbf::design(bbf, 3400, 2000, r1);   // default: 3.4 kHz Blackman, as before
        fir = bd.h;
        firC = bd.hc;                       // the delay line runs oldest first: a correlation, so the taps are conjugated to put the pass-band at +centre
        for (auto& v : firC) v = std::conj(v);
        const int taps = (int)fir.size();
        mf.clear();
        double s = 0;
        for (int i = -6 * kSps; i <= 6 * kSps; i++) { mf.push_back((float)rrc((double)i / kSps, kHfdlRrcAlpha)); s += mf.back(); }
        for (auto& v : mf) v = (float)(v / s);
        for (double f : list) {
            Chan k;
            k.dial = f;
            const int khz = (int)std::lround(f / 1e3);
            k.gsId = std::fabs(f - khz * 1e3) < 1 ? hfdlStationOnKhz(khz) : 0;
            const double offIn = f + kHfdlCarrierHz - c + offset;   // the dial as it is in the input (the near-DC rule of process() uses it)
            double off = offIn - shiftHz;                          // in the channel rate's band (shiftHz is 0 when F == 1)
            k.offHz = offIn;
            k.inBand = cz ? std::fabs(offIn) < rate / 2 - 3000 : std::fabs(off) < halfW;
            if (cz && k.inBand) {                                  // the nearest bin; what is left over is shifted at the sub-band rate
                const int b = (int)std::lround(offIn / cB);
                const int bi = ((b % cN) + cN) % cN;
                if (cbinOf[(size_t)bi] < 0) { cbinOf[(size_t)bi] = (int)cbins.size(); cbins.push_back(bi); }
                k.slot = cbinOf[(size_t)bi];
                off = offIn - b * cB;
            }
            k.rot = std::polar(1.0, -2 * M_PI * off / rW);
            k.D = D; k.M = M;
            k.fh.assign((size_t)taps, cf32(0, 0));
            k.r2 = r1 / M;
            k.mh.assign(mf.size(), cf32(0, 0));
            k.info.freqHz = f; k.info.gsId = k.gsId; k.info.inBand = k.inBand;
            ch.push_back(std::move(k));
        }
        cband.assign(cbins.size(), {});
        tel.inputRate = rate;
        tel.centerHz = center;
    }

    // The shared channeliser (cz): consumes the input, leaves the frames of every used bin in cband[slot] (all the same length, returned).
    // Frame m is sum_n x[mH + n] w[n] e^{-j 2 pi k n / N} (folded to N points, one FFT), times e^{j pi k m} that the hop of N / 2 leaves behind.
    size_t channelise(const cf32* x, size_t n) {
        const size_t old = cbuf.size();
        cbuf.resize(old + n);
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            cbuf[old + i] = v;
        }
        const size_t frames = cbuf.size() >= (size_t)cL ? (cbuf.size() - (size_t)cL) / (size_t)cH + 1 : 0;
        for (auto& o : cband) o.assign(frames, cf32(0, 0));
        if (!frames) return 0;
        const size_t T = std::min<size_t>(cfft.size(), frames);
        auto job = [&](size_t t) {
            const size_t f0 = frames * t / T, f1 = frames * (t + 1) / T;
            cf32* tmp = ctmp[t].data();
            for (size_t f = f0; f < f1; f++) {
                const cf32* in = cbuf.data() + f * (size_t)cH;
                for (int j = 0; j < cN; j++) {
                    cf32 acc(0, 0);
                    for (int q = 0; q < kChanFold; q++) acc += in[j + q * cN] * cw[(size_t)(j + q * cN)];
                    tmp[j] = acc;
                }
                cfft[t].forward(tmp);
                const bool odd = ((cm + (int64_t)f) & 1) != 0;
                for (size_t s = 0; s < cbins.size(); s++) {
                    const cf32 v = tmp[cbins[s]];
                    cband[s][f] = (odd && (cbins[s] & 1)) ? -v : v;
                }
            }
        };
        if (parallel && T > 1 && frames >= 16) ThreadPool::shared().parallelFor(T, job);
        else for (size_t t = 0; t < T; t++) job(t);
        cbuf.erase(cbuf.begin(), cbuf.begin() + (std::ptrdiff_t)(frames * (size_t)cH));
        cm += (int64_t)frames;
        return frames;
    }

    // ------------------------------------------------ front end
    // Shared first stage (F > 1 only, see setup()): mixes the input by -shiftHz, low-passes it and keeps every F-th sample, once for all channels.
    // Its output is not bit-identical to the full-rate path (the filter is not ideal), so the decoded messages of a wide capture may differ slightly.
    void decimate(const cf32* x, size_t n, std::vector<cf32>& out) {
        const size_t L = dfir.size();
        dz.resize(L - 1 + n);
        std::copy(dhist.begin(), dhist.end(), dz.begin());
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            cd y(v.real(), v.imag());
            y *= mixPh; mixPh *= mixRot;
            if (++mixN >= 4096) { mixPh /= std::abs(mixPh); mixN = 0; }
            dz[L - 1 + i] = cf32((float)y.real(), (float)y.imag());
        }
        out.clear();
        for (size_t i = 0; i < n; i++) {
            if (++dcnt < F) continue;
            dcnt = 0;
            cf32 acc(0, 0);
            for (size_t j = 0; j < L; j++) acc += dz[i + L - 1 - j] * dfir[j];
            out.push_back(acc);
        }
        dhist.assign(dz.begin() + (std::ptrdiff_t)n, dz.begin() + (std::ptrdiff_t)(n + L - 1));
    }
    void push(Chan& k, cf32 v) {
        const cd y = cd(v.real(), v.imag()) * k.ph;
        k.ph *= k.rot;
        if (++k.phN >= 4096) { k.ph /= std::abs(k.ph); k.phN = 0; }
        k.i1 += y; k.i2 += k.i1;
        if (++k.dcnt < k.D) return;
        k.dcnt = 0;
        const cd d1 = k.i2 - k.c1; k.c1 = k.i2;
        const cd d2 = d1 - k.c2; k.c2 = d1;
        const cf32 z = cf32(d2 / (double)(k.D * k.D));
        k.fh[k.fpos] = z;
        k.fpos = (k.fpos + 1) % k.fh.size();
        if (++k.mcnt < k.M) return;
        k.mcnt = 0;
        // same taps, same order as the ring index (k.fpos + i) % T, split in two runs so there is no modulo per tap
        cf32 acc(0, 0);
        const size_t T = k.fh.size(), p0 = k.fpos, n1 = T - p0;
        if (firC.empty()) {
            for (size_t i = 0; i < n1; i++) acc += k.fh[p0 + i] * fir[i];
            for (size_t i = n1; i < T; i++) acc += k.fh[i - n1] * fir[i];
        } else {
            for (size_t i = 0; i < n1; i++) acc += k.fh[p0 + i] * firC[i];
            for (size_t i = n1; i < T; i++) acc += k.fh[i - n1] * firC[i];
        }
        // cubic interpolation from r2 to 7200 Hz
        k.h2[0] = k.h2[1]; k.h2[1] = k.h2[2]; k.h2[2] = k.h2[3]; k.h2[3] = acc;
        k.n2++;
        // h2[1] .. h2[2] spans input samples n2-3 .. n2-2
        const double step = k.r2 / kFs;
        while (k.tNext <= (double)(k.n2 - 2)) {
            const double f = k.tNext - (double)(k.n2 - 3);
            if (f >= 0) {
                const float u = (float)f;
                const cf32 a = k.h2[0], b = k.h2[1], c = k.h2[2], d = k.h2[3];
                const cf32 c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
                symSample(k, ((c3 * u + c2) * u + c1) * u + b);
            }
            k.tNext += step;
        }
    }
    void symSample(Chan& k, cf32 v) {
        k.mh[k.mpos] = v;
        k.mpos = (k.mpos + 1) % k.mh.size();
        cf32 acc(0, 0);
        const size_t T = k.mh.size(), p0 = k.mpos, n1 = T - p0;   // same order as (mpos + i) % T, no modulo per tap
        for (size_t i = 0; i < n1; i++) acc += k.mh[p0 + i] * mf[i];
        for (size_t i = n1; i < T; i++) acc += k.mh[i - n1] * mf[i];
        k.x.push_back(acc);
        k.xa.push_back(std::abs(acc));
        k.blkPow += std::norm(acc); k.blkN++;
        if (k.blkN >= 1800) {     // a quarter second: the noise floor follows the quiet blocks
            const double p = k.blkPow / k.blkN;
            if (k.noisePow <= 0) k.noisePow = p;
            else if (p < k.noisePow * 2) k.noisePow += 0.1 * (p - k.noisePow);
            else k.noisePow *= 1.01;
            k.blkPow = 0; k.blkN = 0;
            k.info.noiseDb = (float)(10 * std::log10(k.noisePow + 1e-20));
        }
    }

    // ------------------------------------------------ search
    cf32 X(const Chan& k, int64_t i) const { const int64_t j = i - k.xBase; return j >= 0 && j < (int64_t)k.x.size() ? k.x[(size_t)j] : cf32(0, 0); }

    // A1 + A2 against the reference turned by a few trial frequency offsets: 8-symbol chunks only stay coherent within about
    // +-60 Hz, and a receiver off by a few ppm (or a station reported with the wrong carrier) is 100-250 Hz away
    static const std::vector<std::vector<cf32>>& coarseRefs() {
        static const std::vector<std::vector<cf32>> r = [] {   // built once, thread-safe: the channels search in parallel
            std::vector<std::vector<cf32>> t;
            const auto& ref = hfdlPreambleRef(0);
            for (int h = -kCoarseHyp; h <= kCoarseHyp; h++) {
                std::vector<cf32> v((size_t)kSearchSyms);
                for (int q = 0; q < kSearchSyms; q++) v[(size_t)q] = ref[(size_t)q] * std::polar(1.f, (float)(-2 * M_PI * h * kCoarseStep * q / kHfdlBaud));
                t.push_back(std::move(v));
            }
            return t;
        }();
        return r;
    }
    double coarse(const Chan& k, int64_t s) const {
        const int64_t i0 = s - k.xBase;   // whole samples: cubicAt() would only return x[i] here, at several times the cost
        if (i0 < 0 || i0 + (int64_t)(kSearchSyms - 1) * kSps >= (int64_t)k.x.size()) return 0;
        const cf32* p = &k.x[(size_t)i0];
        const float* pa = &k.xa[(size_t)i0];   // |x| of the same samples, stored by symSample()
        double sumA = 0;
        for (int q = 0; q < kSearchSyms; q++) sumA += pa[q * kSps];
        if (sumA <= 1e-6 * kSearchSyms) return 0;
        double best = 0;
        for (const auto& ref : coarseRefs()) {
            double sumC = 0;
            for (int c = 0; c < kSearchSyms / kChunk + 1; c++) {
                cf32 acc(0, 0);
                const int k0 = c * kChunk, k1 = std::min(kSearchSyms, k0 + kChunk);
                for (int q = k0; q < k1; q++) acc += p[q * kSps] * ref[(size_t)q];
                sumC += std::abs(acc);
            }
            best = std::max(best, sumC);
        }
        return best / sumA;
    }

    // y_k = ref_k * x(t0 + 4k), interpolated
    void refProducts(const Chan& k, double t0, int n, std::vector<cf32>& y) const {
        const auto& ref = hfdlPreambleRef(0);
        y.resize((size_t)n);
        for (int q = 0; q < n; q++) {
            const double pos = t0 + q * kSps - (double)k.xBase;
            y[(size_t)q] = cubicAt(k.x, pos) * ref[(size_t)q];
        }
    }
    static cf32 corrAt(const std::vector<cf32>& y, double f, int k0, int k1) {
        const cf32 r = std::polar(1.f, (float)(-2 * M_PI * f / kHfdlBaud));
        cf32 ph = std::polar(1.f, (float)(-2 * M_PI * f * k0 / kHfdlBaud)), acc(0, 0);
        for (int q = k0; q < k1; q++) { acc += y[(size_t)q] * ph; ph *= r; }
        return acc;
    }

    void process(Chan& k) {
        const int64_t end = k.xBase + (int64_t)k.x.size();
        const int64_t span = (int64_t)kSearchSyms * kSps + 16;
        const double thr = std::fabs(k.offHz) < kNearDcHz ? kCoarseThrNearDc : kCoarseThr;
        if (k.searchAt < k.xBase) k.searchAt = k.xBase;
        if (end < k.waitUntil) return;
        while (k.searchAt + span + (int64_t)(kHfdlPreLen + 8) * kSps < end) {
            const int64_t s = k.searchAt;
            // no look-ahead past the prekey: a false preamble there moves searchAt beyond the real one
            const double m = coarse(k, s);
            if (m < thr) { k.searchAt += 2; continue; }
            int64_t best = s;
            double bm = m;
            for (int64_t q = s - 2; q <= s + 10; q++) {
                if (q < k.xBase) continue;
                const double v = coarse(k, q);
                if (v > bm) { bm = v; best = q; }
            }
            const int r = burst(k, best, end);
            if (r < 0) return;
            if (k.searchAt <= s) k.searchAt = s + kSps * 4;   // never spin on a false preamble
        }
        trim(k);
    }

    void trim(Chan& k) {
        const int64_t keep = k.searchAt - 64;
        if (keep - k.xBase > 40000) { k.x.erase(k.x.begin(), k.x.begin() + (keep - k.xBase)); k.xa.erase(k.xa.begin(), k.xa.begin() + (keep - k.xBase)); k.xBase = keep; }
    }

    // returns -1 when more samples are needed, else 0 (searchAt moved on)
    int burst(Chan& k, int64_t s0, int64_t end) {
        std::vector<cf32> y;
        // coherent frequency search on A1 + A2
        refProducts(k, (double)s0, kSearchSyms, y);
        double bf = 0, bv = -1;
        std::vector<double> mags;
        mags.reserve((size_t)(2 * kMaxCfo / 1.5) + 4);
        for (double f = -kMaxCfo; f <= kMaxCfo + 1e-9; f += 1.5) { const double v = std::abs(corrAt(y, f, 0, kSearchSyms)); mags.push_back(v); if (v > bv) { bv = v; bf = f; } }
        {
            const int i = (int)std::lround((bf + kMaxCfo) / 1.5);
            if (i > 0 && i + 1 < (int)mags.size()) { const double a = mags[(size_t)i - 1], b = mags[(size_t)i], c = mags[(size_t)i + 1]; const double den = a - 2 * b + c; if (den < 0) bf += 1.5 * 0.5 * (a - c) / den; }
        }
        // timing
        double tv[5];
        for (int d = -2; d <= 2; d++) { refProducts(k, (double)(s0 + d), kSearchSyms, y); tv[d + 2] = std::abs(corrAt(y, bf, 0, kSearchSyms)); }
        int bd = 2; for (int d = 0; d < 5; d++) if (tv[d] > tv[bd]) bd = d;
        double tau = bd - 2;
        if (bd > 0 && bd < 4) { const double den = tv[bd - 1] - 2 * tv[bd] + tv[bd + 1]; if (den < 0) tau += 0.5 * (tv[bd - 1] - tv[bd + 1]) / den; }
        const double t0 = (double)s0 + tau;
        refProducts(k, t0, kSearchSyms, y);
        const cf32 C1 = corrAt(y, bf, 0, kHfdlALen), C2 = corrAt(y, bf, kHfdlALen, kSearchSyms);
        const double df = std::arg(C2 * std::conj(C1)) * kHfdlBaud / (2 * M_PI * kHfdlALen);
        if (std::fabs(df) < 6) bf += df;
        const cf32 Call = corrAt(y, bf, 0, kSearchSyms);
        const cf32 h = Call / (float)kSearchSyms;
        { double se = 0; for (const auto& v : y) se += std::norm(v); if (std::norm(Call) < 0.3 * kSearchSyms * se || std::norm(h) < 1e-12) { k.searchAt = std::max(k.searchAt, s0) + 2; return 0; } }   // not A1 + A2 coherently
        // M1
        const cf32 rot = std::polar(1.f, (float)(-2 * M_PI * bf / kHfdlBaud));
        auto sym = [&](int i) { return cubicAt(k.x, t0 + i * kSps - (double)k.xBase) * std::polar(1.f, (float)(-2 * M_PI * bf * i / kHfdlBaud)); };
        (void)rot;
        int m1 = -1; double bs = -1e30;
        std::vector<cf32> ms((size_t)kHfdlM1Len);
        for (int j = 0; j < kHfdlM1Len; j++) ms[(size_t)j] = sym(kSearchSyms + j) * std::conj(h);
        for (int sc = 0; sc < 8; sc++) {
            double sum = 0;
            for (int j = 0; j < kHfdlM1Len; j++) sum += (hfdlMBit(sc, j) ? -1 : 1) * ms[(size_t)j].real();
            if (sum > bs) { bs = sum; m1 = sc; }
        }
        k.info.bursts++;
        lastBurst = nowSec();
        double me = 0; for (const auto& v : ms) me += std::norm(v);
        const double score = bs / std::sqrt(kHfdlM1Len * me + 1e-30);
        if (score < 0.3) { k.searchAt = std::max(k.searchAt + 2, s0 + kSearchSyms * kSps); k.info.bursts--; return 0; }   // not a preamble after all
        const int nsym = hfdlBurstSyms(m1);
        if ((int64_t)std::ceil(t0) + (int64_t)(nsym + kL + 8) * kSps >= end) { k.info.bursts--; k.waitUntil = (int64_t)std::ceil(t0) + (int64_t)(nsym + kL + 8) * kSps + 1; return -1; }
        // half-symbol samples, derotated
        const int nu = 2 * nsym + 4 * kL + 4;
        std::vector<cf32> u((size_t)nu);
        const int uoff = 2 * kL + 2;      // u[uoff + 2i] is symbol i
        for (int m = 0; m < nu; m++) {
            const double si = (m - uoff) / 2.0;
            u[(size_t)m] = cubicAt(k.x, t0 + si * kSps - (double)k.xBase) * std::polar(1.f, (float)(-2 * M_PI * bf * si / kHfdlBaud));
        }
        double pw = 0;
        for (int i = 0; i < kHfdlPreLen; i++) pw += std::norm(u[(size_t)(uoff + 2 * i)]);
        pw /= kHfdlPreLen;
        // reference / known flags
        const HfdlRate& R = hfdlRate(m1);
        const auto& pre = hfdlPreambleRef(m1);
        std::vector<int8_t> scr;
        hfdlScramblerSigns(R.blocks * kHfdlDataLen, scr);
        // equaliser; a burst the equaliser cannot decode (weak signal: 25 taps adapt to the noise) is tried again with the gain and phase tracker alone
        std::vector<HfdlMessage> msgs;
        bool ok = false;
        double sig2 = 1, preErr = 0;
        for (int mode = 0; mode < 3 && !ok; mode++) {   // 0: equaliser, 1: gain / phase tracker with decisions, no equaliser, 2: known symbols only (weak signals)
        const int eqOn = mode == 0 ? 1 : 0;
        const int Nf = 2 * kL + 1;
        std::vector<cf32> w, b, dp;
        w.assign((size_t)Nf, cf32(0, 0)); b.assign((size_t)kNb, cf32(0, 0)); dp.assign((size_t)kNb, cf32(0, 0));
        w[(size_t)kL] = std::conj(h) / (std::norm(h) + 1e-30f);
        float theta = 0;
        double errKnown = 0; int nKnown = 0;
        std::vector<float> soft((size_t)hfdlCodedBits(m1));
        auto run = [&](int i, bool known, cf32 ref, float mu, int di) {
            const cf32* v = &u[(size_t)(uoff + 2 * i - kL)];
            cf32 yv(0, 0);
            float nrm = 1e-9f;
            for (int q = 0; q < Nf; q++) { yv += w[(size_t)q] * v[q]; nrm += std::norm(v[q]); }
            for (int q = 0; q < kNb; q++) { yv += b[(size_t)q] * dp[(size_t)q]; nrm += std::norm(dp[(size_t)q]); }
            const cf32 rr = std::polar(1.f, -theta);
            const cf32 z = yv * rr;
            cf32 d;
            if (known) d = ref;
            else {
                const float sg = (float)scr[(size_t)di];
                const cf32 q = z * sg;
                d = hfdlPsk(R.bitsPerSym, hfdlDecide(R.bitsPerSym, q)) * sg;
            }
            const cf32 e = d - z;
            if (known) { errKnown += std::norm(e); nKnown++; }
            const cf32 E = e * std::conj(rr);
            const float g = mu / nrm;
            if (eqOn) {
                for (int q = 0; q < Nf; q++) w[(size_t)q] += g * E * std::conj(v[q]);
                for (int q = 0; q < kNb; q++) b[(size_t)q] += g * E * std::conj(dp[(size_t)q]);
            }
            theta += 0.03f * (z * std::conj(d)).imag() / (std::norm(d) + 1e-9f);
            for (int q = kNb - 1; q > 0; q--) dp[(size_t)q] = dp[(size_t)q - 1];
            dp[0] = d;
            return z;
        };
        std::vector<cf32> dataSym((size_t)(R.blocks * kHfdlDataLen));
        std::vector<double> probeErr((size_t)R.blocks + 1);
        if (mode == 2) {
            // Known symbols only: the complex gain is measured on the last 9 T probes of the preamble and on the probe after every block,
            // interpolated linearly across the data between them. Decisions on a signal at about 0 dB per symbol are wrong too often for the
            // decision-directed loops above to hold the phase; the 15-symbol probes (a third of the burst) are always right.
            const int nb = R.blocks;
            {   // symbol timing over the burst: a sample clock off by 100 ppm slips a symbol in 10000 and a long burst a quarter of one; the probes of every
                // block give the offset (correlation at -1 / 0 / +1 half symbols, parabola), a weighted line through them gives offset and drift, and the
                // symbols are taken again at the corrected times
                double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (int b2 = 0; b2 < nb; b2++) {
                    const int i0 = kHfdlPreLen + b2 * kHfdlBlockLen + kHfdlDataLen;
                    double c[3];
                    for (int d = -1; d <= 1; d++) {
                        cf32 a(0, 0);
                        for (int q = 0; q < kHfdlTLen; q++) a += u[(size_t)(uoff + 2 * (i0 + q) + d)] * (float)kHfdlT[q];
                        c[d + 1] = std::abs(a);
                    }
                    const double den = c[0] - 2 * c[1] + c[2];
                    if (den >= 0 || c[1] < c[0] || c[1] < c[2]) continue;
                    const double tau = 0.25 * (c[0] - c[2]) / den, wgt = c[1] * c[1], x = i0 + 0.5 * kHfdlTLen;
                    sw += wgt; sx += wgt * x; sy += wgt * tau; sxx += wgt * x * x; sxy += wgt * x * tau;
                }
                const double dd = sw * sxx - sx * sx;
                if (sw > 0 && dd > 1e-9 * sw * sw) {
                    const double slope = (sw * sxy - sx * sy) / dd, off = (sy - slope * sx) / sw;
                    if (std::fabs(slope) < 4e-4 && std::fabs(off) < 0.5) {
                        for (int m = 0; m < nu; m++) {
                            const double si = (m - uoff) / 2.0;
                            u[(size_t)m] = cubicAt(k.x, t0 + (si + off + slope * si) * kSps - (double)k.xBase) * std::polar(1.f, (float)(-2 * M_PI * bf * si / kHfdlBaud));
                        }
                    }
                }
            }
            std::vector<cf32> g((size_t)nb + 1);
            std::vector<double> ctr((size_t)nb + 1);
            auto at = [&](int i) { return u[(size_t)(uoff + 2 * i)]; };
            { cf32 a(0, 0); const int i0 = kHfdlPreLen - kHfdlTrain * kHfdlTLen; for (int i = i0; i < kHfdlPreLen; i++) a += at(i) * pre[(size_t)i]; g[0] = a / (float)(kHfdlPreLen - i0); ctr[0] = 0.5 * (i0 + kHfdlPreLen); }
            for (int b2 = 0; b2 < nb; b2++) {
                const int i0 = kHfdlPreLen + b2 * kHfdlBlockLen + kHfdlDataLen;
                cf32 a(0, 0);
                for (int q = 0; q < kHfdlTLen; q++) a += at(i0 + q) * (float)kHfdlT[q];
                g[(size_t)b2 + 1] = a / (float)kHfdlTLen; ctr[(size_t)b2 + 1] = i0 + 0.5 * kHfdlTLen;
            }
            auto gainAt = [&](double i) {
                size_t k2 = 0;
                while (k2 + 2 < g.size() && i > ctr[k2 + 1]) k2++;
                const float f = (float)((i - ctr[k2]) / (ctr[k2 + 1] - ctr[k2]));
                return g[k2] * (1 - f) + g[k2 + 1] * f;
            };
            double eAll = 0; int nAll = 0;
            probeErr[0] = 1;
            int di2 = 0;
            for (int b2 = 0; b2 < nb; b2++) {
                const int i0 = kHfdlPreLen + b2 * kHfdlBlockLen;
                for (int q = 0; q < kHfdlDataLen; q++, di2++) {
                    const cf32 gi = gainAt(i0 + q);
                    dataSym[(size_t)di2] = at(i0 + q) * std::conj(gi) / (std::norm(gi) + 1e-12f) * (float)scr[(size_t)di2];
                }
                double e = 0;
                for (int q = 0; q < kHfdlTLen; q++) { const cf32 gi = gainAt(i0 + kHfdlDataLen + q); e += std::norm(at(i0 + kHfdlDataLen + q) * std::conj(gi) / (std::norm(gi) + 1e-12f) - cf32((float)kHfdlT[q], 0)); }
                probeErr[(size_t)b2 + 1] = e / kHfdlTLen; eAll += e; nAll += kHfdlTLen;
            }
            preErr = probeErr.size() > 1 ? probeErr[1] : 1;
            probeErr[0] = probeErr.size() > 1 ? probeErr[1] : 1;
            sig2 = std::max(1e-4, eAll / std::max(1, nAll));
        } else {
        for (int pass = 0; pass < 2; pass++) {
            std::fill(dp.begin(), dp.end(), cf32(0, 0));
            theta = 0;
            errKnown = 0; nKnown = 0;
            for (int i = 0; i < kHfdlPreLen; i++) run(i, true, cf32(pre[(size_t)i], 0), pass == 0 ? 0.25f : 0.1f, 0);
        }
        preErr = errKnown / std::max(1, nKnown);
        errKnown = 0; nKnown = 0;
        const float muD = 0.15f, muP = 0.5f;      // normalised LMS steps on decisions and on the probes
        probeErr[0] = preErr;
        int i = kHfdlPreLen, di = 0;
        for (int blk = 0; blk < R.blocks; blk++) {
            for (int q = 0; q < kHfdlDataLen; q++, i++, di++) dataSym[(size_t)di] = run(i, false, cf32(), muD, di) * (float)scr[(size_t)di];
            const double e0 = errKnown; const int n0 = nKnown;
            for (int q = 0; q < kHfdlTLen; q++, i++) run(i, true, cf32((float)kHfdlT[q], 0), muP, 0);
            probeErr[(size_t)blk + 1] = (errKnown - e0) / std::max(1, nKnown - n0);
        }
        sig2 = std::max(1e-4, errKnown / std::max(1, nKnown));
        }
        // each block weighted by the error on the probes beside it (a fade makes its bits less sure)
        for (size_t s = 0; s < dataSym.size(); s++) {
            const size_t blk = s / kHfdlDataLen;
            const double e = std::max(1e-4, 0.5 * (probeErr[blk] + probeErr[blk + 1]));
            hfdlSoftBits(R.bitsPerSym, dataSym[s], (float)(1.0 / e), &soft[s * (size_t)R.bitsPerSym]);
        }
        const std::vector<uint8_t> pdu = hfdlDecodeSoft(m1, soft);
        msgs.clear();
        ok = hfdlParsePdu(pdu.data(), pdu.size(), msgs);
        }
        const double tStart = (double)(t0) / kFs - kHfdlPrekey / kHfdlBaud;
        const float snr = (float)(-10 * std::log10(sig2));
        const float lvl = (float)(10 * std::log10(pw + 1e-20));
        k.searchAt = (int64_t)t0 + (int64_t)(nsym - 40) * kSps;
        k.info.levelDb = lvl; k.info.snrDb = snr; k.info.lastCfoHz = (float)bf;
        if (!ok) { k.info.bad++; k.pend.push_back([this] { tel.blocksBad++; }); k.searchAt = s0 + kSearchSyms * kSps; return 0; }
        k.info.good++; k.info.lastHeardSec = nowSec(); k.info.lastBps = R.bps;
        // the shared lists change in feed(), under mu, one channel after the other: the same order as a sequential run
        const double dial = k.dial;
        k.pend.push_back([this, dial, msgs = std::move(msgs), bf, snr, lvl, tStart, R, m1]() mutable {
            tel.blocksOk++; tel.byRate[m1 & 3]++;
            tel.cfoHz = bf; tel.snrDb = snr; tel.dataValid = true;
            lastGood = nowSec();
            for (auto& m : msgs) {
                m.serial = ++serial; m.timeSec = tStart; m.wallTime = (int64_t)time(nullptr); m.freqHz = dial; m.levelDb = lvl; m.snrDb = snr; m.cfoHz = (float)bf;
                m.bps = R.bps; m.slots = R.twoSlots ? 2 : 1;
                note(dial, m);
                tel.messages.insert(tel.messages.begin(), m);
                if (automation::wants(automation::FHfdl)) {   // the automations (automation/engine.h)
                    automation::Event ev("hfdl.message");
                    char hx[8];
                    snprintf(hx, sizeof hx, "%06X", m.icao & 0xFFFFFF);
                    ev.at(automation::wallNow(), "HFDL", m.freqHz);
                    ev.str("kind", m.kind).str("summary", m.summary).str("icao", m.icao ? hx : "").flag("uplink", m.uplink).num("snr_db", m.snrDb);
                    automation::emit(std::move(ev));
                }
            }
            if (tel.messages.size() > kHfdlMsgMax) tel.messages.resize(kHfdlMsgMax);
        });
        return 0;
    }

    HfdlGroundStation& station(int id) {
        for (auto& g : tel.stations) if (g.id == id) return g;
        HfdlGroundStation g;
        g.id = id;
        if (const HfdlStationInfo* s = hfdlStation(id)) { g.name = s->name; g.hasPos = true; g.lat = s->lat; g.lon = s->lon; }
        tel.stations.push_back(g);
        std::sort(tel.stations.begin(), tel.stations.end(), [](const HfdlGroundStation& a, const HfdlGroundStation& b) { return a.id < b.id; });
        for (auto& x : tel.stations) if (x.id == id) return x;
        return tel.stations.front();
    }

    void note(double dial, HfdlMessage& m) {
        const double now = nowSec();
        if (m.squitter) {
            tel.squitters++;
            HfdlGroundStation& g = station(m.gsId);
            g.squitters++; g.freqHz = dial; g.lastHeardSec = now; g.levelDb = m.levelDb; g.utcSync = m.utcSync; g.frameIndex = m.frameIndex; g.activeKhz = m.activeKhz; g.heardDirect = true;
            if (!logged[m.gsId]) {
                logged[m.gsId] = true;
                if (logCb) { char b[160]; snprintf(b, sizeof b, "HFDL: ground station %d %s heard on %.3f MHz", m.gsId, g.name.c_str(), dial / 1e6); logCb(b); }
            }
            return;
        }
        tel.lpdus++;
        if (m.gsId) { HfdlGroundStation& g = station(m.gsId); g.messages++; if (m.uplink) { g.heardDirect = true; g.lastHeardSec = now; g.freqHz = dial; } }
        const uint32_t slot = (uint32_t)(m.gsId << 8) | (uint32_t)(m.acId & 0xFF);
        if (m.kind == "Logon confirm" || m.kind == "Logon resume confirm") { if (m.icao && m.acId >= 0) acIcao[((uint32_t)m.gsId << 8) | (uint32_t)m.acId] = m.icao; }
        if (!m.icao && m.acId > 0) { auto it = acIcao.find(slot); if (it != acIcao.end()) m.icao = it->second; }
        if (m.acars) tel.acarsMsgs++;
        if (!m.icao && m.acId <= 0 && m.reg.empty()) return;
        const uint32_t key = m.icao ? m.icao : 0x1000000u | slot;
        HfdlAircraft* a = nullptr;
        for (auto& x : tel.aircraft) if (x.key == key) { a = &x; break; }
        if (!a && m.icao) for (auto& x : tel.aircraft) if (x.key == (0x1000000u | slot)) { x.key = key; a = &x; break; }
        if (!a) { tel.aircraft.push_back(HfdlAircraft()); a = &tel.aircraft.back(); a->key = key; }
        a->icao = m.icao ? m.icao : a->icao;
        if (m.acId > 0) a->acId = m.acId;
        a->gsId = m.gsId;
        if (!m.reg.empty()) a->reg = m.reg;
        if (!m.flight.empty()) a->flight = m.flight;
        a->messages++; a->freqHz = dial; a->lastHeardSec = now;
        if (!m.uplink) a->levelDb = m.levelDb;
        if (m.kind == "Logon confirm") a->loggedOn = true;
        if (m.kind == "Logoff request" || m.kind == "Logon denied") a->loggedOn = false;
        if (m.hasPos) {
            tel.positionsTotal++;
            a->hasPos = true; a->lat = m.lat; a->lon = m.lon; a->posKind = m.posKind; a->posTime = now;
            if (m.hasAlt) { a->hasAlt = true; a->altFt = m.altFt; }
            a->track.push_back({m.lat, m.lon, now});
            if (a->track.size() > 24) a->track.erase(a->track.begin());
        }
        std::stable_sort(tel.aircraft.begin(), tel.aircraft.end(), [](const HfdlAircraft& x, const HfdlAircraft& y) { return x.lastHeardSec > y.lastHeardSec; });
        if (tel.aircraft.size() > 200) tel.aircraft.resize(200);
        (void)hex6;
    }
};

HfdlReceiver::HfdlReceiver() : p_(std::make_unique<Impl>()) {}
HfdlReceiver::~HfdlReceiver() = default;

void HfdlReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->rebuild = true; p_->nIn = 0; p_->shiftTimes(-p_->tel.nowSec); }
void HfdlReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->offset != hz) { p_->offset = hz; p_->rebuild = true; } }
void HfdlReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->center != hz) { p_->center = hz; p_->rebuild = true; } }
bool HfdlReceiver::ready() const { return p_->rate >= hfdlTuning().minSampleRate - 1; }
void HfdlReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void HfdlReceiver::setBasebandFilter(const BasebandFilterCfg& c) { std::lock_guard<std::mutex> lk(p_->mu); if (c != p_->bbf) { p_->bbf = c; p_->rebuild = true; } }
void HfdlReceiver::setWholeSpan(bool on) { std::lock_guard<std::mutex> lk(p_->mu); if (on != p_->whole) { p_->whole = on; p_->rebuild = true; } }
void HfdlReceiver::setChannels(const std::vector<double>& hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->want) { p_->want = hz; p_->rebuild = true; } }

void HfdlReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    HfdlTelemetry t;                     // the messages, aircraft and ground stations stay (clearResults); the clock runs on
    t.centerHz = p_->tel.centerHz; t.nowSec = p_->tel.nowSec;
    t.messages = std::move(p_->tel.messages); t.aircraft = std::move(p_->tel.aircraft); t.stations = std::move(p_->tel.stations);
    p_->tel = std::move(t);
    p_->lastGood = p_->lastBurst = -1e9;
    p_->rebuild = true;
    p_->seq++;
}

void HfdlReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    { std::lock_guard<std::mutex> lk(I.mu); if (I.rebuild) I.setup(); }
    if (I.rate < 1 || I.ch.empty()) { I.nIn += n; return; }
    // Every channel in the band: its own filter chain, search and burst decoder, no state shared with the others. They run in parallel on the shared pool
    // (the analysis thread helps and waits); what they find for the shared lists is held per channel and applied below in channel order, so the
    // messages, serial numbers and log lines are those of a sequential run.
    std::vector<Impl::Chan*> act;
    for (auto& k : I.ch) if (k.inBand) act.push_back(&k);
    // the shared first stage for a wide capture (F > 1): the channels take its output, at rate / F
    const cf32* in = x;
    size_t nin = n;
    if (I.F > 1) { I.decimate(x, n, I.dout); in = I.dout.data(); nin = I.dout.size(); }
    // the shared channeliser (cz): each channel takes the frames of its bin, at the sub-band rate
    if (I.cz) nin = I.channelise(x, n);
    auto job = [&](size_t j) {
        Impl::Chan& k = *act[j];
        for (size_t i = 0; i < nin; i++) {
            cf32 v = I.cz ? I.cband[(size_t)k.slot][i] : in[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            I.push(k, v);
        }
        I.process(k);
    };
    if (I.parallel && act.size() > 1 && (I.cz ? n : nin) >= 4096) ThreadPool::shared().parallelFor(act.size(), job);
    else for (size_t j = 0; j < act.size(); j++) job(j);
    {
        std::lock_guard<std::mutex> lk(I.mu);
        for (Impl::Chan* k : act) { for (auto& f : k->pend) f(); k->pend.clear(); }
    }
    I.nIn += n;
    std::lock_guard<std::mutex> lk(I.mu);
    const double now = I.nowSec();
    I.tel.nowSec = now;
    I.tel.channels.clear();
    for (auto& k : I.ch) { k.info.active = k.info.lastHeardSec > 0 && now - k.info.lastHeardSec < 40; I.tel.channels.push_back(k.info); }
    I.tel.state = now - I.lastGood < 120 ? 2 : now - I.lastBurst < 60 ? 1 : 0;
    I.seq++;
}

void HfdlReceiver::setChanneliser(int mode) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->czForce != mode) { p_->czForce = mode; p_->rebuild = true; } }
void HfdlReceiver::setParallel(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->parallel = on; }

void HfdlReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear(); p_->tel.aircraft.clear(); p_->tel.stations.clear(); p_->acIcao.clear();
    p_->seq++;
}
std::string HfdlReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.nowSec, p_->serial, p_->tel.messages, p_->tel.aircraft, p_->tel.stations, p_->acIcao);
}
bool HfdlReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    uint64_t serial = 0;
    HfdlTelemetry l;
    std::map<uint32_t, uint32_t> acIcao;
    if (!results::unpack(blob, t0, serial, l.messages, l.aircraft, l.stations, acIcao)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double now = p_->tel.nowSec;
    p_->tel.messages = std::move(l.messages); p_->tel.aircraft = std::move(l.aircraft); p_->tel.stations = std::move(l.stations);
    p_->acIcao = std::move(acIcao);
    p_->serial = std::max(p_->serial, serial);
    p_->tel.nowSec = t0;
    p_->shiftTimes(now - t0);
    p_->seq++;
    return true;
}

bool HfdlReceiver::telemetry(HfdlTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq) return false;
    out = p_->tel;
    out.seq = p_->seq;
    return true;
}

ModeTuning hfdlTuning() {
    ModeTuning t;
    t.stdMode = 37; t.id = "hfdl"; t.name = "HFDL";
    t.minMhz = 2; t.maxMhz = 30; t.defMhz = 8.9;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 24000;
    t.tuneOffsetHz = 0;          // the channels sit beside the centre; the DC spike falls between them
    return t;
}

} // namespace dect2
