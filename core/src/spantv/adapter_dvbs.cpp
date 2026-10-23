// Whole-span TV adapter for DVB-S / S2 / S2X (docs/WHOLESPAN-TV-DESIGN.md 2.1): a free-frequency standard. The span analysis gives each
// transponder's centre and -3 dB width (its symbol rate); the channeliser cuts it out at 2.5 times that rate, and the existing receiver
// (dvbs_rx.h) does the rest: symbol rate and roll-off refinement, carrier recovery, spectral inversion, DVB-S / S2 / S2X, VCM, the first ISI.
#include "adapters_internal.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/engine/spantv/spantv.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {

namespace {

class DvbsRx : public TvRx {
public:
    DvbsRx(double rate, bool blocking) : rx_(new DvbsReceiver) {
        rx_->setBlocking(blocking);
        rx_->configure(rate);
        rx_->selectCarrier(true, 0.0);   // the cut-out can hold the shoulders of the neighbours: the carrier in the middle is ours
    }
    ~DvbsRx() override { rx_.reset(); }
    void setOut(TsOut* o) override {
        out_ = o;
        rx_->setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { out_->packets(pk, n); out_->burst(secs); });
    }
    void feed(const cf32* x, size_t n) override { rx_->feed(x, n); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override {
        DvbsTelemetry mt;
        if (!rx_->telemetry(mt, seq)) return false;
        t.standard = 7;   // as the engine's own DVB-S poll (engine_telemetry.cpp pollStd)
        t.seq = mt.seq; t.state = mt.state; t.cfoHz = mt.cfoHz; t.dataValid = mt.dataValid; t.dataSnrDb = mt.snrDb;
        t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad; t.rateOk = rx_->ready();
        t.dvbs = std::move(mt);
        return true;
    }
    void fillRow(const RxTelemetry& t, TvChannelRow& r) const override {
        const DvbsTelemetry& d = t.dvbs;
        if (d.symbolRate > 0) r.rsHz = d.symbolRate;
        if (d.rollOff > 0) { r.rollOff = d.rollOff; if (r.rsHz > 0) r.bwHz = r.rsHz * (1 + d.rollOff); }
        r.cfoHz = t.cfoHz;
        char b[96];
        const char* st = d.standard == 1 ? "DVB-S" : d.standard == 2 ? "DVB-S2" : d.standard == 3 ? "DVB-S2X" : "";
        snprintf(b, sizeof b, "%s%s%s%s%s%s", st, d.modulationName.empty() ? "" : " ", d.modulationName.c_str(), d.codeRate.empty() ? "" : " ", d.codeRate.c_str(),
                 d.inverted ? " (inverted)" : "");
        r.params = b;
        r.isi = d.isi;
    }
    void flush() override { rx_->flush(); }

private:
    TsOut* out_ = nullptr;
    std::unique_ptr<DvbsReceiver> rx_;
};

class DvbsAdapter : public TvAdapter {
public:
    int stdMode() const override { return 8; }
    const char* name() const override { return "DVB-S/S2"; }
    bool raster() const override { return false; }
    // narrow SCPC and amateur carriers down to about 10 kBd (the narrow-carrier front end of the receiver, pub/dvbs-narrow-030, does the rest)
    double minBwHz() const override { return 10e3; }
    double maxBwHz() const override { return 72e6; }
    double nativeRate(const SignalCand& c, const SpanTvOptions& o) const override {
        const double rs = c.rsEstimate > 0 ? c.rsEstimate : c.bwHz / 1.35;
        // 2.5 samples per symbol: the receiver needs 1.3 (1 + roll-off), the rest is room for the channeliser's transition and the estimate's error
        return std::min(o.spanRate, std::max(2.5 * rs, 25e3));
    }
    float costPerSecond(const SignalCand& c) const override {
        // PERFORMANCE-DVB.md: DVB-S QPSK 10 MBd 0.52, S2 8PSK 3/4 10 MBd 1.03 (all threads): about 0.08 core per MBd, plus the channeliser
        return 0.03f + (float)(0.08 * std::max(c.rsEstimate, 10e3) / 1e6);
    }
    std::unique_ptr<TvRx> make(const SignalCand&, double outRate, const SpanTvOptions& o) override {
        return std::unique_ptr<TvRx>(new DvbsRx(outRate, o.blocking));
    }
};

} // namespace

std::unique_ptr<TvAdapter> makeDvbsAdapter() { return std::unique_ptr<TvAdapter>(new DvbsAdapter); }

} // namespace dect2
