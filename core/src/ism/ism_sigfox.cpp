// Sigfox uplink receiver. See ism_sigfox.h.
//
// Frame layer: sigfox/sigfox_proto.h, from the public Sigfox radio specification (EP-SPECS Rev. 1.3, 2019). The demodulator (narrow carrier
// tracks on a spectrogram, carrier frequency from the squared signal, symbol timing, differential bits) is this file's own.
// Not stated in the public text: which bit value is a phase change in the D-BPSK (both are tried; the frame type and the CRC decide).
// The authentication code is shown as received; the key is secret, so it is never checked.
#include "dect2/ism/ism_sigfox.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/portable_rng.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <ctime>
#include <random>
#include "dect2/common/crc.h"
#include "dect2/common/mathconst.h"
#include "dect2/sigfox/sigfox_proto.h"

namespace dect2 {

namespace {
constexpr double kBandHz = 98e3;         // the watched span: +-98 kHz around the dial
constexpr int kAvg = 8;                  // spectrogram frames averaged
constexpr double kThr = 3.1;             // carrier threshold over the median of the averaged spectrum
constexpr double kTrackHz = 350;         // peaks within this of a track belong to it
constexpr double kMinDurSec = 0.1;
constexpr double kMaxDurSec = 3.2;
constexpr double kGapSec = 0.15;     // an established track (kSureSec on the air) ends after this long without its peak
constexpr double kSureSec = 0.3;
constexpr double kRingSec = 3.8;
constexpr int kPreambleBits = sigfox::kPreambleBits;
using dect2::math::kPi;

std::string hexOf(uint64_t v, int digits) {
    char b[24];
    snprintf(b, sizeof b, "%0*llX", digits, (unsigned long long)v);
    return b;
}
}

// ------------------------------------------------------------------ test signal

std::vector<uint8_t> sigfoxFrameBits(uint32_t id, uint32_t seq, const std::vector<uint8_t>& payload, bool bf, int rank) {
    return sigfox::buildFrame(id, seq & 0xFFF, payload, rank, bf);
}

void sigfoxAddBurst(cf32* x, size_t n, int64_t first, const SigfoxTx& t, double rate, double freqHz, double amp) {
    const std::vector<uint8_t> bits = sigfox::buildFrame(t.id, t.seq & 0xFFF, t.payload, t.rank, t.downlinkRequest, t.singleBit, t.bitValue);
    std::vector<int> s(bits.size() + 1, 1);   // one reference symbol, then a phase inversion for each 0 bit
    for (size_t i = 0; i < bits.size(); i++) s[i + 1] = bits[i] ? s[i] : -s[i];
    const int64_t i0 = (int64_t)std::llround(t.startSec * rate), cnt = (int64_t)((double)s.size() / t.baud * rate);
    const int64_t k0 = std::max<int64_t>(0, first - i0), k1 = std::min<int64_t>(cnt, first + (int64_t)n - i0);
    for (int64_t k = k0; k < k1; k++) {
        const double tt = (double)k / rate;
        const double pos = tt * t.baud;
        const size_t sy = std::min(s.size() - 1, (size_t)pos);
        const double fr = pos - (double)sy;
        double a = s[sy];
        if (sy > 0 && fr < 0.1) { const double q = 0.5 - 0.5 * std::cos(kPi * fr / 0.1); a = s[sy - 1] * (1 - q) + s[sy] * q; }
        const double ph = 2 * kPi * (freqHz * ((double)(i0 + k) / rate) + 0.5 * t.driftHzPerSec * tt * tt);
        x[i0 + k - first] += cf32((float)(amp * a * std::cos(ph)), (float)(amp * a * std::sin(ph)));
    }
}

std::vector<cf32> sigfoxSynth(double rate, double dialOff, double seconds, double snrDb, double cfoHz, const std::vector<SigfoxTx>& txs, uint32_t seed) {
    const size_t n = (size_t)(seconds * rate);
    std::vector<cf32> x(n);
    PortableRng rng(seed);   // the same noise on every platform
    for (auto& v : x) v = cf32((float)rng.gauss(0.1), (float)rng.gauss(0.1));
    const double n0 = 2 * 0.1 * 0.1 / rate;   // noise power per Hz
    for (const auto& t : txs) {
        const double amp = std::sqrt(std::pow(10.0, snrDb / 10.0) * 100.0 * n0) * std::pow(10.0, t.levelDb / 20.0);
        sigfoxAddBurst(x.data(), n, 0, t, rate, dialOff + t.offsetHz + cfoHz, amp);
    }
    return x;
}

// ------------------------------------------------------------------ receiver

struct SigfoxRx::Impl {
    double rate = 0, dialOff = 0, dialHz = 0;
    bool on = false;
    // front end
    int D1 = 1;
    double fs1 = 0;
    std::vector<float> taps;
    std::complex<double> osc{1, 0}, rot{1, 0};
    std::vector<cf32> pend;
    size_t pendPos = 0;
    // 400 kHz ring and spectrogram
    std::vector<cf32> ring;
    uint64_t total1 = 0;
    int N = 4096, hop = 2048, nb = 0, kLo = 0;
    double binHz = 100;
    std::unique_ptr<Fft> fft;
    std::vector<float> win;
    std::vector<cf32> fb;
    std::vector<std::vector<float>> hist;
    std::vector<float> sum, E, srt;
    int hpos = 0;
    uint64_t nframes = 0;
    std::vector<float> wfAcc;
    std::vector<int> colOf;                     // the waterfall column of each bin
    int wfFrames = 0, rowFrames = 20;
    struct Track { double f = 0; uint64_t start = 0, last = 0; float peak = 0; bool dead = false; };
    std::vector<Track> tracks;
    SigfoxTelemetry tel;
    uint64_t seqNo = 0;
    struct Rep { double sec; };
    std::vector<std::vector<double>> repSecs;   // per message (parallel to tel.messages): the times of its repetitions (parallel to freqs)

    void configure(double r, double off, double dial, bool anyBand) {
        rate = r; dialOff = off; dialHz = dial;
        on = (anyBand || SigfoxRx::onSigfoxBand(dial)) && r >= 4e5;
        tel = SigfoxTelemetry();
        tel.active = on;
        repSecs.clear();
        tracks.clear();
        pend.clear(); pendPos = 0;
        total1 = 0; nframes = 0; hpos = 0; wfFrames = 0;
        if (!on) return;
        D1 = std::max(1, (int)std::floor(r / 400e3));
        fs1 = r / D1;
        const double fc = 108e3;
        const int nt = std::max(15, ((int)std::ceil(3.3 * r / std::max(50e3, fs1 - 2 * fc))) | 1);
        taps.resize(nt);
        double s = 0;
        for (int i = 0; i < nt; i++) {
            const double m = i - (nt - 1) / 2.0, f = fc / r;
            const double h = m == 0 ? 2 * f : std::sin(2 * kPi * f * m) / (kPi * m);
            taps[i] = (float)(h * (0.54 - 0.46 * std::cos(2 * kPi * i / (nt - 1))));
            s += taps[i];
        }
        for (auto& t : taps) t = (float)(t / s);
        osc = {1, 0};
        rot = std::polar(1.0, -2 * kPi * off / r);
        ring.assign((size_t)(kRingSec * fs1), cf32(0, 0));
        N = 256;
        while (N < 65536 && fs1 / (N * 2) >= 80.0) N *= 2;
        hop = N / 2;
        binHz = fs1 / N;
        fft = std::make_unique<Fft>(N);
        win.resize(N);
        for (int i = 0; i < N; i++) win[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / N));
        fb.assign(N, cf32(0, 0));
        const int half = (int)(kBandHz / binHz);
        nb = 2 * half + 1;
        kLo = -half;
        hist.assign(kAvg, std::vector<float>(nb, 0.f));
        sum.assign(nb, 0.f); E.assign(nb, 0.f);
        wfAcc.assign(kSigfoxCols, 0.f);
        colOf.resize(nb);
        for (int j = 0; j < nb; j++) colOf[j] = (int)((int64_t)j * kSigfoxCols / nb);
        rowFrames = std::max(1, (int)std::lround(0.1 * fs1 / hop));
        tel.cols = kSigfoxCols;
    }

    void feed(const cf32* x, size_t n) {
        if (!on) return;
        const size_t nt = taps.size();
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min<size_t>(4096, n - i);
            for (size_t k = 0; k < m; k++) {
                cf32 v = x[i + k];
                if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
                osc *= rot;
                pend.push_back(v * cf32((float)osc.real(), (float)osc.imag()));
            }
            osc /= std::abs(osc);
            i += m;
            while (pendPos + nt <= pend.size()) {
                float re = 0, im = 0;
                const cf32* p = &pend[pendPos];
                for (size_t t = 0; t < nt; t++) { re += taps[t] * p[t].real(); im += taps[t] * p[t].imag(); }
                pendPos += D1;
                push1(cf32(re, im));
            }
            if (pendPos > 0) { pend.erase(pend.begin(), pend.begin() + (ptrdiff_t)pendPos); pendPos = 0; }
        }
    }

    void push1(cf32 v) {
        ring[total1 % ring.size()] = v;
        total1++;
        if (total1 % hop == 0 && total1 >= (uint64_t)N) frame();
    }

    double sec(uint64_t s1) const { return (double)s1 / fs1; }

    void frame() {
        const uint64_t base = total1 - N;
        {   // the last N samples of the ring, in two runs (no division per sample)
            const size_t R = ring.size(), b0 = (size_t)(base % R), n0 = std::min((size_t)N, R - b0);
            for (size_t i = 0; i < n0; i++) fb[i] = ring[b0 + i] * win[i];
            for (size_t i = n0; i < (size_t)N; i++) fb[i] = ring[i - n0] * win[i];
        }
        fft->forward(fb.data());
        std::vector<float>& h = hist[hpos];
        for (int j = 0; j < nb; j++) {
            const int k = kLo + j < 0 ? kLo + j + N : kLo + j;   // (kLo + j + N) % N, kLo > -N
            const float p = std::norm(fb[k]);
            sum[j] += p - h[j];
            h[j] = p;
        }
        hpos = (hpos + 1) % kAvg;
        nframes++;
        if (nframes < (uint64_t)kAvg + 1) return;
        for (int j = 0; j < nb; j++) E[j] = std::max(0.f, sum[j]) / kAvg;
        srt = E;
        std::nth_element(srt.begin(), srt.begin() + nb / 2, srt.end());
        const float noise = std::max(1e-20f, srt[nb / 2]);
        const float thr = (float)(kThr * noise);
        // waterfall
        for (int j = 0; j < nb; j++) {
            const int c = colOf[j];
            wfAcc[c] = std::max(wfAcc[c], E[j] / noise);
        }
        if (++wfFrames >= rowFrames) {
            wfFrames = 0;
            for (int c = 0; c < kSigfoxCols; c++) {
                const double db = 10 * std::log10(std::max(1e-3f, wfAcc[c]));
                tel.rows.push_back((uint8_t)std::max(0.0, std::min(255.0, db * 6.0)));
                wfAcc[c] = 0;
            }
            tel.rowCount++;
            if (tel.rows.size() > kSigfoxRowsMax * kSigfoxCols) tel.rows.erase(tel.rows.begin(), tel.rows.begin() + kSigfoxCols);
        }
        // narrow carriers
        std::vector<char> hit(tracks.size(), 0);
        for (int j = 1; j + 1 < nb; j++) {
            if (!(E[j] > thr && E[j] >= E[j - 1] && E[j] >= E[j + 1])) continue;
            const double a = std::log(E[j - 1] + 1e-30), b = std::log(E[j] + 1e-30), c = std::log(E[j + 1] + 1e-30);
            const double den = a - 2 * b + c;
            const double d = den < -1e-9 ? 0.5 * (a - c) / den : 0.0;
            const double f = (kLo + j + std::max(-1.0, std::min(1.0, d))) * binHz;
            int ti = -1;
            for (size_t t = 0; t < tracks.size(); t++) if (std::fabs(tracks[t].f - f) <= kTrackHz) { ti = (int)t; break; }
            if (ti < 0) {
                Track t;
                t.f = f; t.peak = E[j];
                t.last = total1;
                t.start = total1 > (uint64_t)(N + (kAvg + 2) * hop) ? total1 - (uint64_t)(N + (kAvg + 2) * hop) : 0;
                tracks.push_back(t);
                hit.push_back(1);
            } else {
                Track& t = tracks[ti];
                if (E[j] >= 0.7f * t.peak) t.f = 0.5 * t.f + 0.5 * f;
                t.peak = std::max(t.peak * 0.995f, E[j]);
                t.last = total1;
                hit[ti] = 1;
            }
        }
        for (size_t t = 0; t < tracks.size();) {
            Track& tr = tracks[t];
            if (sec(tr.last - tr.start) > kMaxDurSec) tr.dead = true;
            // a weak carrier drops under the threshold now and then (its spectrum moves with the bits): once it has been there a while only
            // a longer gap ends it; a young track (most often a noise peak) goes at once
            const double gap = sec(tr.last - tr.start) >= kSureSec ? std::max(4.0 * hop, kGapSec * fs1) : 4.0 * hop;
            if (total1 - tr.last > (uint64_t)gap) {
                if (!tr.dead && sec(tr.last - tr.start) >= kMinDurSec) finish(tr);
                tracks.erase(tracks.begin() + (ptrdiff_t)t);
            } else t++;
        }
    }

    // ---------------------------------------------------------------- one carrier

    void finish(const Track& tr) {
        tel.carriers++;
        const uint64_t pad = (uint64_t)(0.06 * fs1);
        uint64_t s0 = tr.start > pad ? tr.start - pad : 0, s1 = std::min(total1, tr.last + pad);
        if (total1 > ring.size() && s0 < total1 - ring.size()) s0 = total1 - ring.size();
        if (s1 <= s0 + 1000) return;
        std::vector<cf32> seg(s1 - s0);
        for (uint64_t i = s0; i < s1; i++) seg[i - s0] = ring[i % ring.size()];
        // the mix to the coarse frequency and its running sum: the same for both bit rates, worked out once
        std::vector<std::complex<double>> C;
        mixSum(seg, tr.f, C);
        for (int baud : {100, 600}) {
            SigfoxMessage m;
            double f = 0;
            if (demod(C, tr.f, baud, m, f)) {
                m.sec = sec(s0);
                report(m, f);
                return;
            }
        }
    }

    // seg mixed to the coarse frequency f0, as a cumulative sum (C[i]: the sum of the first i samples)
    void mixSum(const std::vector<cf32>& seg, double f0, std::vector<std::complex<double>>& C) const {
        const size_t L = seg.size();
        C.resize(L + 1);
        std::complex<double> o(1, 0), r = std::polar(1.0, -2 * kPi * f0 / fs1), acc(0, 0);
        C[0] = 0;
        for (size_t i = 0; i < L; i++) {
            o *= r;
            if ((i & 1023) == 1023) o /= std::abs(o);
            acc += std::complex<double>(seg[i].real(), seg[i].imag()) * o;
            C[i + 1] = acc;
        }
    }

    // C: mixSum of the carrier's samples
    bool demod(const std::vector<std::complex<double>>& C, double f0, int baud, SigfoxMessage& msg, double& fCarrier) {
        const size_t L = C.size() - 1;
        const double R1 = 20.0 * baud, step = fs1 / R1;
        const size_t nOut = (size_t)((double)L / step);
        if (nOut < 20 * 40) return false;
        // cut to 20 samples per symbol with a moving average (the cumulative sum, fractional edges)
        auto at = [&](double t) { const size_t i = std::min(L - 1, (size_t)t); const double fr = t - (double)i; return C[i] + (C[i + 1] - C[i]) * fr; };
        std::vector<std::complex<double>> y(nOut);
        for (size_t k = 0; k < nOut; k++) y[k] = (at((double)(k + 1) * step) - at((double)k * step)) / step;
        // fine frequency: the square of a BPSK signal is a line at twice the carrier
        int NF = 4096;
        while ((size_t)NF < nOut * 4) NF *= 2;
        Fft fz(NF);
        std::vector<cf32> z(NF, cf32(0, 0));
        for (size_t k = 0; k < nOut; k++) {
            const std::complex<double> q = y[k] * y[k] * (0.5 - 0.5 * std::cos(2 * kPi * ((double)k + 0.5) / (double)nOut));
            z[k] = cf32((float)q.real(), (float)q.imag());
        }
        fz.forward(z.data());
        const double df = R1 / NF, range = 0.12 * R1 * 2;
        const int kMax = (int)(range / df);
        std::vector<float> mag(2 * kMax + 1);
        int kb = 0;
        for (int d = -kMax; d <= kMax; d++) {
            mag[d + kMax] = std::norm(z[(d + NF) % NF]);
            if (mag[d + kMax] > mag[kb + kMax]) kb = d;
        }
        double fr = 0;
        {
            std::vector<float> s = mag;
            std::nth_element(s.begin(), s.begin() + (ptrdiff_t)s.size() / 2, s.end());
            if (mag[kb + kMax] > 25.f * s[s.size() / 2] && kb > -kMax && kb < kMax) {
                const double a = std::log(mag[kb + kMax - 1] + 1e-30), b = std::log(mag[kb + kMax] + 1e-30), c = std::log(mag[kb + kMax + 1] + 1e-30);
                const double den = a - 2 * b + c;
                const double d = den < -1e-9 ? 0.5 * (a - c) / den : 0.0;
                fr = (kb + std::max(-1.0, std::min(1.0, d))) * df / 2;
            }
        }
        fCarrier = f0 + fr;
        for (size_t k = 0; k < nOut; k++) y[k] *= std::polar(1.0, -2 * kPi * fr * (double)k / R1);
        // symbol timing: the alignment where the sums over a symbol are strongest
        const int sps = 20;
        int bp = 0;
        double bm = -1;
        for (int p = 0; p < sps; p++) {
            double m = 0;
            for (size_t k = (size_t)p; k + sps <= nOut; k += sps) {
                std::complex<double> s = 0;
                for (int i = 0; i < sps; i++) s += y[k + i];
                m += std::norm(s);
            }
            if (m > bm) { bm = m; bp = p; }
        }
        std::vector<std::complex<double>> S;
        for (size_t k = (size_t)bp; k + sps <= nOut; k += sps) {
            std::complex<double> s = 0;
            for (int i = 0; i < sps; i++) s += y[k + i];
            S.push_back(s / (double)sps);
        }
        const int ns = (int)S.size();
        if (ns < 40) return false;
        std::vector<double> pw(ns);
        for (int i = 0; i < ns; i++) pw[i] = std::norm(S[i]);
        std::vector<double> sp = pw;
        std::nth_element(sp.begin(), sp.begin() + (ptrdiff_t)(sp.size() * 6 / 10), sp.end());
        const double ref = sp[sp.size() * 6 / 10], gate = 0.3 * ref;
        int n0 = -1, n1 = -1;
        for (int i = 0; i + 1 < ns; i++) if (pw[i] > gate && pw[i + 1] > gate) { n0 = i; break; }
        for (int i = ns - 1; i > 0; i--) if (pw[i] > gate && pw[i - 1] > gate) { n1 = i; break; }
        if (n0 < 0 || n1 <= n0 + kPreambleBits) return false;
        // differential products, turned back by the local phase step per symbol: a carrier that drifts during the burst (the specification allows
        // tens of Hz per second, and a residual offset adds to it) turns each product a little; the square of the products removes the
        // modulation, and its average over 31 symbols gives that turn (within +-90 degrees per symbol)
        std::vector<std::complex<double>> dp(ns - 1), d2(ns - 1);
        for (int i = 0; i + 1 < ns; i++) { dp[i] = S[i + 1] * std::conj(S[i]); d2[i] = dp[i] * dp[i] / (std::abs(dp[i]) + 1e-30); }
        std::vector<std::complex<double>> c2(ns);
        c2[0] = 0;
        for (int i = 0; i + 1 < ns; i++) c2[i + 1] = c2[i] + d2[i];
        std::vector<uint8_t> b(ns - 1);
        for (int i = 0; i + 1 < ns; i++) {
            const int lo = std::max(0, i - 15), hi = std::min(ns - 1, i + 16);
            const std::complex<double> w = c2[hi] - c2[lo];
            const double turn = 0.5 * std::arg(w);
            b[i] = (dp[i] * std::polar(1.0, -turn)).real() > 0 ? 1 : 0;   // 1: no phase change (or the other way: both tried)
        }
        // the preamble (alternating bits) starts at the power step, give or take a few symbols: as it repeats every two bits, more than one
        // start fits it, and the frame type and the CRC decide
        sigfox::Uplink u;
        bool found = false;
        for (int pol = 0; pol < 2 && !found; pol++) {
            std::vector<uint8_t> bb = b;
            if (pol) for (auto& v : bb) v ^= 1;
            for (int sh = -2; sh <= 5 && !found; sh++) {
                const int st = n0 + sh;
                if (st < 0 || st + kPreambleBits > (int)bb.size()) continue;
                int ok = 0;
                for (int j = 0; j < kPreambleBits; j++) ok += bb[st + j] == (uint8_t)(j % 2 == 0);
                if (ok < 17) continue;
                if (sigfox::parseFrame(bb, (size_t)st + kPreambleBits, u) && st + kPreambleBits + sigfox::frameBitsAfterPreamble(u.container) <= n1 + 4) found = true;
            }
        }
        if (!found) return false;
        msg = SigfoxMessage();
        msg.baud = baud;
        msg.len = (int)u.payload.size();
        msg.frameType = u.ft;
        msg.li = u.li;
        msg.repeatFlag = u.rep;
        msg.downlinkRequest = u.downlinkRequest;
        msg.singleBit = u.singleBit; msg.bitValue = u.bitValue;
        msg.control = u.control;
        msg.container = u.container;
        msg.rankMask = 1 << (u.rank - 1);
        msg.seq = u.mc;
        msg.id = sigfox::idHex(u.id);
        msg.payload = sigfox::hexOf(u.payload);
        msg.mac = sigfox::hexOf(u.auth);
        msg.crc = hexOf(u.crc, 4);
        double acc = 0;
        for (int i = n0; i <= n1; i++) acc += pw[i];
        msg.levelDb = (float)(10 * std::log10(std::max(1e-30, acc / (n1 - n0 + 1))));
        return true;
    }

    void report(SigfoxMessage m, double fRel) {
        const int64_t now = (int64_t)std::time(nullptr);
        const double fAbs = dialHz > 0 ? dialHz + fRel : fRel;
        tel.frames++;
        for (size_t i = 0; i < tel.messages.size(); i++) {
            SigfoxMessage& q = tel.messages[i];
            if (q.id != m.id || q.seq != m.seq || q.payload != m.payload) continue;
            // the repetitions come within a few seconds of each other, but not always decoded in time order: carriers on other
            // frequencies are finished at different moments
            std::vector<double>& ts = repSecs[i];
            bool nearBy = false, same = false;   // same: one transmission seen twice (a wide 600 bit/s signal gives two peaks)
            for (size_t j = 0; j < ts.size(); j++) {
                nearBy = nearBy || std::fabs(m.sec - ts[j]) <= 20.0;
                same = same || (std::fabs(q.freqs[j] - fAbs) < 2e3 && std::fabs(m.sec - ts[j]) < 0.5);
            }
            if (!nearBy) continue;
            if (!same) { q.reps++; q.freqs.push_back(fAbs); ts.push_back(m.sec); q.sec = std::min(q.sec, m.sec); }
            q.rankMask |= m.rankMask;
            q.wallTime = now;
            q.levelDb = std::max(q.levelDb, m.levelDb);
            seqNo++;
            std::rotate(tel.messages.begin(), tel.messages.begin() + (ptrdiff_t)i, tel.messages.begin() + (ptrdiff_t)i + 1);
            std::rotate(repSecs.begin(), repSecs.begin() + (ptrdiff_t)i, repSecs.begin() + (ptrdiff_t)i + 1);
            return;
        }
        m.wallTime = m.firstWall = now;
        m.reps = 1;
        m.freqs = {fAbs};
        tel.messages.insert(tel.messages.begin(), m);
        repSecs.insert(repSecs.begin(), std::vector<double>{m.sec});
        if (tel.messages.size() > kSigfoxMsgMax) { tel.messages.pop_back(); repSecs.pop_back(); }
        seqNo++;
    }
};

SigfoxRx::SigfoxRx() : p_(std::make_unique<Impl>()) {}
SigfoxRx::~SigfoxRx() = default;
void SigfoxRx::configure(double r, double off, double dial, bool anyBand) { p_->configure(r, off, dial, anyBand); }
void SigfoxRx::feed(const cf32* x, size_t n) { p_->feed(x, n); }
const SigfoxTelemetry& SigfoxRx::telemetry() const { return p_->tel; }
void SigfoxRx::clear() { p_->tel.messages.clear(); p_->repSecs.clear(); p_->tel.carriers = p_->tel.frames = 0; }

bool SigfoxRx::onSigfoxBand(double dialHz) {
    if (dialHz <= 0) return true;
    int nz = 0;
    const sigfox::Zone* z = sigfox::zones(nz);
    for (int i = 0; i < nz; i++) if (std::fabs(dialHz - z[i].ulMhz * 1e6) < 0.15e6) return true;
    return false;
}

} // namespace dect2
