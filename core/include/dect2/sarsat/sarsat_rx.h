// COSPAS-SARSAT 406 MHz distress beacon receiver: burst finder (a narrow carrier that appears for half a second), carrier frequency estimate from the
// 160 ms unmodulated preamble, then 400 bit/s Manchester phase demodulation (+-1.1 rad) with sync search over timing, bit rate and polarity, and the
// message layer in sarsat_proto.h. Receive only.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread,
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "dect2/sarsat/sarsat_proto.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct SarsatTelemetry;

namespace sarsat {
constexpr double kFs = 32000.0;   // the rate the burst processing works at
// The demodulator on its own: x holds a burst at kFs with its carrier at carrierHz; fills the message when a sync pattern and the first BCH check pass.
bool demodBurst(const std::vector<cf32>& x, double carrierHz, Beacon& out);
}

class SarsatReceiver {
public:
    SarsatReceiver();
    ~SarsatReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(SarsatTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Search the whole capture (issue #214: the satellites' L-band downlink, each satellite somewhere in 1544 - 1545 MHz): a bank of
    // burst finders 90 kHz apart across up to 1.1 MHz of the input (as much as the sample rate holds), merged into one beacon list.
    void setWideSearch(bool on);
    bool wideSearch() const;
    double wideSpanHz() const;          // what the bank covers now (0: off)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning sarsatTuning();

} // namespace dect2
