// DMR subscriber TX slot timing (ETSI TS 102 361-1 clauses 4.2, 4.4.1, 4.5 and 5.1.1). A radio talking through a repeater must "synchronize to
// an outbound channel and base its inbound timing entirely on the outbound timing" (4.4.1). OnAir is half-duplex: it listens to the repeater's
// output first (the DMR receiver reports every clean CACH, its TDMA channel bit TC and access type AT, and the centre time of the burst after
// it), then keys up so that its inbound bursts land in the right inbound time slot.
//
// The timing rules used here, from the spec:
//   - the centres of the inbound and outbound bursts are time aligned (4.2);
//   - inbound channels 1 and 2 are offset 30 ms from outbound channels 1 and 2: the CACH in front of outbound slot N announces (TC, AT) the
//     inbound slot N burst that begins one slot later (4.2, 4.5, figure 4.8), so the centre of inbound slot N = centre of outbound slot N + 30 ms;
//   - a logical channel uses inbound slot N with offset channel timing and inbound slot 3-N with aligned channel timing (table 5.1);
//   - a burst is 132 symbols (27.5 ms); symbol 0 lies 65.5 symbols before the burst centre; the rest of the 30 ms slot is guard time.
// When the repeater is not transmitting there is nothing to synchronise to: the radio sends asynchronously ("BS activation", 4.4.1), which is
// what the transmitter does without a fit. Busy inbound slots (AT = 1) are reported as a warning only; nothing here blocks a transmission.
#pragma once
#include <cstddef>
#include <deque>
#include <mutex>

namespace dect2 {

constexpr double kDmrSlotSec = 0.030;                 // one TDMA time slot
constexpr double kDmrFrameSec = 0.060;                // two slots
constexpr double kDmrBurstCentreSym = 65.5;           // symbol 0 to the burst centre, in symbols at 4800 Bd

// Seconds on the steady clock (the time base of DmrSlotClock::global()).
double dmrSteadySec();

class DmrSlotClock {
public:
    // The receiver feeds this one with wall (steady clock) times; the transmitter reads it when a DMR transmission starts.
    static DmrSlotClock& global();

    // One clean CACH of the outbound channel: the centre time (seconds, any clock) of the outbound burst after it, its slot (1 or 2, from TC)
    // and the access type of the inbound slot the CACH announces (1 busy, 0 idle, -1 not known).
    void observe(double centreSec, int slot, int at);
    void clear();

    struct Fit {
        bool ok = false;
        double ts1CentreSec = 0;     // centre of an outbound slot 1 burst (the reference frame)
        double frameSec = kDmrFrameSec;
        int used = 0, seen = 0;      // observations in the fit / in the window
        double rmsMs = 0;            // residual of the fit
        double lastSec = 0;          // the newest observation
        int busy[2] = {-1, -1};      // the last AT reported for inbound slot 1 and 2
    };
    // A fit over the observations of the last `windowSec` seconds before the newest one. ok needs 8 consistent observations.
    Fit fit(double windowSec = 4.0) const;

    // The time of symbol 0 of the first inbound burst of the logical channel (1 or 2) at or after `earliest`, from fit f.
    // aligned: the repeater uses aligned channel timing (table 5.1); the default is offset channel timing.
    static double inboundBurstStart(const Fit& f, int logicalChannel, bool aligned, double earliest);
    // The inbound time slot (1 or 2) a logical channel transmits in.
    static int inboundSlot(int logicalChannel, bool aligned) { const int l = logicalChannel == 2 ? 2 : 1; return aligned ? 3 - l : l; }

private:
    struct Obs { double t; int slot; int at; };
    mutable std::mutex mu_;
    std::deque<Obs> obs_;
    int busy_[2] = {-1, -1};
};

// What the transmitter does for one DMR transmission that starts producing samples at `txStartSec` (steady clock, already including the
// radio's transmit latency): the silence to put in front so that the first inbound burst (gen slot index 0 = the chosen inbound slot) falls in
// its slot. ok false: no fresh fit (repeater not heard in the last `maxAgeSec`), send asynchronously.
struct DmrTxLead {
    bool ok = false;
    double leadSec = 0;          // silence before the generator's first slot
    int inboundSlot = 1;         // the inbound slot used
    int busy = -1;               // AT of that inbound slot at the last CACH (1: the repeater reports traffic)
    double ageSec = 0;           // how old the newest observation is
    double rmsMs = 0;
};
// genDelaySec: the generator's own delay from its first output sample to symbol 0 of its first burst (dmr_gen: shaper delay).
DmrTxLead dmrTxLeadFor(const DmrSlotClock::Fit& f, int logicalChannel, bool aligned, double txStartSec, double genDelaySec, double maxAgeSec = 10.0);

} // namespace dect2
