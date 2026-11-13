// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio-control and mesh link monitor: Reticulum / RNode (LoRa), ExpressLRS 900 MHz (LoRa), MAVLink on SiK radios, FrSky and FlySky. Each of them is its own
// entry in the mode list (a protocol mask, setProtocols). Receive only: nothing is transmitted, nothing here can command a vehicle.
//
// LoRa links use the demodulator of the mesh receiver (mesh_lora.h): one channel filter per channel and bandwidth, a demodulator per spreading factor.
// The FSK links share one burst finder over the whole capture (rc_fsk.h): a hopping link is heard on every hop channel inside the captured band.
// ExpressLRS: without a binding phrase only structure is shown; the phrase is held in memory only and is only ever the one the user typed.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the setters
// from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/rclink/rc_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class RcLinkReceiver {
public:
    RcLinkReceiver();
    ~RcLinkReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);          // where the user's frequency sits in the samples
    void setCenterHz(double hz);              // the radio's centre (0: unknown, a file): the frequency shown with each frame
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    void flush();                             // tests, end of a file: decode what is still open
    bool telemetry(RcTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    // settings (any thread; applied at the next feed())
    void setProtocols(int mask);              // rclink::Proto bits
    void setBindingPhrase(const std::string& phrase);   // "" forgets it. Not stored anywhere.
    void setElrsDomain(int index);            // index into elrsDomains()
    void setLoraPlan(double bwHz, int sf);    // Reticulum: 0 = the automatic search
    void setSikAirRate(int kbps);             // 0 = try every SiK air rate
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct RcElrsDomain { const char* name; double startHz, stopHz; int channels; };
const std::vector<RcElrsDomain>& rcElrsDomains();       // FHSS.cpp, in the order of its table
ModeTuning rcLinkTuning();

} // namespace dect2
