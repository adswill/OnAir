// W1A voiceband modes: HF packet 300 Bd decoder, test impairments and the packet generator (see w1a_dsp.h). Receive only.
// The AFSK receiver and the AX.25 parser are the ones the repo already has (core/src/ham/ham_aprs.cpp, core/src/packet).
#include "w1a_priv.h"
#include "dect2/ham/ham_aprs.h"
#include "dect2/packet/packet_ax25.h"
#include <random>

namespace dect2::w1a {

const char* kindName(int k) {
    static const char* n[kKinds] = {"HF packet 300 Bd", "Voice inversion scrambler", "ELT 121.5 MHz"};
    return k >= 0 && k < kKinds ? n[k] : "W1A";
}

std::unique_ptr<Decoder> makeDecoder(int kind) {
    switch (kind) {
    case Invert: return makeInvertDecoder();
    case Elt: return makeEltDecoder();
    default: return makePacketDecoder();
    }
}

namespace {

// ---------------------------------------------------------------- HF packet, 300 Bd AFSK 1600 / 1800 Hz
class PacketDec : public Decoder {
public:
    PacketDec() { reset(); }
    void reset() override {
        rx_ = std::make_unique<ham::AfskRx>(kRate, 1600.0, 1800.0, 300.0, [this](const std::vector<uint8_t>& d, bool ok) { onFrame(d, ok); });
        good_ = bad_ = 0; last_.clear(); snr_ = 0; blk_ = 0; pTone_ = pTot_ = 0; calls_.clear();
    }
    void push(const cf32* x, size_t n) override {
        buf_.resize(n);
        for (size_t i = 0; i < n; i++) {
            const float v = std::isfinite(x[i].real()) ? x[i].real() : 0.f;
            buf_[i] = v;
            // tone power by two Goertzel-like running correlators over 0.1 s blocks
            const double a = 2 * kPi * 1600.0 / kRate, b = 2 * kPi * 1800.0 / kRate;
            c1_ += (double)v * std::complex<double>(std::cos(a * (double)blk_), -std::sin(a * (double)blk_));
            c2_ += (double)v * std::complex<double>(std::cos(b * (double)blk_), -std::sin(b * (double)blk_));
            pTot_ += (double)v * v;
            if (++blk_ >= 1200) {
                const double pt = 2.0 * (std::norm(c1_) + std::norm(c2_)) / ((double)blk_ * blk_);
                const double pa = pTot_ / (double)blk_;
                if (pt > 1e-7) { const double s = 10 * std::log10(pt / std::max(pa - pt, 1e-9)); snr_ = snr_ == 0 ? s : 0.8 * snr_ + 0.2 * s; sigBlocks_++; }
                blk_ = 0; c1_ = c2_ = {0, 0}; pTot_ = 0;
            }
        }
        rx_->feed(buf_.data(), n);
    }
    Status status() const override {
        Status s;
        s.good = good_; s.bad = bad_;
        s.snrDb = snr_;
        s.cfoHz = rx_->tuneHz();
        s.state = good_ ? 2 : (sigBlocks_ > 3 ? 1 : 0);
        char m[160];
        if (good_) snprintf(m, sizeof m, "%llu frame%s, last %s", (unsigned long long)good_, good_ == 1 ? "" : "s", last_.c_str());
        else snprintf(m, sizeof m, sigBlocks_ > 3 ? "AFSK tones found, no frame yet" : "searching");
        s.mode = m;
        s.params = {{"modulation", "AFSK 1600 / 1800 Hz (200 Hz shift), 300 Bd, upper sideband"}, {"link layer", "AX.25, NRZI, bit stuffing, CRC-16"},
                    {"stations", std::to_string(calls_.size())}, {"tone offset", std::to_string((int)std::lround(rx_->tuneHz())) + " Hz"}};
        return s;
    }
private:
    void onFrame(const std::vector<uint8_t>& d, bool ok) {
        if (!ok) { bad_++; return; }
        ax25::Frame f;
        if (!ax25::parseFrame(d.data(), d.size(), f)) { bad_++; return; }
        good_++;
        const std::string line = f.tnc2() + "  [" + f.typeName() + "]";
        last_ = f.from.str() + ">" + f.to.str();
        if (std::find(calls_.begin(), calls_.end(), f.from.str()) == calls_.end()) { calls_.push_back(f.from.str()); event("station " + f.from.str()); }
        text(line + "\n");
    }
    std::unique_ptr<ham::AfskRx> rx_;
    std::vector<float> buf_;
    std::vector<std::string> calls_;
    std::string last_;
    uint64_t good_ = 0, bad_ = 0, sigBlocks_ = 0;
    double snr_ = 0, pTot_ = 0, pTone_ = 0;
    std::complex<double> c1_, c2_;
    int blk_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> makePacketDecoder() { return std::make_unique<PacketDec>(); }

// ---------------------------------------------------------------- generators and impairments
std::vector<cf32> genPacket(const GenOpts& o) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < o.frames; i++) {
        ax25::Frame f;
        f.to.call = "CQ"; f.to.ssid = 0;
        f.from.call = i % 2 ? "OZ1ABC" : "N0CALL"; f.from.ssid = i % 3;
        f.control = 0x03; f.pid = 0xF0;
        f.info = "HF packet test frame " + std::to_string(i + 1) + " of " + std::to_string(o.frames) + " (simulated)";
        const std::vector<uint8_t> raw = ax25::buildFrame(f);
        const std::vector<uint8_t> b = ax25::hdlcBits(raw, i == 0 ? (int)std::ceil(o.leadSec * 300 / 8) + 2 : 3, i + 1 == o.frames ? 6 : 0);
        bits.insert(bits.end(), b.begin(), b.end());
    }
    const std::vector<uint8_t> lv = ax25::nrziEncode(bits);
    std::vector<cf32> out;
    out.reserve(lv.size() * 40 + 24000);
    double ph = 0;
    for (uint8_t l : lv) {
        const double w = 2 * kPi * (l ? o.markHz : o.spaceHz) / kRate;
        for (int k = 0; k < 40; k++) { out.push_back(cf32((float)std::cos(ph), (float)std::sin(ph)) * 0.5f); ph += w; if (ph > kPi) ph -= 2 * kPi; }
    }
    out.insert(out.end(), (size_t)(o.tailSec * kRate), cf32(0, 0));
    return out;
}

std::vector<cf32> impair(const std::vector<cf32>& x, double cfoHz, double snrDb3k, double ppm, uint32_t seed) {
    std::vector<cf32> y;
    const double r = 1 + ppm * 1e-6;
    y.reserve((size_t)((double)x.size() / r) + 1);
    for (double t = 0; t + 1 < (double)x.size(); t += r) {
        const size_t k = (size_t)t;
        const float f = (float)(t - (double)k);
        y.push_back(x[k] * (1 - f) + x[k + 1] * f);
    }
    double ps = 0; size_t nz = 0;
    for (auto& v : y) { const double p = std::norm(v); if (p > 1e-8) { ps += p; nz++; } }
    ps = nz ? ps / (double)nz : 0;
    const double sig = snrDb3k < 90 ? std::sqrt(4 * ps / std::pow(10.0, snrDb3k / 10) / 2) : 0;
    std::mt19937 rng(seed);
    std::normal_distribution<float> d(0, 1);
    for (size_t i = 0; i < y.size(); i++) {
        const double p = 2 * kPi * cfoHz * (double)i / kRate;
        y[i] *= cf32((float)std::cos(p), (float)std::sin(p));
        if (sig > 0) y[i] += cf32((float)(sig * d(rng)), (float)(sig * d(rng)));
    }
    return y;
}

}  // namespace dect2::w1a
