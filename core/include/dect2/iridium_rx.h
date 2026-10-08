// Iridium receiver: bursts over the whole capture (FFT detector), each burst demodulated on a worker thread (DQPSK, unique word),
// its bits handed to the frame layer (iridium_frame.h), the results kept as telemetry.
#pragma once
#include "mode_tuning.h"
#include "iridium_frame.h"
#include "iridium_tel.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class IridiumReceiver {
public:
    IridiumReceiver();
    ~IridiumReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(IridiumTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    void setCenterMhz(double mhz);       // the tuned centre (default 1622): absolute burst frequencies, simplex channels
    void setThresholdDb(double db);      // burst detection threshold over the noise floor per FFT bin (default 13)
    // Files and tests: feed() waits for the worker instead of dropping bursts when it falls behind (never in the app).
    void setOffline(bool on);
    void flush();                        // waits until every burst found so far is demodulated and decoded
    // Every burst with a unique word, with its frame (tool output, tests). Called on the worker thread.
    void setBurstCallback(std::function<void(const IridiumBurstBits&, const IridiumFrame&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning iridiumTuning();

} // namespace dect2
