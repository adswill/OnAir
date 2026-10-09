// FreeDV decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio and gives back speech. It uses the
// open-source codec2 library (LGPL-2.1, github.com/drowe67/codec2), loaded at run time like the radio vendors' libraries: OnAir builds
// without codec2 headers and the decoder simply reports "not found" when the library is missing. The functions used are declared in
// hfdig_freedv.cpp from codec2's public freedv_api.h, release 1.2.0 (the mode constants are stable across 1.x).
// FreeDV 700D, 700E and 1600 listen to the same audio at the same time (cheap); the one that has sync (the first, in that order, and
// it stays until it has been lost for 2 s) makes the speech, the text channel (callsign) and the SNR shown.
// Library search: DECT2_CODEC2_PATH (a full path; dev and tests), then macOS Contents/Frameworks, /opt/homebrew/lib, /usr/local/lib;
// Linux the system folders; Windows codec2.dll / libcodec2.dll next to the program.
// Test audio (hfdig_gen.h, makeFreedvTestAudio): only with the library. SynthConfig::modeOpt[0] = 2 picks FreeDV (the shell), and
// modeOpt[1] picks the FreeDV mode: 0 = 700D (default), 1 = 700E, 2 = 1600. The transmission is a synthetic speech-like signal with
// the text "ONAIR TEST" in the text channel. Without the library makeFreedvTestAudio() returns nullptr and the shell sends noise.
// This header, hfdig_freedv.cpp and app/hfdig_freedv_ui.cpp belong to FreeDV alone.
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

constexpr int kFreedvModes = 3;   // index 0 700D, 1 700E, 2 1600
const char* freedvModeName(int idx);   // "700D", "700E", "1600"

struct HfdigFreedvTelemetry {
    int state = 0;                   // 0 library not found, 1 listening (no sync), 2 sync on a mode
    uint64_t audioSamples = 0;       // audio samples fed since the last reset
    bool libFound = false;
    std::string libPath;             // where the library was loaded from
    std::string libVersion;          // from its file name ("1.2"), empty when it has none
    int modeChoice = 0;              // the user's choice: 0 automatic, 1 700D, 2 700E, 3 1600
    int mode = -1;                   // the active mode (index as above), -1 = none has sync
    bool open[kFreedvModes] = {};    // the mode could be opened in this library
    bool sync[kFreedvModes] = {};
    float snrDb[kFreedvModes] = {};  // the library's estimate, valid while that mode has sync
    float snr = 0;                   // of the active mode
    std::string text;                // the text channel (callsign) received on the active mode: the last line, or the one in progress
    uint64_t speechFrames = 0;       // speech frames decoded with sync
    uint64_t bitErrors = 0;          // the library's count (meaningful for test frames only)
    uint64_t bits = 0;
};

// The user's choice of mode (0 automatic, 1 700D, 2 700E, 3 1600), shared by all decoders; any thread.
void hfdigFreedvSetMode(int choice);
int hfdigFreedvMode();
// Looks for the library once (a later call gives the same answer): true when found, and where.
bool hfdigFreedvLibrary(std::string* path = nullptr, std::string* version = nullptr);

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
