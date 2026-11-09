#include "dect2/fm/fm_af.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

static int key(double mhz) { return (int)std::lround(mhz * 10); }

void FmAfFollow::reset() { state_ = Idle; request_ = false; probing_ = false; cand_.clear(); }
void FmAfFollow::requestBest() { request_ = true; }

FmAfCommand FmAfFollow::update(double now, double tuned, int pi, float snr, float level, const std::vector<double>& af, const std::function<bool(double, float&)>& levelOf) {
    FmAfCommand c;
    if (state_ == Confirming) {
        const bool timeout = now - since_ > cfg_.confirmSec;
        if (pi != 0 && pi != wantPi_) {          // the proof failed: a regional variant (method B) or another station
            rejected_[key(target_)] = now + cfg_.rejectSec;
            c.type = FmAfCommand::Back; c.mhz = origin_; c.why = "other station on this frequency"; state_ = Idle; holdUntil_ = now + cfg_.holdSec; return c;
        }
        if (pi == wantPi_ && (!probing_ || snr >= originSnr_ + 3)) {
            c.confirmed = true; c.mhz = target_; c.why = "same programme, PI proved"; state_ = Idle; holdUntil_ = now + cfg_.holdSec; return c;
        }
        if (timeout) {                           // nothing proved it in time: back where we came from
            if (pi == 0 && !probing_) rejected_[key(target_)] = now + cfg_.rejectSec;
            c.type = FmAfCommand::Back; c.mhz = origin_; c.why = probing_ ? "not better there" : "no PI after the retune"; state_ = Idle; holdUntil_ = now + cfg_.holdSec;
        }
        return c;
    }
    cand_.clear();
    for (double m : af) {
        if (std::fabs(m - tuned) < 0.05) continue;
        FmAfCandidate x; x.mhz = m;
        float db = -200; x.inSpan = levelOf && levelOf(m, db); x.levelDb = db;
        auto it = rejected_.find(key(m));
        x.rejected = it != rejected_.end() && it->second > now;
        cand_.push_back(x);
    }
    std::sort(cand_.begin(), cand_.end(), [](const FmAfCandidate& a, const FmAfCandidate& b) { return a.levelDb > b.levelDb; });
    if (pi == 0) return c;
    const bool wantAuto = cfg_.autoFollow && snr < cfg_.snrTriggerDb && now >= holdUntil_;
    if (!request_ && !wantAuto) return c;
    const bool manual = request_;
    request_ = false;
    const FmAfCandidate* best = nullptr;
    for (const auto& x : cand_) if (x.inSpan && !x.rejected && x.levelDb > level + (manual ? 0.f : cfg_.marginDb)) { best = &x; break; }
    if (best) {
        state_ = Confirming; origin_ = tuned; target_ = best->mhz; since_ = now; wantPi_ = pi; originSnr_ = snr; probing_ = false;
        c.type = FmAfCommand::Tune; c.mhz = target_; c.why = "stronger alternative frequency";
        return c;
    }
    // no AF known to be stronger inside the capture: a short probe of one that lies outside it
    if (now - lastProbe_ >= cfg_.probeEverySec || manual) {
        std::vector<const FmAfCandidate*> blind;
        for (const auto& x : cand_) if (!x.inSpan && !x.rejected) blind.push_back(&x);
        if (!blind.empty()) {
            const FmAfCandidate* p = blind[probeIdx_++ % blind.size()];
            lastProbe_ = now; state_ = Confirming; origin_ = tuned; target_ = p->mhz; since_ = now; wantPi_ = pi; originSnr_ = snr; probing_ = true;
            c.type = FmAfCommand::Tune; c.mhz = target_; c.why = "checking an alternative frequency";
        }
    }
    return c;
}

} // namespace dect2
