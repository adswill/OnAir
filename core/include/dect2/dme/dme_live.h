// DME (distance measuring equipment) for the engine: IQ in, the channel, the pair rate, the level and the Morse ident out.
// The envelope the receiver works on is the magnitude of the IQ at the channel (the simplest envelope; the pulses are 3.5 us wide,
// so the radio rate must be 2 Msps or more). The pulse detector is dme/dme_rx.h. Family 119, engine standard 121.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks
// for long), telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/dme/dme_rx.h"
#include "dect2/dme/dme_tel.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

class DmeLiveReceiver {
public:
    void configure(double inputRateHz);
    void setSignalOffset(double hz);            // not used: the beacon's pulses are found on the envelope of the channel
    void setCenterHz(double hz);                // the tuned frequency: gives the channel number
    bool ready() const;
    void reset();                               // the pulse detector and the ident; the engine calls it on a new start
    void feed(const cf32* x, size_t n);
    bool telemetry(DmeTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    mutable std::mutex mu_;
    dme::Receiver rx_;
    double rate_ = 0.0;
    bool ready_ = false;
    uint64_t seq_ = 0;
    double centerHz_ = 0.0;
    std::vector<float> env_;
    DmeTelemetry snap_;
    std::string lastIdent_;
    std::function<void(const std::string&)> log_;

    void publish();                             // copies the detector's state into snap_ (mu_ held)
};

ModeTuning dmeTuning();

} // namespace dect2
