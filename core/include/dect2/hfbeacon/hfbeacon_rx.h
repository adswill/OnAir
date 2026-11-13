// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF beacons: a propagation monitor. No decoder: it watches the frequencies of the NCDXF/IARU International Beacon Project, the Long Dashes
// and the Russian single-letter beacons (an editable list, hfbeacon_list.h) in a wide HF capture, tells each from a random carrier by its
// keying and schedule, and keeps SNR and last-heard histories for 24 hours. The processing is in hfbeacon_dsp.h.
// Engine standard 69 (active 68). Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks), telemetry() and the setters from the interface thread.
#pragma once
#include "hfbeacon_list.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct HfbTelemetry;   // hfbeacon_tel.h

class HfbReceiver {
public:
    HfbReceiver();
    ~HfbReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double) {}
    void setCenterHz(double hz);         // the radio's frequency
    void setTimeOrigin(double utcSec, bool isClock);   // the time of the first sample: the computer clock for a radio (isClock), else a stream time from 0
    bool ready() const;
    void reset();                                      // after a retune: the processing restarts, the results stay
    void feed(const cf32* x, size_t n);
    bool telemetry(HfbTelemetry& out, uint64_t lastSeq);
    void clearResults();                               // the histories
    std::string saveResults() const;                   // for the results file (results_io.h)
    bool loadResults(const std::string& blob);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSilent(bool s);

    void setBeacons(const std::vector<hfb::Beacon>& b);   // the list (the user's edited JSON, or the default)
    void setObserver(bool have, double lat, double lon);
    void setSelected(int index);                       // the beacon whose history the telemetry carries
    void setPpm(double ppm);                           // the radio's frequency error: a signal at f is looked for at f * (1 + ppm * 1e-6)
    void setClockOffset(double sec);                   // the computer clock is `sec` seconds ahead of true UTC (NCDXF slots)
    // Listen: the beacon at RF `hz` as USB audio (the carrier a 700 Hz tone) on the sound card, off until switched on; the beacon must be in the capture
    void setListen(bool on);
    void setListenFreqHz(double hz);
    bool listening() const;
    void setVolume(float v);
    void setMuted(bool m);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hfbeaconTuning();
std::string hfbeaconSummary(const HfbTelemetry& t);

} // namespace dect2
