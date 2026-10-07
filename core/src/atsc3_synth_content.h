// The programme of the ATSC 3.0 test signal (atsc3_synth.h): a looping test card and a two tone sound, encoded in memory with libav and cut into
// fragmented MP4, one fragment per second, the way a broadcaster hands them to ROUTE. Internal to the synthesizer.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace atsc3synth {

struct Content {
    std::vector<uint8_t> vinit, ainit;                 // ftyp + moov of the video and the audio component
    std::vector<std::vector<uint8_t>> vseg, aseg;      // moof + mdat of every one second slot of the loop
    uint32_t vTimescale = 0, aTimescale = 0;           // ticks per second of the two tracks
    int64_t vLoopTicks = 0, aLoopTicks = 0;            // duration of the whole loop in those ticks
    int slots = 0;                                     // fragments (seconds) per loop
    std::string videoEncoder, videoCodec;              // for example "libx265" and "hevc"
    double videoBitrate = 0, audioBitrate = 0;         // measured, bit/s
};

// codecPref: 0 automatic (HEVC, then H.264, then MPEG-2 video: the first encoder that works), 1 HEVC, 2 H.264, 3 MPEG-2 video.
// videoKbps: the bit rate asked from the video encoder. The result is made once per combination and kept. Null when nothing could be encoded.
std::shared_ptr<const Content> getContent(int codecPref, int videoKbps);

// A copy of fragment `slot` of the loop as it goes out in loop number `cycle`: the media time and the sequence number go on from the loops before,
// so that the stream never jumps back.
std::vector<uint8_t> shiftedFragment(const Content& c, bool video, int slot, uint64_t cycle);

} // namespace atsc3synth
} // namespace dect2
