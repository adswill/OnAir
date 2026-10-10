// Analog TV receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One decoded picture (both fields woven together, or one field line-doubled when the receiver is set to bob)
struct AtvFrame {
    uint64_t seq = 0;            // grows with every picture
    int width = 0, height = 0;   // 768 x 576 for 625 lines, 640 x 480 for 525
    double fieldRate = 50;       // fields per second of the signal
    bool colour = false;         // the picture has colour (burst locked), otherwise it is grey
    int field = 0;               // the field that was completed last (0 = first = upper lines)
    std::vector<uint8_t> rgba;   // width * height * 4
};

struct AtvTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 partly locked (carrier and line sync), 2 locked and decoding (line and field sync)
    double cfoHz = 0;            // vision carrier offset from the place the standard gives it in the channel (6, 7 or 8 MHz layout: by sound spacing and system; 5.5 MHz sound follows setChannelWidth, default 8)
    float snrDb = 0;             // video signal-to-noise ratio: 0.7 V of luminance against the rms noise on the porches, in the video band
    int modulation = 0;          // 0 AM (broadcast), 1 FM video (FPV)
    bool fmSyncLow = true;       // FM video: the sync tip is the lowest frequency (found by trial)
    bool dataValid = false;      // a picture is coming out (line and field sync are locked)
    uint64_t blocksOk = 0, blocksBad = 0;   // fields decoded with all their lines and with missing or damaged lines since the start

    // ---- this mode's own fields below
    // what was found
    std::string system;          // "PAL B/G 625/50", "NTSC M 525/59.94", ... ("" until known)
    std::string colourSystem;    // "PAL", "NTSC", "SECAM", "PAL-M", "PAL-N", "mono" ("" until known)
    int lines = 0;               // 625, 525 or 0
    double fieldHz = 0;          // measured field rate
    bool colour = false;         // burst locked and the colour decoder on
    bool colourKiller = false;   // colour detected earlier but the burst is too weak now
    bool syncDetector = false;   // the carrier is locked and the picture is demodulated synchronously; false = envelope detector
    // carriers
    double visionHz = 0;         // vision carrier from the channel centre
    double soundHz = 0;          // sound carrier from the channel centre (0 = none found)
    double soundSpacingMhz = 0;
    float carrierDbfs = -120;    // peak (sync tip) vision carrier power, dB re full scale
    float carrierToNoiseDb = 0;  // carrier to noise in the video band, from the spectrum
    // line and field sync
    double lineHz = 0;           // measured line rate, in Hz of the sample clock
    double lineErrPpm = 0;       // against the standard (15625 or 15734.26 Hz); includes the radio's clock error
    float syncQuality = 0;       // 0..1: share of lines with a sync pulse where it was expected
    uint64_t lineCount = 0, fieldCount = 0, frameCount = 0;
    int fieldNo = 0;             // 0 first field, 1 second field (of the picture last completed)
    // levels (normalised: blanking 0, white 1)
    float syncDepthPct = 0;      // height of the sync pulses in percent of the peak carrier (nominal: 26 in 625 lines, 25 in 525; much less = compressed sync)
    float syncCompressionPct = 0; // sync compression found from the colour burst (it is as high as the sync pulse); the picture gain follows the burst when this is not 0
    float whitePeak = 0;         // highest picture level in the last frame (above about 1.1 means the sync is compressed or the carrier overdriven)
    float burstLevel = 0;        // burst amplitude, share of nominal
    float chromaPhaseErrDeg = 0; // burst phase error after the loop
    // sound
    bool soundPresent = false;
    float soundDevKhz = 0;       // peak deviation in the last moment
    float soundLevelDb = -120;   // audio level, dB re full scale
    // scope views
    std::vector<float> lineWave; // one line of video, about 512 points over one line period, normalised levels (sync at the left)
    std::vector<float> vbiWave;  // the vertical interval: about 512 points over 15 lines, around the field sync
    // the spectrum of what the receiver sees (relative power around the vision carrier, dB), -1.5 .. +7 MHz, for the UI
    std::vector<float> specDb;
    float specLoMhz = -1.5f, specHiMhz = 7.0f;
};

// One line for the command line and the log
inline std::string atvSummary(const AtvTelemetry& t) {
    char b[240];
    if (t.system.empty()) snprintf(b, sizeof b, "Analog TV: %s", t.state == 0 ? "no signal" : "carrier found");
    else snprintf(b, sizeof b, "Analog TV: %s%s  video SNR %.1f dB  sync %.0f%%  burst %.0f%%  sound %s  fields ok %llu bad %llu", t.system.c_str(),
                  t.state == 2 ? "" : " (not locked)", t.snrDb, t.syncQuality * 100, t.burstLevel * 100, t.soundPresent ? "yes" : "no",
                  (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    return b;
}

} // namespace dect2
