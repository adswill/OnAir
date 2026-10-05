// ISDB-T receiver: blind detection of the mode (carrier spacing) and guard interval, symbol and carrier tracking, the TMCC signalling for
// the frame and the parameters, then the demodulator core. Reports through the same telemetry as the DVB-T receiver.
#pragma once
#include "t2rx.h"
#include <functional>
#include <memory>
#include <cstdint>

namespace dect2 {

class IsdbtReceiver {
public:
    IsdbtReceiver();
    ~IsdbtReceiver();
    void configure(double inputRateHz);
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(RxTelemetry& out, uint64_t lastSeq);
    // Called with decoded 188-byte transport packets (transport_error_indicator set on those that could not be repaired) and the amount of
    // signal time they represent.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t count, double seconds)> cb);
    // 0 nothing seen, 1 a periodic cyclic prefix was found, 2 TMCC decoded, 3 transport stream flowing
    int detectLevel() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
