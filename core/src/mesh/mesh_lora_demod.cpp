// LoRa demodulator: preamble search on de-chirped FFTs, sync word and SFD alignment, fractional carrier and timing estimates from the
// preamble and the down-chirps, symbol demodulation with a timing loop for clock drift, and the coding chain of mesh_lora_code.cpp.
// The method follows gr-lora_sdr's frame_sync_impl.cc and fft_demod_impl.cc and the paper named in mesh_lora.h; the code is ours.
// STOP: a change here also needs a run of the full mesh rates test, which ctest does not run: test_mesh_rates --full
// (tests/mesh/test_mesh_rates.cpp).
#include "dect2/mesh/mesh_lora.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"
#include "dect2/common/window.h"

namespace dect2 {
namespace lora {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;

double besselI0(double x) { return dect2::window::besselI0(x, 60, 0.0); }
// ln I0(x): the log-likelihood of a noncoherently detected bin (Abramowitz & Stegun 9.8.1, 9.8.2)
float lnI0(float x) {
    if (x < 3.75f) {
        const float t = x / 3.75f, t2 = t * t;
        return std::log(1.f + t2 * (3.5156229f + t2 * (3.0899424f + t2 * (1.2067492f + t2 * (0.2659732f + t2 * (0.0360768f + t2 * 0.0045813f))))));
    }
    return x - 0.5f * std::log(6.2831853f * x) + 0.125f / x;
}
double kaiserBeta(double a) { return a > 50 ? 0.1102 * (a - 8.7) : a > 21 ? 0.5842 * std::pow(a - 21, 0.4) + 0.07886 * (a - 21) : 0; }
} // namespace

// ------------------------------------------------------------------ channel buffer and channelizer

void ChanBuf::trim(int64_t keepFrom) {
    if (keepFrom <= base_) return;
    const int64_t drop = std::min<int64_t>(keepFrom - base_, (int64_t)x_.size());
    // move rarely: only when the dead part is large
    if (drop < (int64_t)(1 << 15) && drop * 4 < (int64_t)x_.size()) return;
    x_.erase(x_.begin(), x_.begin() + drop);
    base_ += drop;
}

namespace {
constexpr int kMixBlock = 4096;
constexpr int kMinPreWins = 4;   // windows a preamble must hold before the sync word is looked for
}

namespace {
std::vector<float> kaiserLowpass(double fp, double fs, double rate, double att) {
    fs = std::min(fs, 0.499 * rate);
    const double fc = 0.5 * (fp + fs) / rate, df = std::max((fs - fp) / rate, 1e-4);
    int n = (int)std::ceil((att - 7.95) / (14.36 * df)) + 1;
    n = std::min(4001, std::max(9, n)) | 1;
    std::vector<float> h(n);
    const double beta = kaiserBeta(att), m = (n - 1) / 2.0;
    double sum = 0;
    std::vector<double> v(n);
    for (int i = 0; i < n; i++) {
        const double x = i - m;
        const double s = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double r = x / (m + 0.5);
        v[i] = s * besselI0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / besselI0(beta);
        sum += v[i];
    }
    for (int i = 0; i < n; i++) h[i] = (float)(v[i] / sum);
    return h;
}
} // namespace

int Channelizer::decimFor(double inRate, double bwHz) { return std::max(1, (int)std::floor(inRate / (kMinRatio * bwHz) + 1e-9)); }

Channelizer::Channelizer(double inRate, double offsetHz, double bwHz) : inRate_(inRate), offsetHz_(offsetHz), bwHz_(bwHz) {
    decim_ = decimFor(inRate, bwHz);
    outRate_ = inRate / decim_;
    // Two stages. The second, at the channel rate, passes the LoRa band plus room for a carrier error and stops 0.3 bw further
    // (noise beyond would fold in when the search samples at the chip rate). The first only keeps the decimation from folding
    // anything onto the second's band.
    const double margin = std::min(25e3, 0.4 * bwHz);
    const double fp = 0.5 * bwHz + margin, fs2 = fp + 0.3 * bwHz;
    taps2_ = kaiserLowpass(fp, fs2, outRate_, 50);
    taps_ = kaiserLowpass(fp, std::max(fs2, outRate_ - fs2), inRate, 60);
    reset();
}

void Channelizer::reset() {
    nIn_ = 0;
    re_.assign(taps_.size() - 1, 0.f);
    im_.assign(taps_.size() - 1, 0.f);
    re2_.assign(taps2_.size() - 1, 0.f);
    im2_.assign(taps2_.size() - 1, 0.f);
}

void Channelizer::process(const cf32* x, size_t n, ChanBuf& buf) {
    const double w = -2 * kPi * offsetHz_ / inRate_;
    size_t i = 0;
    while (i < n) {
        // the mixer's phase restarts exactly at every block of kMixBlock input samples, so chunking does not matter
        const int64_t blk = nIn_ / kMixBlock, off = nIn_ % kMixBlock;
        const size_t run = std::min(n - i, (size_t)(kMixBlock - off));
        const double ph0 = std::fmod(w * (double)(blk * kMixBlock), 2 * kPi);
        const size_t r0 = re_.size();
        re_.resize(r0 + run); im_.resize(r0 + run);
        // the phasor of sample j in the block is P(block) x S[j / 16] x T[j % 16], tables of float phasors (vectorisable, no recurrence)
        if (tr_.empty()) {
            tr_.resize(16); ti_.resize(16); sr_.resize(kMixBlock / 16); si_.resize(kMixBlock / 16);
            for (int k = 0; k < 16; k++) { const cd t = std::polar(1.0, w * k); tr_[(size_t)k] = (float)t.real(); ti_[(size_t)k] = (float)t.imag(); }
            for (int g = 0; g < kMixBlock / 16; g++) { const cd t = std::polar(1.0, w * 16.0 * g); sr_[(size_t)g] = (float)t.real(); si_[(size_t)g] = (float)t.imag(); }
        }
        const cd pb = std::polar(1.0, ph0);
        const float pbr = (float)pb.real(), pbi = (float)pb.imag();
        float* orr = re_.data() + r0; float* oii = im_.data() + r0;
        const cf32* xs = x + i;
        for (size_t j0 = (size_t)off, k = 0; k < run;) {
            const size_t g = j0 >> 4, l0 = j0 & 15, cnt = std::min<size_t>(16 - l0, run - k);
            const float gr = pbr * sr_[g] - pbi * si_[g], gi = pbr * si_[g] + pbi * sr_[g];
            for (size_t l = 0; l < cnt; l++) {
                const float zr = gr * tr_[l0 + l] - gi * ti_[l0 + l], zi = gr * ti_[l0 + l] + gi * tr_[l0 + l];
                const float xr = xs[k + l].real(), xi = xs[k + l].imag();
                orr[k + l] = xr * zr - xi * zi;
                oii[k + l] = xr * zi + xi * zr;
            }
            k += cnt; j0 += cnt;
        }
        i += run;
        nIn_ += (int64_t)run;
    }
    const int p = (int)taps_.size();
    if ((int)re_.size() < p) return;
    const int nOut = ((int)re_.size() - p) / decim_ + 1;
    ore_.resize(nOut); oim_.resize(nOut);
    desamp(re_.data(), decim_, taps_.data(), ore_.data(), nOut, p);
    desamp(im_.data(), decim_, taps_.data(), oim_.data(), nOut, p);
    re2_.insert(re2_.end(), ore_.begin(), ore_.end());
    im2_.insert(im2_.end(), oim_.begin(), oim_.end());
    const int p2 = (int)taps2_.size();
    const int n2 = (int)re2_.size() - p2 + 1;
    if (n2 > 0) {
        ore_.resize(n2); oim_.resize(n2);
        desamp(re2_.data(), 1, taps2_.data(), ore_.data(), n2, p2);
        desamp(im2_.data(), 1, taps2_.data(), oim_.data(), n2, p2);
        tmp_.resize(n2);
        for (int k = 0; k < n2; k++) tmp_[k] = cf32(ore_[k], oim_[k]);
        buf.append(tmp_.data(), (size_t)n2);
        re2_.erase(re2_.begin(), re2_.begin() + n2);
        im2_.erase(im2_.begin(), im2_.begin() + n2);
    }
    const size_t used = (size_t)nOut * decim_;
    re_.erase(re_.begin(), re_.begin() + used);
    im_.erase(im_.begin(), im_.begin() + used);
}

// ------------------------------------------------------------------ chip stream

namespace {
const float* cubicTable() {
    static const std::vector<float> t = [] {
        std::vector<float> v(257 * 4);
        for (int r = 0; r <= 256; r++) {
            const float f = (float)r / 256.f;
            v[(size_t)r * 4 + 0] = -f * (f - 1) * (f - 2) / 6; v[(size_t)r * 4 + 1] = (f + 1) * (f - 1) * (f - 2) / 2;
            v[(size_t)r * 4 + 2] = -(f + 1) * f * (f - 2) / 2; v[(size_t)r * 4 + 3] = (f + 1) * f * (f - 1) / 6;
        }
        return v;
    }();
    return t.data();
}
} // namespace

void ChipStream::extend(const ChanBuf& b) {
    // chip m needs samples floor(m rho) - 1 .. floor(m rho) + 2
    int64_t m = end();
    if (re_.empty()) { const int64_t first = (int64_t)std::ceil(((double)b.begin() + 2) / rho_); if (first > m) { base_ = first; m = first; } }
    else if ((double)m * rho_ - 2 < (double)b.begin()) {            // the buffer dropped what the next chip needs: restart there
        re_.clear(); im_.clear();
        m = base_ = (int64_t)std::ceil(((double)b.begin() + 2) / rho_);
    }
    const double lastPos = (double)b.end() - 4;
    if ((double)m * rho_ > lastPos) return;
    const int64_t mEnd = (int64_t)std::floor(lastPos / rho_);
    if (mEnd < m) return;
    const size_t n = (size_t)(mEnd - m + 1), r0 = re_.size();
    re_.resize(r0 + n); im_.resize(r0 + n);
    const float* tab = cubicTable();
    for (size_t i = 0; i < n; i++) {
        const double p = (double)(m + (int64_t)i) * rho_;
        const int64_t n0 = (int64_t)p;
        const int r = (int)((p - (double)n0) * 256.0 + 0.5);
        const float* w = tab + (size_t)r * 4;
        const cf32* x = b.at(n0 - 1);
        re_[r0 + i] = w[0] * x[0].real() + w[1] * x[1].real() + w[2] * x[2].real() + w[3] * x[3].real();
        im_[r0 + i] = w[0] * x[0].imag() + w[1] * x[1].imag() + w[2] * x[2].imag() + w[3] * x[3].imag();
    }
}

void ChipStream::trimToSample(int64_t keepSample) {
    const int64_t keepChip = (int64_t)std::floor((double)keepSample / rho_);
    if (keepChip <= base_) return;
    const int64_t drop = std::min<int64_t>(keepChip - base_, (int64_t)re_.size());
    if (drop < (1 << 14) && drop * 4 < (int64_t)re_.size()) return;
    re_.erase(re_.begin(), re_.begin() + drop);
    im_.erase(im_.begin(), im_.begin() + drop);
    base_ += drop;
}

// ------------------------------------------------------------------ demodulator

struct Demod::Impl {
    Params p;
    int sf, N;
    double fs, rho;              // channel rate, channel samples per chip
    std::vector<float> dnRe, dnIm;   // conj(up-chirp): multiplying an up-chirp by it leaves a tone
    std::vector<float> re, im, pw, yRe, yIm;   // yRe/yIm: the last de-chirped symbol before its FFT
    // accurate interpolator: windowed sinc with its -6 dB point at bw / 2, kP phases of kK taps
    static constexpr int kP = 64;
    int K = 0;
    std::vector<float> sincTab;
    std::vector<cf32> mixed;
    std::shared_ptr<ChipStream> cs;   // the chip-rate stream the search reads
    bool ownCs = true;
    int stride = 1;              // search windows: every stride-th (a preamble of 8 symbols has enough for 3 of them at stride 2)
    int64_t ochip = 0;           // chip index of the search grid's origin (origin = ochip x rho)
    DemodStats st;

    enum State { Search, Preamble, Data } state = Search;
    double origin = 0;           // channel sample of chip 0 of the search grid
    int64_t win = 0;             // next window of the grid
    std::vector<std::vector<float>> hist;   // power spectra of the last 3 windows (normalised to their mean)
    std::vector<float> s3;       // sum of the three spectra in hist (kept incrementally while searching)
    std::vector<float> preAcc;   // sum of the preamble's spectra
    int histN = 0;
    int kUp = 0;
    float preLevel = 0;          // the preamble's level in its bins (smoothed)
    int64_t preFirst = 0, preEnd = -1;
    // frame
    double posS = 0, perS = 0, chipS = 0;   // next symbol (channel samples), symbol and chip lengths
    double cfoBins = 0, drift = 0;
    double theta0 = 0, lastAdj = 0; bool thetaOk = false;
    double cfoStartBins = 0;
    double cfoRate = 0;          // cfoTrack > 0: the carrier offset's change per symbol, bins (a satellite's Doppler rate)
    double loopInt = 0;
    double posS0 = 0;              // the data start (a frame that fails its CRC is demodulated again from here)
    int retried = 0;
    double snrLin = 0, levelLin = 0, sigPw = 0;   // sigPw: a preamble peak's power in the spectrum's units (soft decoding)
    double frameStart = 0;
    uint8_t syncSeen = 0;
    FrameDecoder fd;
    void startFd() { if (p.implicitHdr) fd.startImplicit(sf, p.ldro, p.implicitLen, p.cr, p.crc); else fd.start(sf, p.ldro); }

    Impl(const Params& pp, double fsChan, std::shared_ptr<ChipStream> shared) : p(pp), sf(pp.sf), N(1 << pp.sf), fs(fsChan), rho(fsChan / pp.bwHz) {
        stride = std::max(1, Demod::searchStride);
        ownCs = !shared || std::fabs(shared->rho() - rho) > 1e-9;
        cs = ownCs ? std::make_shared<ChipStream>(rho) : shared;
        dnRe.resize(N); dnIm.resize(N);
        for (int i = 0; i < N; i++) {
            const double ph = 2 * kPi * ((double)i * i / (2.0 * N) - 0.5 * i);
            dnRe[i] = (float)std::cos(ph); dnIm[i] = (float)-std::sin(ph);
        }
        re.resize(N); im.resize(N); pw.resize(N);
        hist.assign(3, std::vector<float>(N));
        preAcc.assign(N, 0.f);
        s3.assign(N, 0.f);
        // interpolator: -6 dB at bw/2, transition +-0.1 bw, 50 dB
        const double fc = 0.5 / rho, df = 0.2 / rho;
        K = (int)std::ceil((50 - 7.95) / (14.36 * df)) + 1;
        K = std::max(8, (K + 1) & ~1);
        sincTab.assign((size_t)(kP + 1) * K, 0.f);
        const double beta = kaiserBeta(50), half = K / 2.0;
        for (int r = 0; r <= kP; r++) {
            const double f = (double)r / kP;
            double sum = 0;
            for (int k = 0; k < K; k++) {
                const double d = f + half - 1 - k;   // distance from the tap to the wanted point
                const double s = std::fabs(d) < 1e-12 ? 2 * fc : std::sin(2 * kPi * fc * d) / (kPi * d);
                const double rr = d / (half + 0.5);
                const double w = besselI0(beta * std::sqrt(std::max(0.0, 1 - rr * rr))) / besselI0(beta);
                sincTab[(size_t)r * K + k] = (float)(s * w);
                sum += s * w;
            }
            for (int k = 0; k < K; k++) sincTab[(size_t)r * K + k] = (float)(sincTab[(size_t)r * K + k] / sum);
        }
    }

    // samples needed to extract a window that starts at sample t0 with chip spacing step
    double lastSample(double t0, double step) const { return t0 + (N - 1) * step + K / 2 + 3; }
    double firstSample(double t0) const { return t0 - K / 2 - 2; }

    // the N chips from chip index c0 of the shared stream; fuse: +1 / -1 also removes an up-chirp / down-chirp (the next spectrum() call
    // with the same direction skips its own de-chirp pass)
    int fused = 0;
    bool fetch(int64_t c0, int fuse) {
        fused = 0;
        if (c0 < cs->begin() || c0 + N > cs->end()) return false;
        const float* sr = cs->re(c0); const float* si = cs->im(c0);
        if (fuse) {
            const float* dr = dnRe.data(); const float* di = dnIm.data();
            const float sg = fuse < 0 ? -1.f : 1.f;
            for (int i = 0; i < N; i++) { const float vr = sr[i], vi = si[i], cr = dr[i], ci = sg * di[i]; re[i] = vr * cr - vi * ci; im[i] = vr * ci + vi * cr; }
        } else { std::copy(sr, sr + N, re.begin()); std::copy(si, si + N, im.begin()); }
        fused = fuse;
        return true;
    }

    // accurate: mix out cfo (bins), low-pass to the LoRa band and resample at t0 + i * step
    std::vector<float> mre, mim;
    static float dot8(const float* h, const float* x, int K) {
        float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        int k = 0;
        for (; k + 8 <= K; k += 8) for (int l = 0; l < 8; l++) acc[l] += h[k + l] * x[k + l];
        float s = ((acc[0] + acc[4]) + (acc[1] + acc[5])) + ((acc[2] + acc[6]) + (acc[3] + acc[7]));
        for (; k < K; k++) s += h[k] * x[k];
        return s;
    }
    bool extract(const ChanBuf& b, double t0, double step, double cfo) {
        const int64_t lo = (int64_t)std::floor(firstSample(t0)), hi = (int64_t)std::ceil(lastSample(t0, step));
        if (lo < b.begin() || hi >= b.end()) return false;
        const size_t len = (size_t)(hi - lo + 1);
        mre.resize(len); mim.resize(len);
        const double w = -2 * kPi * cfo / (N * rho);          // radians per channel sample
        // mixer: the phasor of sample k is polar(w (lo + 16 g)) x polar(w l) for k = 16 g + l (one sine per 16 samples)
        float tr[16], ti[16];
        for (int l = 0; l < 16; l++) { const cd t = std::polar(1.0, w * l); tr[l] = (float)t.real(); ti[l] = (float)t.imag(); }
        const cf32* x = b.at(lo);
        for (size_t k0 = 0; k0 < len; k0 += 16) {
            const cd g = std::polar(1.0, std::fmod(w * (double)(lo + (int64_t)k0), 2 * kPi));
            const float gr = (float)g.real(), gi = (float)g.imag();
            const size_t cnt = std::min<size_t>(16, len - k0);
            for (size_t l = 0; l < cnt; l++) {
                const float zr = gr * tr[l] - gi * ti[l], zi = gr * ti[l] + gi * tr[l];
                const float xr = x[k0 + l].real(), xi = x[k0 + l].imag();
                mre[k0 + l] = xr * zr - xi * zi; mim[k0 + l] = xr * zi + xi * zr;
            }
        }
        for (int i = 0; i < N; i++) {
            const double t = t0 + i * step - (double)lo;
            int64_t n0 = (int64_t)std::floor(t);
            int r = (int)std::lround((t - (double)n0) * kP);
            const float* h = &sincTab[(size_t)r * K];
            const size_t o = (size_t)(n0 - K / 2 + 1);
            re[i] = dot8(h, &mre[o], K); im[i] = dot8(h, &mim[o], K);
        }
        return true;
    }

    // de-chirp (up: remove an up-chirp; else remove a down-chirp) and FFT; fills pw, returns the mean power
    float spectrum(bool up, bool keep = false) {
        if (fused != (up ? 1 : -1)) {
            for (int i = 0; i < N; i++) {
                const float a = re[i], c = im[i], dr = dnRe[i], di = up ? dnIm[i] : -dnIm[i];
                re[i] = a * dr - c * di; im[i] = a * di + c * dr;
            }
        }
        fused = 0;
        if (keep) { yRe = re; yIm = im; }
        fftSplit(re.data(), im.data(), sf, false);
        double s = 0;
        for (int k = 0; k < N; k++) { pw[k] = re[k] * re[k] + im[k] * im[k]; s += pw[k]; }
        return (float)(s / N + 1e-30);
    }
    int argmax() const { return (int)(std::max_element(pw.begin(), pw.end()) - pw.begin()); }
    int wrap(int k) const { return ((k % N) + N) % N; }
    float near3(const std::vector<float>& v, int k) const { return std::max(v[wrap(k)], std::max(v[wrap(k - 1)], v[wrap(k + 1)])); }
    // fractional peak position around bin k from the complex FFT (Candan's bias-corrected Jacobsen estimator)
    double frac(int k) const {
        const std::complex<double> a(re[wrap(k - 1)], im[wrap(k - 1)]), b(re[wrap(k)], im[wrap(k)]), c(re[wrap(k + 1)], im[wrap(k + 1)]);
        const std::complex<double> den = 2.0 * b - a - c;
        if (std::abs(den) < 1e-20) return 0;
        const double d = std::real((a - c) / den) * std::tan(kPi / N) / (kPi / N);
        return std::max(-0.5, std::min(0.5, d));
    }

    void toSearch(double from) {
        state = Search;
        ochip = (int64_t)std::ceil(from / rho - 1e-6);
        origin = (double)ochip * rho; win = 0; histN = 0; preEnd = -1;
        std::fill(s3.begin(), s3.end(), 0.f);
        for (auto& v : hist) std::fill(v.begin(), v.end(), 0.f);
    }

    double winStart(int64_t w) const { return origin + (double)w * N * rho; }
    int64_t winChip(int64_t w) const { return ochip + w * N; }

    bool sfd(const ChanBuf& b);
    void dataSymbol(const ChanBuf& b, std::vector<RxFrame>& out);
    void run(const ChanBuf& b, std::vector<RxFrame>& out);
};

void Demod::Impl::run(const ChanBuf& b, std::vector<RxFrame>& out) {
    if (ownCs) cs->extend(b);
    for (int guard = 0; guard < 100000; guard++) {
        if (state == Data) {
            if (std::ceil(lastSample(posS, chipS)) + 1 >= (double)b.end()) return;
            dataSymbol(b, out);
            continue;
        }
        if (state == Preamble && preEnd >= 0) {
            // the SFD search looks five windows past the end of the preamble
            if (winStart(preEnd + 6) + K + 4 >= (double)b.end()) return;
            if (!sfd(b)) { st.syncBad++; toSearch(winStart(preEnd)); }
            continue;
        }
        if (state == Search && stride > 1 && (win % stride) != 0) { win++; continue; }   // only every stride-th window looks for a preamble
        if (winChip(win) + N > cs->end()) return;
        if (winChip(win) < cs->begin()) { win++; continue; }    // history was dropped: skip
        fetch(winChip(win), 1);
        const float mean = spectrum(true);
        std::vector<float>& h = hist[(size_t)(win % 3)];
        {
            const float inv = 1.f / mean;
            float* hp = h.data(); float* sp = s3.data(); const float* pp = pw.data();
            for (int k = 0; k < N; k++) { const float v = pp[k] * inv; sp[k] += v - hp[k]; hp[k] = v; }
        }
        histN++;
        if (state == Search) {
            if (histN >= 3) {
                // three windows in a row with their energy in the same pair of bins
                const auto& a = hist[0]; const auto& bb = hist[1]; const auto& c = hist[2];
                int best = 0; float bestQ = 0;
                {
                    const float* sp = s3.data();
                    float mq = 0;
                    for (int k = 0; k < N - 1; k++) { const float q = sp[k] + sp[k + 1]; mq = q > mq ? q : mq; }
                    { const float q = sp[N - 1] + sp[0]; mq = q > mq ? q : mq; }
                    bestQ = mq / 3.f;
                    if (bestQ > 7.5f) {
                        for (int k = 0; k < N; k++) { const float q = (sp[k] + sp[(k + 1) & (N - 1)]) / 3.f; if (q >= mq / 3.f) { best = k; break; } }
                    }
                }
                bool each = true;
                for (int j = 0; j < 3; j++) {
                    const auto& v = hist[(size_t)j];
                    each &= std::max(v[(size_t)best] + v[(size_t)((best + 1) & (N - 1))], std::max(v[(size_t)wrap(best - 1)] + v[(size_t)best], v[(size_t)((best + 1) & (N - 1))] + v[(size_t)((best + 2) & (N - 1))])) > 4.f;
                }
                if (bestQ > 7.5f && each) {
                    state = Preamble;
                    st.preambles++;
                    std::fill(preAcc.begin(), preAcc.end(), 0.f);
                    for (int j = 0; j < 3; j++) for (int k = 0; k < N; k++) preAcc[k] += hist[(size_t)j][k];
                    kUp = (int)(std::max_element(preAcc.begin(), preAcc.end()) - preAcc.begin());
                    preLevel = bestQ;
                    preFirst = win - 2 * stride;
                    preEnd = -1;
                }
            }
        } else {
            // still the preamble while the energy stays near kUp (followed bin by bin: a clock error moves it slowly), above a
            // quarter of the preamble's level so far (two noise bins alone pass a fixed low threshold too often)
            float v = 0;
            for (int d = -2; d <= 2; d++) {
                const int k = wrap(kUp + d);
                const float q = h[(size_t)k] + std::max(h[(size_t)wrap(k - 1)], h[(size_t)wrap(k + 1)]);
                v = std::max(v, q);
            }
            if (v > std::max(5.f, 0.25f * preLevel) && win - preFirst < 80) {
                // the bin of the recent windows (forgetting the old ones follows a drift; one window alone is too noisy)
                for (int k = 0; k < N; k++) preAcc[k] = 0.7f * preAcc[k] + h[k];
                kUp = (int)(std::max_element(preAcc.begin(), preAcc.end()) - preAcc.begin());
                preLevel = 0.8f * preLevel + 0.2f * v;
            } else if (win - preFirst >= 80) {
                toSearch(winStart(win));                // a steady carrier, not a frame
                continue;
            } else if (win - preFirst < kMinPreWins + 2 * (stride - 1)) {
                toSearch(winStart(win));                // three windows with nothing after them: noise, not a preamble
                continue;
            } else {
                preEnd = win;
            }
        }
        win++;
    }
}

// Finds the first down-chirp after the preamble, checks the sync word and measures the carrier and timing offsets.
bool Demod::Impl::sfd(const ChanBuf& b) {
    const int64_t e = preEnd;
    // the down-chirp's bin on the (unaligned) grid: every window that holds a part of the 2.25 down-chirps puts it in the same bin
    std::vector<float> dacc(N, 0.f);
    for (int64_t v = e; v <= e + 3; v++) {
        if (!fetch(winChip(v), -1)) return false;
        const float mean = spectrum(false);
        for (int k = 0; k < N; k++) dacc[k] += pw[k] / mean;
    }
    int kDn = 0; float bestDn = 0;
    for (int k = 0; k < N; k++) {
        const float q = dacc[k] + std::max(dacc[(size_t)wrap(k - 1)], dacc[(size_t)wrap(k + 1)]);
        if (q > bestDn) { bestDn = q; kDn = dacc[(size_t)wrap(k - 1)] > dacc[(size_t)wrap(k + 1)] && dacc[(size_t)wrap(k - 1)] > dacc[k] ? wrap(k - 1) : k; }
    }
    if (bestDn < 10.f + sf) return false;      // noise alone reaches about 14 at SF7, 20 at SF12
    // up-chirp bin = cfo - tau, down-chirp bin = cfo + tau (tau: where the symbols start, in chips, from the grid's windows);
    // both bins may be one off, so tau is tried two chips either side, and half a symbol away (the sum fixes cfo only modulo N / 2)
    const int diff = wrap(kDn - kUp);
    int taus[10]; int nt = 0;
    for (int d = -2; d <= 2; d++) { taus[nt++] = wrap(diff / 2 + d); taus[nt++] = wrap(diff / 2 + d + N / 2); }
    std::vector<uint8_t> syncs{p.syncWord};
    for (uint8_t s : p.moreSync) if (std::find(syncs.begin(), syncs.end(), s) == syncs.end()) syncs.push_back(s);
    // Every candidate (tau, m) is scored first on the cheap windows (cached per tau: 9 up and 6 down windows serve all m),
    // the three best again on windows resampled with their carrier offset mixed out (full sensitivity).
    struct Cand { double score; int tau, m, cfoC, si; };
    std::vector<Cand> cands;
    std::vector<std::vector<float>> upW(9, std::vector<float>(N)), dnW(6, std::vector<float>(N));
    std::vector<int> upOk(9), dnOk(6);
    for (int ti = 0; ti < nt; ti++) {
        const int tau = taus[ti];
        const int cfoC = wrap(kUp + tau);
        // up windows start at (e + j) N + tau for j = -4..4, down windows for j = -1..4
        for (int j = -4; j <= 4; j++) {
            upOk[(size_t)(j + 4)] = fetch(ochip + (e + j) * N + tau, 1);
            if (!upOk[(size_t)(j + 4)]) continue;
            const float m = spectrum(true);
            for (int k = 0; k < N; k++) upW[(size_t)(j + 4)][(size_t)k] = pw[(size_t)k] / m;
        }
        for (int j = -1; j <= 4; j++) {
            dnOk[(size_t)(j + 1)] = fetch(ochip + (e + j) * N + tau, -1);
            if (!dnOk[(size_t)(j + 1)]) continue;
            const float m = spectrum(false);
            for (int k = 0; k < N; k++) dnW[(size_t)(j + 1)][(size_t)k] = pw[(size_t)k] / m;
        }
        for (int m = -1; m <= 3; m++) {
            // first down-chirp at (e + m) N + tau: up windows at m-3, m-2, m-1, down windows at m, m+1
            if (!upOk[(size_t)(m + 1)] || !upOk[(size_t)(m + 2)] || !upOk[(size_t)(m + 3)] || !dnOk[(size_t)(m + 1)] || !dnOk[(size_t)(m + 2)]) continue;
            for (size_t si = 0; si < syncs.size(); si++) {
                const int sw1 = syncSymbol(syncs[si], 0, sf), sw2 = syncSymbol(syncs[si], 1, sf);
                const double sc = near3(upW[(size_t)(m + 1)], cfoC) + near3(upW[(size_t)(m + 2)], cfoC + sw1) + near3(upW[(size_t)(m + 3)], cfoC + sw2) +
                                  near3(dnW[(size_t)(m + 1)], cfoC) + near3(dnW[(size_t)(m + 2)], cfoC);
                cands.push_back({sc, tau, m, cfoC, (int)si});
            }
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score > y.score; });
    if (cands.size() > 2 + syncs.size()) cands.resize(2 + syncs.size());
    std::vector<float> w0(N), w1(N), w2(N), w3(N), w4(N);
    double cfoMix = 0;
    auto norm = [&](bool up, double t0, std::vector<float>& o) {
        if (!extract(b, t0, rho, cfoMix)) return false;
        const float m = spectrum(up);
        for (int k = 0; k < N; k++) o[k] = pw[k] / m;
        return true;
    };
    double bestScore = 0; int64_t bestS = 0; int bestCfo = 0, bestV1 = 0, bestV2 = 0, sw1 = 0, sw2 = 0;
    for (const auto& c : cands) {
        const int cw1 = syncSymbol(syncs[(size_t)c.si], 0, sf), cw2 = syncSymbol(syncs[(size_t)c.si], 1, sf);
        cfoMix = c.cfoC >= N / 2 ? c.cfoC - N : c.cfoC;     // after mixing, the expected bins are relative to it
        const int64_t s = (e + c.m) * N + c.tau;
        const double t = origin;
        if (!norm(true, t + (double)(s - 3 * N) * rho, w0) || !norm(true, t + (double)(s - 2 * N) * rho, w1) ||
            !norm(true, t + (double)(s - N) * rho, w2) || !norm(false, t + (double)s * rho, w3) || !norm(false, t + (double)(s + N) * rho, w4))
            continue;
        const double score = near3(w0, 0) + near3(w1, cw1) + near3(w2, cw2) + near3(w3, 0) + near3(w4, 0);
        if (score > bestScore) {
            bestScore = score; bestS = s; bestCfo = c.cfoC; sw1 = cw1; sw2 = cw2;
            bestV1 = (int)(std::max_element(w1.begin(), w1.end()) - w1.begin());
            bestV2 = (int)(std::max_element(w2.begin(), w2.end()) - w2.begin());
        }
    }
    if (bestScore < 15) return false;
    auto close = [&](int v, int want) { const int d = wrap(v - want); return d <= 2 || d >= N - 2; };
    if (!close(bestV1, sw1) || !close(bestV2, sw2)) return false;
    syncSeen = (uint8_t)(((bestV1 + 4) >> 3) << 4 | (((bestV2 + 4) >> 3) & 0xF));

    // fine estimates on accurately resampled windows, twice
    double s = (double)bestS;                          // chips from the origin
    double cfo = bestCfo >= N / 2 ? bestCfo - N : bestCfo;
    const int preWins = (int)(e - preFirst);
    const int J = std::max(1, std::min(12, preWins - 2));
    double slope = 0;
    for (int it = 0; it < 2; it++) {
        double epsUp = 0, wUp = 0, epsDn = 0, wDn = 0;
        cd acc(0, 0);
        std::vector<double> ej(J + 1, 0.0);
        std::vector<int> okj(J + 1, 0);
        std::vector<double> wj(J + 1, 0.0);
        std::vector<cd> xs((size_t)(J + 1) * 5);         // bins -2..2 of every preamble window
        double sig = 0, noi = 0; int ns = 0;
        for (int j = J; j >= 1; j--) {
            const double t0 = origin + (s - (2 + j) * N) * rho;
            if (!extract(b, t0, rho, cfo)) continue;
            const float mean = spectrum(true);
            int k = 0; float bp = 0;
            for (int d = -2; d <= 2; d++) if (pw[(size_t)wrap(d)] > bp) { bp = pw[(size_t)wrap(d)]; k = d; }
            const double f = k + frac(k);
            epsUp += f * bp; wUp += bp;
            ej[(size_t)j] = f; okj[(size_t)j] = 1; wj[(size_t)j] = bp;
            for (int d = -2; d <= 2; d++) xs[(size_t)j * 5 + (size_t)(d + 2)] = cd(re[(size_t)wrap(d)], im[(size_t)wrap(d)]);
            // noise from the bins far from the peak (the band edges of the filter spread a little energy around it)
            double nf = 0; int nn = 0;
            for (int d = N / 8; d <= N - N / 8; d++) { nf += pw[(size_t)wrap(k + d)]; nn++; }
            nf /= std::max(1, nn);
            sig += std::max(0.0, (double)mean * N - nf * N); noi += nf; ns++;
        }
        {
            // the phase of one bin advances by 2 pi times the carrier offset from window to window
            int kb = 0; double bp = -1;
            for (int d = 0; d < 5; d++) {
                double e = 0;
                for (int j = 1; j <= J; j++) e += std::norm(xs[(size_t)j * 5 + (size_t)d]);
                if (e > bp) { bp = e; kb = d; }
            }
            for (int j = J; j >= 2; j--)
                if (okj[(size_t)j] && okj[(size_t)(j - 1)]) acc += xs[(size_t)(j - 1) * 5 + (size_t)kb] * std::conj(xs[(size_t)j * 5 + (size_t)kb]);
        }
        for (int d = 0; d < 2; d++) {
            const double t0 = origin + (s + d * N) * rho;
            if (!extract(b, t0, rho, cfo)) return false;
            spectrum(false);
            int k = 0; float bp = 0;
            for (int dd = -2; dd <= 2; dd++) if (pw[(size_t)wrap(dd)] > bp) { bp = pw[(size_t)wrap(dd)]; k = dd; }
            epsDn += (k + frac(k)) * bp; wDn += bp;
        }
        if (wUp <= 0 || wDn <= 0) return false;
        epsDn /= wDn;
        // the up-chirps' peak against the window index (a clock error moves it by the drift per symbol), taken where the
        // down-chirps are (j = -2.5)
        {
            double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
            for (int j = 1; j <= J; j++) if (okj[(size_t)j]) { const double w = wj[(size_t)j]; sw += w; sx += w * j; sy += w * ej[(size_t)j]; sxx += w * j * j; sxy += w * j * ej[(size_t)j]; n++; }
            slope = 0;
            if (n >= 4) { const double den = sw * sxx - sx * sx; if (den > 0) slope = (sw * sxy - sx * sy) / den; }
            slope = std::max(-2.5e-4 * N, std::min(2.5e-4 * N, slope));   // up to 250 ppm
            epsUp = (sy - slope * sx) / sw + slope * -2.5;
        }
        double dres = 0.5 * (epsUp + epsDn);
        const double tres = 0.5 * (epsDn - epsUp) - (p.cfoTrack > 0 ? 0.0 : 0.5 * slope);    // at the first down-chirp (a frequency ramp does not move the symbol clock)
        if (std::abs(acc) > 0) {
            double ph = std::arg(acc) / (2 * kPi);
            // a drifting carrier: the phase step measures the offset in the middle of the preamble, the estimate is wanted at the down-chirps
            if (p.cfoTrack > 0) ph += -slope * ((J + 1) / 2.0 + 2.5);
            double dd = ph - dres;
            dd -= std::round(dd);
            if (p.cfoTrack <= 0 || (it > 0 && std::abs(dd) < 0.3)) dres += dd;   // a drifting carrier: the two estimates must agree before the finer one is taken
        }
        cfo += dres;
        s += tres;
        if (ns > 0) {
            // SNR in the LoRa band from the preamble: peak bins against the mean (which is the noise per bin)
            const double sMean = sig / ns, nMean = noi / ns;
            snrLin = std::max(1e-4, sMean / (N * std::max(nMean, 1e-30)));
            levelLin = sMean / ((double)N * N);
            sigPw = sMean;
        }
    }
    cfoBins = cfo;
    drift = slope;                                     // chips per symbol
    cfoRate = 0; thetaOk = false; lastAdj = 0;
    if (p.cfoTrack > 0) {
        // a carrier that drifts (Doppler rate): what the preamble showed as a clock drift is a frequency ramp, which the loop in
        // dataSymbol() follows; the symbol clock stays at the nominal rate
        double clk = 0;   // the part of the ramp that is the stretched symbol clock (chips per symbol): the timing loop's, not the carrier's
        if (p.carrierHz > 0) clk = -1.0 * (cfoBins * p.bwHz / N) / p.carrierHz * N;
        cfoRate = -(slope - clk);
        cfoBins += (1.75 + sfdExtraSymbols(sf)) * cfoRate;   // the estimate is valid half a symbol into the down-chirps
        cfoStartBins = cfoBins;
        drift = 0;
    }
    chipS = rho * (1 + drift / N);
    perS = N * chipS;
    // the data start 2.25 symbols after the first down-chirp (and the clock error of 2.25 symbols later)
    posS = origin + (s + (2.25 + sfdExtraSymbols(sf)) * (N + drift)) * rho;
    frameStart = origin + (s - (2.0 + p.preamble) * N) * rho;   // the first up-chirp of the preamble (of the configured length)
    loopInt = 0;
    posS0 = posS; retried = 0;
    startFd();
    state = Data;
    return true;
}

void Demod::Impl::dataSymbol(const ChanBuf& b, std::vector<RxFrame>& out) {
    if (!extract(b, posS, chipS, cfoBins)) { toSearch(posS); return; }
    const float mean = spectrum(true, true);
    SoftSym ss;
    {
        // the four strongest bins (ties: the lower bin first, like argmax) and their noncoherent log-likelihoods
        float tv[SoftSym::M]; int ti[SoftSym::M];
        int cnt = 0;
        for (int i = 0; i < N; i++) {
            const float v = pw[(size_t)i];
            if (cnt == SoftSym::M && v <= tv[SoftSym::M - 1]) continue;
            int pos = cnt < SoftSym::M ? cnt++ : SoftSym::M - 1;
            while (pos > 0 && tv[pos - 1] < v) { tv[pos] = tv[pos - 1]; ti[pos] = ti[pos - 1]; pos--; }
            tv[pos] = v; ti[pos] = i;
        }
        const double nz = std::max(1e-30, ((double)mean * N - tv[0]) / (N - 1));
        const double amp = std::sqrt(std::max(sigPw, 1e-30));
        const double sc = 2.0 * amp / nz;
        for (int c = 0; c < cnt; c++) { ss.bin[c] = (uint16_t)ti[c]; ss.m[c] = lnI0((float)(sc * std::sqrt((double)tv[c]))); }
        ss.n = cnt;
        ss.floorM = ss.m[cnt - 1];
    }
    const int k = ss.bin[0];
    const double q = pw[(size_t)k] / mean;
    if (sigPw > 0 && Demod::softOn) fd.pushSoft(ss); else fd.push((uint16_t)k);
    // Carrier offset (cfoTrack > 0): the phase of the peak bin advances by 2 pi times the carrier offset (in bins) from symbol to
    // symbol (the chirps' phase is continuous and returns to zero at every symbol start), once the mixer's own phase at the window
    // start is added back. Moving the window by x chips turns the phase by x / 2 cycles (lastAdj). The offset comes out modulo one
    // bin, relative to the loop's own estimate.
    double eF = 0;
    bool haveF = false;
    if (p.cfoTrack > 0) {
        const double theta = std::arg(cd(re[(size_t)k], im[(size_t)k])) + 2 * kPi * cfoBins * posS / ((double)N * rho);
        if (thetaOk) {
            const double d = (theta - theta0) / (2 * kPi) - 0.5 * lastAdj;
            eF = d - (cfoBins - 0.5 * cfoRate);   // the phase step is the offset half a symbol back
            eF -= std::round(eF);
            eF = std::max(-0.5, std::min(0.5, eF));
            haveF = true;
        }
        theta0 = theta; thetaOk = true;
    }
    // Timing loop. The peak's fractional position does not show a timing error: the shift of the tone is undone, on average,
    // by the phase step where the chirp wraps. That step itself measures it: the de-chirped symbol's two parts (before and after
    // the wrap at chip N - k) differ in phase by pi (tau + residual carrier offset), tau = how late the symbol starts, in chips.
    double adj = 0;
    // wider loop for the first 40 symbols (the drift from a short preamble may be well off), then the narrow one
    constexpr double kI0 = 0.3, kP0 = 0.8;
    constexpr int kAcq = 40;
    const bool acq = fd.pushed() <= kAcq;
    const double kI = acq ? kI0 : 0.02, kP_ = acq ? kP0 : 0.3;
    const int fold = (N - k) % N;
    const double w = 4.0 * fold * (N - fold) / ((double)N * N);
    if (q > 8 && w > 0.25) {
        cd s1(0, 0), s2(0, 0), rot(1, 0);
        const cd stp = std::polar(1.0, -2 * kPi * k / N);
        for (int i = 0; i < N; i++) {
            const cd v = cd(yRe[(size_t)i], yIm[(size_t)i]) * rot;
            if (i < fold) s1 += v; else s2 += v;
            rot *= stp;
        }
        const double tau = std::arg(s2 * std::conj(s1)) / kPi - eF;   // the carrier offset (known from the phase) is not a timing error
        const double e = std::max(-0.5, std::min(0.5, tau)) * w;
        loopInt += kI * e;
        adj = kP_ * e + loopInt;
    } else adj = loopInt;
    if (p.cfoTrack > 0) {
        if (haveF) {
            const double kp = 0.5 * p.cfoTrack, ki = 0.1 * p.cfoTrack;
            cfoRate += ki * eF;
            cfoBins += cfoRate + kp * eF;
        } else cfoBins += cfoRate;
        lastAdj = adj;
    }
    posS += perS + adj * chipS;
    if (fd.pushed() == 8 && !fd.header().ok) { st.headerBad++; toSearch(posS); return; }
    if (fd.done()) {
        RxFrame f;
        int corr = 0;
        fd.finish(f.payload, f.crcOk, corr);
        if (fd.header().crc && !f.crcOk && retried == 0 && !p.implicitHdr && p.jumpChips > 0) {
            // samples lost before the demodulator (a radio or link dropping a block) move every later symbol by the same amount (#386)
            std::vector<uint8_t> fixed;
            if (const int jumps = recoverJumps(sf, p.ldro, fd.bins(), p.jumpChips, fixed); jumps > 0) {
                f.payload = fixed; f.crcOk = true; f.jumpsFixed = jumps; st.jumpFixed++;
            }
        }
        if (fd.header().crc && !f.crcOk && retried < 3 && p.cfoTrack <= 0) {
            // A short preamble at a low SNR leaves the clock drift poorly known: the timing loop catches up, but the first symbols
            // may be lost. The loop's integral then holds the rest of the drift: demodulate the frame again from its start with it.
            retried++;
            const double dd = loopInt;
            drift += dd;
            chipS = rho * (1 + drift / N);
            perS = N * chipS;
            posS0 += 1.75 * dd * rho;
            posS = posS0;           // the start was placed 2.25 symbols past the down-chirp, less half the slope
            loopInt = 0;
            startFd();
            return;
        }
        f.hdr = fd.header();
        f.corrected = corr;
        f.softFixed = fd.softFixed() ? 1 : 0;
        f.cfoHz = cfoBins * p.bwHz / N;
        if (p.cfoTrack > 0) { f.cfoStartHz = cfoStartBins * p.bwHz / N; f.cfoRateHzS = cfoRate * p.bwHz * p.bwHz / ((double)N * N); }
        f.sfoPpm = (drift + loopInt) / N * 1e6;
        f.snrDb = 10 * std::log10(snrLin);
        f.levelDb = 10 * std::log10(std::max(levelLin, 1e-20));
        f.startSample = frameStart;
        f.endSample = posS;
        f.syncWord = syncSeen;
        st.frames++;
        if (f.hdr.crc && !f.crcOk) st.crcBad++;
        out.push_back(std::move(f));
        toSearch(posS);
    }
}

Demod::Demod(const Params& p, double fsChan, int, std::shared_ptr<ChipStream> shared) : p_(std::make_unique<Impl>(p, fsChan, shared)) {}
Demod::~Demod() = default;
const Params& Demod::params() const { return p_->p; }
void Demod::reset(int64_t from) { p_->toSearch((double)from); p_->st = DemodStats(); }
void Demod::process(const ChanBuf& b, std::vector<RxFrame>& out) {
    p_->run(b, out);
    if (p_->ownCs) p_->cs->trimToSample(oldestNeeded());
}
bool Demod::busy() const { return p_->state != Impl::Search; }
const DemodStats& Demod::stats() const { return p_->st; }
int64_t Demod::oldestNeeded() const {
    const Impl& m = *p_;
    if (m.state == Impl::Data) return (int64_t)std::floor(m.retried >= 2 ? m.posS : std::min(m.posS, m.posS0 - 4 * m.rho)) - m.K - 8;
    // the SFD search and the fine estimates look back up to 13 windows
    const double back = m.winStart(m.win - 14);
    return (int64_t)std::floor(std::max(back, m.origin - 8.0)) - m.K - 8;
}

} // namespace lora
} // namespace dect2
