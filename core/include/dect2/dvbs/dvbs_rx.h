// DVB-S/S2 receiver: DVB-S, DVB-S2 and DVB-S2X satellite television (L-band IF after the LNB).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
// The carrier must be near the centre of the input. Its symbol rate, roll-off, modulation and code rate are found by the receiver.
#pragma once
#include "dvbs_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
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
    void resetNow();                                     // reset() at once, on the calling thread (nothing may be feeding): the worker threads are stopped when it returns
    bool telemetry(DvbsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // The decoded transport stream: packets of 188 bytes, `secs` of signal time since the previous call. Runs on the receiver thread.
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);

    // ---- controls (any thread)
    void setSymbolRate(double hz);                       // 0 = find it from the spectrum
    void setStandardHint(int standard);                  // 0 automatic, 1 DVB-S only, 2 DVB-S2 only (S2X signalling is reported either way)
    void setRollOff(double rollOff);                     // 0 = automatic: the spectrum, then the stream's own signalling
    void setCidSearch(bool on);                          // look for the DVB-CID signal under the carrier (costs processing; off by default)
    void selectCarrier(bool on, double offsetHz);        // wide inputs with several carriers: work on the one nearest this offset (off: the one nearest the centre)
    void setIsi(int isi);                                // DVB-S2 multiple input streams: the stream to decode, -1 the first one seen
    void setPlScrambling(int n);                         // DVB-S2 physical layer scrambling code, -1 = 0 (the default for broadcast)
    void setComputeMode(int mode);                       // 0 CPU, 1 GPU, 2 auto: where LDPC (DVB-S2) and Viterbi (DVB-S) run; the output is the same
    void setBlocking(bool blocking);                     // recordings: wait for the decoder threads instead of dropping frames when behind
    // IP data (GSE here; MPE and ULE through feedTs): the PCAP file to write every IP packet to ("" off), and a restart of the statistics
    void setIpPcap(const std::string& path);
    void clearIp();
    // Every recovered symbol (after timing and carrier recovery, super-frame pilots removed) as interleaved float32 I/Q to a file ("" off), at most 512 MB; for other tools
    void setSymbolDump(const std::string& path);
    // transport stream packets of the stream being received, for MPE and ULE (the engine hands them over where it hands them to the TS analysis)
    void feedTs(const uint8_t* packets, size_t nPackets);
    void flush();                                        // waits until everything fed so far is decoded and delivered (recordings, tests; not for the engine)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dvbsTuning();

// The highest symbol rate the receiver accepts at an input rate: about 1.3 samples per symbol times (1 + roll-off) of bandwidth
double dvbsMaxSymbolRate(double inputRateHz, double rollOff = 0.35);
// The lowest symbol rate that the spectrum analysis can measure at an input rate (below it a manual symbol rate is needed): about 8 kSym/s at
// any input rate, since a second, finer spectrum (dvbsFineFftSize) looks for the narrow carriers
double dvbsMinAutoSymbolRate(double inputRateHz);
// Carriers below this rate are measured in the fine spectrum, the ones above in the coarse 4096-point one (about 100 of its bins)
double dvbsNarrowSymbolRate(double inputRateHz);
// The fine spectrum's size at an input rate: bins of about 200 Hz or less (8192 to 131072 points)
int dvbsFineFftSize(double inputRateHz);

} // namespace dect2
