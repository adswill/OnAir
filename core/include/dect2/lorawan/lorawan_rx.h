// LoRaWAN monitor (wide capture): every channel of a regional plan (EU868, US915, AU915, AS923-1..4, EU433) that fits the radio's captured band is
// demodulated at once, at all the spreading factors the plan's data rates use (SF7 - SF12 at 125 kHz, SF7 at 250 kHz, SF8 at 500 kHz uplink,
// SF7 - SF12 at 500 kHz downlink), including the gateways' inverted-IQ frames, and the plan's FSK channel (DR7, 50 kbit/s GFSK: lorawan_fsk.h; synthetic
// tests only so far) as one more receiver over the same samples. The frames are parsed down to the MAC layer (lorawan_plan.h): message type,
// DevAddr, frame control, counter, port, MIC, MAC commands, join request / accept framing. RECEIVE ONLY and keyless: the payloads are encrypted with
// the devices' session keys and stay bytes, no key is handled, nothing is decrypted, no MIC is checked, nothing is sent.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the
// setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/lorawan/lorawan_tel.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class LorawanReceiver {
public:
    LorawanReceiver();
    ~LorawanReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the user's dial sits relative to 0 Hz in the input (-tuneOffsetHz)
    void setCenterHz(double hz);                         // the radio's centre frequency (0: not known: a file or the test signal, the plan's own centre is assumed)
    bool ready() const;
    void reset();                                        // after a retune: forget the signal (the frame log stays: clearResults)
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(LorawanTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setSettings(const LorawanSettings& s);          // any thread; takes effect at the next feed()
    LorawanSettings settings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning lorawanTuning();
// The frequency a plan is centred on when the radio's own is not known, and the one the dial is set to for it
double lorawanPlanCentreMhz(int plan);

} // namespace dect2
