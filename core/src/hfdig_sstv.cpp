// SSTV decoder of the HF digital receiver: skeleton (see hfdig_sstv.h), and its test audio for the generator (hfdig_gen.h).
#include "dect2/hfdig_sstv.h"
#include "dect2/hfdig_gen.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {

struct HfdigSstv::Impl {
    HfdigSstvTelemetry tel;
};

HfdigSstv::HfdigSstv() : p_(std::make_unique<Impl>()) {}
HfdigSstv::~HfdigSstv() = default;
void HfdigSstv::reset() { p_->tel = HfdigSstvTelemetry(); }
void HfdigSstv::feedAudio(const float*, size_t n) { p_->tel.audioSamples += n; }
void HfdigSstv::telemetry(HfdigSstvTelemetry& out) const { out = p_->tel; }

std::unique_ptr<HfdigSstv> makeSstvDecoder() { return std::make_unique<HfdigSstv>(); }

std::unique_ptr<HfdigTestAudio> makeSstvTestAudio(const SynthConfig&) { return nullptr; }   // no test signal yet

} // namespace dect2
