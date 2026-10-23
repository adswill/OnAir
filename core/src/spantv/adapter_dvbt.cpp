// Whole-span TV adapters for DVB-T2 and DVB-T: the receivers, BB unpacking and loss marking exactly as MultiTv's slots had them.
#include "adapters_internal.h"
#include "dect2/dvbt/dvbt_rx.h"
#include "dect2/engine/spantv/spantv.h"
#include "dect2/t2/t2rx.h"
#include <algorithm>
#include <mutex>

namespace dect2 {

namespace {

class T2Rx : public TvRx {
public:
    T2Rx(double rate, double widthMhz) : rx_(new T2Receiver) {
        rx_->configure(rate, widthMhz);
        rx_->setPlpCallback([this](const PlpResult& r) { onPlp(r); });
    }
    ~T2Rx() override { rx_.reset(); }   // its threads call onPlp: they go first
    void setOut(TsOut* o) override {
        out_ = o;
        unpack_.setSink([this](const uint8_t* pkt) { buf_.insert(buf_.end(), pkt, pkt + 188); });   // under mu_ (onPlp)
    }
    void feed(const cf32* x, size_t n) override { rx_->feed(x, n); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override { return rx_->telemetry(t, seq); }
    BbStats bbStats() override { std::lock_guard<std::mutex> lk(mu_); return unpack_.stats(); }

private:
    void flushBuf() { if (!buf_.empty()) { out_->packets(buf_.data(), buf_.size() / 188); buf_.clear(); } }
    void lost() { unpack_.lost(); flushBuf(); out_->loss(); }
    void onPlp(const PlpResult& r) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            buf_.clear();
            if (lastT2Frame_ >= 0) {
                const int mod = r.numT2Frames > 0 ? r.numT2Frames : 256;
                if (r.t2Frame != (lastT2Frame_ + std::max(1, r.frameInterval)) % mod) lost();
            }
            lastT2Frame_ = r.t2Frame;
            for (const auto& f : r.frames) {
                if (f.bits.empty()) { lost(); continue; }
                unpack_.push(f.bits, f.header);
            }
            flushBuf();
        }
        out_->burst(r.frameSec);
    }
    std::mutex mu_;
    BbUnpacker unpack_;
    std::vector<uint8_t> buf_;
    int lastT2Frame_ = -1;
    TsOut* out_ = nullptr;
    std::unique_ptr<T2Receiver> rx_;
};

class DvbtRx : public TvRx {
public:
    DvbtRx(double rate, double widthMhz) : rx_(new DvbtReceiver) { rx_->configure(rate, widthMhz); }
    ~DvbtRx() override { rx_.reset(); }
    void setOut(TsOut* o) override {
        out_ = o;
        rx_->setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { out_->packets(pk, n); out_->burst(secs); });
    }
    void feed(const cf32* x, size_t n) override { rx_->feed(x, n); }
    bool telemetry(RxTelemetry& t, uint64_t seq) override { return rx_->telemetry(t, seq); }

private:
    TsOut* out_ = nullptr;
    std::unique_ptr<DvbtReceiver> rx_;
};

class DvbtAdapter : public TvAdapter {
public:
    explicit DvbtAdapter(bool dvbt) : dvbt_(dvbt) {}
    int stdMode() const override { return dvbt_ ? 2 : 1; }
    const char* name() const override { return dvbt_ ? "DVB-T" : "DVB-T2"; }
    bool raster() const override { return true; }
    double minBwHz() const override { return 1.7e6; }
    double maxBwHz() const override { return 10e6; }
    double nativeRate(const SignalCand&, const SpanTvOptions& o) const override { return tvChannelRate(o.widthMhz, o.spanRate); }
    float costPerSecond(const SignalCand&) const override { return dvbt_ ? 0.51f : 0.95f; }   // PERFORMANCE-DVB.md: T 8K 64-QAM, T2 32K 256-QAM
    std::unique_ptr<TvRx> make(const SignalCand&, double outRate, const SpanTvOptions& o) override {
        if (dvbt_) return std::unique_ptr<TvRx>(new DvbtRx(outRate, o.widthMhz));
        return std::unique_ptr<TvRx>(new T2Rx(outRate, o.widthMhz));
    }

private:
    bool dvbt_;
};

} // namespace

std::unique_ptr<TvAdapter> makeDvbtAdapter(bool dvbt) { return std::unique_ptr<TvAdapter>(new DvbtAdapter(dvbt)); }

} // namespace dect2
