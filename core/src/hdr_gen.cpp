// HD Radio test signal: skeleton (see hdr_gen.h).
#include "dect2/hdr_gen.h"
#include "dect2/gen_util.h"
#include <cstddef>
#include <memory>

namespace dect2 {

namespace {
class HdrSynth : public ModeSynth {
public:
    explicit HdrSynth(double rate) : rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        noise_.add(out, n, 0.02f);
    }
private:
    double rate_;
    genutil::NoiseSource noise_{1};
};
} // namespace

std::unique_ptr<ModeSynth> makeHdrSynth(const SynthConfig&, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HdrSynth>(sampleRate);
}

} // namespace dect2
