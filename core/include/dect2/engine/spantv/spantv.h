// Whole-span TV (docs/WHOLESPAN-TV-DESIGN.md): one wide capture, one receiver per occupied signal, for every television standard with an
// adapter (spantv/adapter.h).
//
//   feed(span) -> raster standards (DVB-T/T2 ...): SpanScanner measures the slots of the channel raster
//                 free-frequency standards (DVB-S/S2 ...): SpanAnalyzer finds the signals, SignalTracker gives them stable ids
//              -> planPool (spantv/pool.h): which signals get a receiver (cap, CPU budget, priority to the selected one)
//              -> per signal, on its own thread: ChannelFilter (mix, low-pass, resample to the adapter's rate) -> TvRx -> TsDemux
//
// Every signal keeps its own receiver, demultiplexer, service list and EPG. One is "selected": its packets go to the sink (the engine's player,
// outputs and network tuner) while the others keep decoding. MultiTv (multitv.h) is this class configured for DVB-T / DVB-T2.
#pragma once
#include "adapter.h"
#include "pool.h"
#include "signal_tracker.h"
#include "span_analyzer.h"
#include "dect2/engine/channelizer.h"
#include "dect2/tv/ts.h"
#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct SpanTvOptions {
    int stdMode = 1;              // the engine's standard: 1 DVB-T2, 2 DVB-T, 8 DVB-S/S2 (makeTvAdapter)
    double spanRate = 0;          // sample rate of what feed() gets
    // raster standards
    double widthMhz = 8;          // channel width (7 VHF, 8 UHF Europe, 6 Americas / Asia)
    double gridOffsetHz = 0;      // a channel centre on the raster, relative to the centre of the capture (0: the capture is centred on a channel)
    bool autoGrid = false;        // pick 0 or half a channel width, whichever finds more channels
    // the pool
    int maxChannels = 2;          // receivers running at once (hard cap)
    double cpuBudgetCores = 0;    // 0: no CPU budget (the cap only), < 0: automatic (defaultPoolBudget), > 0: this many cores
    bool decodeAll = true;        // false: only the selected signal gets a receiver
    double lockSecs = 0;          // a receiver that has not locked after this long drops to the lowest priority (0: the adapter's default)
    bool blocking = true;         // a slow receiver holds feed() back (files, tests); false: it loses blocks (a live radio)
    double thresholdDb = 6;       // a slot / signal is occupied this far above the noise floor
};

struct MultiService {
    int slot = 0;                 // the signal (raster slot or SignalId)
    double offsetHz = 0;          // its centre relative to the centre of the capture
    TsService svc;
    // whole-span TV
    int stdMode = 0;
    double bwHz = 0, rsHz = 0;
    double snrDb = 0;
    bool locked = false;
    int onid = 0, tsid = 0;      // from the signal's NIT / PAT
    int duplicateOf = -1;         // services(true): the same service (network, stream, service id) on a better signal: that signal's id
};

class SpanTv {
public:
    using Sink = std::function<void(int slot, const uint8_t* packets, size_t count, double seconds, const TsSnapshot& snap)>;

    SpanTv();
    virtual ~SpanTv();
    SpanTv(const SpanTv&) = delete;
    SpanTv& operator=(const SpanTv&) = delete;

    // false when the standard has no adapter or the span cannot hold one channel of a raster standard
    bool configure(const SpanTvOptions& o);
    const SpanTvOptions& spanOptions() const { return o_; }
    bool raster() const { return raster_; }
    void reset();                          // stops the receivers; the scan starts again
    void feed(const cf32* x, size_t n);
    void wait();                           // returns when every receiver has taken what feed() gave it (tests, end of a file)
    void flush();                          // wait() and then every receiver's own flush (tests)

    // The selected signal's packets (a burst, with the demultiplexer's snapshot after it); called on a receiver thread
    void setSink(Sink s);
    void setSelected(int slot);            // a running signal, or a known one that then gets a receiver
    int selected() const { return sel_.load(); }

    // per signal (ids as in TvChannelRow::slot)
    void setPinned(int slot, bool on);     // a recording / stream / network tuner client holds it: priority after the selected one
    void setDecode(int slot, bool on);     // the user's own choice for this signal (off: no receiver unless selected)
    void setDecodeAll(bool on);
    void setMaxChannels(int n);
    void setBudget(double cores);          // as SpanTvOptions::cpuBudgetCores

    // Polls every receiver; true when the selected signal has a new telemetry
    bool poll(RxTelemetry& t);
    std::vector<TvChannelRow> channels();
    std::vector<MultiService> services(bool markDuplicates = false);
    TsSnapshot snapshot(int slot);
    std::map<int, std::vector<EpgEvent>> epg(int slot);
    BbStats bbStats(int slot);
    std::vector<int> runningSlots();
    double gridOffsetHz() const { return gridOff_; }
    double poolCores();                    // CPU of all receivers now (cores)
    double budgetCores() const;            // the budget in force (0: none)
    std::vector<SlotPower> lastMeasure();  // raster test hook: the slots the scan measured (the last evaluation)
    std::vector<SignalCand> lastSignals(); // free-frequency: the candidates of the last evaluation
    const SpanAnalyzer& analyzer() const { return ana_; }

private:
    struct Slot;
    void evaluate();
    void evaluateRaster();
    void evaluateFree();
    void applyPlan(const std::vector<PoolEntry>& entries, const std::map<int, SignalCand>& cands);
    void deliver(int slot, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap);   // a receiver thread: the selected signal's packets go on
    bool startSlot(int id, const SignalCand& c);
    void stopSlot(size_t i);   // under mu_
    Slot* find(int slot);
    double now() const { return o_.spanRate > 0 ? (double)fed_ / o_.spanRate : 0; }

    SpanTvOptions o_;
    std::unique_ptr<TvAdapter> ad_;
    bool raster_ = true;
    double gridOff_ = 0;
    bool gridFixed_ = false;
    SpanScanner scan_;
    std::vector<ChannelSlotPos> slots_;
    std::vector<SlotPower> measured_;
    SpanAnalyzer ana_;
    SignalTracker trk_;
    std::vector<SignalCand> lastCands_;
    size_t sinceEval_ = 0;
    uint64_t fed_ = 0;
    size_t queueLimit_ = 1u << 19;   // samples that may wait for one receiver
    std::mutex mu_;
    std::vector<std::unique_ptr<Slot>> live_;
    std::map<int, bool> pinned_, decodeOff_;
    std::map<int, double> budgetStopAt_;
    std::atomic<int> sel_{INT_MIN / 2};
    Sink sink_;
    std::mutex sinkMu_;
    int lastSelPolled_ = INT_MIN / 2;
};

} // namespace dect2
