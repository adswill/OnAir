// SSTV decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio, finds the VIS header (or, when it was missed,
// the line sync pulses at a known line period), and builds the picture line by line. Modes: Robot 36 and 72, Martin M1 and M2, Scottie
// S1, S2 and DX, PD50, PD90, PD120 and PD180, Wraase SC2-180. This header, hfdig_sstv.cpp (+ hfdig_sstv_*.cpp) and app/hfdig_sstv_ui.cpp
// belong to SSTV alone.
//
// The decoder reports one picture at a time (the one being received, or the last one) and the last kSstvHistory finished ones. Pictures
// are shared, never changed after they are handed out: the telemetry copy is cheap.
//
// Test audio (hfdig_gen.h; modeOpt[0] = 1 picks SSTV):
//   modeOpt[1]  mode, index into the table (sstvModeInfo(): 0 Robot 36, 1 Robot 72, 2 Martin M1, 3 Martin M2, 4 Scottie S1, 5 Scottie S2,
//               6 Scottie DX, 7 PD50, 8 PD90, 9 PD120, 10 PD180, 11 Wraase SC2-180)
//   modeOpt[2]  1: leave out the VIS header (the decoder then picks the mode from the line period)
//   modeVal[0]  tuning error of the audio in Hz (all tones move up by it); cfg.sroPpm: error of the sample clock in ppm
// The picture is sstvTestPicture(): colour bars, a grey gradient and "OnAir" in blocks; it is sent once with a pause before and after, over
// and over.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kSstvHistory = 8;

struct HfdigSstvImage {
    uint64_t id = 0;                 // one number per picture, the same for all its copies; grows with every picture
    std::string mode;                // "Robot 36", ...
    int width = 0, height = 0;
    int lines = 0;                   // picture rows decoded so far (height when complete)
    bool complete = false;           // all lines received
    double slantPpm = 0;
    int64_t unixTime = 0;            // when the picture started
    std::vector<uint8_t> rgb;        // width * height * 3, row by row
};

struct HfdigSstvTelemetry {
    int state = 0;                   // 0 idle (searching for a VIS header), 1 receiving, 2 done (the last picture is complete or was cut off)
    uint64_t audioSamples = 0;       // audio samples fed since the last reset
    std::string mode;                // of the picture being received or the last one
    int visCode = -1;                // VIS code that started it, -1 = none seen (mode picked from the line period)
    int width = 0, height = 0;
    int lines = 0;                   // picture rows decoded
    double slantPpm = 0;             // line period error of the signal against the receiver's clock
    double offsetHz = 0;             // tuning error measured on the sync pulses
    uint64_t imageSeq = 0;           // grows whenever `image` changed
    uint64_t picturesDone = 0;       // pictures finished since the program started
    std::shared_ptr<const HfdigSstvImage> image;                  // the picture being received, or the last one; null before the first
    std::vector<std::shared_ptr<const HfdigSstvImage>> history;   // finished pictures, newest first, up to kSstvHistory
};

// The modes the decoder knows, in the order of the test audio's modeOpt[1]
struct SstvModeInfo {
    int vis;                         // VIS code
    const char* name;
    int width, height;
    double lineMs;                   // time of one transmitted line (PD: two picture rows)
    double syncMs;                   // length of the 1200 Hz line sync pulse
};
int sstvModeCount();
const SstvModeInfo& sstvModeInfo(int index);

// The test picture at this size (colour bars, grey gradient, blocks), RGB bytes row by row
void sstvTestPicture(int width, int height, std::vector<uint8_t>& rgb);

class HfdigSstv : public HfdigDecoder {
public:
    HfdigSstv();
    ~HfdigSstv() override;
    void reset() override;                           // the picture being received is dropped; the finished ones stay
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigSstvTelemetry& out) const;   // the receiver thread, between feedAudio() calls

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<HfdigSstv> makeSstvDecoder();

} // namespace dect2
