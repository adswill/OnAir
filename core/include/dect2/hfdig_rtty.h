// RTTY decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio and decodes ITA2 (Baudot) frequency shift
// keying: 1 start, 5 data (least significant bit first), 1.5 stop bits; 45.45, 50, 75 or 100 baud; shifts of 170, 200, 425, 450 or
// 850 Hz. Normal: the mark tone is the lower one. The decoder looks for the pair of tones itself (300 .. 3300 Hz) and locks to it.
// Pipeline: spectrum of the audio -> pair search at the chosen shift -> a matched filter (one bit time of integration) at each tone ->
// envelopes -> ATC decision (each envelope against its own slowly decaying peak) -> start bit edge, bit sampling, stop bit check.
// This header, hfdig_rtty.cpp and app/hfdig_rtty_ui.cpp belong to RTTY alone.
//
// Test audio (hfdig_gen.h, modeOpt[0] = 0): a looping RTTY transmission of kRttyTestText, amplitude 0.5, with the lower tone at 2125 Hz.
//   modeOpt[1]  baud index  (0 45.45, 1 50, 2 75, 3 100)
//   modeOpt[2]  shift index (0 170, 1 200, 2 425, 3 450, 4 850)
//   modeOpt[3]  1: reversed, the mark is the upper tone
//   modeVal[0]  frequency of the lower tone in Hz (0: 2125)
#pragma once
#include "hfdig_rx.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

constexpr int kRttyBauds = 4, kRttyShifts = 5;
constexpr double kRttyBaudTable[kRttyBauds] = {45.45, 50.0, 75.0, 100.0};
constexpr double kRttyShiftTable[kRttyShifts] = {170.0, 200.0, 425.0, 450.0, 850.0};
constexpr size_t kRttyTextMax = 8000;   // characters kept in the telemetry text

// what the test transmission sends in one loop (a line feed ends it; letters, figures and the shifts between them)
constexpr const char* kRttyTestText = "RYRYRY CQ CQ DE ONAIR TEST 0123456789 1/2 -.:? THE QUICK BROWN FOX\n";

struct HfdigRttyTelemetry {
    int state = 0;                   // 0 searching for a signal, 1 locked to a pair of tones, 2 receiving characters
    uint64_t audioSamples = 0;       // audio samples fed since the last reset
    std::string text;                // the decoded text, the last kRttyTextMax characters
    uint64_t chars = 0;              // characters decoded (without the shift codes)
    uint64_t framingErrors = 0;      // characters dropped because the start or stop bit was wrong
    double markHz = 0, spaceHz = 0;  // the tones found (0 while searching)
    double baud = 45.45, shiftHz = 170;
    bool reverse = false, unshiftOnSpace = false;
    float quality = 0;               // 0 .. 1: how clean the signal is
};

class HfdigRtty : public HfdigDecoder {
public:
    HfdigRtty();
    ~HfdigRtty() override;
    void reset() override;                           // keeps the settings
    void feedAudio(const float* x, size_t n) override;
    void telemetry(HfdigRttyTelemetry& out) const;   // the receiver thread, between feedAudio() calls

    // settings: any thread, taken up by the next feedAudio()
    void setBaudIndex(int i);          // kRttyBaudTable
    void setShiftIndex(int i);         // kRttyShiftTable
    void setReverse(bool r);
    void setUnshiftOnSpace(bool u);    // a space returns the figures shift to letters
    void clearText();
    int baudIndex() const;
    int shiftIndex() const;
    bool reverse() const;
    bool unshiftOnSpace() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<HfdigRtty> makeRttyDecoder();

} // namespace dect2
