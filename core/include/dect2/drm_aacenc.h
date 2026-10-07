// DRM AAC encoder for the test signal: mono AAC-LC with the 960 transform, written as a DRM audio frame (side information, then the spectral data in
// codeword reordering order, clause 5.4.1), and the test sounds it codes (melody, tone, silence, noise). libavcodec's AAC encoder cannot be used: it only
// makes 1024 sample frames and no HCR. The decoder side (drm_aac.h) reads these frames and hands them to libavcodec's AAC-LC decoder, which is the
// independent check that the frames are right.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 { namespace drm {

class AacFrameEncoder {
public:
    explicit AacFrameEncoder(int rateHz);      // 12000 or 24000
    ~AacFrameEncoder();
    void reset();                              // forget the previous block (the next frame overlaps with silence)
    // pcm: the next 960 samples of the mono signal (+-1.0 is full scale). The frame that comes out codes the 1920 samples of the previous and this block
    // (the usual 50 % overlap, so a frame lags the sound by one block). The frame is exactly budgetBytes long: the data are padded with zero bytes, and a
    // coarser quantiser is used until they fit. Returns false when no quantiser fits (budget too small), the frame is then silent but valid.
    // siBits: bits of the side information (what the CRC of the frame covers, and what has to sit in the higher protected part).
    bool encode(const float* pcm, int budgetBytes, std::vector<uint8_t>& frame, int* siBits = nullptr);
    int rateHz() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// Test sounds, a pure function of the sample index (so a second instance with the same parameters makes the same sound, which the receiver tests use as the reference)
enum class DrmTestSound { kMelody = 0, kTone = 1, kSilence = 2, kNoise = 3 };
// Fills n samples starting at sample index first of the sound at rate rateHz (12000 or 24000): peak below 0.5.
void drmTestSound(DrmTestSound kind, int rateHz, uint32_t seed, uint64_t first, int n, float* out);

}} // namespace dect2::drm
