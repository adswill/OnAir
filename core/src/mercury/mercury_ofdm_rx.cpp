// Mercury OFDM burst receiver (see mercury_phy.h).
//
// Acquisition. The 8 kHz input (1500 Hz of audio at 0 Hz) is low-passed and decimated into three streams: 1 kHz for the 3 and 4
// carrier modes, 2 kHz for the 9 carrier ones, 4 kHz for the wide ones. In each, every distinct preamble and postamble (the modes that
// share a carrier layout share them: DATAC13 / 15 / 16, DATAC0 / 3) is correlated with the stream by FFT, at frequency offsets one
// FFT bin (3.9 Hz) apart over the search range. A correlation peak above the noise is an anchor: a preamble says where a burst starts,
// a postamble where it ends, for every mode of the layout (their packets differ in length). Anchors that agree on a burst add up, so a
// burst is acquired from its preamble, its postamble or both together, at a lower level than either alone.
//
// Demodulation (8 kHz). Timing and frequency are refined against the preamble and postamble; every symbol goes through a DFT at the
// carrier frequencies with the window starting half a cyclic prefix early (room for multipath and timing error); the residual frequency
// offset is measured from the known rows (preamble, pilots, postamble) and taken out. The channel of every carrier is measured on the
// known rows and interpolated linearly in time between pilot rows; the noise from the difference of neighbouring known rows. Soft bits
// (max-log for 16-QAM), the unique word removed, the interleaver undone, the shortened information bits added as known ones, and the
// LDPC decoder runs. A burst that does not decode is tried again with the channel smoothed more (slow fading, low SNR) and less (fast
// fading); the first that decodes is kept.
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_mfsk.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace dect2 {
namespace mercury {

namespace {

using dect2::math::kPi;
constexpr double kFs = 8000.0;
constexpr int kRingBits = 17, kRingN = 1 << kRingBits, kRingMask = kRingN - 1;   // 16.4 s at 8 kHz
constexpr double kMixHz = 1500.0;

std::vector<float> lowpass(int taps, double cutoffNorm) {   // Blackman windowed sinc, unit DC gain; cutoff in cycles per sample
    std::vector<float> h((size_t)taps);
    double sum = 0;
    for (int k = 0; k < taps; k++) {
        const double t = k - (taps - 1) * 0.5;
        const double s = t == 0 ? 2 * cutoffNorm : std::sin(2 * kPi * cutoffNorm * t) / (kPi * t);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (taps - 1)) + 0.08 * std::cos(4 * kPi * k / (taps - 1));
        h[(size_t)k] = (float)(s * w);
        sum += s * w;
    }
    for (float& v : h) v = (float)(v / sum);
    return h;
}

// A template: the preamble or postamble of a carrier layout, at 8 kHz (mixed like the input) and at its stream's rate
struct Tmpl {
    std::vector<cf> t8, td;
    std::vector<cf> spec;      // FFT of td, zero padded to the stream's block
    double e8 = 0, ed = 0;     // energies
    int len8 = 0, lend = 0;
};

struct Layout {
    std::vector<int> modes;
    int stream = 0;            // index into Impl::streams
    Tmpl pre, post;
    float leff = 1;            // effective number of independent samples in a template (noise statistics)
};

struct Stream {
    int D = 1;
    std::vector<float> h;
    int half = 0;
    int nfft = 0;
    std::vector<cf> y;         // decimated output, y[i] is sample yBase + i (8 kHz time (yBase + i) * D)
    int64_t yBase = 0;
    int64_t nextOut = 0;       // the next decimated sample to produce
    int64_t blockStart = 0;    // the first lag of the next correlation block
    int lmax = 0, hop = 0;
    std::vector<int> layouts;
    std::unique_ptr<Fft> fft;
    std::vector<double> cumE;  // prefix sums of |y|^2 (same indexing as y, one longer)
    double avgPow = 0;         // slow average of |y|^2 (silence is not searched)
};

// A run of correlation peaks of one template: the best one is the anchor. The background is a slow average of the correlation itself
// (the maximum over the frequency offsets): with noise alone it is about 4 / L for a template of L samples, beside a strong signal of
// another layout (or the same burst's other frames) it is higher, and a peak counts as much as it stands above it (CFAR).
struct Cluster { bool on = false; int64_t lag = 0; float metric = 0, k = 0; double f = 0; float bg = 0; };

struct Anchor { int layout; bool post; int64_t t8; double f; float k; };

struct Cand {
    int mode;
    int64_t start;    // 8 kHz
    double f;
    float preK = 0, postK = 0;
    bool tried = false;
};

float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

} // namespace

struct OfdmReceiver::Impl {
    RxOptions opt;
    std::vector<cf> ring;
    int64_t n8 = 0;
    double power = 0;
    int64_t nPower = 0;
    float level = -200;
    uint64_t nDet = 0, nFail = 0;
    std::vector<Stream> streams;
    std::vector<Layout> layouts;
    std::vector<Cluster> clusters;     // [layout * 2 + post]
    std::vector<Cand> cands;
    std::vector<OfdmFrame> out;
    struct Pending { OfdmFrame f; int64_t start, end; float score; bool pair; };
    std::vector<Pending> failed;        // failed decodes held until no overlapping candidate is left
    std::vector<std::pair<int64_t, int64_t>> decodedSpans;
    // per mode: DFT twiddles [k][n] for carriers 1 .. nc
    std::vector<std::vector<cf>> tw;
    std::vector<std::unique_ptr<LdpcDecoder>> dec;
    MfskReceiver mfsk1{kMfsk}, mfsk5{kMfsk500};   // the MFSK floor, 1 kHz and 500 Hz (mercury_mfsk.h)

    Impl() {
        ring.assign(kRingN, cf(0, 0));
        // the streams
        const int Ds[3] = {8, 4, 2};
        const double pass[3] = {250, 400, 1150};
        for (int s = 0; s < 3; s++) {
            Stream st;
            st.D = Ds[s];
            const double fsD = kFs / st.D;
            const int taps = s == 0 ? 255 : s == 1 ? 127 : 63;
            st.h = lowpass(taps, (pass[s] + 0.5 * (fsD / 2 - pass[s])) / kFs);
            st.half = (taps - 1) / 2;
            st.nfft = s == 0 ? 256 : s == 1 ? 512 : 1024;
            st.fft = std::make_unique<Fft>(st.nfft);
            streams.push_back(std::move(st));
        }
        // the layouts: modes with the same carriers, symbol timing and constellation share the preamble and postamble
        for (int id = 0; id < kNumOfdmModes; id++) {
            const OfdmMode& m = ofdmMode(id);
            int found = -1;
            for (size_t l = 0; l < layouts.size(); l++) {
                const OfdmMode& o = ofdmMode(layouts[l].modes[0]);
                if (o.nc == m.nc && o.ts == m.ts && o.tcp == m.tcp && o.bps == m.bps && o.ns == m.ns) found = (int)l;
            }
            if (found >= 0) { layouts[(size_t)found].modes.push_back(id); continue; }
            Layout L;
            L.modes.push_back(id);
            L.stream = m.nc <= 4 ? 0 : m.nc <= 9 ? 1 : 2;
            layouts.push_back(std::move(L));
        }
        for (size_t l = 0; l < layouts.size(); l++) {
            Layout& L = layouts[l];
            Stream& st = streams[(size_t)L.stream];
            const Geometry& g = geometry(L.modes[0]);
            for (int w = 0; w < 2; w++) {
                Tmpl& T = w ? L.post : L.pre;
                T.len8 = g.samplesPerFrame;
                T.t8 = frameWave(g, w ? g.postamble : g.preamble);
                for (const cf& v : T.t8) T.e8 += std::norm(v);
                // through the stream's filter: td[m] = sum_k h[k] t8[m D + half - k]
                T.lend = (T.len8 + st.D - 1) / st.D;
                T.td.assign((size_t)T.lend, cf(0, 0));
                for (int mm = 0; mm < T.lend; mm++) {
                    cf acc(0, 0);
                    for (int k = 0; k < (int)st.h.size(); k++) {
                        const int i = mm * st.D + st.half - k;
                        if (i >= 0 && i < T.len8) acc += st.h[(size_t)k] * T.t8[(size_t)i];
                    }
                    T.td[(size_t)mm] = acc;
                }
                for (const cf& v : T.td) T.ed += std::norm(v);
                T.spec.assign((size_t)st.nfft, cf(0, 0));
                std::copy(T.td.begin(), T.td.end(), T.spec.begin());
                st.fft->forward(T.spec.data());
                st.lmax = std::max(st.lmax, T.lend);
            }
            // with noise alone the normalised correlation is exponential with mean 1 / (template length) whatever the stream's band
            L.leff = (float)L.pre.lend;
            st.layouts.push_back((int)l);
        }
        for (Stream& st : streams) st.hop = st.nfft - st.lmax + 1;
        clusters.assign(layouts.size() * 2, Cluster());
        for (int id = 0; id < kNumOfdmModes; id++) {
            const Geometry& g = geometry(id);
            std::vector<cf> t((size_t)(g.nc * g.M));
            for (int k = 1; k <= g.nc; k++) {
                const double frel = (g.nlower + k) * (double)g.rs - kMixHz;
                for (int n = 0; n < g.M; n++) t[(size_t)((k - 1) * g.M + n)] = std::polar(1.f, (float)(-2 * kPi * frel * n / kFs));
            }
            tw.push_back(std::move(t));
            dec.push_back(nullptr);
        }
    }

    // one frame of known symbols as 8 kHz samples, mixed by -1500 Hz (as the input is)
    static std::vector<cf> frameWave(const Geometry& g, const std::vector<cf>& rows) {
        std::vector<cf> x((size_t)g.samplesPerFrame);
        for (int r = 0; r < g.ns; r++) {
            for (int i = 0; i < g.sps; i++) {
                const int nBody = (i - g.Ncp + g.M) % g.M;   // the cyclic prefix repeats the end of the body
                std::complex<double> acc(0, 0);
                for (int k = 1; k <= g.nc; k++) {
                    const cf X = rows[(size_t)(r * (g.nc + 2) + k)];
                    const double ph = 2 * kPi * (double)(g.nlower + k) * nBody / g.M;
                    acc += std::complex<double>(X.real(), X.imag()) * std::polar(1.0, ph);
                }
                const int64_t n = (int64_t)r * g.sps + i;
                acc *= std::polar(1.0 / g.M, -2 * kPi * kMixHz * (double)n / kFs);
                x[(size_t)n] = cf((float)acc.real(), (float)acc.imag());
            }
        }
        return x;
    }

    cf at(int64_t i) const { return ring[(size_t)(i & kRingMask)]; }
    bool have(int64_t a, int64_t b) const { return a >= 0 && b <= n8 && n8 - a <= kRingN; }

    void reset() {
        std::fill(ring.begin(), ring.end(), cf(0, 0));
        n8 = 0; power = 0; nPower = 0; level = -200;
        for (Stream& st : streams) { st.y.clear(); st.yBase = 0; st.nextOut = 0; st.blockStart = 0; st.cumE.assign(1, 0.0); st.avgPow = 0; }
        std::fill(clusters.begin(), clusters.end(), Cluster());
        cands.clear(); out.clear(); failed.clear(); decodedSpans.clear();
        mfsk1.reset(); mfsk5.reset();
    }

    // ---------------------------------------------------------------- acquisition

    void produce(Stream& st) {
        // decimated sample m needs the 8 kHz samples up to m D + half
        while (st.nextOut * st.D + st.half < n8) {
            const int64_t c = st.nextOut * st.D + st.half;
            cf acc(0, 0);
            if (c - (int64_t)st.h.size() + 1 >= 0 && n8 - (c - (int64_t)st.h.size() + 1) <= kRingN) {
                for (int k = 0; k < (int)st.h.size(); k++) acc += st.h[(size_t)k] * at(c - k);
            }
            if (st.y.empty()) { st.yBase = st.nextOut; st.cumE.assign(1, 0.0); }
            st.y.push_back(acc);
            st.cumE.push_back(st.cumE.back() + std::norm(acc));
            st.avgPow += 0.0005 * (std::norm(acc) - st.avgPow);
            st.nextOut++;
        }
    }

    void correlate(Stream& st) {
        while (st.blockStart + st.nfft <= st.yBase + (int64_t)st.y.size()) {
            const int64_t b0 = st.blockStart - st.yBase;
            std::vector<cf> Y((size_t)st.nfft);
            for (int i = 0; i < st.nfft; i++) Y[(size_t)i] = st.y[(size_t)(b0 + i)];
            st.fft->forward(Y.data());
            const double binHz = (kFs / st.D) / st.nfft;
            const int K = (int)std::ceil(opt.maxCfoHz / binHz);
            std::vector<cf> C((size_t)st.nfft);
            for (int l : st.layouts) {
                Layout& L = layouts[(size_t)l];
                bool want = false;
                for (int id : L.modes) want |= (opt.modeMask >> id) & 1;
                if (!want) continue;
                for (int w = 0; w < 2; w++) {
                    const Tmpl& T = w ? L.post : L.pre;
                    std::vector<float> best((size_t)st.hop, 0.f);
                    std::vector<int> bestK((size_t)st.hop, 0);
                    for (int k0 = -K; k0 <= K; k0++) {
                        for (int k = 0; k < st.nfft; k++) {
                            const int ks = ((k - k0) % st.nfft + st.nfft) % st.nfft;
                            C[(size_t)k] = Y[(size_t)k] * std::conj(T.spec[(size_t)ks]);
                        }
                        st.fft->inverse(C.data());
                        for (int t = 0; t < st.hop; t++) {
                            const float v = std::norm(C[(size_t)t]);
                            if (v > best[(size_t)t]) { best[(size_t)t] = v; bestK[(size_t)t] = k0; }
                        }
                    }
                    const float scale = 1.f / ((float)st.nfft * (float)st.nfft);
                    Cluster& cl = clusters[(size_t)(l * 2 + w)];
                    for (int t = 0; t < st.hop; t++) {
                        const int64_t i0 = b0 + t;
                        const double ey = st.cumE[(size_t)(i0 + T.lend)] - st.cumE[(size_t)i0];
                        const bool live = ey > 1e-18 * T.lend && ey > 0.02 * st.avgPow * T.lend;   // not silence
                        const float metric = live ? (float)(best[(size_t)t] * scale / (T.ed * ey + 1e-30)) : 0.f;
                        const int64_t lag = st.blockStart + t;
                        if (cl.on && lag - cl.lag > T.lend) { finishCluster(l, w, cl); cl.on = false; }
                        const float floor = 4.f / L.leff;
                        const float k = metric * 4.f / std::max(cl.bg, floor);   // about metric * L with noise alone
                        cl.bg += (metric - cl.bg) / (2.f * (float)T.lend);
                        if (k >= 8.f && (!cl.on || k > cl.k)) {
                            cl.on = true; cl.lag = lag; cl.metric = metric; cl.k = k; cl.f = bestK[(size_t)t] * binHz;
                        }
                    }
                }
            }
            st.blockStart += st.hop;
            // keep the stream short: the next block and the energy prefix are all that is needed
            const int64_t keepFrom = st.blockStart - st.yBase;
            if (keepFrom > 4 * st.nfft) {
                st.y.erase(st.y.begin(), st.y.begin() + keepFrom);
                const double base = st.cumE[(size_t)keepFrom];
                st.cumE.erase(st.cumE.begin(), st.cumE.begin() + keepFrom);
                for (double& v : st.cumE) v -= base;
                st.yBase += keepFrom;
            }
        }
    }

    void finishCluster(int l, int w, const Cluster& cl) {
        const Layout& L = layouts[(size_t)l];
        const Stream& st = streams[(size_t)L.stream];
        Anchor a{l, w == 1, cl.lag * st.D, cl.f, cl.k};
        for (int id : L.modes) {
            if (!((opt.modeMask >> id) & 1)) continue;
            const Geometry& g = geometry(id);
            const int64_t start = a.post ? a.t8 - (int64_t)(g.np + 1) * g.samplesPerFrame : a.t8;
            if (start < 0) continue;
            bool merged = false;
            for (Cand& c : cands) {
                if (c.mode != id || std::llabs(c.start - start) > 2 * st.D + 2 || std::fabs(c.f - a.f) > 8) continue;
                if (a.post) c.postK = std::max(c.postK, a.k); else c.preK = std::max(c.preK, a.k);
                merged = true;
            }
            if (!merged) {
                Cand c;
                c.mode = id; c.start = start; c.f = a.f;
                (a.post ? c.postK : c.preK) = a.k;
                cands.push_back(c);
            }
        }
    }

    // ---------------------------------------------------------------- the candidates

    static float score(const Cand& c) { return c.preK + c.postK; }
    static bool worthTrying(const Cand& c) {
        if (std::max(c.preK, c.postK) >= 20.f) return true;
        return c.preK >= 8.f && c.postK >= 8.f && c.preK + c.postK >= 26.f;
    }

    void service(bool flushAll) {
        // candidates whose burst is in the ring (and whose other anchor had time to arrive)
        for (;;) {
            int pick = -1;
            for (size_t i = 0; i < cands.size(); i++) {
                const Cand& c = cands[i];
                const Geometry& g = geometry(c.mode);
                const int64_t end = c.start + g.burstSamples;
                const bool ready = flushAll ? end <= n8 + g.samplesPerFrame : end + 6000 <= n8;
                if (!ready) continue;
                if (pick < 0 || score(c) > score(cands[(size_t)pick])) pick = (int)i;
            }
            if (pick < 0) break;
            Cand c = cands[(size_t)pick];
            cands.erase(cands.begin() + pick);
            const Geometry& g = geometry(c.mode);
            const int64_t end = c.start + g.burstSamples;
            bool covered = false;
            for (const auto& sp : decodedSpans) if (overlap(c.start, end, sp.first, sp.second) > 0.1) covered = true;
            if (covered || !worthTrying(c)) continue;
            OfdmFrame f;
            if (!have(c.start - 64, std::min(n8, end + 64))) continue;
            nDet++;
            const bool ok = decodeBurst(c, f);
            if (ok) {
                decodedSpans.push_back({f.startSample, f.endSample});
                mfsk1.addDecodedSpan(f.startSample, f.endSample);
                mfsk5.addDecodedSpan(f.startSample, f.endSample);
                failed.erase(std::remove_if(failed.begin(), failed.end(), [&](const Pending& p) { return overlap(p.start, p.end, f.startSample, f.endSample) > 0.1; }), failed.end());
                out.push_back(std::move(f));
            } else {
                nFail++;
                bool dup = false;
                const bool pair = c.preK >= 12.f && c.postK >= 12.f;
                for (Pending& p : failed) if (overlap(p.start, p.end, c.start, end) > 0.3) { dup = true; if (score(c) > p.score) { p.f = f; p.score = score(c); p.start = c.start; p.end = end; p.pair = pair; } }
                if (!dup) failed.push_back({f, c.start, end, score(c), pair});
            }
        }
        // a failed burst is reported once nothing that overlaps it is left to try
        for (size_t i = 0; i < failed.size();) {
            bool open = false;
            for (const Cand& c : cands) if (overlap(c.start, c.start + geometry(c.mode).burstSamples, failed[i].start, failed[i].end) > 0.3) open = true;
            // ...and once a longer burst (an MFSK500 frame is 26 s) that overlaps it would have decoded too
            if ((open || n8 < failed[i].start + 30 * 8000) && !flushAll) { i++; continue; }
            bool decoded = false;
            for (const auto& sp : decodedSpans) if (overlap(failed[i].start, failed[i].end, sp.first, sp.second) > 0.1) decoded = true;
            if (!decoded && failed[i].pair) out.push_back(std::move(failed[i].f));   // only a burst found by both its preamble and postamble is worth a row
            failed.erase(failed.begin() + (long)i);
        }
        // forget old candidates that never became worth trying, and old spans
        cands.erase(std::remove_if(cands.begin(), cands.end(), [&](const Cand& c) { return n8 - c.start > kRingN - 8000; }), cands.end());
        decodedSpans.erase(std::remove_if(decodedSpans.begin(), decodedSpans.end(), [&](const std::pair<int64_t, int64_t>& s) { return n8 - s.second > 120000; }), decodedSpans.end());
    }

    static double overlap(int64_t a0, int64_t a1, int64_t b0, int64_t b1) {
        const int64_t lo = std::max(a0, b0), hi = std::min(a1, b1);
        if (hi <= lo) return 0;
        return (double)(hi - lo) / (double)std::max<int64_t>(1, std::min(a1 - a0, b1 - b0));
    }

    // ---------------------------------------------------------------- demodulation

    // correlation of the ring at t with a template (8 kHz, mixed), at frequency offset f: |c|^2 / (E_t E_x)
    double corrAt(const Tmpl& T, int64_t t, double f) const {
        if (!have(t, t + T.len8)) return 0;
        std::complex<double> acc(0, 0);
        double ex = 0;
        const std::complex<double> step = std::polar(1.0, -2 * kPi * f / kFs);
        std::complex<double> rot(1, 0);
        for (int n = 0; n < T.len8; n++) {
            const cf x = at(t + n);
            const std::complex<double> xd(x.real(), x.imag());
            acc += xd * rot * std::conj(std::complex<double>(T.t8[(size_t)n].real(), T.t8[(size_t)n].imag()));
            ex += std::norm(xd);
            rot *= step;
            if ((n & 127) == 127) rot /= std::abs(rot);
        }
        return std::norm(acc) / (T.e8 * ex + 1e-30);
    }

    // one symbol through the DFT: Y[k - 1] for carriers 1 .. nc, the window starting at w (8 kHz), frequency offset f
    void symbolDft(const Geometry& g, int64_t w, double f, cf* Y) const {
        const std::vector<cf>& T = tw[(size_t)g.id];
        std::vector<cf> v((size_t)g.M);
        // the input is the audio mixed by -1500 Hz from sample 0: undo the mixer's phase at the window, take out the offset
        const double ph0 = 2 * kPi * (kMixHz - f) * (double)w / kFs;
        const std::complex<double> step = std::polar(1.0, -2 * kPi * f / kFs);
        std::complex<double> rot = std::polar(1.0, std::fmod(ph0, 2 * kPi));
        for (int n = 0; n < g.M; n++) {
            const cf x = at(w + n);
            const std::complex<double> r = std::complex<double>(x.real(), x.imag()) * rot;
            v[(size_t)n] = cf((float)r.real(), (float)r.imag());
            rot *= step;
        }
        for (int k = 0; k < g.nc; k++) {
            cf acc(0, 0);
            const cf* t = &T[(size_t)(k * g.M)];
            for (int n = 0; n < g.M; n++) acc += v[(size_t)n] * t[n];
            Y[k] = acc / (float)g.M;
        }
    }

    // all rows of the burst (preamble, packet, postamble): rows x nc
    void demodRows(const Geometry& g, int64_t t0, double f, int adv, std::vector<cf>& Y) const {
        const int rows = (g.np + 2) * g.ns;
        Y.assign((size_t)(rows * g.nc), cf(0, 0));
        for (int r = 0; r < rows; r++) symbolDft(g, t0 + (int64_t)r * g.sps + g.Ncp - adv, f, &Y[(size_t)(r * g.nc)]);
    }

    // the known symbol of row r, carrier k (1 .. nc), or 0 for a data symbol
    static cf known(const Geometry& g, int r, int k) {
        const int rows = (g.np + 2) * g.ns;
        if (r < g.ns) return g.preamble[(size_t)(r * (g.nc + 2) + k)];
        if (r >= rows - g.ns) return g.postamble[(size_t)((r - (rows - g.ns)) * (g.nc + 2) + k)];
        if ((r - g.ns) % g.ns == 0) return cf(g.pilots[(size_t)k], 0);
        return cf(0, 0);
    }

    // residual frequency offset from the phase progression of the known rows (consecutive preamble / postamble rows, and pilot rows)
    static double residualCfo(const Geometry& g, const std::vector<cf>& Y) {
        const int rows = (g.np + 2) * g.ns;
        std::complex<double> a1(0, 0), a5(0, 0);
        auto H = [&](int r, int k) { const cf x = known(g, r, k + 1); return std::complex<double>(Y[(size_t)(r * g.nc + k)] / x); };
        for (int k = 0; k < g.nc; k++) {
            for (int r = 0; r + 1 < g.ns; r++) a1 += H(r + 1, k) * std::conj(H(r, k));
            for (int r = rows - g.ns; r + 1 < rows; r++) a1 += H(r + 1, k) * std::conj(H(r, k));
        }
        const double symT = g.sps / kFs;
        double f = std::arg(a1) / (2 * kPi * symT);
        // pilot rows ns symbols apart give the finer estimate (unambiguous within +-rs / (2 ns)), centred on the coarse one
        for (int k = 0; k < g.nc; k++)
            for (int r = 0; r + g.ns < rows; r += g.ns) a5 += H(r + g.ns, k) * std::conj(H(r, k)) * std::polar(1.0, -2 * kPi * f * g.ns * symT);
        f += std::arg(a5) / (2 * kPi * g.ns * symT);
        return f;
    }

    bool decodeBurst(const Cand& c, OfdmFrame& f) {
        const Geometry& g = geometry(c.mode);
        const OfdmMode& m = ofdmMode(c.mode);
        const Layout* L = nullptr;
        for (const Layout& l : layouts) for (int id : l.modes) if (id == c.mode) L = &l;
        const int D = streams[(size_t)L->stream].D;
        f.mode = c.mode;
        // fine timing and frequency: preamble and postamble together
        const int64_t postOff = (int64_t)(g.np + 1) * g.samplesPerFrame;
        double bestM = -1, bestF = c.f;
        int64_t bestT = c.start;
        const int span = 2 * D + 2;
        for (int pass = 0; pass < 2; pass++) {
            const double fc = pass == 0 ? c.f : bestF;
            const double fspan = pass == 0 ? 2.5 : 0.4, fstep = pass == 0 ? 0.5 : 0.1;
            const int64_t tc = pass == 0 ? c.start : bestT;
            const int tsp = pass == 0 ? span : 1;
            for (int64_t t = tc - tsp; t <= tc + tsp; t++)
                for (double ff = fc - fspan; ff <= fc + fspan + 1e-9; ff += fstep) {
                    const double mpre = corrAt(L->pre, t, ff), mpost = corrAt(L->post, t + postOff, ff);
                    if (mpre + mpost > bestM) { bestM = mpre + mpost; bestT = t; bestF = ff; }
                }
        }
        const double mpre = corrAt(L->pre, bestT, bestF), mpost = corrAt(L->post, bestT + postOff, bestF);
        f.metric = (float)std::max(mpre, mpost);
        f.viaPostamble = c.preK < 8.f && c.postK >= 8.f;
        f.startSample = bestT;
        f.endSample = bestT + g.burstSamples;
        const int adv = g.Ncp / 2;
        std::vector<cf> Y;
        double fo = bestF;
        demodRows(g, bestT, fo, adv, Y);
        for (int it = 0; it < 2; it++) {
            const double df = residualCfo(g, Y);
            if (std::fabs(df) < 0.02) break;
            fo += clampf((float)df, -4.f, 4.f);
            demodRows(g, bestT, fo, adv, Y);
        }
        f.cfoHz = fo;
        // decode with three channel estimates: plain, smoothed, strongly smoothed
        static const int kSmooth[3][2] = {{1, 0}, {0, 0}, {2, 2}};   // {frequency taps half width, pilot rows half width}
        bool ok = false;
        for (int v = 0; v < 3 && !ok; v++) ok = equaliseAndDecode(g, m, Y, kSmooth[v][0], kSmooth[v][1], f);
        return ok;
    }

    bool equaliseAndDecode(const Geometry& g, const OfdmMode& m, const std::vector<cf>& Y, int fHalf, int tHalf, OfdmFrame& f) {
        const int rows = (g.np + 2) * g.ns, nc = g.nc;
        // raw channel at the known rows
        std::vector<cf> Hraw((size_t)(rows * nc), cf(0, 0));
        std::vector<uint8_t> isKnown((size_t)rows, 0);
        for (int r = 0; r < rows; r++) {
            if (known(g, r, 1) == cf(0, 0)) continue;
            isKnown[(size_t)r] = 1;
            for (int k = 0; k < nc; k++) Hraw[(size_t)(r * nc + k)] = Y[(size_t)(r * nc + k)] / known(g, r, k + 1);
        }
        // noise: half the mean squared difference of neighbouring known rows (preamble and postamble rows are 1 symbol apart)
        double nsum = 0, ssum = 0;
        int ncount = 0, scount = 0;
        for (int r = 0; r + 1 < rows; r++) {
            if (!isKnown[(size_t)r] || !isKnown[(size_t)r + 1]) continue;
            for (int k = 0; k < nc; k++) { nsum += std::norm(Hraw[(size_t)((r + 1) * nc + k)] - Hraw[(size_t)(r * nc + k)]); ncount++; }
        }
        for (int r = 0; r < rows; r++) if (isKnown[(size_t)r]) for (int k = 0; k < nc; k++) { ssum += std::norm(Hraw[(size_t)(r * nc + k)]); scount++; }
        double sigma2 = ncount ? nsum / (2.0 * ncount) : 1e-3;
        const double sig = scount ? ssum / scount : 1.0;
        sigma2 = std::max(sigma2, sig * 1e-4);
        const double esno = std::max(1e-3, sig / sigma2 - 1.0);
        f.esNoDb = (float)(10 * std::log10(esno));
        f.snrDb = (float)(f.esNoDb + 10 * std::log10(nc / (3000.0 * (m.ts + m.tcp))));
        if (f.esNoDb < -4.f) return false;   // nothing there: no mode decodes below about 0 dB per symbol
        // the estimate on the known rows: smoothed over neighbouring carriers and known rows of the same kind
        std::vector<int> krows;
        for (int r = 0; r < rows; r++) if (isKnown[(size_t)r] && (r % g.ns == 0)) krows.push_back(r);   // one row per frame: the pilot rows (and the first rows of pre- / postamble)
        std::vector<cf> Hk(krows.size() * (size_t)nc, cf(0, 0));
        for (size_t j = 0; j < krows.size(); j++) {
            for (int k = 0; k < nc; k++) {
                std::complex<double> acc(0, 0);
                double wsum = 0;
                for (int dj = -tHalf; dj <= tHalf; dj++) {
                    const int jj = (int)j + dj;
                    if (jj < 0 || jj >= (int)krows.size()) continue;
                    for (int dk = -fHalf; dk <= fHalf; dk++) {
                        const int kk = k + dk;
                        if (kk < 0 || kk >= nc) continue;
                        const double w = (1.0 + tHalf - std::abs(dj)) * (1.0 + fHalf - std::abs(dk));
                        const cf h = Hraw[(size_t)(krows[(size_t)jj] * nc + kk)];
                        acc += w * std::complex<double>(h.real(), h.imag());
                        wsum += w;
                    }
                }
                // in the preamble and postamble the neighbouring rows are known too: average them in (they are one symbol apart)
                const int r0 = krows[j];
                if (tHalf > 0 && (r0 < g.ns || r0 >= rows - g.ns)) {
                    for (int dr = 1; dr < g.ns; dr++) {
                        const cf h = Hraw[(size_t)((r0 + dr) * nc + k)];
                        acc += std::complex<double>(h.real(), h.imag()) * 0.5;
                        wsum += 0.5;
                    }
                }
                Hk[j * (size_t)nc + (size_t)k] = cf((float)(acc.real() / wsum), (float)(acc.imag() / wsum));
            }
        }
        // the data symbols of the packet, equalised, and their soft bits
        const int bps = g.bps;
        std::vector<cf> zs((size_t)g.symsPerPacket);
        std::vector<float> hs((size_t)g.symsPerPacket);   // |H|^2 / sigma2 of each symbol
        int s = 0;
        for (int r = g.ns; r < rows - g.ns; r++) {
            if ((r - g.ns) % g.ns == 0) continue;
            // between pilot rows j0 (row r0) and j0 + 1
            int j0 = 0;
            while (j0 + 1 < (int)krows.size() && krows[(size_t)j0 + 1] <= r) j0++;
            const int j1 = std::min((int)krows.size() - 1, j0 + 1);
            const double a = j1 == j0 ? 0.0 : (double)(r - krows[(size_t)j0]) / (krows[(size_t)j1] - krows[(size_t)j0]);
            for (int k = 0; k < nc; k++, s++) {
                const cf H = Hk[(size_t)j0 * nc + k] * (float)(1 - a) + Hk[(size_t)j1 * nc + k] * (float)a;
                const float h2 = std::norm(H);
                const cf y = Y[(size_t)(r * nc + k)];
                zs[(size_t)s] = h2 > 1e-20f ? y / H : cf(0, 0);
                hs[(size_t)s] = (float)(h2 / sigma2);
            }
        }
        // unique word check, the payload symbols in their order
        std::vector<cf> pz((size_t)g.codedSyms);
        std::vector<float> ph((size_t)g.codedSyms);
        int p = 0, u = 0, uwErr = 0;
        for (int i = 0; i < g.symsPerPacket; i++) {
            if (g.isUw[(size_t)i]) {
                const cf ref = g.uwSyms[(size_t)u++];
                if (bps == 2) uwErr += (std::real(zs[(size_t)i] * std::conj(ref)) < 0) ? 2 : 0;
                else uwErr += std::abs(zs[(size_t)i] - ref) > 0.5f ? 1 : 0;
                continue;
            }
            if (p < g.codedSyms) { pz[(size_t)p] = zs[(size_t)i]; ph[(size_t)p] = hs[(size_t)i]; p++; }
        }
        f.uwErrors = uwErr;
        // deinterleave and soft bits
        std::vector<float> cw((size_t)g.codedBits);
        const cf* q16 = qam16Table();
        const cf rot45 = std::polar(1.f, (float)(kPi / 4));
        for (int i = 0; i < g.codedSyms; i++) {
            const int j = (int)(((int64_t)g.interleaverB * i) % g.codedSyms);
            const cf z = pz[(size_t)j];
            const float snr = ph[(size_t)j];
            if (bps == 2) {
                // gray QPSK: rotate by 45 degrees, the first bit is the sign of the imaginary part, the second of the real part
                const cf r = z * rot45;
                const float scale = 2.f * 1.41421356f * snr;   // 2 a / var with a = 1 / sqrt 2 (equalised), var = 1 / (2 snr) per component
                cw[(size_t)(2 * i)] = clampf(r.imag() * scale, -30, 30);
                cw[(size_t)(2 * i + 1)] = clampf(r.real() * scale, -30, 30);
            } else {
                for (int b = 0; b < 4; b++) {
                    float d0 = 1e30f, d1 = 1e30f;
                    for (int x = 0; x < 16; x++) {
                        const float d = std::norm(z - q16[x]);
                        if ((x >> (3 - b)) & 1) d1 = std::min(d1, d); else d0 = std::min(d0, d);
                    }
                    cw[(size_t)(4 * i + b)] = clampf((d1 - d0) * snr, -30, 30);
                }
            }
        }
        // constellation for the display
        f.constellation.clear();
        for (int i = 0; i < g.codedSyms && i < 400; i++) f.constellation.push_back(pz[(size_t)i]);
        // the full codeword of the code: used information bits, the shortened ones (known 1), parity
        const LdpcCode& code = ldpcCode(m.code);
        std::vector<float> llr((size_t)code.n);
        for (int i = 0; i < g.dataBits; i++) llr[(size_t)i] = cw[(size_t)i];
        for (int i = g.dataBits; i < code.k; i++) llr[(size_t)i] = -50.f;
        for (int i = 0; i < code.m; i++) llr[(size_t)(code.k + i)] = cw[(size_t)(g.dataBits + i)];
        if (!dec[(size_t)g.id]) dec[(size_t)g.id] = std::make_unique<LdpcDecoder>(m.code);
        std::vector<uint8_t> bits((size_t)code.n);
        int iters = 0;
        const bool ok = dec[(size_t)g.id]->decode(llr.data(), opt.maxIter, bits.data(), &iters);
        f.ldpcOk = ok;
        f.ldpcIters = iters;
        int raw = 0;
        for (int i = 0; i < g.dataBits; i++) raw += (cw[(size_t)i] < 0) != (bits[(size_t)i] != 0);
        for (int i = 0; i < code.m; i++) raw += (cw[(size_t)(g.dataBits + i)] < 0) != (bits[(size_t)(code.k + i)] != 0);
        f.rawErrors = raw;
        std::vector<uint8_t> bytes((size_t)g.frameBytes, 0);
        for (int i = 0; i < g.dataBits; i++) if (bits[(size_t)i]) bytes[(size_t)(i >> 3)] |= (uint8_t)(0x80 >> (i & 7));
        f.payload.assign(bytes.begin(), bytes.begin() + g.payloadBytes);
        f.crcRx = (uint16_t)(bytes[(size_t)g.payloadBytes] << 8 | bytes[(size_t)g.payloadBytes + 1]);
        f.crcCalc = frameCrc(bytes.data(), (size_t)g.payloadBytes);
        f.crcPlain = f.crcRx == f.crcCalc;
        return ok;
    }

    // the MFSK receivers get the same samples (the last n pushed, from the ring)
    void mfskFeed(MfskReceiver& r, size_t k, bool on) {
        std::vector<cf> v(k);
        for (size_t i = 0; i < k; i++) v[i] = at(n8 - (int64_t)k + (int64_t)i);
        r.push(v.data(), k);
        for (OfdmFrame& f : r.take()) {
            if (!on) continue;
            if (f.ldpcOk) {
                decodedSpans.push_back({f.startSample, f.endSample});
                (&r == &mfsk1 ? mfsk5 : mfsk1).addDecodedSpan(f.startSample, f.endSample);
                failed.erase(std::remove_if(failed.begin(), failed.end(), [&](const Pending& p) { return overlap(p.start, p.end, f.startSample, f.endSample) > 0.1; }), failed.end());
                out.push_back(std::move(f));
            } else {
                const int64_t s0 = f.startSample, s1 = f.endSample;
                failed.push_back({std::move(f), s0, s1, 30.f, true});   // reported, as the OFDM failures, unless something decodes there
            }
        }
    }

    void push(const cf* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            cf v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf(0, 0);
            ring[(size_t)(n8 & kRingMask)] = v;
            n8++;
            power += std::norm(v);
            nPower++;
        }
        if (nPower >= 4000) { level = (float)(10 * std::log10(power / nPower + 1e-20)); power = 0; nPower = 0; }
        for (Stream& st : streams) { produce(st); correlate(st); }
        mfskFeed(mfsk1, n, (opt.modeMask >> kMfsk) & 1);       // always fed (their sample count stays the ring's), heard only when asked for
        mfskFeed(mfsk5, n, (opt.modeMask >> kMfsk500) & 1);
        service(false);
    }
};

OfdmReceiver::OfdmReceiver(const RxOptions& o) : p_(std::make_unique<Impl>()) { p_->opt = o; p_->reset(); }
OfdmReceiver::~OfdmReceiver() = default;
void OfdmReceiver::reset() { p_->reset(); }
void OfdmReceiver::push(const cf* x, size_t n) {
    // in chunks, so that the acquisition keeps up with the ring
    while (n) {
        const size_t k = std::min<size_t>(n, 2048);
        p_->push(x, k);
        x += k; n -= k;
    }
}
void OfdmReceiver::pushSideband(const cf* x, size_t n) {
    std::vector<cf> y(std::min<size_t>(n, 4096));
    while (n) {
        const size_t k = std::min<size_t>(n, y.size());
        // 1500 / 8000 = 3 / 16: the mixer repeats every 16 samples, counted from the receiver's first sample
        for (size_t i = 0; i < k; i++) y[i] = x[i] * std::polar(1.f, (float)(-2 * kPi * 3.0 * (double)((p_->n8 + (int64_t)i) % 16) / 16.0));
        push(y.data(), k);
        x += k; n -= k;
    }
}
void OfdmReceiver::flush() {
    // run the last samples through the correlators (zeros after the end), then everything that is waiting
    std::vector<cf> z(2048, cf(0, 0));
    for (int i = 0; i < 3; i++) p_->push(z.data(), z.size());
    for (size_t l = 0; l < p_->clusters.size(); l++) if (p_->clusters[l].on) { p_->finishCluster((int)(l / 2), (int)(l % 2), p_->clusters[l]); p_->clusters[l].on = false; }
    p_->service(true);
}
std::vector<OfdmFrame> OfdmReceiver::take() {
    std::vector<OfdmFrame> v;
    v.swap(p_->out);
    std::stable_sort(v.begin(), v.end(), [](const OfdmFrame& a, const OfdmFrame& b) { return a.startSample < b.startSample; });
    return v;
}
int64_t OfdmReceiver::samples() const { return p_->n8; }
float OfdmReceiver::levelDb() const { return p_->level; }
uint64_t OfdmReceiver::detections() const { return p_->nDet + p_->mfsk1.detections() + p_->mfsk5.detections(); }
uint64_t OfdmReceiver::failures() const { return p_->nFail + p_->mfsk1.failures() + p_->mfsk5.failures(); }
void OfdmReceiver::setOptions(const RxOptions& o) { p_->opt = o; }

// ------------------------------------------------------------------------------------------- audio to the receiver's input

AudioToBaseband::AudioToBaseband() {
    taps_ = lowpass(63, 1500.0 / kFs);   // keeps +-1.25 kHz around 1500 Hz (the carriers), removes the image at -3 kHz
    hist_.assign(2 * taps_.size(), cf(0, 0));
}
void AudioToBaseband::reset() { std::fill(hist_.begin(), hist_.end(), cf(0, 0)); pos_ = 0; n_ = 0; }
void AudioToBaseband::process(const float* x, size_t n, std::vector<cf>& out) {
    const int T = (int)taps_.size();
    for (size_t i = 0; i < n; i++) {
        const double ph = -2 * kPi * kMixHz * (double)(n_ % 16) / kFs;   // 1500 / 8000 = 3 / 16: the mixer repeats every 16 samples
        n_++;
        const cf v = x[i] * std::polar(1.f, (float)ph);
        hist_[(size_t)pos_] = v; hist_[(size_t)(pos_ + T)] = v;
        if (++pos_ >= T) pos_ = 0;
        cf acc(0, 0);
        const cf* w = &hist_[(size_t)pos_];
        for (int k = 0; k < T; k++) acc += w[k] * taps_[(size_t)(T - 1 - k)];
        out.push_back(acc * 2.f);   // the analytic signal: twice the positive half
    }
}

} // namespace mercury
} // namespace dect2
