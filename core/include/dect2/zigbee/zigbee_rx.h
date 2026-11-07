// Zigbee / IEEE 802.15.4 monitor: the frames devices send openly on the 2.4 GHz channels 11 - 26 (2405 + 5 (k - 11) MHz), the same information
// a sniffer shows. Receive only: it never transmits, joins or answers a network; payloads that are encrypted (the network key is not
// obtainable here) are shown as encrypted, not decoded; only what is sent in the clear is decoded.
//
// PHY (IEEE 802.15.4-2006 clause 6 / 2015 clause 12): O-QPSK with half-sine pulses (constant envelope, MSK equivalent), 2 Mchip/s, every 4-bit
// symbol spread to one of 16 near-orthogonal 32-chip sequences, 250 kbit/s; preamble 8 symbols of 0, SFD 0xA7, PHR (7-bit length), PSDU with
// the FCS (CRC-16 ITU-T). The channel is mixed to 0 Hz, low-pass filtered and resampled to 8 Msps (4 samples per chip), and the phase change over
// every chip window is taken (+-pi/2 plus the carrier offset). Each of the four sample phases slices the chips against a running mean and looks
// for the preamble symbol (3 symbols in a row, a few chip errors allowed); the frame decoder then estimates the carrier offset from the
// preamble, correlates every symbol with the 16 sequences (soft, with symbol by symbol timing tracking for the clock error), finds the
// SFD, reads the length and the PSDU, and keeps the frame when the FCS passes.
// Decoded: MAC header, beacons (Zigbee beacon payload), MAC commands, Zigbee NWK and, where clear, APS headers (zigbee_proto.h).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct ZigbeeTelemetry;   // zigbee_tel.h

class ZigbeeReceiver {
public:
    ZigbeeReceiver();
    ~ZigbeeReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setThreadMode(bool on);          // Thread: the same 802.15.4 receiver, the frames read as 6LoWPAN / Thread (thread_proto.h) instead of Zigbee
    void setCenterHz(double hz);          // the radio's centre: which channel the tuned frequency is (0: unknown)
    bool ready() const;
    void reset();                         // the demodulator only: the tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(ZigbeeTelemetry& out, uint64_t lastSeq);
    // The tables and the frame log stay through reset(), configure() and retunes: only clearResults() empties them
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning zigbeeTuning();
ModeTuning threadTuning();   // 2.4 GHz channels 11 - 26 only (the sub-GHz BPSK PHY is Zigbee's)

} // namespace dect2
