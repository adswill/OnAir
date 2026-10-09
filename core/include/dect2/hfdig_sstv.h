// SSTV decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio. Skeleton: counts the audio, nothing is
// decoded yet. This header, hfdig_sstv.cpp and app/hfdig_sstv_ui.cpp belong to SSTV alone.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <memory>

namespace dect2 {

struct HfdigSstvTelemetry {
    int state = 0;                   // 0 no decoder yet
    uint64_t audioSamples = 0;       // audio samples fed since the last reset
};

class HfdigSstv : public HfdigDecoder {
public:
    HfdigSstv();
    ~HfdigSstv() override;
    void reset() override;
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigSstvTelemetry& out) const;   // the receiver thread, between feedAudio() calls

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<HfdigSstv> makeSstvDecoder();

} // namespace dect2
