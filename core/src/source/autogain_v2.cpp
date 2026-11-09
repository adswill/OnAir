// Auto gain v2 (docs/AUTOGAIN.md, "Auto v2"): a hill climb on the MEASURED objective (the decoder's MER / SNR while it locks, else the channel SNR from
// the flanks) under constraints (no clipping, a headroom floor from the 99.99 % point or the 2 s peak-hold, no intermodulation or compression seen by
// the gain-slope test on every step), instead of servoing the wideband headroom to a per-class constant. Plain logic, no clock of its own.
#include "dect2/source/autogain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>

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
    pj_.clear(); pjMark_ = -1; base_ = Run(); tri_ = Run(); mBase_ = Run(); mTri_ = Run(); specPow_.clear(); specCnt_ = 0; specBase_.clear(); settledAt_ = -1e9; settledJ_ = 0;
    decB_ = Run(); decT_ = Run(); settledDec_ = -1e9; refValid_ = false; lastSearchAt_ = -1e9; lastWakeAt_ = -1e9; blindRest_ = false; splitTried_ = false; splitKept_ = false; cutActive_ = false; cutFrom_ = -1e9; cutAt_ = -1e9;
    bracketedOnce_ = false;
    wasLocked_ = false; lockLostAt_ = -1; status_.clear(); limit_.clear(); lastDecision_.clear(); retuneWhy_.clear(); slope_ = 1;
    lastSigAt_ = -1e9; lastRecoverAt_ = -1e9; recWait_ = 1.5;
    dwellFrom_ = now; jRecent_ = 0; jRecentN_ = 0; clDir_ = 1; statusAt_ = -1e9; rejUp_ = 0; idleCycles_ = 0; goodGain_ = -1e9;
}

bool AutoGainController::objective(double& j) const {
    const Metric m = resolvedMetric(q_);
    double v = 0;
    // pulsed message modes (ADS-B): the messages per second at this gain, docked for a high share of loud ones (readsb's adaptive gain). The
    // pulses are far under 0.1 % of the samples, so the floor/SNR numbers of the other modes do not describe them.
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid) {
        if (!pj_.ready()) return false;
        j = pj_.objective();
        return true;
    }
    switch (m) {
    case Metric::Mer: case Metric::Snr: case Metric::Ber: case Metric::Lock:
        if (fl_.valid) { j = fl_.snrDb; return true; }
        return decoderValue(j);   // no spectrum: only the decoder's number
    case Metric::ChannelSnr: case Metric::Sensitivity:
        if (score(m, q_, fl_, adcLast_, tgt_, minHead_(), v)) { j = v; return true; }   // headroom is a floor in the climb, not a set-point
        return false;
    default: return false;
    }
}

// The decoder's own number while it is locked, the quality the user cares about (a product inside the channel reads as signal in the spectrum, so level alone cannot tell).
// Kept apart from the level objective: a lock that flickers changes which of the two is valid, and a mean over both reads as a swing of tens of dB.
bool AutoGainController::decoderValue(double& v) const {
    if (!q_.locked) return false;
    const Metric m = resolvedMetric(q_);
    if (m == Metric::Sensitivity || m == Metric::Headroom) return false;
    if (m == Metric::ChannelSnr) { if (!q_.merValid && !q_.snrValid) return false; v = q_.merValid ? q_.merDb : q_.snrDb; return true; }
    return score(m, q_, fl_, adcLast_, tgt_, headroomTarget(), v);
}

bool AutoGainController::hasSignal() const {
    if (tgt_.kind == ModeKind::WideSpan) return true;   // a capture always has something in it: the headroom floor is what stops the gain
    if (q_.locked) return true;
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid) return pj_.started() && pj_.msgs() > 0;
    // a channel that fills the capture has no floor to compare with (its SNR reads a few dB on nothing): a mode with a decoder then needs the decoder to say so
    if (tgt_.lockSnrDb < 1e8 && !(fl_.oobDbfs > -199)) return false;
    // a mode with a decoder: level alone must be close to what would lock it (FM stations' products read 6-10 dB in an empty channel and the loop chased them)
    // ... and after the first minute a decoder that does not lock says there is nothing to judge a gain by, whatever the level reads
    if (tgt_.lockSnrDb < 1e8 && nowT_ - startedAt_ > 60.0) return false;
    return fl_.valid && fl_.snrDb >= (tgt_.lockSnrDb < 1e8 ? std::max(6.0, tgt_.lockSnrDb - 6.0) : 3.0);
}

double AutoGainController::minHead_() const { return tgt_.minHeadroomDb; }

// Clipping spreads roughly 30 times the clipped fraction of the span's power over all bins. A strong carrier 60 dB over the floor: a fraction far under the mode's limit already
// buries a weak channel beside it, and the spectrum reads clean until it is too late. So the limit is the fraction whose distortion stays under the noise floor per bin
// (never above the mode's own, never below 2 % of it, Auto v2 only). An OFDM channel's floor is close to its span: its limit does not move.
double AutoGainController::clipLimitNow() const {
    if (!set_.v2 || !fl_.valid || fl_.spanDbfs < -150 || specBins_ <= 0) return tgt_.clipLimit;
    const double allowed = std::pow(10.0, (fl_.floorDbfs - fl_.spanDbfs) / 10.0) * specBins_ / 30.0;
    return std::clamp(allowed, tgt_.clipLimit * 0.02, tgt_.clipLimit);
}

bool AutoGainController::floorFine() const { return fl_.valid && fl_.adcMarginDb >= tgt_.marginGoalDb - 2.0; }

void AutoGainController::noteCut(double now) {
    if (now - lastRecoverAt_ < 120.0) recWait_ = std::min(600.0, recWait_ * 3.0);   // the jump back clipped again: the next one waits longer (a keyed transmitter must not pump the gain)
    cutOobIn_ = -300;   // measured once the cut has settled: during the clip the out-of-band power is the distortion's
    if (!cutActive_) { cutActive_ = true; cutFrom_ = clTrial_ ? basePlan_.totalDb : cur_.totalDb; cutWasOob_ = fl_.valid && (!(fl_.oobDbfs > -199) || (fl_.strongestDbfs > fl_.floorDbfs + 20 && fl_.strongestDbfs > fl_.chanDbfs + 6)); }   // a line standing out of the floor outside the channel (clipping's distortion is flat); a channel that fills the capture cannot tell: treated as possibly from outside
    cutAt_ = now;
}

void AutoGainController::settleHere(const AgInput& in) {
    settled_ = true; settledAt_ = in.now; settledJ_ = base_.mean; cand_ = 0; clStep_ = 2;
    settledDec_ = decB_.n >= 3 ? decB_.mean : -1e9;
    refValid_ = fl_.valid;
    if (fl_.valid) { floorRef_ = fl_.floorDbfs - cur_.totalDb; oobRef_ = fl_.oobDbfs - cur_.totalDb; }
    headRef_ = headAvg_ + cur_.totalDb;
    if (!(in.adc.clipFraction > clipLimitNow()) && (!cutActive_ || cur_.totalDb >= cutFrom_ - 1)) goodGain_ = cur_.totalDb;
    if (cutActive_ && (cur_.totalDb >= cutFrom_ - 1 || !cutWasOob_)) cutActive_ = false;   // a cut on the channel's own signal is the right gain for it: nothing to come back to
}

// Input-referred (the gain subtracted): a change in the floor or in what lies outside the channel is the world's, not ours. The channel's own level is left out: a call that
// ends or a burst that passes says nothing about the gain.
bool AutoGainController::worldChanged(const AgInput& in, std::string& why) {
    if (!refValid_ || !fl_.valid || in.now - lastSearchAt_ < 30.0 / std::max(0.25, set_.loopSpeed)) return false;
    const double floorIn = fl_.floorDbfs - cur_.totalDb, oobIn = fl_.oobDbfs - cur_.totalDb;
    if (floorIn > floorRef_ + 4.0) { why = f2("the noise floor rose %.1f dB", floorIn - floorRef_); return true; }
    if (fl_.oobDbfs > -199) { if (oobIn < oobRef_ - 6.0) { why = f2("a strong signal went away (%.1f dB less outside the channel)", oobRef_ - oobIn); return true; } }
    else if (headAvg_ + cur_.totalDb <= headRef_ + 6.0) headRiseAt_ = -1;
    else if (headRiseAt_ < 0) headRiseAt_ = in.now;
    else if (hasSignal() && in.now - headRiseAt_ >= 1.5 /* longer than a decoder takes to report that the channel itself has gone */) { why = f2("a strong signal went away (%.1f dB more headroom)", headAvg_ + cur_.totalDb - headRef_); return true; }   // the channel fills the capture: only the headroom can tell
    return false;
}

// the real gain change of a step: the calibration when there is one, else the nominal step times what this unit was seen to do
double AutoGainController::stepReal(double from, double to) const {
    if (radio_.calibrated()) return radio_.realDelta(from, to);
    return (to - from) * (radio_.driverSplits && radio_.singleControl() ? unitScale_ : 1.0);
}

// The headroom the climb keeps above its floor. Pulsed modes with message counters: the pulse peak (the 2 s peak-hold) alone, since a pulse is under
// 0.1 % of the samples and the 99.99 % point only measures the noise; clipping is still caught by the clipped fraction. Else the lower of the two.
double AutoGainController::effHeadroom(const AgInput& in) const {
    const bool hold = in.adc.holdHeadroomDb < 119;
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid && hold) return std::min(headAvg_, std::max(in.adc.holdHeadroomDb, minHead_() + 1.0));   // (the noise itself near full scale still counts) messages say more than one peak: the nearest aircraft may touch full scale; the loud share (tooLoud) and the clip fraction take the gain down
    double eff = headAvg_;
    if (tgt_.usePeakHold && hold) eff = std::min(eff, in.adc.holdHeadroomDb);
    return eff;
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
    double dv = 0;
    if (decoderValue(dv)) (clTrial_ ? decT_ : decB_).add(dv);
    if (fl_.valid) (clTrial_ ? mTri_ : mBase_).add(fl_.adcMarginDb);
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
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid) {
        if (pjMark_ != changedAt_ || !pj_.started()) { pj_.restart(now, q_.pulseMsgs, q_.pulseLoud); pjMark_ = changedAt_; }   // a new gain: the counts start again
        else pj_.update(now, q_.pulseMsgs, q_.pulseLoud);
    }
    // too many loud messages: the near aircraft overload the front end. Down at once, a step at a time, as readsb does
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid && pj_.tooLoud() && !clTrial_ && now - lastDown_ >= 2.0 / sp && now - lastUp_ >= 1.0 / sp) {
        const double to = nextStep(cur_.totalDb, -2.0, pol_);
        if (to < cur_.totalDb - 0.01) {
            limit_ = "loud messages";
            move(now, to, pol_, f2("%.0f %% of the messages are near full scale: gain down", 100.0 * pj_.loudShare()), out);
            lastDecision_ = out.why; settled_ = false; cand_ = 0;
            pj_.restart(now, q_.pulseMsgs, q_.pulseLoud); pjMark_ = changedAt_;
            return true;
        }
    }
    double j = 0;
    bool pulseWait = false;   // pulsed: the message counts at this gain are not enough yet; the guards below still act, no decision is taken
    if (!objective(j)) {
        if (!(tgt_.kind == ModeKind::Pulsed && q_.pulseValid)) return false;
        pulseWait = true; j = 0;
    }
    // the world moved the gain under a trial (an overload cut): the trial is void
    if (clTrial_ && changedAt_ != trialAt_) { clTrial_ = false; settled_ = false; cand_ = 0; clMark_ = -1; }
    if (clMark_ != changedAt_) {   // a new gain: new statistics
        (clTrial_ ? tri_ : base_) = Run();
        (clTrial_ ? mTri_ : mBase_) = Run();
        (clTrial_ ? decT_ : decB_) = Run();
        specPow_.clear(); specCnt_ = 0; clMark_ = changedAt_; dwellFrom_ = now;
    }
    if (!pulseWait) climbSample(in, j);
    // re-run triggers: the decoder lost for 10 s
    if (q_.locked) { wasLocked_ = true; lockLostAt_ = -1; }
    else if (wasLocked_) {
        if (lockLostAt_ < 0) lockLostAt_ = now;
        else if (now - lockLostAt_ > 10 / sp) {
            wasLocked_ = false; lockLostAt_ = -1;
            const bool levelGone = fl_.valid && (chanRef_ < -299 || fl_.chanDbfs - cur_.totalDb < chanRef_ - 6.0);   // (never seen locked here: nothing says the signal is the problem)
            if (hasSignal() && !levelGone && now - lastSearchAt_ >= 30 / sp) { wantBracket_ = true; lastSearchAt_ = now; retuneWhy_ = "the decoder lost lock for 10 s while the signal is there"; settled_ = false; return false; }   // a transmitter that went away: no search, nothing to find
        }
    }
    double eff = effHeadroom(in);
    const double minHead = minHead_();
    const double dwell = set_.dwellSec / sp;
    if (hasSignal()) lastSigAt_ = now;
    if (q_.locked && fl_.valid) chanRef_ = std::max(fl_.chanDbfs - cur_.totalDb, chanRef_ - 0.002);   // the channel's level while the decoder locks (a slow decay: a decoder reports late, the seconds after a call ends must not read as its level)
    // a signal was here not long ago and is gone now: the gain waits for it (a call that ended is no reason to raise the gain to the noise; the next burst would clip). The overload guards below still act
    const bool idleHold = !hasSignal() && now - lastSigAt_ < 600.0 / sp && (!cutActive_ || (!cutWasOob_ && now - cutAt_ < 30.0 / sp));   // (a cut far below the signal's level loses the decoder too: that is not the signal going away)
    // after a cut (a clip or the headroom floor): back up fast (up to 12 dB a step, 1.1 s apart) to the gain it was cut from once the room is there again. The interferer has
    // gone when the headroom says so, whatever cut the gain; a burst must not cost minutes. A soft ceiling only holds while the headroom does not clearly allow more
    {
        if (cutActive_ && cutOobIn_ < -299 && !clTrial_ && now >= changedAt_ + settleTime() && now - cutAt_ >= settleTime() && fl_.valid) cutOobIn_ = fl_.oobDbfs - cur_.totalDb;
        const bool recovering = cutActive_ && cutWasOob_ && !idleHold && cur_.totalDb < cutFrom_ - 1 && now - cutAt_ > recWait_ / sp;
        if (!clTrial_ && recovering && now - lastUp_ >= 1.1 / sp && now - lastDown_ >= 1.5 / sp) {
            const double room = effHeadroom(in) - minHead_() - 1.0;
            const double sc = radio_.calibrated() ? 1.0 : (radio_.driverSplits && radio_.singleControl() ? unitScale_ : 1.0);
            const double scSafe = !unitLearned_ && !radio_.calibrated() && (radio_.stepsUncertain || radio_.driverSplits) ? std::max(sc, 2.5) : sc;   // an unmeasured step table: count on the worst
            double target = room < 5.0 ? cur_.totalDb : std::min({maxGain(), cutFrom_, cur_.totalDb + std::max(0.0, room) / std::max(0.5, scSafe)});
            if (!(fl_.oobDbfs > -199)) target = std::min(target, cutFrom_ - 3.0);   // a channel that fills the capture cannot tell a neighbour from itself: not back to the gain that clipped
            // the soft ceiling holds unless the out-of-band signal that cut the gain has clearly gone (an IMD pair that overloads at this gain must not be climbed into again)
            if (fl_.valid && cutOobIn_ > -299 && fl_.oobDbfs - cur_.totalDb < cutOobIn_ - 6.0) { linearUntil_ = std::min(linearUntil_, now); imdAt_ = -1e9; }   // the out-of-band signal has gone: so has the reason for the linear split
            else target = std::min(target, ceilingDb(now));
            if (cur_.totalDb < target - 0.5) {
                const double to = std::min(nextStep(cur_.totalDb, std::min(recWait_ > 1.6 ? 6.0 : 12.0, target - cur_.totalDb), pol_), target);
                if (to > cur_.totalDb + 0.01 && planStages(radio_, to, pol_, set_.stageMask, cur_.db, cur_.ampOn).totalDb > cur_.totalDb + 0.01) {
                    move(now, to, pol_, f2("recovering after a cut: back toward %.0f dB", cutFrom_), out);
                    lastRecoverAt_ = now;
                    settled_ = false; cand_ = 0;
                    if (now >= cutAt_ + 300 / sp && cur_.totalDb >= cutFrom_ - 1) cutActive_ = false;
                    return true;
                }
            }
            if (now - cutAt_ > 900 / sp) cutActive_ = false;   // never came back in 15 minutes: this is the new normal
        }
    }
    // the stage split comes back to the mode's own once the overload that forced the linear one has passed (a split the loop chose itself stays until the next acquisition)
    if (!splitKept_ && !clTrial_ && pol_ != tgt_.policy && now >= linearUntil_ && now - changedAt_ > 3 / sp && now - lastDown_ > 5 / sp && now >= imdAt_ + 60 / sp) {
        const StagePlan p = planStages(radio_, cur_.totalDb, tgt_.policy, set_.stageMask, cur_.db, cur_.ampOn);
        if (!(p == cur_)) { move(now, cur_.totalDb, tgt_.policy, std::string("stages back to the ") + policyName(tgt_.policy) + " split: the overload has passed", out); settled_ = false; cand_ = 0; return true; }
        pol_ = tgt_.policy;
    }
    // too close to full scale: down at once, in proportion (a trial that gets there is undone, see evaluateTrial)
    if (eff < minHead && !clTrial_ && now - lastDown_ >= 0.8 / sp) {
        const double to = nextStep(cur_.totalDb, -std::min(10.0, minHead - eff + 1.0), pol_);
        if (to < cur_.totalDb - 0.01) {
            noteCut(now);
            limit_ = "headroom";
            move(now, to, pol_, f2("headroom %.1f dB is under the %.0f dB minimum: gain down", eff, minHead), out);
            lastDecision_ = out.why; settled_ = false; cand_ = 0;
            return true;
        }
    }
    // a trial that has already run out of headroom does not wait for its dwell
    if (clTrial_ && tri_.n >= 2 && now >= changedAt_ + settleTime() && eff < minHead - 0.5) return evaluateTrial(in, out);
    const bool idleNoTrials = !hasSignal() && now - lastSigAt_ < 600.0 / sp && (!cutActive_ || now - cutAt_ > 90.0 / sp);   // (just after a cut the lost lock may be the cut's doing: the climb may look for a while)
    if (idleNoTrials && !clTrial_) { if (settled_) { blindRest_ = true; settledAt_ = now; } phase_ = Phase::Tracking; limit_.clear(); lastDecision_ = "the signal went away: the gain waits for the next one"; return false; }
    if (pulseWait || now - dwellFrom_ < dwell || (clTrial_ ? tri_.n : base_.n) < 3) return false;
    if (clTrial_) return evaluateTrial(in, out);
    if (settled_) {
        const bool comf = comfortable();
        std::string wc;
        if (worldChanged(in, wc)) {
            lastSearchAt_ = now; settled_ = false; idleCycles_ = 0; recWait_ = 1.5;
            linearUntil_ = std::min(linearUntil_, now); imdAt_ = -1e9;   // what forced the linear split has gone
            ceilUntil_ = std::min(ceilUntil_, now);   // what capped the gain has gone
            wantBracket_ = true; retuneWhy_ = wc;
            return false;
        }
        // the decoder's own number fell away although it is still locked: the world changed, search again (level alone is no reason: a call that ends is not a problem)
        if (settledDec_ > -1e8 && decB_.n >= 5 && decB_.mean < settledDec_ - 3.0 && !(tgt_.kind == ModeKind::Pulsed && q_.pulseValid) && now - lastSearchAt_ >= 30 / sp) {
            wantBracket_ = true; lastSearchAt_ = now; settled_ = false; retuneWhy_ = f2("the decoder's quality fell %.1f dB", settledDec_ - decB_.mean);
            return false;
        }
        phase_ = comf ? Phase::Frozen : Phase::Tracking;
        // nothing to judge a gain by and the floor is where it should be: rest (no signal is not a reason to move). A signal that returns wakes the loop, at most every 10 minutes
        const bool blind = !hasSignal();
        if (blind) { if (floorFine()) { blindRest_ = true; phase_ = Phase::Tracking; lastDecision_ = "no signal in the channel: the gain waits for one"; return false; } }
        else if (blindRest_) {
            blindRest_ = false;
            if (now - lastWakeAt_ < 600.0 / sp) { settledAt_ = now; return false; }
            lastWakeAt_ = now;   // (the back-off stays: every call that starts is no reason for a new round of looks)
        }
        // a settled loop looks at its neighbours ever more rarely: 6, 12, 24 ... up to 15 minutes, 30 minutes while the decoder is comfortable. Each look is a gain step the decoder hears
        const double every = tgt_.kind == ModeKind::Pulsed && q_.pulseValid ? std::min(1800.0, 12.0 * std::pow(3.0, (double)std::min(idleCycles_, 6))) :   // the sky changes, slowly: ADS-B keeps looking at its neighbours
                             tgt_.lockSnrDb > 1e8 && tgt_.metric == Metric::Sensitivity && !q_.pulseValid ? std::min(1800.0, 60.0 * std::pow(3.0, (double)std::min(idleCycles_, 4))) :   // no decoder to serve (a wide span, GNSS): the floor does not change by itself
                             comf && idleCycles_ >= 2 ? std::min(1800.0, 120.0 * std::pow(2.0, (double)std::min(idleCycles_, 5))) : std::min(900.0, 6.0 * std::pow(3.0, (double)std::min(idleCycles_, 6)));
        if (now - settledAt_ < every / sp) {
            if (now - dwellFrom_ > 12 / sp) { base_ = Run(); decB_ = Run(); specPow_.clear(); specCnt_ = 0; dwellFrom_ = now; }   // a fresh baseline for the next probe
            return false;
        }
        settled_ = false; cand_ = 0;
    }
    // nothing to judge a gain by and the floor is fine: settle here without trying anything
    if (!hasSignal() && floorFine()) { settleHere(in); idleCycles_++; blindRest_ = true; lastDecision_ = "no signal in the channel: the gain waits for one"; return false; }
    return startTrial(in, out);
}

bool AutoGainController::startTrial(const AgInput& in, AgCommand& out) {
    const double now = in.now, sp = std::max(0.25, set_.loopSpeed);
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    const Policy pol = pol_;   // a split that bought objective stays; a product or an overload caps the gain (an overload cut itself moves to the linear split)
    double eff = effHeadroom(in);
    const double minHead = minHead_();
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
            if (cur_.totalDb >= ceiling - 0.01 || (wasted && !q_.locked && !hasSignal()) || room < 0.7) { limit_ = room < 0.7 ? "headroom" : wasted ? "noise floor already far over the ADC's" : cur_.totalDb >= maxGain() - 0.01 ? "top of the gain range" : "ceiling after an overload"; cand_++; continue; }
            const double sc = radio_.calibrated() ? 1.0 : (radio_.driverSplits && radio_.singleControl() ? unitScale_ : 1.0);
            double want = std::min({clStep_, tgt_.maxStepDb, 0.7 * room / std::max(0.5, sc)});
            if (radio_.stepsUncertain && !unitLearned_ && !radio_.calibrated()) want = std::min(want, 1.0);   // the real dB of one index step is not known yet: the smallest step first   // 30 % of the room stays for what the table does not know
            to = nextStep(cur_.totalDb, want, pol);
            double snapTo = planStages(radio_, to, pol, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
            if (snapTo > ceiling + 0.01) {
                snapTo = cur_.totalDb;
                for (double g : totalSteps(radio_, pol, set_.stageMask, cur_.ampOn)) if (g <= ceiling + 0.01 && g > snapTo + 0.01) snapTo = g;
            }
            if (snapTo <= cur_.totalDb + 0.01 || stepReal(cur_.totalDb, snapTo) > room * 0.85 + 0.3) { cand_++; continue; }
            to = snapTo;
            what = f2("probe +%.1f dB", stepReal(cur_.totalDb, to));
        } else if (kind == 2) {   // down
            if (cur_.totalDb <= minGain() + 0.01) { cand_++; continue; }
            to = nextStep(cur_.totalDb, -std::min(clStep_, 2.0), pol);   // a look downwards stays small: it costs signal, and a real change is found by the search
            if (to >= cur_.totalDb - 0.01) { cand_++; continue; }
            what = f2("probe %.1f dB", stepReal(cur_.totalDb, to));
        } else {   // the other stage split at the same total (HackRF: LNA first or VGA first)
            if (splitCount < 2 || cur_.ampOn || splitTried_ || comfortable()) { cand_++; continue; }   // once per acquisition, never on a comfortable decoder
            tp = pol == Policy::Linearity ? Policy::Sensitivity : Policy::Linearity;
            if (imdSeen_ || now < linearUntil_) { cand_++; continue; }   // after an overload the linear split stays
            const StagePlan p = planStages(radio_, cur_.totalDb, tp, set_.stageMask, cur_.db, cur_.ampOn);
            splitTried_ = true;
            if (p == cur_) { cand_++; continue; }
            if (p.nfDb > cur_.nfDb + 2.0 && !strongNb) { cand_++; continue; }   // a worse noise figure and nothing strong nearby: it can only cost the signal
            what = std::string("probe split: ") + policyName(tp);
        }
        // the baseline is complete: remember it and move
        basePlan_ = cur_; basePol_ = pol_;
        specBase_ = meanDb(specPow_, specCnt_);
        candKind_ = kind;
        specPow_.clear(); specCnt_ = 0;
        move(now, to, tp, what, out);
        clTrial_ = true; trialAt_ = changedAt_; tri_ = Run(); decT_ = Run(); mTri_ = Run(); clMark_ = changedAt_; dwellFrom_ = now;
        return true;
    }
    // every candidate in the cycle was rejected or is not available: settled here
    settleHere(in); idleCycles_++;
    const bool comf = comfortable();
    phase_ = comf ? Phase::Frozen : Phase::Tracking;
    lastDecision_ = comf ? "settled: the decoder is comfortably over its threshold" : "settled: no neighbouring gain is better";
    return false;
}

bool AutoGainController::evaluateTrial(const AgInput& in, AgCommand& out) {
    const double now = in.now;
    // which number decides: the decoder's while it locks on both sides, a lost or gained lock by itself, else the level objective
    const double shB = base_.n > 0 ? decB_.n / base_.n : 0, shT = tri_.n > 0 ? decT_.n / tri_.n : 0;
    const bool decDomain = shB >= 0.5 && shT >= 0.5 && decB_.n >= 2 && decT_.n >= 2;
    const bool lostLock = !decDomain && shB >= 0.5 && shT < shB - 0.3, gainedLock = !decDomain && shT >= 0.5 && shB < shT - 0.3;
    const Run& rb = decDomain ? decB_ : base_;
    const Run& rt = decDomain ? decT_ : tri_;
    const double j0 = rb.mean, j1 = lostLock ? rb.mean - 20.0 : gainedLock ? rb.mean + 20.0 : rt.mean;
    const double neff0 = std::max(1.0, rb.n / 6.0), neff1 = std::max(1.0, rt.n / 6.0);
    // a gentle slope (a few tenths of a dB per step) must still be climbed: the floor of the threshold is 40 % of the mode's, the noise term does the rest
    const double thr = std::max(tgt_.improveDb, 1.5 * std::sqrt(rb.var() / neff0 + rt.var() / neff1));
    const double dJ = j1 - j0;
    // the channel's own level moved far more than the gain could have made it (a call started or ended during the look): this trial says nothing, back to the baseline
    if (!decDomain && !lostLock && !gainedLock ? std::fabs(tri_.mean - base_.mean) > std::fabs(cur_.totalDb - basePlan_.totalDb) * 1.3 + 3.0
                                                : ((lostLock || gainedLock) && std::fabs(tri_.mean - base_.mean) > std::fabs(cur_.totalDb - basePlan_.totalDb) * 1.3 + 6.0)) {
        out.plan = basePlan_; out.why = "the channel changed during the look: gain put back";
        logEvent(now, cur_.totalDb, basePlan_.totalDb, out.why);
        cur_ = basePlan_; pol_ = basePol_; changedAt_ = now; nAvg_ = 0; changes_++; clTrial_ = false; cand_ = 0; clMark_ = -1;
        return true;
    }
    const double dgNom = cur_.totalDb - basePlan_.totalDb;
    // what this unit really does per nominal dB: the strongest line well over the floor rises with the gain (a product would rise faster, a
    // compressed line slower, so only the strongest is used)
    if (!radio_.calibrated() && radio_.driverSplits && radio_.singleControl() && std::fabs(dgNom) >= 1.0 && specCnt_ > 2 && !specBase_.empty()) {
        const std::vector<float> hi0 = meanDb(specPow_, specCnt_);
        if (hi0.size() == specBase_.size() && hi0.size() > 64) {
            size_t kk = 0; float mx = -300; double fl = 0; std::vector<float> srt(specBase_);
            std::nth_element(srt.begin(), srt.begin() + (long)(srt.size() / 4), srt.end());
            fl = srt[srt.size() / 4];
            for (size_t k = 4; k + 4 < hi0.size(); k++) if (std::abs((long)k - (long)hi0.size() / 2) > 4 && specBase_[k] > mx) { mx = specBase_[k]; kk = k; }
            if (mx > fl + 20 && hi0[kk] < -3.0) {
                const double sc = std::clamp((double)(hi0[kk] - specBase_[kk]) / dgNom, 0.5, 3.0);
                unitScale_ = rejUp_ == 0 && unitScale_ == 1.0 ? sc : 0.5 * unitScale_ + 0.5 * sc;
                unitLearned_ = true;
            }
        }
    }
    const double dg = radio_.calibrated() ? radio_.realDelta(basePlan_.totalDb, cur_.totalDb) : dgNom * (radio_.driverSplits && radio_.singleControl() ? unitScale_ : 1.0);
    double eff = effHeadroom(in);
    // the gain-slope test, free on every step the loop makes: the spectrum before and after against the real gain change
    bool imd = false, comp = false;
    std::string slopeWhy;
    slope_ = 1;
    const bool slopeKnown = !radio_.stepsUncertain || radio_.calibrated() || unitLearned_;   // an index of an unknown radio: its real dB per step first
    if (slopeKnown && std::fabs(dg) >= 1.5 && specCnt_ > 2 && !specBase_.empty()) {
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
            if (dg > 0 && fh.valid && fb.valid && !(in.adc.clipFraction > clipLimitNow()) && fh.floorDbfs - fb.floorDbfs > 1.3 * dg + 0.8) { imd = true; slopeWhy = f2("the noise floor rose %.1f dB for %.1f dB of gain", fh.floorDbfs - fb.floorDbfs, dg); }
        }
    }
    met_.slopeDbPerDb = slope_;
    const bool clip = in.adc.clipFraction > clipLimitNow();
    const bool tight = eff < minHead_() || (tgt_.kind == ModeKind::Pulsed && q_.pulseValid && pj_.tooLoud());
    bool accept = false, sensWhy = false;
    std::string why;
    if (candKind_ == 1) {
        const bool bad = imd || comp || clip || tight;
        accept = !bad && dJ > thr;
        // more gain that still measurably helps and leaves room above the headroom floor costs nothing: take it (a radio whose noise figure keeps falling with gain, a gentle hill)
        if (!accept && !bad && dJ > 0.3 && dJ > 0.5 * thr && eff > minHead_() + 3.0) accept = true;
        // a wide capture has no decoder and the floor rises with the gain whatever the noise figure does: as much gain as the headroom floor and the slope test allow
        if (!accept && !bad && tgt_.kind == ModeKind::WideSpan && eff >= minHead_() - 0.2) accept = true;
        // the objective is blind while the wanted signal is under the noise (its SNR reads ~0 whatever the gain): the floor over the ADC's own noise
        // decides then, as the old loop's sensitivity rule did
        const double mGain = mTri_.mean - mBase_.mean;
        const bool floorLow = mBase_.n >= 3 && mBase_.mean < tgt_.marginGoalDb - 2.0;
        const bool signalJustLeft = !hasSignal() && now - lastSigAt_ < 600.0 / std::max(0.25, set_.loopSpeed);   // the floor is no reason to raise a gain that a burst set a moment ago
        if (!accept && !bad && !signalJustLeft && floorLow && mTri_.n >= 3 && mGain > 1.0 && dJ > -thr) { accept = true; sensWhy = true; }
        // the floor sits AT the ADC's own noise (within 1.5 dB): the antenna's noise is still under it and the wanted signal may be too, so neither the floor nor the SNR
        // moves with a step (a radio with a coarse gain table, or a network radio, gets there from the bottom: SpyServer, an RTL-SDR at index 0). More gain can only help
        // until the antenna's noise shows (the margin rises), whatever the objective says
        if (!accept && !bad && !signalJustLeft && mBase_.n >= 3 && mBase_.mean < 1.5 && mTri_.n >= 3 && dJ > -thr) { accept = true; sensWhy = true; }
        if (accept) rejUp_ = 0;
        if (bad) {
            // the gain above is refused for 2 min, 3 min after a product, longer each time it fails again (up to 8 min)
            rejUp_ = std::min(rejUp_ + 1, 4);
            const double hold = ((imd || comp) ? 180.0 : 120.0) * (1.0 + 0.5 * (rejUp_ - 1));
            ceil_ = (now < ceilUntil_) ? std::min(ceil_, basePlan_.totalDb + 0.01) : basePlan_.totalDb + 0.01;
            ceilUntil_ = std::max(ceilUntil_, now + hold / std::max(0.25, set_.loopSpeed));
            if (imd || comp) { imdSeen_ = imdSeen_ || imd; compSeen_ = compSeen_ || comp; imdAt_ = now; linearUntil_ = std::max(linearUntil_, now + 60); }
            limit_ = imd ? "intermodulation" : comp ? "compression" : clip ? "overload" : "headroom";
            why = f2("+%.1f dB undone: ", dg) + (imd || comp ? slopeWhy : clip ? std::string("the ADC clipped") : f2("headroom %.1f dB under the %.0f dB minimum", eff, minHead_()));
        } else why = accept ? (sensWhy ? f2("+%.1f dB kept: the noise floor was only %.1f dB over the ADC's", dg, mBase_.mean) : f2("+%.1f dB kept: objective +%.1f dB", dg, dJ)) : f2("+%.1f dB undone: objective only %+.1f dB (needs %.1f)", dg, dJ, thr);
    } else if (candKind_ == 2) {
        const bool nearLimit = basePlan_.totalDb > 0 && (tight || eff < minHead_() + 1.0);
        // the objective can be blind to a front end that compresses or makes products at the higher gain (a floor margin only grows with gain): the slope test decides then
        const bool slopeSaysDown = (imd || comp) && dg < 0;
        accept = dJ > thr || (nearLimit && dJ > -0.25 * thr) || slopeSaysDown;
        if (slopeSaysDown) { limit_ = imd ? "intermodulation" : "compression"; ceil_ = cur_.totalDb + 0.01; ceilUntil_ = std::max(ceilUntil_, now + (tgt_.kind == ModeKind::WideSpan ? 1800.0 : 180.0) / std::max(0.25, set_.loopSpeed)); }
        why = accept ? (slopeSaysDown ? f2("%.1f dB kept: ", dg) + slopeWhy : f2("%.1f dB kept: objective %+.1f dB", dg, dJ)) : f2("%.1f dB undone: objective %+.1f dB", dg, dJ);
    } else {
        accept = !imd && !comp && !clip && !tight && dJ > thr;
        why = accept ? f2("other stage split kept: objective +%.1f dB", dJ) : f2("other stage split undone: objective %+.1f dB", dJ);
    }
    lastDecision_ = why;
    if (accept) {
        clTrial_ = false;
        base_ = tri_; decB_ = decT_; mBase_ = mTri_;   // what was measured is the new baseline (and the spectrum accumulating now is its own)
        if (candKind_ == 1) clDir_ = 1; else if (candKind_ == 2) { clDir_ = -1; cutActive_ = false; }   // lower measurably better than what recovery wants: the cut gain was right
        if (candKind_ != 3) clStep_ = std::min(tgt_.maxStepDb, clStep_ * 1.5);
        cand_ = 0; settled_ = false; rejectRun_ = 0; idleCycles_ = std::max(0, idleCycles_ - 2);   // a kept step: the back-off shortens, it does not start over
        if (candKind_ == 3) splitKept_ = true;
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
    clTrial_ = false; rejectRun_++;
    // a gentle slope under the threshold (the last few tenths of a dB per step): the same direction again with a bigger step before giving up, so a shallow hill is still climbed
    if (candKind_ != 3 && !(imd || comp || clip || tight) && idleCycles_ <= 1 && dJ > 0.25 * thr && clStep_ < tgt_.maxStepDb - 0.1 && (candKind_ != 1 || eff - minHead_() > 3.0))   /* (a step the headroom already limited would not get bigger) */ clStep_ = std::min(tgt_.maxStepDb, clStep_ * 2.0);
    else { cand_++; clStep_ = std::max(2.0, clStep_ * 0.5); }
    return true;
}

void AutoGainController::setStatus(const std::string& lead) {
    (void)lead;
    std::string s = f2("Gain %.0f dB", cur_.totalDb);
    double j = 0;
    if (tgt_.kind == ModeKind::Pulsed && q_.pulseValid && pj_.started()) s += f2(": %.1f messages/s", pj_.rate()) + f2(", %.0f %% loud", 100.0 * pj_.loudShare());
    else if (q_.locked && (q_.merValid || q_.snrValid)) s += f2(": decoder %.1f dB", q_.merValid ? q_.merDb : q_.snrDb);
    else if (fl_.valid) s += f2(": channel SNR %.1f dB", fl_.snrDb);
    (void)j;
    s += f2(", headroom %.1f dB (min %.0f)", headAvg_, minHead_());
    if (br_ || wantBracket_) s += std::string(", searching") + (retuneWhy_.empty() ? "" : " (" + retuneWhy_ + ")");
    else if (clTrial_) s += ", trying a neighbouring gain";
    else if (settled_) s += blindRest_ ? ", waiting for a signal" : comfortable() ? ", settled (decoder comfortably locked)" : ", settled";
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
