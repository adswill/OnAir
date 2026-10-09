// FreeDV decoder of the HF digital receiver: skeleton (see hfdig_freedv.h), and its test audio for the generator (hfdig_gen.h).
#include "dect2/hfdig_freedv.h"
#include "dect2/hfdig_gen.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace dect2 {

struct HfdigFreedv::Impl {
    HfdigFreedvTelemetry tel;
    std::function<void(const float*, size_t)> speech;   // setSpeechOut(): where decoded speech goes (unused until there is a decoder)
};

HfdigFreedv::HfdigFreedv() : p_(std::make_unique<Impl>()) {}
HfdigFreedv::~HfdigFreedv() = default;
void HfdigFreedv::reset() { p_->tel = HfdigFreedvTelemetry(); }
void HfdigFreedv::feedAudio(const float*, size_t n) { p_->tel.audioSamples += n; }
void HfdigFreedv::telemetry(HfdigFreedvTelemetry& out) const { out = p_->tel; }
void HfdigFreedv::setSpeechOut(std::function<void(const float* x, size_t n)> out) { p_->speech = std::move(out); }

std::unique_ptr<HfdigFreedv> makeFreedvDecoder() { return std::make_unique<HfdigFreedv>(); }

std::unique_ptr<HfdigTestAudio> makeFreedvTestAudio(const SynthConfig&) { return nullptr; }   // no test signal yet

} // namespace dect2
