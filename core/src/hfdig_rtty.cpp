// RTTY decoder of the HF digital receiver: skeleton (see hfdig_rtty.h), and its test audio for the generator (hfdig_gen.h).
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_gen.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {

struct HfdigRtty::Impl {
    HfdigRttyTelemetry tel;
};

HfdigRtty::HfdigRtty() : p_(std::make_unique<Impl>()) {}
HfdigRtty::~HfdigRtty() = default;
void HfdigRtty::reset() { p_->tel = HfdigRttyTelemetry(); }
void HfdigRtty::feedAudio(const float*, size_t n) { p_->tel.audioSamples += n; }
void HfdigRtty::telemetry(HfdigRttyTelemetry& out) const { out = p_->tel; }

std::unique_ptr<HfdigRtty> makeRttyDecoder() { return std::make_unique<HfdigRtty>(); }

std::unique_ptr<HfdigTestAudio> makeRttyTestAudio(const SynthConfig&) { return nullptr; }   // no test signal yet

} // namespace dect2
