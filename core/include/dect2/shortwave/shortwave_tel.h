// Shortwave mode telemetry, shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning (they follow the general receiver's).
#pragma once
#include "dect2/general/general_tel.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// A picture a decoder made (SSTV, Hellschreiber): RGB, or grey when gray
struct SwImage {
    int w = 0, h = 0;
    bool gray = false;
    std::vector<uint8_t> px;     // w * h * 3 (RGB) or w * h (grey)
};

// One running decoder, as the panel shows it
struct SwDecoderStatus {
    int id = 0;                  // swr::Dec
    int state = 0;               // 0 searching, 1 signal found, 2 decoding
    bool valid = false;          // it has produced something that checks out (text, a decode, a frame): what Auto looks at
    float quality = 0;           // 0 .. 1 when the decoder measures one
    uint64_t chars = 0;          // characters / decodes / frames so far
    std::string status;          // one line, English
    std::string text;            // the newest part of what was decoded (lines for the Lines decoders)
    std::string note;            // why it cannot work: "needs the Codec2 library", "the capture is too narrow"
    std::shared_ptr<const SwImage> image;
    uint64_t imageSeq = 0;
};

struct ShortwaveTelemetry {
    uint64_t seq = 0;
    int state = 0;
    double cfoHz = 0;
    float snrDb = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    float levelDb = -200;

    GeneralTelemetry general;              // the listening side: demodulator, S-meter, squelch, recording
    std::vector<SwDecoderStatus> decoders; // the running decoders, in table order
    double channelOffsetHz = 0;            // the listened channel from the input's 0 Hz (what the decoders follow)
    double inputRate = 0;
    int detected = -1;                     // swr::Dec lit by the identifier (-1 none)
    float detectedConfidence = 0;
    // Auto: 0 off, 1 trying the likely decoders, 2 finished
    int autoPhase = 0;
    int autoPick = -1;                     // the decoder that produced valid text (-1 none yet)
    double autoSec = 0;                    // seconds spent trying
};

}  // namespace dect2
