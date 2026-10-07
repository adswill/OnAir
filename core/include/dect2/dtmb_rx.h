// DTMB receiver: DTMB / DTMB-A digital terrestrial television (China, Hong Kong), 8 MHz channel.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "dtmb_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class DtmbReceiver {
public:
    DtmbReceiver();
    ~DtmbReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(DtmbTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // The decoded transport stream: packets of 188 bytes, `secs` of signal time since the previous call. Runs on the receiver thread.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);
    // Waits for the decoder threads to finish what they hold and delivers the packets (command line tools and tests; not for the engine)
    void flush();
    // Number of threads of the LDPC decoder (default: about half of the cores, at most 4); takes effect at the next lock
    void setDecoderThreads(int n);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dtmbTuning();

} // namespace dect2
