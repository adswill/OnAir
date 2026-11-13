// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RTTY decoder of the HF digital receiver (hfdig_rx.h): gets the 8 kHz upper sideband audio and decodes ITA2 (Baudot) frequency shift
// keying: 1 start, 5 data (least significant bit first), 1.5 stop bits; 45.45, 50, 75 or 100 baud; shifts of 170, 200, 425, 450 or
// 850 Hz. Normal: the mark tone is the lower one. The decoder looks for the pair of tones itself (300 .. 3300 Hz) and locks to it.
// Pipeline: spectrum of the audio -> pair search at the chosen shift -> a matched filter (one bit time of integration) at each tone ->
// envelopes -> ATC decision (each envelope against its own slowly decaying peak) -> start bit edge, bit sampling, stop bit check.
// This header, hfdig_rtty.cpp and app/hfdig/hfdig_rtty_ui.cpp belong to RTTY alone.
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
#include <vector>

namespace dect2 {

constexpr int kRttyBauds = 4, kRttyShifts = 5;
constexpr double kRttyBaudTable[kRttyBauds] = {45.45, 50.0, 75.0, 100.0};
constexpr double kRttyShiftTable[kRttyShifts] = {170.0, 200.0, 425.0, 450.0, 850.0};
constexpr size_t kRttyTextMax = 8000;   // characters kept in the telemetry text

// what the test transmission sends in one loop (a line feed ends it; letters, figures and the shifts between them)
constexpr const char* kRttyTestText = "RYRYRY CQ CQ DE ONAIR TEST 0123456789 1/2 -.:? THE QUICK BROWN FOX\n";

// One guess of the signal's parameters (rttyAutoAnalyse): the pair of tones, shift, speed and polarity, with how well the text came out
struct RttyAutoHyp {
    double lowHz = 0;                // the lower tone, Hz
    double shiftHz = 0, baud = 0;    // from the tables
    int shiftIdx = 0, baudIdx = 0;
    bool reverse = false;            // the mark is the upper tone
    float score = 0;                 // good characters minus 2.5 per framing error, over the audio analysed
    uint32_t chars = 0, errors = 0;
};
// One click: finds the pair of tones (the two spectral lines, shift from the table), then runs the decoder over the audio for every speed and both
// polarities and ranks the guesses by the text they give. audio: 8000 Hz, a few seconds to about 20 s of a signal that is on. Best first, at most
// maxHyps; empty when nothing sends RTTY.
std::vector<RttyAutoHyp> rttyAutoAnalyse(const float* audio, size_t n, int maxHyps = 3);

class HfdigTestAudio;
// Custom RTTY audio (8000 Hz). Empty text sends kRttyTestText. reverse: the mark is the upper tone. USB/LSB is the HF mixer, not this flag.
std::unique_ptr<HfdigTestAudio> makeRttyTxAudio(const std::string& text, int baudIdx, int shiftIdx, bool reverse, double lowToneHz = 2125);

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
    int autoState = 0;               // auto lock: 0 not asked, 1 waiting for audio, 2 done and applied, 3 nothing found
    bool autoOn = false, autoLocked = false;   // Auto is on / it has found the signal and the decoder is on it
    std::vector<RttyAutoHyp> autoHyps;   // its guesses, best first (the first one is in use)
};

// One RTTY signal found by whole-span decoding (hfdig_wide.h): its own pair of tones, speed and text
struct RttyWideSignal {
    double lowHz = 0;                // the lower tone, Hz above the dial frequency of the radio (the whole span, not only the passband)
    double shiftHz = 0, baud = 0;
    bool reverse = false;            // the mark is the upper tone
    bool active = false;             // still on the air (else: gone, its text stays)
    bool resolved = false;           // the speed and polarity are settled (else: the best of several guesses is shown)
    uint64_t chars = 0, framingErrors = 0;
    float quality = 0;
    double lastSec = 0;              // receiver seconds of the last character
    std::string text;
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
    void setFixedPair(double lowToneHz);   // whole-span decoding: the pair is given (lower tone, Hz), the decoder does not search (0: it does)
    void setReverse(bool r);
    void setUnshiftOnSpace(bool u);    // a space returns the figures shift to letters
    // Auto: speed, shift and polarity follow the signal (on by default in the receiver). The analysis runs when the decoder has no good text
    // and re-runs when the text stops or turns to framing errors. setBaudIndex, setShiftIndex and setReverse turn it off: the user decides.
    void setAuto(bool on);
    bool autoOn() const;
    void autoLock();                   // one click: speed, shift and polarity from the last seconds of audio (needs about 6 s of signal), see rttyAutoAnalyse
    void clearText();
    void setText(const std::string& text, uint64_t chars);   // the receiver thread: a loaded results file (reset() keeps the text)
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
