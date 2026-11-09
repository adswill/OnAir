// Bluetooth Classic test signal. See btc_gen.h.
#include "dect2/btc/btc_gen.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDev = 160e3;

// Gaussian-filtered rectangular pulse of one symbol (BT 0.5), sampled at the rate, 3 symbols each side
std::vector<float> gaussPulse(double rate) {
    const double alpha = kPi * std::sqrt(2.0 / std::log(2.0)) * 0.5;
    const int half = (int)std::ceil(2.5 * rate / 1e6);
    std::vector<float> g;
    for (int i = -half; i <= half; i++) {
        const double t = (double)i * 1e6 / rate;   // in symbols
        g.push_back((float)(0.5 * (std::erf(alpha * (t + 0.5)) - std::erf(alpha * (t - 0.5)))));
    }
    return g;
}

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double rate) {
    const std::vector<float> g = gaussPulse(rate);
    const int half = (int)(g.size() / 2);
    const double sps = rate / 1e6;
    const size_t n = (size_t)(((double)bits.size() + 6) * sps);
    std::vector<float> f(n, 0.f);
    for (size_t k = 0; k < bits.size(); k++) {
        const double centre = ((double)k + 3.5) * sps;
        const float a = bits[k] ? 1.f : -1.f;
        for (int j = -half; j <= half; j++) {
            const long idx = (long)std::lround(centre) + j;
            if (idx >= 0 && (size_t)idx < n) f[(size_t)idx] += a * g[(size_t)(j + half)];
        }
    }
    std::vector<cf32> w(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        ph += 2 * kPi * kDev * f[i] / rate;
        w[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    return w;
}
} // namespace

std::vector<BtcGenDevice> btcTestScene() {
    std::vector<BtcGenDevice> v;
    auto resp = [&](uint32_t lap, uint8_t uap, uint16_t nap, uint32_t cod, int ch, double off, double per, double ph, int hc) {
        BtcGenDevice d; d.kind = BtcGenDevice::Response; d.channel = ch; d.offsetHz = off; d.period = per; d.phase = ph; d.hdrClk = hc;
        d.fhs.lap = lap; d.fhs.uap = uap; d.fhs.nap = nap; d.fhs.cod = cod; d.fhs.clk = 0x123456u + lap % 1000; d.fhs.scanRepMode = 1;
        v.push_back(d);
    };
    // the class of device: a phone (major 2, minor 3 smartphone; services telephony, object transfer) and a headset (audio, wearable headset)
    resp(0xA1B2C3, 0x5E, 0x1234, (1u << 22) | (1u << 20) | (2u << 8) | (3u << 2) | 1, 38, 20e3, 0.50, 0.10, 0x15);
    resp(0x0F0E0D, 0x7A, 0xC0DE, (1u << 21) | (4u << 8) | (1u << 2) | 1, 44, -30e3, 0.55, 0.22, 0x2A);
    BtcGenDevice scan; scan.kind = BtcGenDevice::Id; scan.lap = btc::kGiac; scan.channel = 41; scan.offsetHz = 10e3; scan.period = 0.45; scan.phase = 0.30; v.push_back(scan);
    BtcGenDevice p1; p1.kind = BtcGenDevice::Piconet; p1.lap = 0x3C5A96; p1.channel = 35; p1.offsetHz = -40e3; p1.period = 0.40; p1.phase = 0.05; v.push_back(p1);
    BtcGenDevice p2; p2.kind = BtcGenDevice::Piconet; p2.lap = 0xE17B24; p2.channel = 47; p2.offsetHz = 55e3; p2.period = 0.43; p2.phase = 0.17; v.push_back(p2);
    return v;
}

struct BtcGenerator::Impl {
    BtcGenConfig c;
    std::mt19937 rng;
    dect2::normal_distribution<float> g{0.f, 1.f};
    struct Dev { std::vector<cf32> wave; double offsetHz; double amp; double period, phase; std::vector<double> dummy; };
    std::vector<Dev> devs;
    std::vector<int> count;
    std::vector<cf32> wifi;
    uint64_t n = 0;
    float sigma = 0;

    explicit Impl(const BtcGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        std::mt19937 r2(cfg.seed + 77);
        for (const auto& d : c.devs) {
            std::vector<uint8_t> bits;
            if (d.kind == BtcGenDevice::Id) bits = btc::makeAccessCode(d.lap, false);
            else if (d.kind == BtcGenDevice::Response) bits = btc::makeFhsPacket(d.fhs, d.hdrClk);
            else {
                bits = btc::makeAccessCode(d.lap, true);
                for (int i = 0; i < 54 + 240; i++) bits.push_back((uint8_t)(r2() & 1));   // header and payload of some packet
            }
            bits.resize((size_t)std::max(8.0, std::floor((double)bits.size() * d.truncate)));
            Dev v;
            v.wave = modulate(bits, c.rate);
            v.offsetHz = d.offsetHz + (2402e6 + d.channel * 1e6 - c.centerHz) + c.cfoHz;
            v.amp = c.amp * std::pow(10.0, d.levelDb / 20);
            v.period = d.period; v.phase = d.phase;
            devs.push_back(std::move(v));
        }
        count.assign(devs.size(), 0);
        // noise: carrier power amp^2 over noise in 1 MHz; the noise fills the whole rate
        sigma = c.snrDb >= 99 ? 0.f : (float)(c.amp * std::sqrt(c.rate / 1e6 / std::pow(10.0, c.snrDb / 10) / 2.0));
    }

    void gen(cf32* out, size_t cnt) {
        for (size_t i = 0; i < cnt; i++) out[i] = sigma > 0 ? cf32(sigma * g(rng), sigma * g(rng)) : cf32(0, 0);
        const double t0 = (double)n / c.rate;
        for (size_t di = 0; di < devs.size(); di++) {
            const Dev& d = devs[di];
            const double dur = (double)d.wave.size() / c.rate;
            long j0 = (long)std::floor((t0 - dur - d.phase) / d.period);
            if (j0 < 0) j0 = 0;
            for (long j = j0;; j++) {
                const double ts = d.phase + (double)j * d.period;
                if (ts >= t0 + (double)cnt / c.rate) break;
                if (ts + dur <= t0) continue;
                const long s0 = (long)std::llround(ts * c.rate);
                for (size_t i = 0; i < cnt; i++) {
                    const long k = (long)(n + i) - s0;
                    if (k < 0 || (size_t)k >= d.wave.size()) continue;
                    const double a = 2 * kPi * d.offsetHz * (double)(n + i) / c.rate;
                    out[i] += (float)d.amp * d.wave[(size_t)k] * cf32((float)std::cos(a), (float)std::sin(a));
                }
                if (s0 >= (long)n && s0 < (long)(n + cnt)) count[di] = (int)j + 1;
            }
        }
        if (c.wifiAt >= 0) {   // a 20 MHz wide noise burst every second
            const long wl = (long)(c.wifiLen * c.rate);
            for (size_t i = 0; i < cnt; i++) {
                const double t = (double)(n + i) / c.rate - c.wifiAt;
                if (t < 0) continue;
                const double f = t - std::floor(t);
                if (f * c.rate < (double)wl) out[i] += (float)(c.amp * std::pow(10.0, c.wifiDb / 20) / std::sqrt(c.rate / 20e6)) * cf32(g(rng), g(rng));
            }
        }
        n += cnt;
    }
};

BtcGenerator::BtcGenerator(const BtcGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
BtcGenerator::~BtcGenerator() = default;
void BtcGenerator::generate(cf32* out, size_t n) { p_->gen(out, n); }
double BtcGenerator::timeSec() const { return (double)p_->n / p_->c.rate; }
int BtcGenerator::sent(size_t dev) const { return dev < p_->count.size() ? p_->count[dev] : 0; }

namespace {
class BtcSynth : public ModeSynth {
public:
    BtcSynth(const SynthConfig& s, double rate) : rate_(rate) {
        BtcGenConfig c;
        c.rate = rate; c.snrDb = s.snrDb; c.cfoHz = s.cfoHz; c.devs = btcTestScene();
        gen_ = std::make_unique<BtcGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<BtcGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeBtcSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<BtcSynth>(cfg, sampleRate);
}

} // namespace dect2
