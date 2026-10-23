// Test signals for whole-span TV: several satellite transponders (DVB-S / S2 / S2X, any symbol rate and roll-off) in one IF capture, each from
// the DVB-S test generator (dvbs_gen.h) at its own offset and level with its own named service, plus white noise, an optional IF tilt and 8-bit
// quantisation (an RTL-SDR). Streamed: seconds at 20 Msps need no memory.
#pragma once
#include "dect2/common/stable_random.h"
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/dvbt/dvbt_gen.h"
#include <cmath>
#include <complex>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace spantvtest {
using namespace dect2;

struct SatSpec {
    int standard = 2;          // 1 DVB-S, 2 DVB-S2
    int mod = dvbs::kQpsk;
    int rate = 5;              // DvbsTxConfig::rate
    double rs = 1e6;
    double rollOff = 0.35;
    double offsetHz = 0;
    double esn0Db = 14;        // of this carrier in the capture
    int serviceId = 1;
    std::string name = "Sat";
    bool pilots = false;
    bool inverted = false;
    unsigned seed = 1;
};

class SatCapture {
public:
    // tiltDb: the level falls by this much from the bottom to the top of the span (an LNB's IF); quantise: 8 bits at `full` (rms 0.25 by default)
    SatCapture(const std::vector<SatSpec>& sats, double rate, double tiltDb = 0, bool quantise = false, unsigned seed = 7) : rate_(rate), quant_(quantise), rng_(seed) {
        // one noise density for the span: the first carrier's Es/N0 sets it, the others get theirs through their level
        const double n0 = 1.0;   // noise power per Hz (relative)
        double total = 0;
        for (const SatSpec& s : sats) {
            dvbs::DvbsSignalConfig c;
            c.tx.standard = s.standard; c.tx.mod = s.mod; c.tx.rate = s.rate; c.tx.symbolRate = s.rs; c.tx.rollOff = s.rollOff; c.tx.pilots = s.pilots;
            c.sampleRate = rate; c.snrDb = 200; c.cfoHz = s.offsetHz; c.inverted = s.inverted; c.seed = s.seed;
            c.ts = dvbt::testTsSourceNamed(s.seed, s.serviceId, s.name);
            // carrier power = Es/N0 * N0 * Rs
            const double p = std::pow(10.0, s.esn0Db / 10) * n0 * s.rs;
            c.level = std::sqrt(p);
            total += p;
            src_.emplace_back(new dvbs::DvbsSignal(c));
        }
        noiseSigma_ = std::sqrt(n0 * rate / 2);
        total += n0 * rate;
        scale_ = (float)(0.25 / std::sqrt(total));   // the whole capture at an rms of 0.25 of full scale
        if (tiltDb != 0) {   // a 63-tap FIR whose gain falls linearly in dB from 0 at -fs/2 to -tiltDb at +fs/2 (frequency sampling, Hann window)
            const int N = 63;
            fir_.assign(N, cf32(0, 0));
            for (int n = 0; n < N; n++) {
                std::complex<double> acc = 0;
                for (int k = 0; k < N; k++) {
                    const double f = (double)(k - N / 2) / N;   // -0.5 .. 0.5 of the rate
                    const double g = std::pow(10.0, -tiltDb * (f + 0.5) / 20.0);
                    acc += g * std::polar(1.0, 2 * M_PI * f * (n - N / 2));
                }
                const double w = 0.5 - 0.5 * std::cos(2 * M_PI * (n + 0.5) / N);
                fir_[(size_t)n] = cf32((float)(acc.real() * w / N), (float)(acc.imag() * w / N));
            }
            hist_.assign(N - 1, cf32(0, 0));
        }
    }
    void pull(size_t n, std::vector<cf32>& out) {
        out.assign(n, cf32(0, 0));
        tmp_.resize(n);
        for (auto& s : src_) { s->generate(tmp_.data(), n); for (size_t i = 0; i < n; i++) out[i] += tmp_[i]; }
        dect2::normal_distribution<float> nd(0.f, (float)noiseSigma_);
        for (auto& v : out) v += cf32{nd(rng_), nd(rng_)};
        if (!fir_.empty()) {
            const size_t L = fir_.size();
            std::vector<cf32> x(hist_);
            x.insert(x.end(), out.begin(), out.end());
            for (size_t i = 0; i < n; i++) {
                cf32 acc(0, 0);
                for (size_t k = 0; k < L; k++) acc += fir_[k] * x[i + L - 1 - k];
                out[i] = acc;
            }
            hist_.assign(x.end() - (long)(L - 1), x.end());
        }
        for (auto& v : out) {
            v *= scale_;
            if (quant_) {
                auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
                v = cf32(q(v.real()), q(v.imag()));
            }
        }
    }

private:
    double rate_;
    bool quant_;
    std::mt19937 rng_;
    std::vector<std::unique_ptr<dvbs::DvbsSignal>> src_;
    std::vector<cf32> tmp_;
    double noiseSigma_ = 0;
    float scale_ = 1;
    std::vector<cf32> fir_, hist_;
};

} // namespace spantvtest
