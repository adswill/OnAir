// DVB-SH OFDM (SH-A) generator: EN 302 583 V1.2.1 from the transport stream to OFDM symbols.
#include "dect2/dvbsh/dvbsh_gen.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>

namespace dect2 {
namespace dvbsh {

// One SH frame of 13056 IU: NCW codewords, the padding CUs (the PRBS restarted for every SH frame), all through the time interleaver
void Generator::Chain::fillShFrame() {
    const int ncw = codewordsPerShFrame(pat);
    std::vector<uint8_t> sh;
    sh.reserve((size_t)kShFrameIu * kIuBits);
    uint8_t pk[188 * kPackets];
    std::vector<uint8_t> ef(kTcInput), cw;
    for (int c = 0; c < ncw; c++) {
        for (int k = 0; k < kPackets; k++) src(pk + k * 188);
        buildEframe(pk, c, ef.data());
        encodeCodeword(ef.data(), pat, cw);
        sh.insert(sh.end(), cw.begin(), cw.end());
        codewords++;
    }
    const size_t pad = (size_t)(kShFrameCu - dataCu(pat)) * kCuBits;
    const size_t at = sh.size();
    sh.resize(at + pad);
    prbsBits(pad, sh.data() + at);
    uint8_t iu[kIuBits];
    for (int i = 0; i < kShFrameIu; i++) {
        ti.process(sh.data() + (size_t)i * kIuBits, iu);
        bits.insert(bits.end(), iu, iu + kIuBits);
    }
}

Generator::Generator(const Params& p, PacketSource hp, PacketSource lp)
    : p_(p), N_(fftN(p.mode)), G_(guardSamples(p.mode, p.guard)), K_(carriersK(p.mode)), fft_(std::make_unique<Fft>(fftN(p.mode))) {
    if (p_.mod != k16Qam) p_.hier = 0;
    std::vector<int> L;
    if (!tiDelays(p_.ti, kShFrameCu, L)) { p_.ti = TiConfig{}; tiDelays(p_.ti, kShFrameCu, L); }
    hp_.pat = p_.patHp;
    hp_.src = hp ? std::move(hp) : dvbt::testTsSource(1);
    hp_.ti.configure(L, false);
    if (p_.hier) {
        lp_.pat = p_.patLp;
        lp_.src = lp ? std::move(lp) : dvbt::testTsSourceNamed(2, 2, "Test service LP");
        lp_.ti.configure(L, false);
    }
    tpsVal_.assign(tpsCarriers(p_.mode).size(), 1.f);
}

Generator::~Generator() = default;

void Generator::nextSymbol(std::vector<cf32>& out) {
    const int Nd = dataCarriers(p_.mode);
    const int v = streamBitsPerCell(p_);
    auto take = [&](Chain& c, int n, std::vector<uint8_t>& dst) {
        while ((int)c.bits.size() < n) c.fillShFrame();
        dst.assign(c.bits.begin(), c.bits.begin() + n);
        c.bits.erase(c.bits.begin(), c.bits.begin() + n);
    };
    std::vector<uint8_t> xb, xl, words(Nd);
    take(hp_, Nd * v, xb);
    if (p_.hier) take(lp_, Nd * 2, xl);
    // bit demultiplexing (5.7.1.2) into v-bit words y'q, y0 the MSB
    for (int q = 0; q < Nd; q++) {
        if (p_.hier) words[q] = (uint8_t)(xb[2 * q] << 3 | xb[2 * q + 1] << 2 | xl[2 * q] << 1 | xl[2 * q + 1]);
        else if (v == 4) words[q] = (uint8_t)(xb[4 * q] << 3 | xb[4 * q + 2] << 2 | xb[4 * q + 1] << 1 | xb[4 * q + 3]);
        else words[q] = (uint8_t)(xb[2 * q] << 1 | xb[2 * q + 1]);
    }
    // symbol interleaver (5.7.2)
    const auto& H = symbolPermutation(p_.mode);
    std::vector<uint8_t> y(Nd);
    if (sym_ % 2) for (int q = 0; q < Nd; q++) y[q] = words[H[q]];
    else for (int q = 0; q < Nd; q++) y[H[q]] = words[q];
    std::vector<cf32> pts;
    dvbt::constellation(p_.mod, p_.hier, pts);

    std::vector<uint8_t> roles;
    carrierRoles(p_.mode, sym_, roles);
    const auto& w = dvbt::prbsW();
    std::vector<cf32> X(N_, cf32(0, 0));
    const int kc = (K_ - 1) / 2;
    auto bin = [&](int k) { return ((k - kc) % N_ + N_) % N_; };
    int di = 0;
    for (int k = 0; k < K_; k++) {
        if (roles[k] == 0) X[bin(k)] = pts[y[di++]];
        else if (roles[k] == 1 || roles[k] == 2) X[bin(k)] = cf32((w[k] ? -1.f : 1.f) * 4.f / 3.f, 0);
    }
    const int fi = frames_ % 4;
    const auto tps = tpsBits(p_, fi, (frames_ / 4) % superFramesPerShFrame(p_));
    const auto& tk = tpsCarriers(p_.mode);
    for (size_t i = 0; i < tk.size(); i++) {
        if (sym_ == 0) tpsVal_[i] = w[tk[i]] ? -1.f : 1.f;
        else if (tps[sym_]) tpsVal_[i] = -tpsVal_[i];
        X[bin(tk[i])] = cf32(tpsVal_[i], 0);
    }
    fft_->inverse(X.data());
    const float scale = 1.f / std::sqrt((float)K_ * 1.04f);
    out.resize((size_t)N_ + G_);
    for (int i = 0; i < N_; i++) out[G_ + i] = X[i] * scale;
    for (int i = 0; i < G_; i++) out[i] = out[N_ + i];
    if (++sym_ == 68) { sym_ = 0; frames_++; }
}

} // namespace dvbsh
} // namespace dect2
