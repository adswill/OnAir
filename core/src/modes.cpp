// The modes added after FM, seen together: tuning table, test signals, status lines.
#include "dect2/dvbs_rx.h"
#include "dect2/dvbs_gen.h"
#include "dect2/dtmb_rx.h"
#include "dect2/dtmb_gen.h"
#include "dect2/atv_rx.h"
#include "dect2/atv_gen.h"
#include "dect2/dmr_rx.h"
#include "dect2/dmr_gen.h"
#include "dect2/drm_rx.h"
#include "dect2/drm_gen.h"
#include "dect2/adsb_rx.h"
#include "dect2/adsb_gen.h"
#include "dect2/modes.h"
#include "dect2/demo_ts.h"
#include "dect2/exact_resampler.h"
#include "dect2/fm_gen.h"
#include "dect2/isdbt_gen.h"
#include <cmath>
#include <random>
#include <vector>

namespace dect2 {

namespace {
// The test signals of two of the older modes, so that every mode can be tried without a radio: FM (stereo with RDS) and ISDB-T (the test programme).
class FmSynth : public ModeSynth {
public:
    FmSynth(const SynthConfig& c, double rate) : rate_(rate) {
        FmGenConfig g;
        g.rate = rate; g.cnrDb = c.snrDb >= 40 ? 200 : c.snrDb + 10; g.cfoHz = c.cfoHz;
        g.ps = "ONAIR FM"; g.rt = "OnAir FM test signal: 1 kHz left, 3 kHz right";
        gen_ = std::make_unique<FmGenerator>(g);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        buf_.clear();
        gen_->generate(n, buf_);
        for (size_t i = 0; i < n; i++) out[i] = buf_[i] * 0.3f;
    }
private:
    double rate_;
    std::unique_ptr<FmGenerator> gen_;
    std::vector<cf32> buf_;
};

class IsdbtSynth : public ModeSynth {
public:
    IsdbtSynth(const SynthConfig& c, double rate) : rate_(rate), cfo_(c.cfoHz), sigma_((float)std::sqrt(std::pow(10.0, -c.snrDb / 10.0) / 2.0)) {
        isdbt::Params p;   // as isdbtgen: a one-segment layer A and a twelve-segment layer B that carries the programme
        p.mode = 3; p.guard = isdbt::kGi8; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = isdbt::kQpsk; p.layer[0].rate = isdbt::kR23; p.layer[0].ti = 2;
        p.layer[1].segments = 12; p.layer[1].mod = isdbt::k64Qam; p.layer[1].rate = isdbt::kR34; p.layer[1].ti = 1;
        gen_ = std::make_unique<isdbt::Generator>(p, isdbt::singleLayerSource(1, demoTsSource(isdbt::layerBitrate(p, 1) * 0.9)), 1);
        rs_.configure(isdbt::kSampleRate, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) {
            gen_->nextFrame(frame_);
            rs_.process(frame_.data(), frame_.size(), pending_);
        }
        for (size_t i = 0; i < n; i++, count_++) {
            cf32 v = pending_[pos_ + i];
            const double ph = 2 * M_PI * cfo_ * (double)(count_ % (uint64_t)rate_) / rate_;
            v *= cf32((float)std::cos(ph), (float)std::sin(ph));
            v += cf32(nd_(rng_), nd_(rng_)) * sigma_;
            out[i] = v * 0.25f;
        }
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    double rate_, cfo_;
    float sigma_;
    std::unique_ptr<isdbt::Generator> gen_;
    ExactResampler rs_;
    std::vector<cf32> frame_, pending_;
    size_t pos_ = 0;
    uint64_t count_ = 0;
    std::mt19937 rng_{7};
    std::normal_distribution<float> nd_{0.f, 1.f};
};
}

static const std::vector<ModeTuning>& table() {
    static const std::vector<ModeTuning> t = {dvbsTuning(), dtmbTuning(), atvTuning(), dmrTuning(), drmTuning(), adsbTuning()};
    return t;
}

const ModeTuning* modeTuning(int stdMode) {
    for (const auto& m : table()) if (m.stdMode == stdMode) return &m;
    return nullptr;
}

const ModeTuning* modeTuningById(const std::string& id) {
    for (const auto& m : table()) if (id == m.id) return &m;
    return nullptr;
}

std::unique_ptr<ModeSynth> makeModeSynth(int stdMode, const SynthConfig& cfg, double sampleRate) {
    switch (stdMode) {
    case 6: return std::make_unique<IsdbtSynth>(cfg, sampleRate);
    case 7: return std::make_unique<FmSynth>(cfg, sampleRate);
    case 8: return makeDvbsSynth(cfg, sampleRate);
    case 9: return makeDtmbSynth(cfg, sampleRate);
    case 10: return makeAtvSynth(cfg, sampleRate);
    case 11: return makeDmrSynth(cfg, sampleRate);
    case 12: return makeDrmSynth(cfg, sampleRate);
    case 13: return makeAdsbSynth(cfg, sampleRate);
    default: return nullptr;
    }
}

std::string modeSummary(const RxTelemetry& t) {
    switch (t.standard) {
    case 7: return dvbsSummary(t.dvbs);
    case 8: return dtmbSummary(t.dtmb);
    case 9: return atvSummary(t.atv);
    case 10: return dmrSummary(t.dmr);
    case 11: return drmSummary(t.drm);
    case 12: return adsbSummary(t.adsb);
    default: return "";
    }
}

} // namespace dect2
