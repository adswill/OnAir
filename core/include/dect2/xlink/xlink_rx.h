// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Extra links (family 600, engine standard 602, telemetry 601): digital video and data links with their own entry each. wfb-ng and OpenHD now: 802.11n HT frames injected in monitor mode,
// structure only (the video stream is encrypted with keys this program never holds). Receive only.
//
// Engine contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/xlink/xlink_proto.h"
#include "dect2/xlink/xlink_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class XlinkReceiver {
public:
    XlinkReceiver();
    ~XlinkReceiver();
    void configure(double sampleRate);
    bool ready() const;                       // the capture is fast enough for the channel
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    void flush();
    bool telemetry(XlinkTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setProtocols(int mask);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning xlinkTuning();
ModeTuning xlinkNarrowTuning();   // the narrow-band members of the family

} // namespace dect2
