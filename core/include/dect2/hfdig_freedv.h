// FreeDV decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio and gives back speech. Skeleton: counts
// the audio, nothing is decoded and no speech is made yet. This header, hfdig_freedv.cpp and app/hfdig_freedv_ui.cpp belong to FreeDV alone.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace dect2 {

struct HfdigFreedvTelemetry {
    int state = 0;                   // 0 no decoder yet
    uint64_t audioSamples = 0;       // audio samples fed since the last reset
};

class HfdigFreedv : public HfdigDecoder {
public:
    HfdigFreedv();
    ~HfdigFreedv() override;
    void reset() override;
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigFreedvTelemetry& out) const;   // the receiver thread, between feedAudio() calls
    // Where the decoded speech goes: 8000 Hz mono, -1 .. 1, called from inside feedAudio(). The receiver sets it once; it plays the
    // speech on the sound card with the app's volume and mute.
    void setSpeechOut(std::function<void(const float* x, size_t n)> out);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<HfdigFreedv> makeFreedvDecoder();

} // namespace dect2
