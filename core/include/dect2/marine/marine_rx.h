// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine receiver: NAVTEX (SITOR-B FEC), DSC (MF/HF 100 bd and VHF channel 70 1200 bd) and HF weather fax on one tuned channel.
// The channel is mixed to 0 Hz (setSignalOffset), brought to 24 kHz complex, and the services run on it. HF signals are taken as on a
// communications receiver: the dial frequency is the carrier (FSK centre), fax is upper sideband with the tones above it.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "marine_tel.h"
#include "marine_fax.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class MarineReceiver {
public:
    MarineReceiver();
    ~MarineReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the NAVTEX and DSC lists stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(MarineTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // ---- controls (any thread)
    void setVhfNarrowChannel(bool on);                   // 4.0: VHF DSC reads through a 4.5 kHz channel filter with a carrier follow-up (default on; off = the 3.9 receiver)
    void setService(int s);                              // 0 auto (by frequency; all services when the frequency is not known), 1 NAVTEX, 2 DSC, 3 weather fax
    void setFrequencyHz(double hz);                      // the tuned (dial) frequency: picks the service in auto mode and MF/HF or VHF for DSC; 0 = unknown
    void setParallelDsc(const std::vector<double>& hz);   // extra MF/HF DSC carriers to decode at the same time; those outside the capture are ignored
    void setFaxLpm(int lpm);                             // 60, 90, 120, 240; 0 = detect
    void setFaxIoc(int ioc);                             // 576 or 288; 0 = from the start tone
    void setFaxSlantPpm(double ppm);
    void setFaxAutoSlant(bool on);
    void setListen(bool on);                             // weather fax: play its tones (the upper sideband) on the sound card (default off)
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but never open the sound device
    void clearFax();                                     // clears the weather fax picture and the decoder's lines; phasing and the line count restart (any thread)
    void clearMessages();                                // empties the NAVTEX and DSC lists
    // The NAVTEX and DSC lists stay through reset(), configure() and retunes: only clearResults() (clearMessages()) empties them
    void clearResults();                                 // the NAVTEX and DSC lists and the fax picture
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes

    // The fax picture: true and a copy when it changed since `seq` (updated to the new value).
    bool latestImage(FaxImage& out, uint64_t& seq) const;
    FaxStatus faxStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning marineTuning();

} // namespace dect2
