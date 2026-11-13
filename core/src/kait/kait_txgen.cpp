// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAIT transmit generator: see kait_txgen.h.
#include "dect2/kait/kait_txgen.h"
#include "dect2/kait/kait_frame.h"
#include "dect2/kait/kait_image.h"
#include "dect2/kapp/kapp_frame.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2 {

namespace {

class KaitGen : public TxGenerator {
public:
    KaitGen(double rate, std::vector<kait::cf> burst, double gapSec) : rate_(rate), burst_(std::move(burst)) {
        gap_ = (size_t)(gapSec * kait::kInternalRate);
        if (std::fabs(rate - kait::kInternalRate) > 0.5) rs_ = std::make_unique<kapp::Resampler>(kait::kInternalRate, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        size_t have = 0;
        while (have < n) {
            if (rdPos_ < queue_.size()) {
                const size_t k = std::min(n - have, queue_.size() - rdPos_);
                std::copy(queue_.begin() + (long)rdPos_, queue_.begin() + (long)(rdPos_ + k), out + have);
                rdPos_ += k;
                have += k;
                continue;
            }
            queue_.clear();
            rdPos_ = 0;
            refill();
        }
    }
private:
    void refill() {   // 20 ms of the cycle (burst, then silence) at the internal rate, converted to the output rate
        const size_t chunk = 12640;
        std::vector<kait::cf> c(chunk, kait::cf{0.f, 0.f});
        for (size_t i = 0; i < chunk; i++) {
            if (pos_ < burst_.size()) c[i] = burst_[pos_];
            if (++pos_ >= burst_.size() + gap_) pos_ = 0;
        }
        if (rs_) queue_ = rs_->process(c.data(), c.size());
        else queue_ = std::move(c);
    }
    double rate_;
    std::vector<kait::cf> burst_;
    size_t gap_ = 0, pos_ = 0, rdPos_ = 0;
    std::unique_ptr<kapp::Resampler> rs_;
    std::vector<kait::cf> queue_;
};

}  // namespace

std::unique_ptr<TxGenerator> makeKaitTxGenerator(const TxRequest& r, double rateHz, std::string& err) {
    if (rateHz < 400000.0 - 1) { err = "KAIT needs a transmit rate of at least 400 kHz (632 kHz is its own)"; return nullptr; }
    std::string cs;
    for (char c : r.callsign) cs += (char)std::toupper((unsigned char)c);
    while (!cs.empty() && cs.back() == ' ') cs.pop_back();
    if (cs.empty()) cs = "ONAIR";   // an empty field is the demo identity, like every other mode
    if (cs.size() > (size_t)kait::kMaxCallsign) { err = "KAIT: the callsign is at most 9 characters"; return nullptr; }
    for (char c : cs)
        if (!(c == '/' || c == '-' || c == '.' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) { err = "KAIT: the callsign may hold only letters, digits, / - and ."; return nullptr; }
    const int profile = r.variant;
    if (!kait::profileById(profile)) { err = "KAIT: the profile is 0 to 3"; return nullptr; }
    kait::Image img;
    if (r.file.empty()) img = kait::syntheticPicture();   // the demo picture
    else if (!kait::loadPicture(r.file, img, err)) { err = "KAIT: " + err; return nullptr; }
    img = kait::fitPicture(img);
    const kait::BurstPlan plan = kait::planBurst(img, cs, profile);
    if (!plan.ok) { err = "KAIT: the burst could not be planned"; return nullptr; }
    std::vector<kait::cf> burst = kait::buildBurst(plan);
    float pk = 0;
    for (const kait::cf& v : burst) pk = std::max(pk, std::max(std::fabs(v.real()), std::fabs(v.imag())));
    if (pk > 0) for (kait::cf& v : burst) v *= 0.9f / pk;
    const double gapSec = std::min(120.0, std::max(0.5, r.o("kait.gap", 2.0)));   // silence between two bursts
    return std::make_unique<KaitGen>(rateHz, std::move(burst), gapSec);
}

} // namespace dect2
