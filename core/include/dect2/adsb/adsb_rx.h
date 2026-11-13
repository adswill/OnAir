// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ADS-B receiver: ADS-B / Mode S aircraft messages on 1090 MHz.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "adsb_tel.h"
#include "adsb_feedin.h"
#include "adsb_track.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class AdsbReceiver {
public:
    AdsbReceiver();
    ~AdsbReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output (the aircraft stay)
    void feed(const cf32* x, size_t n);
    bool telemetry(AdsbTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Setters for the interface; safe while feed() runs
    void setReference(double latDeg, double lonDeg);     // the receiver's position: local decoding of the first position, distance and bearing, surface positions
    void clearReference();
    void setCorrection(int bits);                        // error correction of DF11 / 17 / 18: 0 off, 1 one bit, 2 up to three bits among the least certain ones (the app default)
    void setDropSec(double seconds);                     // the display option: how long a silent aircraft stays in the table and on the map (0: the expiry above; a huge value: never)
    void setExpiry(double seconds);                      // an aircraft that has been silent this long leaves the table (default 60)
    // Feeds in from other receivers (Beast or SBS over TCP, "host:port" each): their aircraft go into the same table. Positions from other
    // receivers are shown, not computed (no MLAT). Hosts outside the local network are refused unless allowNonLan. Works with or without a radio running.
    void setExternalSources(const std::vector<std::string>& hostPorts, bool allowNonLan);
    void resetStats();                                   // the range-ring statistics only
    void clearAircraft();                                // empty the table and the statistics
    // The aircraft table and the message list stay through reset(), configure() and retunes (an aircraft still leaves it after the
    // expiry time of signal without a message): only clearResults() empties them at once
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    // Tests and tools: every good message, called from feed() (not under the receiver's lock)
    void setFrameCallback(std::function<void(const AdsbFrame&)> cb);
    void setThresholds(float pulseOverNoise, float pulseOverGap);   // preamble test, normally 3.0 and 2.0
    void setDetector(float sum, float minPulse, float data, float worthDb);   // test knob: AdsbDemod::setDetector
    void setCoherent(bool on);
    void setOverlap(bool on);                                            // test knob: overlapping-reply recovery (AdsbDemod::setOverlap), on by default
    // How much extra decoding the weak candidates get: -1 automatic (default: all passes, fewer when feed() takes more than half of the signal's
    // duration, back when it drops below a quarter), or 2 / 1 / 0 fixed (AdsbDemod::setEffort)
    void setEffort(int e);
    int effort() const;                                                  // test knob: AdsbDemod::setCoherent

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning adsbTuning();

} // namespace dect2
