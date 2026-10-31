// Multi-channel TV: two or three adjacent DVB-T / DVB-T2 channels decoded from ONE wide capture (a LimeSDR, USRP, BladeRF or PlutoSDR covers
// 2-3 channels of 7 / 8 MHz at once).
//
//   feed(span) -> SpanScanner (which raster slots carry a signal)
//              -> per occupied slot, on the slot's own thread: ChannelFilter (mix, low-pass, resample) -> receiver -> BB unpack -> TsDemux
//
// Every instance keeps its own receiver, transport-stream demultiplexer, service list and EPG. One slot is "selected": its packets go to the
// sink (the engine's player, outputs and network tuner) while the others keep decoding. The slots follow the whole-span design (wide_bank.h):
// no state is shared between receivers.
#pragma once
#include "dect2/tv/bbunpack.h"
#include "channelizer.h"
#include "dect2/t2/t2rx.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "transponders.h"
#include "dect2/tv/ts.h"
#include "dect2/channels/tvchan.h"
#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct MultiTvOptions {
    bool dvbt = false;            // false DVB-T2, true DVB-T
    bool sat = false;             // DVB-S/S2 ("Whole span"): no channel raster; the transponders are found in the spectrum (transponders.h), one receiver each
    double spanRate = 0;          // sample rate of what feed() gets
    double widthMhz = 8;          // channel width (7 VHF, 8 UHF Europe, 6 Americas / Asia)
    double gridOffsetHz = 0;      // a channel centre on the raster, relative to the centre of the capture (0: the capture is centred on a channel)
    bool autoGrid = false;        // pick 0 or half a channel width, whichever finds more channels
    int maxChannels = 2;          // receivers running at once
    bool blocking = true;         // a slow receiver holds feed() back (files, tests); false: it loses blocks (a live radio)
    double thresholdDb = 6;       // a slot is occupied this far above the quietest part of the span
    double minRsHz = 1e6;         // sat: the narrowest transponder looked for
};

struct MultiService {
    int slot = 0;
    double offsetHz = 0;
    TsService svc;
};

// A transponder followed over successive looks at the spectrum (DVB-S/S2); id is stable while it stays
struct TpTrack {
    int id = 0;
    Transponder tp;
    int seen = 0;      // looks in a row that found it
    int absent = 0;    // looks in a row that did not
};

class MultiTv {
public:
    using Sink = std::function<void(int slot, const uint8_t* packets, size_t count, double seconds, const TsSnapshot& snap)>;

    MultiTv();
    ~MultiTv();
    MultiTv(const MultiTv&) = delete;
    MultiTv& operator=(const MultiTv&) = delete;

    // false when the span cannot hold even one channel of this width
    bool configure(const MultiTvOptions& o);
    const MultiTvOptions& options() const { return o_; }
    void reset();                          // stops the receivers; the scan starts again
    void feed(const cf32* x, size_t n);
    void wait();                           // returns when every receiver has taken what feed() gave it (tests, end of a file)

    // The selected slot's packets (a burst, with the demultiplexer's snapshot after it); called on a receiver thread
    void setSink(Sink s);
    // Only these slots get (and keep) a receiver; a running receiver on another slot is stopped. Not called: every occupied slot (up to
    // maxChannels). Used by the wide session to place each slot on the standard it was identified as, and to switch a channel off.
    void setAllowedSlots(const std::vector<int>& slots);
    void clearAllowedSlots();
    void setSelected(int slot);
    int selected() const { return sel_.load(); }

    // Polls every receiver; true when the selected slot has a new telemetry (then in t, with t.tvChannels filled in)
    bool poll(RxTelemetry& t);
    std::vector<TvChannelRow> channels();
    std::vector<MultiService> services();
    TsSnapshot snapshot(int slot);
    std::map<int, std::vector<EpgEvent>> epg(int slot);
    BbStats bbStats(int slot);
    std::vector<int> runningSlots();
    double gridOffsetHz() const { return gridOff_; }
    // test hook: the slots the scan measured (the last evaluation)
    std::vector<SlotPower> lastMeasure();
    // sat: the transponders seen now (test hook and the table)
    std::vector<Transponder> lastTransponders();

private:
    struct Slot;
    void evaluate();
    void mtvDeliver(int slot, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap);   // a receiver thread: the selected slot's packets go on
    void startSlot(const SlotPower& p);
    void evaluateSat();
    void startSat(const TpTrack& t);
    void stopSlot(size_t i);   // under mu_
    Slot* find(int slot);

    MultiTvOptions o_;
    double gridOff_ = 0;
    bool gridFixed_ = false;
    SpanScanner scan_;
    std::vector<ChannelSlotPos> slots_;
    std::vector<SlotPower> measured_;
    size_t sinceEval_ = 0;
    size_t queueLimit_ = 1u << 19;   // samples that may wait for one receiver
    std::mutex mu_;
    std::vector<std::unique_ptr<Slot>> live_;
    std::atomic<int> sel_{INT_MIN / 2};
    Sink sink_;
    std::mutex sinkMu_;
    int lastSelPolled_ = INT_MIN / 2;
    std::vector<TpTrack> tracks_;   // sat: the transponders followed over time (under mu_)
    bool restrict_ = false;
    std::vector<int> allowed_;
    int nextId_ = 1;
};

} // namespace dect2
