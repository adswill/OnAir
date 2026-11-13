// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio (NRSC-5, USA): the digital sidebands of hybrid FM (MP1, MP3) and AM (MA1) stations, and all-digital AM (MA3). Decodes everything but the sound: station
// information (SIS), the program list, program service data (ID3), the station information guide and the large objects (pictures).
// The audio is HDC, a patented codec that OnAir does not ship: its packets are counted, and played when the user has installed a patched
// FAAD2 (hdr_hdc.h, docs/CODECS.md).
// FM or AM is found by itself: both receivers search until one of them has block sync. Below 500 ksps only AM is searched for (an AM
// station is 30 kHz wide: the HF radios' slower rates, such as an Airspy HF+ at 912 or 768 ksps, are plenty).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the other getters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "hdr_tel.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class HdrReceiver {
public:
    HdrReceiver();
    ~HdrReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(HdrTelemetry& out, uint64_t lastSeq);
    // Sound (any thread): the program to hear (0 = HD1), volume 0..1, mute; silent = decode but never open the sound device
    void setProgram(int p);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setAudioTap(std::function<void(const float* stereo48k, size_t frames)> cb);   // tests: the sound that goes to the device
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // The bytes of a complete large object (LOT), e.g. a picture listed in HdrTelemetry::lots; false when not (yet) complete.
    bool lotBytes(int port, int lot, std::vector<uint8_t>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hdrTuning();                      // FM (the default); AM below 30 MHz takes the rates of hdrSampleRate()
constexpr double kHdrFmMinRate = 1500000;   // the FM receiver needs the 400 kHz of both digital sidebands and room for the filters
constexpr double kHdrAmMinRate = 200000;    // the AM receiver needs +-15 kHz; the resampler takes any rate from here
double hdrSampleRate(double mhz);           // the rate to ask the radio for: AM (below 30 MHz) 1 Msps, FM the tuning table's 2 Msps
double hdrMinSampleRate(double mhz);        // the least rate that works there (AM kHdrAmMinRate, FM kHdrFmMinRate)
bool hdrIsAmBand(double mhz);               // below 30 MHz: the medium wave band (530 to 1710 kHz) and the HF radios that reach it

} // namespace dect2
