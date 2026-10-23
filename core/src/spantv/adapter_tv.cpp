// Whole-span TV adapters for the other transport-stream standards (docs/WHOLESPAN-TV-DESIGN.md 2.2-2.7): DVB-C (single carrier, free frequency:
// cable plans differ, the symbol rate comes from the span analysis and the receiver refines it), and the raster standards ISDB-T (6 MHz),
// ATSC 1.0 (6 MHz, 8VSB), ATSC 3.0 (6 MHz) and DTMB (8 MHz). Each wraps the existing single-signal receiver: configure, feed, telemetry, packets.
#include "adapters_internal.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/atsc3/atsc3_rx.h"
#include "dect2/dtmb/dtmb_rx.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/engine/spantv/spantv.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {

namespace {

// the common part every mode's telemetry has, copied as the engine's pollStd does (engine_telemetry.cpp)
template <class Tel>
void common(RxTelemetry& t, const Tel& m, int standard) {
    t.standard = standard; t.seq = m.seq; t.state = m.state; t.cfoHz = m.cfoHz; t.dataValid = m.dataValid; t.dataSnrDb = m.snrDb;
    t.blocksOk = m.blocksOk; t.blocksBad = m.blocksBad;
}

// a receiver with setPacketCallback(pk, n, secs) and flush()
template <class Rx>
class PacketRx : public TvRx {
public:
    PacketRx() : rx_(new Rx) {}
    ~PacketRx() override { rx_.reset(); }   // its threads call the packet callback: they go first
    void setOut(TsOut* o) override {
        out_ = o;
        rx_->setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { out_->packets(pk, n); out_->burst(secs); });
    }
    void feed(const cf32* x, size_t n) override { rx_->feed(x, n); }
    Rx& rx() { return *rx_; }

protected:
    TsOut* out_ = nullptr;
    std::unique_ptr<Rx> rx_;
};

class DvbcRx : public PacketRx<DvbcReceiver> {
public:
    DvbcRx(double rate, bool blocking) { rx_->setBlocking(blocking); rx_->configure(rate); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override {
        DvbcTelemetry m;
        if (!rx_->telemetry(m, seq)) return false;
        common(t, m, 41); t.rateOk = rx_->ready();
        t.dvbc = std::move(m);
        return true;
    }
    void fillRow(const RxTelemetry& t, TvChannelRow& r) const override {
        static const char* q[5] = {"16-QAM", "32-QAM", "64-QAM", "128-QAM", "256-QAM"};
        if (t.dvbc.symbolRate > 0) { r.rsHz = t.dvbc.symbolRate; r.rollOff = t.dvbc.annexC ? 0.13 : 0.15; r.bwHz = r.rsHz * (1 + r.rollOff); }
        r.cfoHz = t.cfoHz;
        r.params = std::string("DVB-C") + (t.dvbc.qam >= 0 && t.dvbc.qam < 5 ? std::string(" ") + q[t.dvbc.qam] : "");
    }
    void flush() override { rx_->flush(); }
};

class IsdbtRx : public PacketRx<IsdbtReceiver> {
public:
    IsdbtRx(double rate, int bwMhz) { rx_->configure(rate, bwMhz); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override { return rx_->telemetry(t, seq); }
};

class AtscRx : public PacketRx<AtscReceiver> {
public:
    AtscRx(double rate, bool blocking) { rx_->setBlocking(blocking); rx_->configure(rate); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override {   // as the engine's own ATSC poll (engine_loops.cpp)
        AtscTelemetry at;
        if (!rx_->telemetry(at, seq)) return false;
        t.standard = 2; t.seq = at.seq;
        t.state = at.fieldSync ? 2 : at.segSync ? 1 : 0;
        t.cfoHz = at.cfoHz; t.sroPpm = at.sroPpm;
        t.dataValid = at.fieldSync && at.eqTrained;
        t.dataSnrDb = (float)at.snrDb;
        t.blocksOk = at.rsClean + at.rsCorrected; t.blocksBad = at.rsFailed;
        t.rateOk = rx_->rateOk();
        t.atsc = at;
        return true;
    }
    void fillRow(const RxTelemetry& t, TvChannelRow& r) const override { r.cfoHz = t.cfoHz; r.params = "ATSC 8VSB"; }
    void flush() override { rx_->flush(); }
};

class Atsc3TvRx : public PacketRx<Atsc3Rx> {
public:
    Atsc3TvRx(double rate, bool blocking) { rx_->setPaced(true); rx_->setBlocking(blocking); rx_->configure(rate); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override {   // as the engine's own ATSC 3.0 poll (engine_loops.cpp)
        Atsc3Telemetry at;
        if (!rx_->telemetry(at, seq)) return false;
        t.standard = 4; t.seq = at.seq;
        t.state = at.locked ? 2 : at.bootstraps > 0 ? 1 : 0;
        t.cfoHz = at.cfoHz;
        t.dataValid = at.locked && at.serviceReady;
        t.blocksOk = at.bbPackets - at.bbBad; t.blocksBad = at.bbBad;
        t.rateOk = true;
        return true;
    }
    void fillRow(const RxTelemetry& t, TvChannelRow& r) const override { r.cfoHz = t.cfoHz; r.params = "ATSC 3.0"; }
};

class DtmbRx : public PacketRx<DtmbReceiver> {
public:
    DtmbRx(double rate, double bwMhz) { rx_->configure(rate, bwMhz); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override {
        DtmbTelemetry m;
        if (!rx_->telemetry(m, seq)) return false;
        common(t, m, 8); t.rateOk = rx_->ready();
        t.dtmb = std::move(m);
        return true;
    }
    void fillRow(const RxTelemetry& t, TvChannelRow& r) const override { r.cfoHz = t.cfoHz; r.params = "DTMB"; }
    void flush() override { rx_->flush(); }
};

class DvbcAdapter : public TvAdapter {
public:
    int stdMode() const override { return 42; }
    const char* name() const override { return "DVB-C"; }
    bool raster() const override { return false; }
    double minBwHz() const override { return 1e6; }
    double maxBwHz() const override { return 8.5e6; }
    double nativeRate(const SignalCand& c, const SpanTvOptions& o) const override {
        const double rs = c.rsEstimate > 0 ? c.rsEstimate : c.bwHz / 1.15;
        return std::min(o.spanRate, std::max(2.0 * rs, 2e6));
    }
    float costPerSecond(const SignalCand& c) const override { return 0.05f + (float)(0.16 * c.rsEstimate / 1e6); }   // 256-QAM 6.95 MBd: 1.13
    std::unique_ptr<TvRx> make(const SignalCand&, double outRate, const SpanTvOptions& o) override { return std::unique_ptr<TvRx>(new DvbcRx(outRate, o.blocking)); }
};

// the raster standards: one channel width, the rate 1.25 x the width as DVB-T has it
class RasterAdapter : public TvAdapter {
public:
    RasterAdapter(int sm, const char* n, float cost) : sm_(sm), n_(n), cost_(cost) {}
    int stdMode() const override { return sm_; }
    const char* name() const override { return n_; }
    bool raster() const override { return true; }
    double minBwHz() const override { return 1.5e6; }
    double maxBwHz() const override { return 10e6; }
    double nativeRate(const SignalCand&, const SpanTvOptions& o) const override {
        if (sm_ == 3) return std::min(o.spanRate, std::max(tvChannelRate(o.widthMhz, o.spanRate), 10.76e6 * 1.0));   // ATSC wants >= 1 sample per symbol of 10.76 MBd
        return tvChannelRate(o.widthMhz, o.spanRate);
    }
    float costPerSecond(const SignalCand&) const override { return cost_; }
    std::unique_ptr<TvRx> make(const SignalCand&, double outRate, const SpanTvOptions& o) override {
        switch (sm_) {
        case 3: return std::unique_ptr<TvRx>(new AtscRx(outRate, o.blocking));
        case 5: return std::unique_ptr<TvRx>(new Atsc3TvRx(outRate, o.blocking));
        case 6: return std::unique_ptr<TvRx>(new IsdbtRx(outRate, (int)std::lround(o.widthMhz)));
        case 9: return std::unique_ptr<TvRx>(new DtmbRx(outRate, o.widthMhz));
        default: return nullptr;
        }
    }

private:
    int sm_;
    const char* n_;
    float cost_;
};

} // namespace

std::unique_ptr<TvAdapter> makeDvbcAdapter() { return std::unique_ptr<TvAdapter>(new DvbcAdapter); }
// PERFORMANCE-DVB.md baselines: ATSC 8 Msps 0.90, ATSC 3.0 10 Msps 0.40, ISDB-T 8 Msps 0.57, DTMB 10 Msps 0.49
std::unique_ptr<TvAdapter> makeRasterAdapter(int stdMode) {
    switch (stdMode) {
    case 3: return std::unique_ptr<TvAdapter>(new RasterAdapter(3, "ATSC", 0.90f));
    case 5: return std::unique_ptr<TvAdapter>(new RasterAdapter(5, "ATSC 3.0", 0.40f));
    case 6: return std::unique_ptr<TvAdapter>(new RasterAdapter(6, "ISDB-T", 0.57f));
    case 9: return std::unique_ptr<TvAdapter>(new RasterAdapter(9, "DTMB", 0.49f));
    default: return nullptr;
    }
}

} // namespace dect2
