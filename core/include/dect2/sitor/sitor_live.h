// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SITOR for the engine (family 119, active standard 120, engine standard 121): IQ in, SITOR-B (FEC, collective broadcast, as used by
// NAVTEX) text out. The FSK front end, tone finder and slicer are the marine ones; the framing is NavtexDecoder's (bit phase search over
// the 7-bit words, time diversity of the repeated characters, either tone polarity) with its character callback, so the text is shown
// without the ZCZC message framing. SITOR-A / AMTOR (ARQ) is read by a monitor on the same bits (sitor_arq.h): both stations of the
// link, the repeats dropped by following the control signals.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/marine/marine_navtex.h"
#include "dect2/sitor/sitor_arq.h"
#include "dect2/sitor/sitor_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

class SitorLiveReceiver {
public:
    SitorLiveReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(SitorTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    mutable std::mutex mu_;
    marine::Front front_;
    marine::Spectrum spec_;
    marine::FskSlicer fsk_;
    marine::NavtexDecoder dec_[2] = {marine::NavtexDecoder(false), marine::NavtexDecoder(true)};
    std::string text_[2];
    sitor::ArqMonitor arq_[2] = {sitor::ArqMonitor(false), sitor::ArqMonitor(true)};
    std::string arqLog_[2];
    std::vector<cf32> base_;
    double rate_ = 0.0, offsetHz_ = 0.0;
    bool ready_ = false;
    uint64_t seq_ = 0;
    SitorTelemetry snap_;
    std::function<void(const std::string&)> log_;
    marine::Spectrum::Fsk info_;
    void publish();
};

ModeTuning sitorTuning();

} // namespace dect2
