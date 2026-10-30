// Frequency-domain channeliser for several LoRa channels of one wide capture: one forward FFT of the input (overlap-save) shared by all
// channels; per channel only a small inverse FFT of the bins around its centre (the decimation) with a smooth band-pass on them.
// STOP: a change here also needs a run of the full mesh rates test, which ctest does not run: test_mesh_rates --full
// (tests/mesh/test_mesh_rates.cpp).
#include "dect2/mesh/mesh_lora.h"
#include "dect2/dsp/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace lora {

namespace {
using dect2::math::kPi;
using cd = std::complex<double>;
int ilog2(int x) { int l = 0; while ((1 << l) < x) l++; return l; }
} // namespace

struct FdChannelizer::Chan {
    Spec spec;
    int D = 1, Mc = 0, lc = 0;
    int64_t kc = 0;             // centre bin of the wideband FFT
    double fr = 0;              // the channel centre's distance from that bin, Hz
    double outRate = 0;
    std::vector<float> H;       // response on the Mc bins, FFT order, scaled by 1 / M
    std::vector<float> re, im;  // work (Mc)
    int64_t outN = 0;           // output samples produced so far
    float tr[16], ti[16];       // residual-frequency mixer: table of the 16 lanes
};

FdChannelizer::FdChannelizer(double inRate, const std::vector<Spec>& chans) : inRate_(inRate) {
    // decimations are powers of two (the channel's inverse FFT is M / D bins) giving 2 to 4 samples per chip
    double bwMin = 1e30;
    int dMax = 1;
    for (const Spec& s : chans) {
        Chan c;
        c.spec = s;
        int D = 1;
        while (inRate / (2.0 * D) >= 2.0 * s.bwHz * (1 - 1e-9)) D *= 2;   // the rate stays at 2 bandwidths or more
        c.D = D;
        c.outRate = inRate / D;
        dMax = std::max(dMax, D);
        bwMin = std::min(bwMin, s.bwHz);
        ch_.push_back(std::move(c));
    }
    // overlap: the band-pass's transition is 0.3 bw wide: its response dies out within about 5 / (0.3 bw) seconds each side
    int half = (int)std::ceil(5.0 * inRate / (0.3 * bwMin));
    int O = 2;
    while (O < 2 * half || O < 2 * dMax) O *= 2;
    int M = 4096;
    while (M < 8 * O) M *= 2;
    M_ = M; O_ = O; H_ = M - O; lm_ = ilog2(M);
    for (Chan& c : ch_) {
        c.Mc = M / c.D; c.lc = ilog2(c.Mc);
        const double binHz = inRate / M;
        c.kc = (int64_t)std::llround(c.spec.offsetHz / binHz);
        c.fr = c.spec.offsetHz - (double)c.kc * binHz;
        const double margin = std::min(25e3, 0.4 * c.spec.bwHz);
        const double fp = 0.5 * c.spec.bwHz + margin;
        const double fs = std::min(fp + 0.3 * c.spec.bwHz, 0.5 * c.outRate - binHz);
        c.H.assign((size_t)c.Mc, 0.f);
        for (int j = -c.Mc / 2; j < c.Mc / 2; j++) {
            // the response is centred on the channel's true centre (bins are on a grid): distance of bin j from it
            const double f = std::fabs((double)j * binHz - c.fr);
            double a = 0;
            if (f <= fp) a = 1;
            else if (f < fs) a = 0.5 * (1 + std::cos(kPi * (f - fp) / (fs - fp)));
            c.H[(size_t)((j + c.Mc) % c.Mc)] = (float)(a / M);
        }
        c.re.assign((size_t)c.Mc, 0.f); c.im.assign((size_t)c.Mc, 0.f);
        const double w = -2 * kPi * c.fr / c.outRate;
        for (int l = 0; l < 16; l++) { const cd t = std::polar(1.0, w * l); c.tr[l] = (float)t.real(); c.ti[l] = (float)t.imag(); }
    }
    reset();
}

FdChannelizer::~FdChannelizer() = default;

void FdChannelizer::reset() {
    in_.assign((size_t)O_ / 2, cf32(0, 0));
    inBase_ = -(int64_t)O_ / 2;
    block_ = 0;
    for (Chan& c : ch_) c.outN = 0;
    xr_.assign((size_t)M_, 0.f); xi_.assign((size_t)M_, 0.f);
}

size_t FdChannelizer::channels() const { return ch_.size(); }
double FdChannelizer::outRate(size_t i) const { return ch_[i].outRate; }
int FdChannelizer::decim(size_t i) const { return ch_[i].D; }

void FdChannelizer::process(const cf32* x, size_t n, const std::vector<ChanBuf*>& out) {
    in_.insert(in_.end(), x, x + n);
    for (;;) {
        const int64_t s = block_ * (int64_t)H_ - O_ / 2;          // first input sample of this segment
        const int64_t rel = s - inBase_;
        if (rel + M_ > (int64_t)in_.size()) break;
        const cf32* seg = in_.data() + rel;
        for (int j = 0; j < M_; j++) { xr_[(size_t)j] = seg[j].real(); xi_[(size_t)j] = seg[j].imag(); }
        fftSplit(xr_.data(), xi_.data(), lm_, false);
        for (size_t ci = 0; ci < ch_.size(); ci++) {
            Chan& c = ch_[ci];
            const int half = c.Mc / 2;
            // bins kc - half .. kc + half - 1 into FFT order of the small transform
            for (int j = -half; j < half; j++) {
                int64_t k = (c.kc + j) % M_;
                if (k < 0) k += M_;
                const size_t t = (size_t)((j + c.Mc) % c.Mc);
                const float h = c.H[t];
                c.re[t] = xr_[(size_t)k] * h; c.im[t] = xi_[(size_t)k] * h;
            }
            fftSplit(c.re.data(), c.im.data(), c.lc, true);
            // valid outputs: segment samples O/2 .. O/2 + H - 1 (input samples block x H ..), every D-th
            const int first = (O_ / 2) / c.D, cnt = H_ / c.D;
            std::vector<cf32>& tmp = tmp_;
            tmp.resize((size_t)cnt);
            const double wl = -2 * kPi * c.fr / c.outRate;
            // the bin shift by kc restarts its phase at every segment: add back kc x (segment start) / M cycles
            int64_t sm = s % M_; if (sm < 0) sm += M_;
            int64_t km = c.kc % M_; if (km < 0) km += M_;
            const double phb = -2 * kPi * (double)((km * sm) % M_) / (double)M_;
            for (int k0 = 0; k0 < cnt; k0 += 16) {
                const cd g = std::polar(1.0, std::fmod(wl * (double)(c.outN + k0), 2 * kPi) + phb);
                const float gr = (float)g.real(), gi = (float)g.imag();
                const int m = std::min(16, cnt - k0);
                for (int l = 0; l < m; l++) {
                    const float zr = gr * c.tr[l] - gi * c.ti[l], zi = gr * c.ti[l] + gi * c.tr[l];
                    const float yr = c.re[(size_t)(first + k0 + l)], yi = c.im[(size_t)(first + k0 + l)];
                    tmp[(size_t)(k0 + l)] = cf32(yr * zr - yi * zi, yr * zi + yi * zr);
                }
            }
            c.outN += cnt;
            out[ci]->append(tmp.data(), (size_t)cnt);
        }
        block_++;
        // forget what no later segment needs
        const int64_t keepFrom = block_ * (int64_t)H_ - O_ / 2;
        const int64_t drop = keepFrom - inBase_;
        if (drop > 0 && drop >= (int64_t)(1 << 16)) { in_.erase(in_.begin(), in_.begin() + drop); inBase_ += drop; }
    }
    // keep memory bounded when no block completes for a while (never more than one segment is pending)
}

} // namespace lora
} // namespace dect2
