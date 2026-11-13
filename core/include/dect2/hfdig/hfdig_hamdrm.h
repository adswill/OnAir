// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HamDRM / EasyPal decoder of the HF digital receiver (hfdig_rx.h): digital SSTV as QSSTV and EasyPal send it, a DRM-style OFDM signal in
// a 2.3 or 2.5 kHz SSB channel carrying a file (usually a JPEG picture) in MOT-style segments, sent several times so that a receiver can
// fill in what it missed. This header, hfdig_hamdrm*.cpp and app/hfdig/hfdig_hamdrm_ui.cpp belong to HamDRM alone.
//
// The modem reuses the DRM receiver's parts: the robustness mode timing (drm::modeParams), the FFT, the multilevel code and its rate
// tables (MLC with the DRM punctured convolutional code), the QAM maps, the interleaver, the energy dispersal and the CRCs.
//   modes        A (Tu 24 ms, Tg 2.7 ms), B (Tu 21.3 ms, Tg 5.3 ms), E (Tu 14.7 ms, Tg 5.3 ms: DRM mode C timing, the robust one)
//   occupancy    2.3 or 2.5 kHz, centred on 1600 Hz of the audio
//   QAM          4, 16 or 64 on the data cells, protection high or normal
//   frame        Ns symbols (15, mode E 20): a known reference symbol, then data with scattered pilots; the frame information (mode,
//                occupancy, QAM, protection) in 4-QAM cells of the second symbol
//   data         one data group per frame: header (type, transport id, segment, last segment, length), the segment, CRC-16
//   file         a header segment (size, content type, file name, callsign) and the body segments; repeated transmissions fill the gaps
// Not tested against QSSTV or EasyPal recordings (none at hand): the cell layout and the data groups follow the description above, not
// their exact bit layout.
//
// Test audio (hfdig_gen.h; modeOpt[0] = 7 picks HamDRM):
//   modeOpt[1]  mode 1 A, 2 B, 3 E (0: B)             modeOpt[2]  occupancy 1 2.3 kHz, 2 2.5 kHz (0: 2.5 kHz)
//   modeOpt[3]  QAM 1 4, 2 16, 3 64 (0: 16)            modeOpt[4]  >0: leave out every n-th body segment in the first transmission
//   modeOpt[5]  transmissions before the pause (0 = 2) modeVal[0]  tuning error of the audio in Hz
// The file is hamdrmTestPicture() (a 64x48 JPEG), "onair-test.jpg" from LY0AIR.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SynthConfig;
class HfdigTestAudio;

constexpr int kHamdrmHistory = 8;
constexpr int kHamdrmMaxMissing = 64;    // missing segment numbers listed at most

const char* hamdrmModeName(int mode);    // "A", "B", "E"
double hamdrmOccupancyKhz(int occ);      // 2.3, 2.5
int hamdrmQam(int qam);                  // 4, 16, 64

struct HamdrmFile {
    uint64_t id = 0;                     // grows with every file
    std::string name;                    // the file name the sender gave ("" when its header was never received)
    std::string callsign;
    std::string mime;                    // "image/jpeg", "image/png", ... from the content type
    int64_t unixTime = 0;                // when it was complete
    int transmissions = 0;               // how many it took
    std::vector<uint8_t> data;           // the file, byte for byte
};

struct HfdigHamdrmTelemetry {
    int state = 0;                       // 0 searching, 1 receiving frames, 2 the transmission ended (the signal went away)
    uint64_t audioSamples = 0;
    int mode = -1, occ = -1, qam = -1, prot = -1;   // of the signal being received (or the last one); -1 unknown
    float snrDb = 0;                     // from the data cells
    double offsetHz = 0;                 // tuning error measured on the OFDM signal
    uint64_t framesOk = 0, framesBad = 0;
    // the file being received (or the last one)
    bool haveFile = false;
    uint16_t transportId = 0;
    std::string name, callsign;
    int64_t bodySize = 0;                // from the header segment, 0 unknown
    int segTotal = 0, segHave = 0;       // body segments: all (0 = not known yet), received
    std::vector<int> missing;            // the body segment numbers still missing (at most kHamdrmMaxMissing), known once the last one was seen
    int transmissions = 0;               // transmissions of this file heard so far
    bool fileDone = false;
    uint64_t filesDone = 0;
    std::vector<std::shared_ptr<const HamdrmFile>> history;   // finished files, newest first, up to kHamdrmHistory
};

class HfdigHamdrm : public HfdigDecoder {
public:
    HfdigHamdrm();
    ~HfdigHamdrm() override;
    void reset() override;                            // the signal is forgotten; segments already received and finished files stay
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigHamdrmTelemetry& out) const;  // the receiver thread, between feedAudio() calls
    void setLogCallback(std::function<void(const std::string&)> cb);   // "transmission ended ... missing ...", "received ..."
    void clearFiles();                                // drops the partly received files too
    // the receiver thread: the finished files from a results file, or none (the user's Clear)
    void setHistory(std::vector<std::shared_ptr<const HamdrmFile>> h);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
std::unique_ptr<HfdigHamdrm> makeHamdrmDecoder();

// The transmitter (test audio and tests): the file sent `repeats` times with a pause after each round, endless
struct HamdrmTxConfig {
    int mode = 1, occ = 1, qam = 1, prot = 1;   // prot 0 high, 1 normal
    std::string name = "onair-test.jpg", callsign = "LY0AIR", mime = "image/jpeg";
    uint16_t transportId = 1;
    int repeats = 2;                     // transmissions in one round
    int dropEvery = 0;                   // >0: the first transmission of a round leaves out body segments n-1, 2n-1, ...
    double offsetHz = 0;                 // tuning error of the audio
    double pauseSec = 2.0;               // silence after each transmission
    double leadSec = 0.3;                // silence at the very start
};
std::unique_ptr<HfdigTestAudio> makeHamdrmTx(const HamdrmTxConfig& cfg, const std::vector<uint8_t>& file);
int hamdrmSegmentBytes(int mode, int occ, int qam, int prot);   // file bytes one body segment carries
int hamdrmBodySegments(const HamdrmTxConfig& cfg, size_t fileBytes);
const std::vector<uint8_t>& hamdrmTestPicture();

} // namespace dect2
