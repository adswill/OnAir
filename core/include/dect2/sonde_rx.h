// Radiosonde receiver: finds every sonde in the captured band (up to 8 at once), follows each on its own channel, tells the type from the
// frame sync and decodes it. Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis
// thread (never blocks: the work is done on the receiver's own thread), telemetry() and the setters from the interface thread.
//
// Per channel: mixer and two decimating filters (to about 100 kHz), FM discriminator, symbol timing per baud rate, and the frame
// decoders of all types (RS41 here; DFM, M10, M20 from sonde_bits.h), which are fed until one of them reports good frames.
#pragma once
#include "mode_tuning.h"
#include "sonde_tel.h"
#include "ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class SondeReceiver {
public:
    SondeReceiver();
    ~SondeReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the wanted channel sits relative to 0 Hz in the input
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state (the sonde list too)
    void feed(const cf32* x, size_t n);
    bool telemetry(SondeTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread

    // Setters for the interface; safe while feed() runs
    void setCenterMhz(double mhz);                       // the tuned centre frequency: lets the telemetry carry absolute frequencies
    void clearSondes();                                  // empty the list of sondes heard (channels keep following their carriers)
    void setMaxChannels(int n);                          // 1 to 8 (default 8)
    // Tests and tools
    void setSynchronous(bool on);                        // process inside feed() instead of on the worker thread (deterministic; set before the first feed)
    void flush();                                        // wait until everything fed so far has been processed (thread mode)
    uint64_t droppedSamples() const;                     // samples feed() could not queue (thread mode)
    double cpuSeconds() const;                           // CPU time of the processing thread so far (wall time where the system has no thread clock)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sondeTuning();

} // namespace dect2
