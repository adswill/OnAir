// DRM AAC core decoder (see drm_aac.cpp): one audio frame of an AAC audio super frame in, 960 samples of mono sound at the core rate out.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

enum class AacFrameStatus { kOk, kDamaged, kUnsupported };

class AacCoreDecoder {
public:
    AacCoreDecoder();
    ~AacCoreDecoder();
    // rateHz: 12000 or 24000, the core rate of the stream (SBR, when present, is not decoded). false: not decodable here, why says it.
    bool configure(int rateHz, bool stereo, std::string* why = nullptr);
    bool open() const;
    int rateHz() const;
    void reset();                                           // lost frames: forget the overlap with the previous frame
    // One frame of the audio super frame with the CRC byte of its header. Appends 960 mono samples at the core rate; for a frame that is damaged (CRC, structure) or uses
    // something that is not handled (pulse data), 960 samples of silence.
    AacFrameStatus frame(const uint8_t* data, int len, uint8_t crc, std::vector<float>& out);
    uint64_t framesDamaged() const;
    uint64_t framesUnsupported() const;
    uint64_t framesDecoded() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}} // namespace dect2::drm
