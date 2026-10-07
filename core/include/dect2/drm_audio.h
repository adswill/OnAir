// DRM audio: from the logical frame of an audio stream to sound. xHE-AAC goes through libavcodec's USAC decoder; AAC (ER AAC with a 960 transform)
// has its own decoder (drm_aac.h), because libavcodec cannot read the DRM variant of that stream.
#pragma once
#include "dect2/drm_msg.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Describes the stream (SDC data entity type 9). Returns false when this build cannot decode it; info() then says why.
    bool configure(const SdcAudio& a, bool modeE);
    bool configured() const;
    bool decodable() const;                 // configure() worked and the decoder is open
    void reset();                           // frames were lost: forget the frames in progress and restart the decoder at the next frame that can start it

    // One logical frame of the stream without the 4 bytes of the text message. Appends 48 kHz interleaved stereo to out.
    void superFrame(const uint8_t* data, int len, int lenA, std::vector<float>& out);

    std::string info() const;               // "xHE-AAC, 32 kHz stereo" or why the stream cannot be played
    int state() const;                      // 0 not configured, 1 frames come but nothing is decoded, 2 decoding
    uint64_t framesOk() const;              // audio frames that passed their CRC (and decoded)
    uint64_t framesBad() const;             // audio frames lost, failing their CRC, or that the decoder refused
    uint64_t samples() const;               // 48 kHz samples produced

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// xHE-AAC: the AudioSpecificConfig (audio object type 42) that libavcodec takes as extradata, built from the SDC entity: the sampling rate and audio mode
// fields and the xHE-AAC static config (clause 5.3.2, a compact UsacConfig). Empty when the config cannot be read.
std::vector<uint8_t> xheAscFromSdc(const SdcAudio& a);

// xHE-AAC audio super frame (clause 5.3.1): the frames it completes, each with its CRC checked. The parser keeps the bytes of the frame in progress.
struct XheFrame { std::vector<uint8_t> au; bool crcOk = false; };
class XheSuperFrameParser {
public:
    void reset();
    // len: bytes of the audio super frame. Returns false when the header CRC fails or the directory is inconsistent (the frame in progress is dropped).
    bool parse(const uint8_t* data, int len, std::vector<XheFrame>& frames);
    int lastBorders() const { return lastBorders_; }
    int lastReservoir() const { return lastReservoir_; }
private:
    std::vector<uint8_t> buf_;         // bytes since the start of the frame in progress (empty: waiting for a frame border)
    bool inFrame_ = false;
    std::vector<uint8_t> tail_;        // the last 2 bytes of the previous payload (delayed borders point into them)
    int lastBorders_ = 0, lastReservoir_ = 0;
};

}} // namespace dect2::drm
