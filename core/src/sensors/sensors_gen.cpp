// Sub-GHz sensors test signal, see sensors_gen.h
#include "dect2/sensors/sensors_gen.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

PulseList sensorsNexusPulses(int id, int channel, bool batteryOk, double tempC, int humidity, int repeats) {
    // [id 8][B T CC][temp 12 signed tenths][1111][humidity 8]
    const int t10 = (int)std::lround(tempC * 10.0) & 0xfff;
    uint64_t v = 0;
    v = (uint64_t)(id & 0xff);
    v = (v << 4) | (uint64_t)((batteryOk ? 8 : 0) | ((channel - 1) & 3));
    v = (v << 12) | (uint64_t)t10;
    v = (v << 4) | 0xf;
    v = (v << 8) | (uint64_t)(humidity & 0xff);
    PulseList p;
    for (int r = 0; r < repeats; r++) {
        for (int i = 35; i >= 0; i--) {
            const bool bit = (v >> i) & 1;
            p.push_back({500.0, bit ? 2000.0 : 1000.0});
        }
        p.push_back({500.0, 4000.0});   // the closing pulse: the gap after it is the sync gap that ends the row
    }
    return p;
}

PulseList sensorsUnknownPwmPulses(uint32_t bits, int nbits, int repeats) {
    PulseList p;
    for (int r = 0; r < repeats; r++)
        for (int i = nbits - 1; i >= 0; i--)
            p.push_back({((bits >> i) & 1) ? 820.0 : 280.0, i == 0 ? 6000.0 : 610.0});
    return p;
}

std::vector<cf32> sensorsOokSignal(const PulseList& p, double rate, double amp, double leadUs, double tailUs) {
    std::vector<cf32> out;
    const double sps = rate / 1e6;
    const size_t ramp = std::max<size_t>(1, (size_t)(6.0 * sps));
    out.assign((size_t)(leadUs * sps), cf32(0, 0));
    for (const auto& pg : p) {
        const size_t np = (size_t)std::lround(pg.first * sps), ng = (size_t)std::lround(pg.second * sps);
        for (size_t i = 0; i < np; i++) {
            const double e = std::min({1.0, (double)(i + 1) / (double)ramp, (double)(np - i) / (double)ramp});
            out.push_back(cf32((float)(amp * e), 0));
        }
        out.insert(out.end(), ng, cf32(0, 0));
    }
    out.insert(out.end(), (size_t)(tailUs * sps), cf32(0, 0));
    return out;
}

namespace {
class SensorsSynth : public ModeSynth {
public:
    SensorsSynth(const SynthConfig& s, double rate) : rate_(rate), cfo_(s.cfoHz), noUnknown_(s.modeOpt[0] == 1), rng_(7) {
        // SNR in the sensor channel (250 kHz): the noise per sample follows the sample rate
        const double snr = s.snrDb >= 99 ? 99 : s.snrDb;
        const double sigPow = amp_ * amp_ / 2;   // mean power of the on-off carrier
        noiseSigma_ = snr >= 99 ? 0.0 : std::sqrt(sigPow / std::pow(10.0, snr / 10.0) * (rate / 250e3) / 2);
        sensors_ = {{0x6a, 1, true, 21.4, 47}, {0x2c, 2, true, -3.6, 81}, {0xb1, 3, false, 18.9, 33}};
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        std::normal_distribution<float> nd(0.f, 1.f);
        for (size_t i = 0; i < n; i++) {
            if (pos_ >= cur_.size()) next();
            cf32 v = cur_[pos_++];
            if (cfo_ != 0) {
                ph_ += 2 * math::kPi * cfo_ / rate_;
                if (ph_ > math::kPi) ph_ -= 2 * math::kPi;
                v *= cf32((float)std::cos(ph_), (float)std::sin(ph_));
            }
            if (noiseSigma_ > 0) v += cf32(nd(rng_) * (float)noiseSigma_, nd(rng_) * (float)noiseSigma_);
            out[i] = v;
        }
    }
private:
    void next() {
        pos_ = 0;
        const int k = (int)(count_++ % 7);
        PulseList p;
        if (k == 6 && !noUnknown_) {
            p = sensorsUnknownPwmPulses(0x5a3c96e1u, 32, 6);
        } else {
            const auto& s = sensors_[(size_t)(k % 3)];
            // the temperature drifts a little between the messages
            const double t = s.temp + 0.1 * (double)(count_ / 7 % 5);
            p = sensorsNexusPulses(s.id, s.ch, s.batt, t, s.hum, 12);
        }
        cur_ = sensorsOokSignal(p, rate_, amp_, 300000.0, 100000.0);
    }
    struct Sens { int id, ch; bool batt; double temp; int hum; };
    double rate_, cfo_, ph_ = 0, amp_ = 0.5, noiseSigma_ = 0;
    bool noUnknown_;
    std::mt19937 rng_;
    std::vector<Sens> sensors_;
    std::vector<cf32> cur_;
    size_t pos_ = 0;
    uint64_t count_ = 0;
};
} // namespace

std::unique_ptr<ModeSynth> makeSensorsSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<SensorsSynth>(cfg, sampleRate); }

} // namespace dect2
