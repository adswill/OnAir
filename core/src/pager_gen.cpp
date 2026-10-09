// Pagers test signal (see pager_gen.h).
#include "dect2/pager_gen.h"
#include "dect2/gen_util.h"
#include "dect2/mode_tuning.h"
#include "dect2/pager_proto.h"
#include "dect2/pager_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGapSec = 0.5;                // silence after every transmission
constexpr double kEdge = 0.3;                  // the frequency changes over the 30 % either side of a symbol boundary

// One stretch of a transmission at one symbol rate. Symbol levels are -3, -1, 1, 3: the frequency shift in thirds of the full deviation.
struct Run {
    int baud = 0;
    std::vector<int8_t> sym;
};
struct Tx {
    std::vector<Run> runs;
    double dev = 4500;                         // full deviation, Hz
    double seconds() const { double s = 0; for (const Run& r : runs) s += (double)r.sym.size() / r.baud; return s; }
};

const int kPocsagBaud[3] = {512, 1200, 2400};

std::vector<pager::PocsagPage> pocsagPages(int speed) {
    using pager::PocsagPage;
    auto page = [](uint32_t ric, int fn, int type, const std::string& t) { PocsagPage p; p.ric = ric; p.fn = fn; p.type = type; p.text = t; return p; };
    switch (speed) {
    case kPocsag512:  return {page(1234561, 0, kPagerNumeric, "555-0123 U 45"), page(1234563, 3, kPagerAlpha, "OnAir test 512: hello")};
    case kPocsag1200: return {page(1234561, 0, kPagerNumeric, "12345 U 678"), page(1234562, 2, kPagerTone, ""), page(1234567, 3, kPagerAlpha, "OnAir test 1200: hello")};
    default:          return {page(1234561, 0, kPagerNumeric, "2400-9876"), page(1234563, 3, kPagerAlpha, "OnAir test 2400: hello")};
    }
}

std::vector<pager::FlexPage> flexPages(int speed) {
    using pager::FlexPage;
    const std::string name = std::string(pagerSpeedName(speed)).substr(5);       // "1600/2" from "FLEX 1600/2"
    auto page = [](uint32_t cap, int type, const std::string& t) { FlexPage p; p.cap = cap; p.type = type; p.text = t; return p; };
    return {page(1234567, kPagerAlpha, "OnAir test " + name + ": hello"), page(1234568, kPagerNumeric, "555-0123 U 45"), page(1234569, kPagerTone, ""),
            page(1234570, kPagerAlpha, "Second page " + name)};
}

bool included(int speed, int onlyKind, int onlySpeed) {
    if (onlySpeed > 0) return speed == onlySpeed - 1;
    if (onlyKind == 1) return speed < 3;
    if (onlyKind == 2) return speed >= 3;
    return true;
}

Tx pocsagTx(int speed) {
    Tx tx;
    tx.dev = 4500;
    Run r;
    r.baud = kPocsagBaud[speed];
    for (uint8_t b : pager::pocsagBits(pocsagPages(speed))) r.sym.push_back(b ? -3 : 3);      // a 1 is the lower frequency
    tx.runs.push_back(std::move(r));
    return tx;
}

Tx flexTx(int speed, int cycle, int frame) {
    using namespace pager;
    const FlexMode* m = flexModeForSpeed(speed);
    Tx tx;
    tx.dev = 4800;
    // sync 1 and the frame information word: always 1600 baud, two levels
    Run a;
    a.baud = 1600;
    auto bit2 = [&](int b) { a.sym.push_back(b ? -3 : 3); };
    for (int i = 0; i < 64; i++) bit2(~i & 1);                       // dotting for the AGC
    for (int i = 0; i < 32; i++) bit2(~i & 1);                       // bit sync 1
    for (int i = 15; i >= 0; i--) bit2((m->code >> i) & 1);
    for (int i = 31; i >= 0; i--) bit2((kFlexMarker >> i) & 1);
    for (int i = 15; i >= 0; i--) bit2(((uint16_t)~m->code >> i) & 1);
    for (int i = 0; i < 16; i++) bit2(~i & 1);
    const uint32_t fiw = encodeWord(rev21(flexFiwData(cycle, frame)));
    for (int i = 31; i >= 0; i--) bit2((fiw >> i) & 1);
    tx.runs.push_back(std::move(a));
    // sync 2 (dotting at the data rate), then the 11 blocks
    Run d;
    d.baud = m->baud;
    for (int i = 0; i < m->baud * 25 / 1000; i++) d.sym.push_back(i & 1 ? 3 : -3);
    const int nph = speed == kFlex1600_2 ? 1 : (speed == kFlex6400_4 ? 4 : 2);
    std::vector<FlexPage> per[4];
    const std::vector<FlexPage> pages = flexPages(speed);
    for (size_t k = 0; k < pages.size(); k++) per[k % (size_t)nph].push_back(pages[k]);
    std::vector<uint8_t> ph[4];
    for (int p = 0; p < nph; p++) ph[p] = flexPhaseBits(per[p]);
    auto lev4 = [](int a1, int b1) -> int8_t { return a1 ? (b1 ? -1 : -3) : (b1 ? 1 : 3); };
    const size_t nb = 2816;
    for (size_t i = 0; i < nb; i++) {
        switch (speed) {
        case kFlex1600_2: d.sym.push_back(ph[0][i] ? -3 : 3); break;
        case kFlex3200_2: d.sym.push_back(ph[0][i] ? -3 : 3); d.sym.push_back(ph[1][i] ? -3 : 3); break;
        case kFlex3200_4: d.sym.push_back(lev4(ph[0][i], ph[1][i])); break;
        default:          d.sym.push_back(lev4(ph[0][i], ph[1][i])); d.sym.push_back(lev4(ph[2][i], ph[3][i])); break;
        }
    }
    tx.runs.push_back(std::move(d));
    return tx;
}

std::vector<Tx> buildCycle(int onlyKind, int onlySpeed) {
    std::vector<Tx> v;
    int frame = 3;
    for (int s = 0; s < kPagerSpeeds; s++) {
        if (!included(s, onlyKind, onlySpeed)) continue;
        if (s < 3) v.push_back(pocsagTx(s));
        else { v.push_back(flexTx(s, 0, frame)); frame = (frame + 37) & 127; }
    }
    return v;
}

class PagerSynth : public ModeSynth {
public:
    PagerSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfo_(cfg.cfoHz), inv_(cfg.modeOpt[1] == 1), noise_(cfg.modeOpt[3] > 0 ? (uint32_t)cfg.modeOpt[3] : 1u) {
        tx_ = buildCycle(cfg.modeOpt[0], cfg.modeOpt[2]);
        for (const Tx& t : tx_) len_.push_back((int64_t)std::llround(t.seconds() * rate));
        gap_ = (int64_t)std::llround(kGapSec * rate);
        // amplitude and noise: the noise decides the scale so that its peaks stay below 0.9 (the source rounds to 8 bits)
        amp_ = 0.45f;
        sigma_ = 0.f;
        if (cfg.snrDb > -100) {
            const double sigRel = std::sqrt(rate / (2.0 * 25000.0 * std::pow(10.0, cfg.snrDb / 10.0)));      // per real component, for an amplitude of 1
            amp_ = (float)std::min(0.45, 0.85 / (4.0 * sigRel));
            sigma_ = (float)(sigRel * amp_);
        }
        enterTx();
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const double w0 = 2 * kPi * (cfo_ - pagerTuning().tuneOffsetHz) / rate_;
        for (size_t i = 0; i < n; i++) {
            cf32 v(0.f, 0.f);
            if (tx_.empty()) { out[i] = v; continue; }
            if (pos_ < len_[idx_]) {
                const Tx& t = tx_[idx_];
                const double ts = (double)pos_ / rate_;
                while (run_ + 1 < t.runs.size() && ts >= runEnd_) { runStart_ = runEnd_; run_++; runEnd_ = runStart_ + (double)t.runs[run_].sym.size() / t.runs[run_].baud; }
                const Run& r = t.runs[run_];
                const double u = (ts - runStart_) * r.baud;
                int64_t k = (int64_t)u;
                const int64_t ns = (int64_t)r.sym.size();
                if (k >= ns) k = ns - 1;
                const double fr = u - (double)k;
                double lev = r.sym[(size_t)k];
                if (fr < kEdge && k > 0) {
                    const double w = 0.5 + 0.5 * std::sin(kPi / 2 * fr / kEdge);
                    lev = r.sym[(size_t)k - 1] + (lev - r.sym[(size_t)k - 1]) * w;
                } else if (fr > 1 - kEdge && k + 1 < ns) {
                    const double w = 0.5 - 0.5 * std::sin(kPi / 2 * (1 - fr) / kEdge);
                    lev = lev + (r.sym[(size_t)k + 1] - lev) * w;
                }
                const double f = (inv_ ? -1.0 : 1.0) * lev / 3.0 * t.dev;
                phase_ += w0 + 2 * kPi * f / rate_;
                if (phase_ > kPi) phase_ -= 2 * kPi; else if (phase_ < -kPi) phase_ += 2 * kPi;
                const double ramp = std::min({1.0, ts / 0.001, ((double)len_[idx_] / rate_ - ts) / 0.001});
                const float a = amp_ * (float)std::max(0.0, ramp);
                v = cf32(a * (float)std::cos(phase_), a * (float)std::sin(phase_));
            }
            out[i] = v;
            if (++pos_ >= len_[idx_] + gap_) { idx_ = (idx_ + 1) % tx_.size(); enterTx(); }
        }
        if (sigma_ > 0) noise_.add(out, n, sigma_);
    }

private:
    void enterTx() {
        pos_ = 0; run_ = 0; runStart_ = 0;
        runEnd_ = tx_.empty() || tx_[idx_].runs.empty() ? 0 : (double)tx_[idx_].runs[0].sym.size() / tx_[idx_].runs[0].baud;
    }
    double rate_, cfo_;
    bool inv_;
    genutil::NoiseSource noise_;
    std::vector<Tx> tx_;
    std::vector<int64_t> len_;
    int64_t gap_ = 0, pos_ = 0;
    size_t idx_ = 0, run_ = 0;
    double runStart_ = 0, runEnd_ = 0, phase_ = 0;
    float amp_ = 0.45f, sigma_ = 0.f;
};

} // namespace

std::vector<PagerTestMessage> pagerTestMessages(int onlyKind, int onlySpeed) {
    std::vector<PagerTestMessage> v;
    for (int s = 0; s < kPagerSpeeds; s++) {
        if (!included(s, onlyKind, onlySpeed)) continue;
        if (s < 3) {
            for (const auto& p : pocsagPages(s)) { PagerTestMessage m; m.speed = s; m.flex = false; m.address = p.ric; m.function = p.fn; m.type = p.type; m.text = p.text; v.push_back(m); }
        } else {
            for (const auto& p : flexPages(s)) { PagerTestMessage m; m.speed = s; m.flex = true; m.address = p.cap; m.function = -1; m.type = p.type; m.text = p.text; v.push_back(m); }
        }
    }
    return v;
}

double pagerCycleSeconds(int onlyKind, int onlySpeed) {
    double s = 0;
    for (const Tx& t : buildCycle(onlyKind, onlySpeed)) s += t.seconds() + kGapSec;
    return s;
}

std::unique_ptr<ModeSynth> makePagerSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<PagerSynth>(cfg, sampleRate);
}

} // namespace dect2
