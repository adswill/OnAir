// CDR test signal: skeleton (see cdr_gen.h).
#include "dect2/cdr_gen.h"
#include "dect2/gen_util.h"
#include <cstddef>
#include <memory>

namespace dect2 {

namespace {
class CdrSynth : public ModeSynth {
public:
    explicit CdrSynth(double rate) : rate_(rate) {}
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

std::unique_ptr<ModeSynth> makeCdrSynth(const SynthConfig&, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<CdrSynth>(sampleRate);
}

} // namespace dect2
