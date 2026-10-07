// DVB-S/S2 receiver: DVB-S, DVB-S2 and DVB-S2X satellite television (L-band IF after the LNB).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
// The carrier must be near the centre of the input. Its symbol rate, roll-off, modulation and code rate are found by the receiver.
#pragma once
#include "dvbs_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class DvbsReceiver {
public:
    DvbsReceiver();
    ~DvbsReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(DvbsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // The decoded transport stream: packets of 188 bytes, `secs` of signal time since the previous call. Runs on the receiver thread.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);

    // ---- controls (any thread)
    void setSymbolRate(double hz);                       // 0 = find it from the spectrum
    void setStandardHint(int standard);                  // 0 automatic, 1 DVB-S only, 2 DVB-S2 only (S2X signalling is reported either way)
    void setRollOff(double rollOff);                     // 0 = automatic: the spectrum, then the stream's own signalling
    void setIsi(int isi);                                // DVB-S2 multiple input streams: the stream to decode, -1 the first one seen
    void setPlScrambling(int n);                         // DVB-S2 physical layer scrambling code, -1 = 0 (the default for broadcast)
    void setBlocking(bool blocking);                     // recordings: wait for the decoder threads instead of dropping frames when behind
    void flush();                                        // waits until everything fed so far is decoded and delivered (recordings, tests; not for the engine)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dvbsTuning();

// The highest symbol rate the receiver accepts at an input rate: about 1.3 samples per symbol times (1 + roll-off) of bandwidth
double dvbsMaxSymbolRate(double inputRateHz, double rollOff = 0.35);
// The lowest symbol rate that the spectrum analysis can measure at an input rate (below it a manual symbol rate is needed)
double dvbsMinAutoSymbolRate(double inputRateHz);

} // namespace dect2
