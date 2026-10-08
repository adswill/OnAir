// Marine receiver: NAVTEX (SITOR-B FEC), DSC (MF/HF 100 bd and VHF channel 70 1200 bd) and HF weather fax on one tuned channel.
// The channel is mixed to 0 Hz (setSignalOffset), brought to 24 kHz complex, and the services run on it. HF signals are taken as on a
// communications receiver: the dial frequency is the carrier (FSK centre), fax is upper sideband with the tones above it.
#pragma once
#include "mode_tuning.h"
#include "marine_tel.h"
#include "marine_fax.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class MarineReceiver {
public:
    MarineReceiver();
    ~MarineReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(MarineTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // ---- controls (any thread)
    void setService(int s);                              // 0 auto (by frequency; all services when the frequency is not known), 1 NAVTEX, 2 DSC, 3 weather fax
    void setFrequencyHz(double hz);                      // the tuned (dial) frequency: picks the service in auto mode and MF/HF or VHF for DSC; 0 = unknown
    void setFaxLpm(int lpm);                             // 60, 90, 120, 240; 0 = detect
    void setFaxIoc(int ioc);                             // 576 or 288; 0 = from the start tone
    void setFaxSlantPpm(double ppm);
    void setFaxAutoSlant(bool on);
    void clearMessages();                                // empties the NAVTEX and DSC lists

    // The fax picture: true and a copy when it changed since `seq` (updated to the new value).
    bool latestImage(FaxImage& out, uint64_t& seq) const;
    FaxStatus faxStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning marineTuning();

} // namespace dect2
