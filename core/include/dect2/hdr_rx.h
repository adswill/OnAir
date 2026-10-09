// HD Radio (NRSC-5, USA): the digital sidebands of hybrid FM (MP1) and AM (MA1) stations. Decodes everything but the sound: station
// information (SIS), the program list, program service data (ID3), the station information guide and the large objects (pictures).
// The audio is HDC, a patented codec that OnAir does not decode: its packets are counted only.
// FM or AM is found by itself: both receivers search until one of them has block sync.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the other getters from the interface thread.
#pragma once
#include "mode_tuning.h"
#include "hdr_tel.h"
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class HdrReceiver {
public:
    HdrReceiver();
    ~HdrReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal and the decoded state
    void feed(const cf32* x, size_t n);
    bool telemetry(HdrTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // The bytes of a complete large object (LOT), e.g. a picture listed in HdrTelemetry::lots; false when not (yet) complete.
    bool lotBytes(int port, int lot, std::vector<uint8_t>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hdrTuning();

} // namespace dect2
