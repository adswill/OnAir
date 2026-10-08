// DTMB test signal (see dtmb_gen.h).
#include "dect2/dtmb_gen.h"
#include "dect2/demo_ts.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include "dtmb_simd.h"

namespace dect2 {
namespace dtmb {

namespace {
constexpr int kHalf = 24;                 // pulse span on each side, symbols
constexpr int kPhases = 2048;
constexpr size_t kPool = 1u << 19;
constexpr size_t kHist = 1u << 16;

uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
}

std::function<void(uint8_t*)> testPacketSource(uint32_t seed) {
    auto n = std::make_shared<uint32_t>(0);
    return [=](uint8_t* p) {
        const uint32_t k = (*n)++;
        p[0] = 0x47; p[1] = 0x01; p[2] = 0x00; p[3] = (uint8_t)(0x10 | (k & 15));
        p[4] = (uint8_t)(k >> 24); p[5] = (uint8_t)(k >> 16); p[6] = (uint8_t)(k >> 8); p[7] = (uint8_t)k;
        uint32_t s = (seed ^ (k * 0x9E3779B1u)) | 1u;
        for (int i = 8; i < 188; i++) p[i] = (uint8_t)(xorshift(s) >> 11);
    };
}

bool checkTestPacket(const uint8_t* p, uint32_t seed, uint32_t* number) {
    if (p[0] != 0x47 || p[1] != 0x01 || p[2] != 0x00) return false;
    const uint32_t k = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
    if (number) { if (*number != 0xFFFFFFFFu && *number != k) { *number = k; return false; } *number = k; }
    if (p[3] != (uint8_t)(0x10 | (k & 15))) return false;
    uint32_t s = (seed ^ (k * 0x9E3779B1u)) | 1u;
    for (int i = 8; i < 188; i++) if (p[i] != (uint8_t)(xorshift(s) >> 11)) return false;
    return true;
}

Signal::Signal(const SignalConfig& cfg, FrameTx::TsSource ts) : cfg_(cfg), tx_(cfg.tx, std::move(ts)) {
    phases_ = kPhases; taps_ = 2 * kHalf;
    table_.resize((size_t)phases_ * (size_t)taps_);
    for (int p = 0; p < phases_; p++) {
        const double f = (double)p / phases_;
        for (int k = 0; k < taps_; k++) table_[(size_t)p * (size_t)taps_ + (size_t)k] = (float)srrcPulse((kHalf - 1 - k) + f);
    }
    step_ = cfg.symbolRate / cfg.rate * (1.0 + cfg.sroPpm * 1e-6);
    dph_ = 2.0 * 3.14159265358979323846 * cfg.cfoHz / cfg.rate;
    rot_ = cf32((float)std::cos(dph_), (float)std::sin(dph_));
    // echoes: delays relative to the earliest path
    long mainDelay = 0;
    for (const auto& e : cfg.echoes) mainDelay = std::max(mainDelay, -(long)std::lround(e.delay));
    paths_.push_back({1.0, mainDelay});
    double power = 1.0;
    for (const auto& e : cfg.echoes) {
        const double g = std::pow(10.0, -e.attenuationDb / 20.0);
        paths_.push_back({g, mainDelay + (long)std::lround(e.delay)});
        power += g * g;
    }
    if (paths_.size() > 1) hist_.assign(kHist, cf32(0, 0));
    // noise: C/N in the symbol rate bandwidth; the noise is white over the whole output band
    const double nPow = cfg.snrDb > 150 ? 0.0 : power * std::pow(10.0, -cfg.snrDb / 10.0) * (cfg.rate / cfg.symbolRate);
    noiseSigma_ = (float)std::sqrt(nPow / 2.0);   // per component
    gain_ = (float)(cfg.rms / std::sqrt(power + nPow));
    if (nPow > 0) {
        std::mt19937 rng(cfg.seed * 2654435761u + 17u);
        std::normal_distribution<float> nd(0.f, 1.f);
        pool_.resize(kPool);
        for (auto& v : pool_) v = cf32(nd(rng), nd(rng));
    }
    rnd_ = cfg.seed * 747796405u + 2891336453u;
    if (!rnd_) rnd_ = 1;
    // start with a few frames so that the first output sample has its full pulse window
    tau_ = 0;
}

void Signal::ensure(long upTo) {
    // make symbols with index < upTo available
    const size_t fl = (size_t)tx_.frameLength();
    while (base_ + (long)re_.size() < upTo) {
        frame_.resize(fl);
        tx_.nextFrame(frame_.data());
        for (size_t i = 0; i < fl; i++) { re_.push_back(frame_[i].real()); im_.push_back(frame_[i].imag()); }
    }
}

void Signal::generate(cf32* out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const long i0 = (long)std::floor(tau_);
        ensure(i0 + kHalf + 1);
        const long first = i0 - kHalf + 1;
        // before the first symbol the pulse sums over zeros: pad the front
        float ar = 0.f, ai = 0.f;
        const int p = (int)std::lround((tau_ - (double)i0) * phases_);
        const float* w = &table_[(size_t)(p >= phases_ ? phases_ - 1 : p) * (size_t)taps_];
        const long off = first - base_;
        if (off >= 0) {
            dotRI(&re_[(size_t)off], &im_[(size_t)off], w, taps_, ar, ai);
        } else {
            for (int k = 0; k < taps_; k++) {
                const long j = off + k;
                if (j < 0) continue;
                ar += w[k] * re_[(size_t)j]; ai += w[k] * im_[(size_t)j];
            }
        }
        cf32 v(ar, ai);
        if (!hist_.empty()) {
            hist_[histPos_ & (kHist - 1)] = v;
            cf32 acc(0, 0);
            for (const Path& pa : paths_) acc += (float)pa.gain * hist_[(histPos_ - (size_t)pa.delay) & (kHist - 1)];
            histPos_++;
            v = acc;
        }
        // carrier offset
        v *= cur_;
        cur_ *= rot_;
        if ((++produced_ & 4095) == 0) cur_ /= std::abs(cur_);
        out[i] = v;
        tau_ += step_;
    }
    // noise and level, in chunks so that the pool is not walked in step with the signal
    if (noiseSigma_ > 0) {
        size_t i = 0;
        while (i < n) {
            const size_t len = std::min<size_t>(512, n - i);
            const uint32_t r = xorshift(rnd_);
            const size_t start = r & (kPool - 1);
            const float sgn = (r >> 31) ? -1.f : 1.f;
            for (size_t k = 0; k < len; k++) out[i + k] += noiseSigma_ * sgn * pool_[(start + k) & (kPool - 1)];
            i += len;
        }
    }
    for (size_t i = 0; i < n; i++) {
        float re = out[i].real() * gain_, im = out[i].imag() * gain_;
        re = std::max(-0.98f, std::min(0.98f, re)); im = std::max(-0.98f, std::min(0.98f, im));
        out[i] = cf32(re, im);
    }
    // drop symbols that are no longer needed
    const long keep = (long)std::floor(tau_) - kHalf - 2;
    if (keep - base_ > 65536) {
        const size_t d = (size_t)(keep - base_);
        re_.erase(re_.begin(), re_.begin() + (long)d);
        im_.erase(im_.begin(), im_.begin() + (long)d);
        base_ += (long)d;
    }
}

} // namespace dtmb

namespace {
class DtmbSynth : public ModeSynth {
public:
    DtmbSynth(const dtmb::SignalConfig& c, dtmb::FrameTx::TsSource ts, double rate) : sig_(c, std::move(ts)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { sig_.generate(out, n); }
private:
    dtmb::Signal sig_;
    double rate_;
};
}

std::unique_ptr<ModeSynth> makeDtmbSynth(const SynthConfig& cfg, double sampleRate) {
    using namespace dtmb;
    SignalConfig sc;
    sc.symbolRate = symbolRateFor(cfg.modeOpt[7] == 1 ? 6 : 8);   // modeOpt[7]: 1 = a 6 MHz channel
    if (sampleRate < sc.symbolRate * 1.0516 - 1) return nullptr;
    sc.rate = sampleRate;
    sc.snrDb = cfg.snrDb;
    sc.cfoHz = cfg.cfoHz;
    sc.sroPpm = cfg.sroPpm;
    sc.tx.header = cfg.modeOpt[0] == 1 ? Header::Pn595 : cfg.modeOpt[0] == 2 ? Header::Pn420 : Header::Pn945;
    static const Mapping maps[5] = {Mapping::Qam64, Mapping::Qam32, Mapping::Qam16, Mapping::Qam4, Mapping::Qam4Nr};
    sc.tx.profile.map = maps[std::max(0, std::min(4, cfg.modeOpt[1]))];
    static const Rate rates[3] = {Rate::R06, Rate::R04, Rate::R08};
    sc.tx.profile.rate = rates[std::max(0, std::min(2, cfg.modeOpt[2]))];
    if (!profileValid(sc.tx.profile)) sc.tx.profile.rate = Rate::R08;
    sc.tx.profile.mode2 = cfg.modeOpt[3] == 1;
    sc.tx.phaseRotate = cfg.modeOpt[5] == 0;
    if (cfg.modeOpt[4] == 1) { sc.tx.carriers = 1; sc.tx.header = Header::Pn595; sc.tx.phaseRotate = false; }   // single carrier is defined with PN595 only
    if (cfg.echoDb > 0) sc.echoes.push_back({cfg.echoDb, (double)cfg.echoDelay});
    if (cfg.modeVal[0] > 0) sc.echoes.push_back({cfg.modeVal[0], cfg.modeVal[1]});
    FrameTx::TsSource ts = cfg.modeOpt[6] == 1 ? testPacketSource(1) : demoTsSource(netBitrate(sc.tx.header, sc.tx.profile, sc.symbolRate));
    return std::make_unique<DtmbSynth>(sc, std::move(ts), sampleRate);
}

} // namespace dect2
