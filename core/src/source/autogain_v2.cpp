// Auto gain v2 (docs/AUTOGAIN.md, "Auto v2"): a hill climb on the MEASURED objective (the decoder's MER / SNR while it locks, else the channel SNR from
// the flanks) under constraints (no clipping, a headroom floor from the 99.99 % point or the 2 s peak-hold, no intermodulation or compression seen by
// the gain-slope test on every step), instead of servoing the wideband headroom to a per-class constant. Plain logic, no clock of its own.
#include "dect2/source/autogain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>

namespace dect2::ag {

namespace {
std::string f2(const char* f, double a, double b = 0, double c = 0, double d = 0) {
    char buf[320];
    snprintf(buf, sizeof buf, f, a, b, c, d);
    return buf;
}
}

void AutoGainController::climbReset(double now) {
    clTrial_ = false; settled_ = false; cand_ = 0; candKind_ = 0; clStep_ = 2; rejectRun_ = 0; clMark_ = -1;
    base_ = Run(); tri_ = Run(); specPow_.clear(); specCnt_ = 0; specBase_.clear(); settledAt_ = -1e9; settledJ_ = 0;
    wasLocked_ = false; lockLostAt_ = -1; status_.clear(); limit_.clear(); lastDecision_.clear(); retuneWhy_.clear(); slope_ = 1;
    dwellFrom_ = now; jRecent_ = 0; jRecentN_ = 0; clDir_ = 1; statusAt_ = -1e9;
}

bool AutoGainController::objective(double& j) const {
    const Metric m = resolvedMetric(q_);
    double v = 0;
    switch (m) {
    case Metric::Mer: case Metric::Snr: case Metric::Ber: case Metric::Lock:
        if (q_.locked && score(m, q_, fl_, adcLast_, tgt_, headroomTarget(), v)) { j = 50.0 + v; return true; }   // a locked decoder always beats an unlocked one
        if (fl_.valid) { j = fl_.snrDb; return true; }
        return false;
    case Metric::ChannelSnr: case Metric::Sensitivity:
        if (score(m, q_, fl_, adcLast_, tgt_, headroomTarget(), v)) { j = v; return true; }
        return false;
    default: return false;
    }
}

bool AutoGainController::comfortable() const {
    if (!q_.locked || tgt_.lockSnrDb > 1e8) return false;
    const double v = q_.merValid ? q_.merDb : q_.snrValid ? q_.snrDb : -1e9;
    if (v < -1e8) return false;
    return v >= tgt_.lockSnrDb + tgt_.comfortDb;
}

void AutoGainController::climbSample(const AgInput& in, double j) {
    Run& r = clTrial_ ? tri_ : base_;
    r.add(j);
    met_.objective = j; met_.objectiveValid = true;
    if (in.spectrum && !in.spectrum->empty()) {
        if (specPow_.size() != in.spectrum->size()) { specPow_.assign(in.spectrum->size(), 0.0); specCnt_ = 0; }
        for (size_t k = 0; k < in.spectrum->size(); k++) specPow_[k] += std::pow(10.0, (double)(*in.spectrum)[k] * 0.1);
        specCnt_++;
    }
}

static std::vector<float> meanDb(const std::vector<double>& p, int n) {
    std::vector<float> v(p.size());
    for (size_t k = 0; k < p.size(); k++) v[k] = (float)(10.0 * std::log10(std::max(p[k] / std::max(1, n), 1e-14)));
    return v;
}

bool AutoGainController::tickClimb(const AgInput& in, AgCommand& out) {
    const double now = in.now, sp = std::max(0.25, set_.loopSpeed);
    phase_ = Phase::Tracking;
    adcLast_ = in.adc;
    double j = 0;
    if (!objective(j)) return false;
    // the world moved the gain under a trial (an overload cut): the trial is void
    if (clTrial_ && changedAt_ != trialAt_) { clTrial_ = false; settled_ = false; cand_ = 0; clMark_ = -1; }
    if (clMark_ != changedAt_) {   // a new gain: new statistics
        (clTrial_ ? tri_ : base_) = Run();
        specPow_.clear(); specCnt_ = 0; clMark_ = changedAt_; dwellFrom_ = now;
    }
    climbSample(in, j);
    // re-run triggers: the decoder lost for 10 s
    if (q_.locked) { wasLocked_ = true; lockLostAt_ = -1; }
    else if (wasLocked_) {
        if (lockLostAt_ < 0) lockLostAt_ = now;
        else if (now - lockLostAt_ > 10 / sp) { wasLocked_ = false; lockLostAt_ = -1; wantBracket_ = true; retuneWhy_ = "the decoder lost lock for 10 s"; settled_ = false; return false; }
    }
    double eff = headAvg_;
    if (tgt_.usePeakHold && in.adc.holdHeadroomDb < 119) eff = std::min(eff, in.adc.holdHeadroomDb);
    const double minHead = tgt_.minHeadroomDb;
    const double dwell = set_.dwellSec / sp;
    // too close to full scale: down at once, in proportion (a trial that gets there is undone, see evaluateTrial)
    if (eff < minHead && !clTrial_ && now - lastDown_ >= 0.8 / sp) {
        const double to = nextStep(cur_.totalDb, -std::min(10.0, minHead - eff + 1.0), pol_);
        if (to < cur_.totalDb - 0.01) {
            limit_ = "headroom";
            move(now, to, pol_, f2("headroom %.1f dB is under the %.0f dB minimum: gain down", eff, minHead), out);
            lastDecision_ = out.why; settled_ = false; cand_ = 0;
            return true;
        }
    }
    if (now - dwellFrom_ < dwell || (clTrial_ ? tri_.n : base_.n) < 3) return false;
    if (clTrial_) return evaluateTrial(in, out);
    if (settled_) {
        const bool comf = comfortable();
        // a settled loop watches for the objective falling away (3 dB): the world changed, search again
        if (base_.n >= 5 && base_.mean < settledJ_ - 3.0) {
            wantBracket_ = true; settled_ = false; retuneWhy_ = f2("the objective fell %.1f dB", settledJ_ - base_.mean);
            return false;
        }
        phase_ = comf ? Phase::Frozen : Phase::Tracking;
        // comfortable: rest 30 s; a decoder barely locked (or none) keeps optimising: look again after 6 s
        if (now - settledAt_ < (comf ? 30.0 : 6.0) / sp) {
            if (now - dwellFrom_ > 12 / sp) { base_ = Run(); specPow_.clear(); specCnt_ = 0; dwellFrom_ = now; }   // a fresh baseline for the next probe
            return false;
        }
        settled_ = false; cand_ = 0;
    }
    return startTrial(in, out);
}

bool AutoGainController::startTrial(const AgInput& in, AgCommand& out) {
    const double now = in.now, sp = std::max(0.25, set_.loopSpeed);
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    const Policy pol = (now < linearUntil_ || strongNb || imdSeen_) ? Policy::Linearity : tgt_.policy;
    double eff = headAvg_;
    if (tgt_.usePeakHold && in.adc.holdHeadroomDb < 119) eff = std::min(eff, in.adc.holdHeadroomDb);
    const double ceiling = std::min(maxGain(), ceilingDb(now));
    const bool wasted = fl_.valid && fl_.adcMarginDb > tgt_.marginGoalDb + 15;   // the antenna's noise is far above the ADC's: more gain buys nothing
    const int seq[3] = {clDir_ >= 0 ? 1 : 2, clDir_ >= 0 ? 2 : 1, 3};
    // the policy changed at the same gain (a neighbour appeared / went): re-split first, without a trial
    if (pol != pol_ && now - changedAt_ > 3 / sp) {
        StagePlan p = planStages(radio_, cur_.totalDb, pol, set_.stageMask, cur_.db, cur_.ampOn);
        if (!(p == cur_)) { move(now, cur_.totalDb, pol, std::string("stages re-split for ") + policyName(pol), out); return true; }
        pol_ = pol;
    }
    int splitCount = 0;
    for (const auto& s : radio_.stages) if (s.settable && !s.userOnly) splitCount++;
    while (cand_ < 3) {
        const int kind = seq[cand_];
        double to = cur_.totalDb;
        Policy tp = pol;
        std::string what;
        if (kind == 1) {   // up: the largest step that keeps the headroom floor, not over the ceiling
            const double room = eff - minHead;
            if (cur_.totalDb >= ceiling - 0.01 || wasted || room < 0.7) { limit_ = room < 0.7 ? "headroom" : wasted ? "noise floor already far over the ADC's" : cur_.totalDb >= maxGain() - 0.01 ? "top of the gain range" : "ceiling after an overload"; cand_++; continue; }
            const double want = std::min({clStep_, tgt_.maxStepDb, room});
            to = nextStep(cur_.totalDb, want, pol);
            double snapTo = planStages(radio_, to, pol, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
            if (snapTo > ceiling + 0.01) {
                snapTo = cur_.totalDb;
                for (double g : totalSteps(radio_, pol, set_.stageMask, cur_.ampOn)) if (g <= ceiling + 0.01 && g > snapTo + 0.01) snapTo = g;
            }
            if (snapTo <= cur_.totalDb + 0.01 || radio_.realDelta(cur_.totalDb, snapTo) > room + 0.5) { cand_++; continue; }
            to = snapTo;
            what = f2("probe +%.1f dB", radio_.realDelta(cur_.totalDb, to));
        } else if (kind == 2) {   // down
            if (cur_.totalDb <= minGain() + 0.01) { cand_++; continue; }
            to = nextStep(cur_.totalDb, -clStep_, pol);
            if (to >= cur_.totalDb - 0.01) { cand_++; continue; }
            what = f2("probe %.1f dB", radio_.realDelta(cur_.totalDb, to));
        } else {   // the other stage split at the same total (HackRF: LNA first or VGA first)
            if (splitCount < 2 || cur_.ampOn) { cand_++; continue; }
            tp = pol == Policy::Linearity ? Policy::Sensitivity : Policy::Linearity;
            if (imdSeen_ || now < linearUntil_) { cand_++; continue; }   // after an overload the linear split stays
            const StagePlan p = planStages(radio_, cur_.totalDb, tp, set_.stageMask, cur_.db, cur_.ampOn);
            if (p == cur_) { cand_++; continue; }
            what = std::string("probe split: ") + policyName(tp);
        }
        // the baseline is complete: remember it and move
        basePlan_ = cur_; basePol_ = pol_;
        specBase_ = meanDb(specPow_, specCnt_);
        candKind_ = kind;
        specPow_.clear(); specCnt_ = 0;
        move(now, to, tp, what, out);
        clTrial_ = true; trialAt_ = changedAt_; tri_ = Run(); clMark_ = changedAt_; dwellFrom_ = now;
        return true;
    }
    // every candidate in the cycle was rejected or is not available: settled here
    settled_ = true; settledAt_ = now; settledJ_ = base_.mean; cand_ = 0;
    const bool comf = comfortable();
    phase_ = comf ? Phase::Frozen : Phase::Tracking;
    lastDecision_ = comf ? "settled: the decoder is comfortably over its threshold" : "settled: no neighbouring gain is better";
    return false;
}

bool AutoGainController::evaluateTrial(const AgInput& in, AgCommand& out) {
    const double now = in.now;
    const double j0 = base_.mean, j1 = tri_.mean;
    const double neff0 = std::max(1.0, base_.n / 6.0), neff1 = std::max(1.0, tri_.n / 6.0);
    const double thr = std::max(tgt_.improveDb, 1.5 * std::sqrt(base_.var() / neff0 + tri_.var() / neff1));
    const double dJ = j1 - j0;
    const double dg = radio_.realDelta(basePlan_.totalDb, cur_.totalDb);
    double eff = headAvg_;
    if (tgt_.usePeakHold && in.adc.holdHeadroomDb < 119) eff = std::min(eff, in.adc.holdHeadroomDb);
    // the gain-slope test, free on every step the loop makes: the spectrum before and after against the real gain change
    bool imd = false, comp = false;
    std::string slopeWhy;
    slope_ = 1;
    if (std::fabs(dg) >= 1.5 && specCnt_ > 0 && !specBase_.empty()) {
        const std::vector<float> hi = meanDb(specPow_, specCnt_);
        if (hi.size() == specBase_.size()) {
            const std::vector<float>& a = dg > 0 ? specBase_ : hi;
            const std::vector<float>& b = dg > 0 ? hi : specBase_;
            const ImdReport r = imdCompare(a, b, std::fabs(dg));
            if (r.valid) {
                slope_ = r.slopeDbPerDb;
                if (r.spurs > 0) { imd = true; slopeWhy = f2("%.0f spurs rose %.1f dB more than the gain", r.spurs, r.worstExcessDb); }
                if (r.compressed) { comp = true; slopeWhy = f2("the wideband power rose only %.2f dB per dB (compression)", r.slopeDbPerDb); }
            }
            const FloorEstimate fh = estimateFloor(hi, in.rateHz, in.chanBwHz, in.offsetHz, radio_.adcBits);
            const FloorEstimate fb = estimateFloor(specBase_, in.rateHz, in.chanBwHz, in.offsetHz, radio_.adcBits);
            if (dg > 0 && fh.valid && fb.valid && fh.floorDbfs - fb.floorDbfs > 1.3 * dg + 0.8) { imd = true; slopeWhy = f2("the noise floor rose %.1f dB for %.1f dB of gain", fh.floorDbfs - fb.floorDbfs, dg); }
        }
    }
    met_.slopeDbPerDb = slope_;
    const bool clip = in.adc.clipFraction > tgt_.clipLimit;
    const bool tight = eff < tgt_.minHeadroomDb;
    bool accept = false;
    std::string why;
    if (candKind_ == 1) {
        const bool bad = imd || comp || clip || tight;
        accept = !bad && dJ > thr;
        if (bad) {
            const double hold = (imd || comp) ? 120.0 : 30.0;
            ceil_ = basePlan_.totalDb + 0.01; ceilUntil_ = now + hold / std::max(0.25, set_.loopSpeed);
            if (imd || comp) { imdSeen_ = imdSeen_ || imd; compSeen_ = compSeen_ || comp; linearUntil_ = std::max(linearUntil_, now + 60); }
            limit_ = imd ? "intermodulation" : comp ? "compression" : clip ? "overload" : "headroom";
            why = f2("+%.1f dB undone: ", dg) + (imd || comp ? slopeWhy : clip ? std::string("the ADC clipped") : f2("headroom %.1f dB under the %.0f dB minimum", eff, tgt_.minHeadroomDb));
        } else why = accept ? f2("+%.1f dB kept: objective +%.1f dB", dg, dJ) : f2("+%.1f dB undone: objective only %+.1f dB (needs %.1f)", dg, dJ, thr);
    } else if (candKind_ == 2) {
        const bool nearLimit = basePlan_.totalDb > 0 && (tight || eff < tgt_.minHeadroomDb + 1.0);
        accept = dJ > thr || (nearLimit && dJ > -0.25 * thr);
        why = accept ? f2("%.1f dB kept: objective %+.1f dB", dg, dJ) : f2("%.1f dB undone: objective %+.1f dB", dg, dJ);
    } else {
        accept = !imd && !comp && !clip && !tight && dJ > thr;
        why = accept ? f2("other stage split kept: objective +%.1f dB", dJ) : f2("other stage split undone: objective %+.1f dB", dJ);
    }
    lastDecision_ = why;
    if (accept) {
        clTrial_ = false;
        base_ = tri_;               // what was measured is the new baseline (and the spectrum accumulating now is its own)
        if (candKind_ == 1) clDir_ = 1; else if (candKind_ == 2) clDir_ = -1;
        if (candKind_ != 3) clStep_ = std::min(tgt_.maxStepDb, clStep_ * 1.5);
        cand_ = 0; settled_ = false; rejectRun_ = 0;
        pol_ = candKind_ == 3 ? pol_ : pol_;
        logEvent(now, basePlan_.totalDb, cur_.totalDb, why);
        specBase_.clear();
        limit_.clear();
        out.why = why;
        return false;   // already at the trial's gain: nothing to apply
    }
    // undone: back to the baseline plan (exactly), measure it again
    out.plan = basePlan_; out.why = why;
    logEvent(now, cur_.totalDb, basePlan_.totalDb, why);
    if (basePlan_.totalDb > cur_.totalDb + 0.01) { lastUp_ = now; dir_ = 1; } else if (basePlan_.totalDb < cur_.totalDb - 0.01) { lastDown_ = now; dir_ = -1; }
    cur_ = basePlan_; pol_ = basePol_;
    changedAt_ = now; nAvg_ = 0; changes_++;
    clTrial_ = false; cand_++; clStep_ = std::max(2.0, clStep_ * 0.5); rejectRun_++;
    return true;
}

void AutoGainController::setStatus(const std::string& lead) {
    (void)lead;
    std::string s = f2("Gain %.0f dB", cur_.totalDb);
    double j = 0;
    if (q_.locked && (q_.merValid || q_.snrValid)) s += f2(": decoder %.1f dB", q_.merValid ? q_.merDb : q_.snrDb);
    else if (fl_.valid) s += f2(": channel SNR %.1f dB", fl_.snrDb);
    (void)j;
    s += f2(", headroom %.1f dB (min %.0f)", headAvg_, tgt_.minHeadroomDb);
    if (br_ || wantBracket_) s += std::string(", searching") + (retuneWhy_.empty() ? "" : " (" + retuneWhy_ + ")");
    else if (clTrial_) s += ", trying a neighbouring gain";
    else if (settled_) s += comfortable() ? ", settled (decoder comfortably locked)" : ", settled";
    else s += ", measuring";
    if (!limit_.empty()) s += "; limited by " + limit_;
    if (!lastDecision_.empty()) s += "; last: " + lastDecision_;
    status_ = s;
}

std::string AutoGainController::traceLine(double now) const {
    char b[512];
    snprintf(b, sizeof b, "%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.5f,%s,\"%s\"", now, cur_.totalDb, met_.objectiveValid ? met_.objective : 0.0, fl_.valid ? fl_.snrDb : 0.0,
             met_.headroomDb, fl_.valid ? fl_.floorDbfs : -200.0, met_.clipFraction, phaseName(phase_), lastDecision_.c_str());
    return b;
}

} // namespace dect2::ag
