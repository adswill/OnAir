// Whole-span TV (docs/WHOLESPAN-TV-DESIGN.md): what SpanTv needs from one television standard. A TvAdapter knows the standard's channel
// layout (a fixed raster, or free frequency with any bandwidth), the rate its receiver wants, what a receiver costs, and makes one TvRx per
// signal. A TvRx is one receiver for one signal: it takes the signal mixed to 0 Hz at its own rate and hands transport stream packets to a TsOut.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/channels/tvchan.h"
#include "dect2/tv/bbunpack.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct RxTelemetry;

// One signal the span analysis found (spantv/span_analyzer.h) or one raster slot that is occupied
struct SignalCand {
    double centerHz = 0;      // centre, relative to the centre of the span
    double bwHz = 0;          // occupied bandwidth (the -3 dB width plus the roll-off: Rs (1 + alpha) for a single carrier)
    double bw3Hz = 0;         // width at half power over the noise (= the symbol rate of a root raised cosine carrier)
    double plateauDb = -200;  // median level of the centre half (dB)
    double floorDb = -200;    // the noise floor under it (dB)
    double snrDb = 0;         // plateau over the floor
    double edgeHz = 0;        // mean width of a shoulder, 90 % to 10 % of the plateau's power
    double rsEstimate = 0;    // symbol rate (single carrier): bw3Hz
    double rollOff = 0;       // from the shoulders, snapped to the nearest standard value (0: not judged)
    bool clipped = false;     // runs into the edge of the usable span: not started
    uint32_t stdMask = 0;
};

// Where a receiver's transport stream goes (SpanTv's per-signal demultiplexer). Called on the receiver's thread, in this order: packets() for
// the packets of a burst (possibly several calls, with loss() between them where the receiver lost data), then burst() with its signal time.
class TsOut {
public:
    virtual ~TsOut() = default;
    virtual void packets(const uint8_t* pk, size_t n) = 0;
    virtual void loss() = 0;
    virtual void burst(double secs) = 0;
};

// One receiver for one signal
class TvRx {
public:
    virtual ~TvRx() = default;
    virtual void setOut(TsOut* out) = 0;                                // before the first feed()
    virtual void feed(const cf32* x, size_t n) = 0;                     // the receiver thread of the pool
    virtual bool telemetry(RxTelemetry& out, uint64_t lastSeq) = 0;     // any thread; true when there is a newer report than lastSeq
    virtual BbStats bbStats() { return BbStats(); }                     // standards with BBFRAMEs
    virtual void fillRow(const RxTelemetry& t, TvChannelRow& row) const { (void)t; (void)row; }   // standard-specific columns of the table
    virtual void flush() {}                                             // tests: everything fed so far is decoded and delivered
};

struct SpanTvOptions;

class TvAdapter {
public:
    virtual ~TvAdapter() = default;
    virtual int stdMode() const = 0;                                    // the engine's standard (1 DVB-T2, 2 DVB-T, 8 DVB-S/S2, ...)
    virtual const char* name() const = 0;
    virtual bool raster() const = 0;                                    // true: channels on a fixed raster of SpanTvOptions::widthMhz (SpanScanner);
                                                                        // false: signals at any frequency and width (SpanAnalyzer)
    virtual double minBwHz() const = 0;
    virtual double maxBwHz() const = 0;
    virtual double nativeRate(const SignalCand& c, const SpanTvOptions& o) const = 0;   // the rate the receiver is fed at
    virtual float costPerSecond(const SignalCand& c) const = 0;         // CPU seconds per second of signal (a seed; the pool measures the real one)
    virtual float probe(const cf32* x, size_t n, double rate, SignalCand& c) { (void)x; (void)n; (void)rate; (void)c; return 0; }   // blind detection 0..1 (phase 6)
    virtual std::unique_ptr<TvRx> make(const SignalCand& c, double outRate, const SpanTvOptions& o) = 0;
};

// The adapter of an engine standard; null when that standard has no whole-span support
std::unique_ptr<TvAdapter> makeTvAdapter(int stdMode);
// cheap (the feed path asks it): the standards with an adapter (core/src/spantv/adapters.cpp must agree)
inline bool spanTvSupported(int stdMode) {
    switch (stdMode) { case 1: case 2: case 3: case 5: case 6: case 8: case 9: case 42: return true; default: return false; }   // T2 T ATSC ATSC3 ISDB-T S/S2 DTMB DVB-C
}
bool spanTvRaster(int stdMode);          // the standard's channels sit on a raster (false: free frequency, e.g. satellite)
inline bool spanTvFreeFrequency(int stdMode) { return stdMode == 8 || stdMode == 42; }   // DVB-S/S2, DVB-C: cheap form of !spanTvRaster
const char* spanTvName(int stdMode);

} // namespace dect2
