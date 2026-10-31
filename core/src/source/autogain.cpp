// Automatic gain, part 4 and 5: the controller (Auto / Optimise once / Safe start), the per-radio memory, the blocking runner and the
// engine adapter, the read-only JSON. See docs/AUTOGAIN.md.
#include "dect2/source/autogain.h"
#include "dect2/engine/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <thread>

namespace dect2::ag {

const char* agModeName(AgMode m) {
    switch (m) {
    case AgMode::Auto: return "auto";
    case AgMode::Once: return "optimise once";
    case AgMode::Safe: return "safe start";
    default: return "off";
    }
}
const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Settling: return "settling";
    case Phase::Tracking: return "tracking";
    case Phase::Frozen: return "holding (decoder locked)";
    case Phase::Sweeping: return "sweeping";
    case Phase::Refining: return "refining";
    case Phase::Ramping: return "ramping up";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    default: return "idle";
    }
}

namespace {
std::string fmt(const char* f, double a, double b = 0, double c = 0) {
    char buf[160];
    snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}
}

// ------------------------------------------------------------------------------------------------------------------------------ setup
void AutoGainController::configure(const RadioProfile& r, const ModeTarget& t, const AgSettings& s) {
    radio_ = r; tgt_ = t; set_ = s;
    mode_ = s.mode;
    pol_ = t.policy;
    phase_ = Phase::Idle;
}

void AutoGainController::setSettings(const AgSettings& s) {
    const bool modeChanged = s.mode != set_.mode;
    set_ = s;
    if (modeChanged) { mode_ = s.mode; phase_ = Phase::Idle; }
}

double AutoGainController::bracketHead() const { return set_.v2 ? tgt_.minHeadroomDb + 2.0 : headroomTarget(); }
double AutoGainController::headroomTarget() const { return set_.headroomDb >= 0 ? set_.headroomDb : tgt_.headroomDb; }
double AutoGainController::settleTime() const { return radio_.settleSec + set_.settleSec / std::max(0.25, set_.loopSpeed); }

double AutoGainController::maxGain() const {
    double hi = radio_.maxTotal(cur_.ampOn);
    if (radio_.id == "airspyhf" && !set_.ampAllowed) hi -= 6;   // the top 6 dB switch its preamp on
    return std::max(minGain(), std::min(hi, set_.maxGainDb));
}
double AutoGainController::minGain() const { return radio_.minTotal(cur_.ampOn); }

std::vector<double> AutoGainController::stepsFor(Policy pol) const {
    std::vector<double> v;
    for (double t : totalSteps(radio_, pol, set_.stageMask, cur_.ampOn)) if (t >= minGain() - 1e-6 && t <= maxGain() + 1e-6) v.push_back(t);
    if (v.empty()) v.push_back(cur_.totalDb);
    return v;
}

double AutoGainController::nextStep(double from, double delta, Policy pol) const {
    const std::vector<double> st = stepsFor(pol);
    double best = from, bestErr = 1e9;
    for (double v : st) {
        if (delta > 0 && v <= from + 1e-6) continue;
        if (delta < 0 && v >= from - 1e-6) continue;
        const double err = std::fabs(v - (from + delta));
        if (err < bestErr - 1e-9) { bestErr = err; best = v; }
    }
    // a step bigger than asked for by more than half: only when it is the only one in that direction and the ask is at least half of it
    if (best != from && std::fabs(best - from) > std::fabs(delta) * 1.5 + 0.01 && std::fabs(delta) < 0.5 * std::fabs(best - from)) return from;
    return best;
}

void AutoGainController::start(double now, double total, bool ampOn, const StagePlan* current) {
    cur_ = current ? *current : planStages(radio_, total, tgt_.policy, set_.stageMask, {}, ampOn);
    cur_.ampOn = ampOn;
    if (cur_.db.size() != radio_.stages.size()) cur_ = planStages(radio_, total, tgt_.policy, set_.stageMask, cur_.db, ampOn);
    cascade(radio_, cur_);
    startPlan_ = cur_;
    mode_ = set_.mode;
    pol_ = tgt_.policy;
    startedAt_ = now;
    changedAt_ = now - 1e3; measuredFrom_ = now; nAvg_ = 0;
    lastUp_ = lastDown_ = dirAt_ = -1e9; dir_ = 0;
    ceil_ = 1e9; ceilUntil_ = 0; linearUntil_ = 0; ovTimes_.clear();
    guard_ = false; probe_ = 0; probeAt_ = now;
    trials_.clear(); queue_.clear(); ti_ = -1; safeBase_ = -200; refined_ = false;
    br_ = 0; wantBracket_ = mode_ == AgMode::Auto && set_.quickStart; bracketDone_ = false; expl_.clear(); reasons_.clear();
    result_.clear(); used_ = Metric::Auto; bestMetric_ = 0; changes_ = 0; imdSeen_ = compSeen_ = false;
    met_ = AgMeters();
    climbReset(now);
    if (mode_ == AgMode::Off) { phase_ = Phase::Idle; return; }
    phase_ = mode_ == AgMode::Auto ? Phase::Tracking : mode_ == AgMode::Once ? Phase::Sweeping : Phase::Ramping;
    if (mode_ == AgMode::Once) {
        // the coarse sweep: about 10 gains over the range, ascending (the low ones are safe; the sweep stops going up once a step overloads)
        const std::vector<double> st = stepsFor(tgt_.policy);
        const double lo = st.front(), hi = st.back();
        double step = set_.stepDb > 0 ? set_.stepDb : std::max(3.0, (hi - lo) / 9.0);
        step = std::min(step, std::max(1.0, hi - lo));
        double lastQ = -1e9;
        for (double g = lo; g <= hi + 1e-6; g += step) {
            double best = st.front();
            for (double v : st) if (std::fabs(v - g) < std::fabs(best - g)) best = v;
            if (best > lastQ + 0.05) { queue_.push_back({best, tgt_.policy}); lastQ = best; }
        }
        if (queue_.empty() || queue_.back().first < hi - 0.05) queue_.push_back({hi, tgt_.policy});
    }
    logEvent(now, cur_.totalDb, cur_.totalDb, std::string("start: ") + agModeName(mode_) + " for " + modeKindName(tgt_.kind) + " on " + radio_.name);
}

void AutoGainController::stop() {
    mode_ = AgMode::Off;
    phase_ = Phase::Idle;
    queue_.clear();
    probe_ = 0;
}

void AutoGainController::logEvent(double now, double from, double to, const std::string& why) {
    log_.push_back({now, from, to, why});
    while (log_.size() > 200) log_.pop_front();
}

void AutoGainController::move(double now, double total, Policy pol, const std::string& why, AgCommand& out) {
    StagePlan p = planStages(radio_, total, pol, set_.stageMask, cur_.db, cur_.ampOn);
    out.plan = p; out.why = why;
    logEvent(now, cur_.totalDb, p.totalDb, why);
    if (p.totalDb > cur_.totalDb + 0.01) { lastUp_ = now; if (dir_ < 0) dirAt_ = now; dir_ = 1; }
    else if (p.totalDb < cur_.totalDb - 0.01) { lastDown_ = now; if (dir_ > 0) dirAt_ = now; dir_ = -1; }
    cur_ = p;
    pol_ = pol;
    changedAt_ = now;
    nAvg_ = 0;
    changes_++;
}

// ------------------------------------------------------------------------------------------------------------------------------ measuring
void AutoGainController::measure(const AgInput& in) {
    const BlockStats& a = in.adc;
    met_.valid = true;
    met_.peakDbfs = 20 * std::log10(std::max(a.peak, 1e-9));
    met_.rmsDbfs = a.rmsDbfs;
    met_.crestDb = a.crestDb;
    met_.clipFraction = a.clipFraction;
    met_.headroomDb = a.headroomDb;
    met_.holdHeadroomDb = a.holdHeadroomDb;
    adcLast_ = a;
    met_.clipping = a.clipFraction > tgt_.clipLimit;
    q_ = in.q;
    if (in.spectrum && !in.spectrum->empty()) fl_ = estimateFloor(*in.spectrum, in.rateHz, in.chanBwHz, in.offsetHz, radio_.adcBits);
    else fl_ = FloorEstimate();
    met_.floorValid = fl_.valid;
    met_.floorDbfs = fl_.floorDbfs; met_.adcMarginDb = fl_.adcMarginDb; met_.chanSnrDb = fl_.snrDb; met_.oobDbfs = fl_.oobDbfs;
    met_.overload = met_.clipping || a.headroomDb < 0.3;
    met_.imd = imdSeen_; met_.compressed = compSeen_;
    if (in.now < changedAt_ + settleTime()) return;   // what the radio gives right after a change is not yet the new gain
    if (nAvg_ == 0) { measuredFrom_ = in.now; rmsAvg_ = a.rmsDbfs; headAvg_ = a.headroomDb; }
    const double al = std::max(0.12, 1.0 / (nAvg_ + 1));
    rmsAvg_ += al * (a.rmsDbfs - rmsAvg_);
    headAvg_ += al * (a.headroomDb - headAvg_);
    if (a.rmsDbfs > -40) crestAvg_ += 0.05 * (std::max(2.0, std::min(16.0, a.crestDb)) - crestAvg_);
    nAvg_++;
}

bool AutoGainController::score(Metric m, const Quality& q, const FloorEstimate& fl, const BlockStats& adc, const ModeTarget& t, double hTarget, double& out) {
    switch (m) {
    case Metric::Mer:
        if (q.merValid) { out = q.merDb; return true; }
        if (q.snrValid) { out = q.snrDb; return true; }
        return false;
    case Metric::Snr:
        if (q.snrValid) { out = q.snrDb; return true; }
        if (q.merValid) { out = q.merDb; return true; }
        return false;
    case Metric::Ber:
        if (q.berValid) { out = -10 * std::log10(std::max(q.ber, 1e-12)); return true; }
        return false;
    case Metric::Lock:
        if (!q.locked) { out = 0; return true; }
        out = 10 + (q.snrValid ? q.snrDb : 0) + (q.fecOk >= 0 ? 10 * q.fecOk : 0);
        return true;
    case Metric::ChannelSnr:
        if (fl.valid) { out = fl.snrDb; return true; }
        return false;
    case Metric::Sensitivity:   // the noise floor over the ADC's own noise, worth nothing beyond goal + 6 dB; a peak too close to full scale costs
        if (fl.valid) {
            out = std::min(fl.adcMarginDb, t.marginGoalDb + 6) - 2.0 * std::max(0.0, hTarget - adc.headroomDb);
            return true;
        }
        return false;
    case Metric::Headroom: out = -std::fabs(adc.headroomDb - hTarget); return true;
    default: return false;
    }
}

Metric AutoGainController::resolvedMetric(const Quality& q) const {
    Metric m = set_.metric != Metric::Auto ? set_.metric : tgt_.metric;
    if ((m == Metric::Mer || m == Metric::Snr) && !(q.merValid || q.snrValid)) m = fl_.valid ? Metric::ChannelSnr : Metric::Headroom;
    if (m == Metric::Ber && !q.berValid) m = q.snrValid ? Metric::Snr : fl_.valid ? Metric::ChannelSnr : Metric::Headroom;
    if ((m == Metric::ChannelSnr || m == Metric::Sensitivity) && !fl_.valid) m = Metric::Headroom;
    return m;
}

bool AutoGainController::tick(const AgInput& in, AgCommand& out) {
    measure(in);
    bool changed = false;
    if (mode_ == AgMode::Off || phase_ == Phase::Idle || phase_ == Phase::Done || phase_ == Phase::Failed) { /* metering only */ }
    else if (mode_ == AgMode::Auto) { changed = tickAuto(in, out); if (changed && !br_) explain(out.why); else if (expl_.empty() && nAvg_ >= 3 && !br_ && !wantBracket_) explain("holding"); }
    else if (mode_ == AgMode::Once) changed = tickOnce(in, out);
    else changed = tickSafe(in, out);
    if (mode_ == AgMode::Auto && set_.v2 && in.now - statusAt_ >= 0.25) { statusAt_ = in.now; setStatus(""); }
    if (in.now - histAt_ >= 0.5) {
        histAt_ = in.now;
        AgPoint p;
        p.t = in.now; p.gainDb = cur_.totalDb; p.headroomDb = met_.headroomDb; p.floorDbfs = fl_.valid ? fl_.floorDbfs : -200;
        double v = 0;
        if (score(resolvedMetric(q_), q_, fl_, in.adc, tgt_, headroomTarget(), v)) { p.metric = v; p.metricValid = true; }
        hist_.push_back(p);
        while (hist_.size() > 1200) hist_.pop_front();
    }
    return changed;
}

// ------------------------------------------------------------------------------------------------------------------------------ Auto
bool AutoGainController::tickAuto(const AgInput& in, AgCommand& out) {
    const double now = in.now, sp = std::max(0.25, set_.loopSpeed);
    const double tgtH = headroomTarget(), win = tgt_.windowDb;
    if (br_) return tickBracket(in, out);   // the short search owns the gain until it is done (it backs off on a clip by itself)
    // overload acts at once, whatever else is going on (settling included: a burst does not wait)
    const bool over = in.adc.clipFraction > tgt_.clipLimit || (in.adc.peak >= 0.999 && in.adc.clipFraction > 0);
    if (over && now >= changedAt_ + radio_.settleSec + 0.1) {   // only the radio's own delay: a burst does not wait for the loop's settle time
        const double c = in.adc.clipFraction;
        // at acquisition (start, retune) the start gain may be anywhere in the range: cut much harder while the ADC is hard against full scale
        const bool acq = now - startedAt_ < 6.0 / sp;
        double cut = c > 0.2 ? (acq ? 40 : 30) : c > 5e-2 ? (acq ? 28 : 20) : c > 1e-2 ? 14 : c > 2e-3 ? 10 : 6;
        // v2: proportional: 6 dB first and measure again; the big cuts only when the ADC is pinned at full scale (the cut is 100 ms away from the next look)
        if (set_.v2 && c <= 0.2) cut = c > 5e-2 ? 14 : c > 1e-2 ? 9 : 6;
        // one burst takes several cuts: they are one overload episode (a new one after 3 s without clipping)
        const bool episode = ovTimes_.empty() || now - ovTimes_.back() > 3;
        // a burst that returns while the gain is still climbing back (well under the gain that clipped before) says nothing new about the gain
        // that is safe: the ceiling and the gain to recover to stay where the first episode put them (3.2: a transmitter that keys up every few
        // seconds used to push the ceiling down by 4 dB per burst until the weak signal was lost). Only a clip at the recovered gain lowers them.
        const bool returning = episode && !ovTimes_.empty() && cur_.totalDb < preOvDb_ - 4;
        if (episode) { ovTimes_.push_back(now); met_.overloads++; } else ovTimes_.back() = now;
        while (!ovTimes_.empty() && ovTimes_.front() < now - 60) ovTimes_.erase(ovTimes_.begin());
        // a soft ceiling under the gain that clipped: 20 s, 90 s when it keeps happening (three episodes in a minute)
        if (episode && !returning) { ceil_ = cur_.totalDb - 3; preOvDb_ = cur_.totalDb; }
        ceilUntil_ = now + (ovTimes_.size() >= 3 ? 90 : 20) / sp;
        linearUntil_ = now + 30;
        probe_ = 0; guard_ = false;
        havePrior_ = false;   // a remembered gain that clips is stale: the search is the wide one
        if (cur_.ampOn && set_.ampAllowed) {   // the amp first: it is the most exposed stage
            const double t = cur_.totalDb;
            cur_.ampOn = false;
            move(now, std::max(minGain(), t - cut), Policy::Linearity, fmt("overload (%.2f %% clipped): amp off, gain cut", c * 100), out);
            return true;
        }
        const double to = nextStep(cur_.totalDb, -cut, Policy::Linearity);
        if (to < cur_.totalDb - 0.01 || pol_ != Policy::Linearity) { move(now, to, Policy::Linearity, fmt("overload (%.2f %% clipped): gain cut %.0f dB", c * 100, cur_.totalDb - to), out); return true; }
        return false;
    }
    if (now < changedAt_ + settleTime()) { phase_ = Phase::Settling; return false; }
    if (nAvg_ < 3 || now - measuredFrom_ < 0.3 / sp) { phase_ = Phase::Settling; return false; }
    if (wantBracket_ && set_.quickStart) { beginBracket(in); return tickBracket(in, out); }
    wantBracket_ = false;
    if (set_.v2) { double j; if ((tgt_.kind == ModeKind::Pulsed && q_.pulseValid) || objective(j)) return tickClimb(in, out); }   // the hill climb on the measured objective; no objective (a span with no channel): the headroom loop below
    // the policy: the mode's own, or linearity after an overload / near a strong neighbour
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    const Policy pol = (now < linearUntil_ || strongNb || imdSeen_) ? Policy::Linearity : tgt_.policy;
    const double e = headAvg_ - tgtH;   // > 0: room for more gain
    // IMD probe: a short 3 dB step up, its spectrum against the one before
    if (probe_ == 1) {
        if (now - measuredFrom_ < 0.6 / sp) return false;
        probe_ = 0;
        if (in.spectrum && !probeLo_.empty() && in.spectrum->size() == probeLo_.size()) {
            const ImdReport r = imdCompare(probeLo_, *in.spectrum, cur_.totalDb - probeFrom_);
            if (r.overload) {
                imdSeen_ = imdSeen_ || r.spurs > 0; compSeen_ = compSeen_ || r.compressed;
                ceil_ = probeFrom_ - 3; ceilUntil_ = now + 300 / sp;
                move(now, nextStep(probeFrom_, -3, Policy::Linearity), Policy::Linearity,
                     r.spurs ? fmt("intermodulation: %.0f spurs rose %.1f dB more than the gain: gain lowered", r.spurs, r.worstExcessDb) : std::string("front end compressing: gain lowered"), out);
                return true;
            }
        }
        move(now, probeFrom_, pol, "intermodulation check: clean", out);
        return true;
    }
    const bool lockedGood = q_.locked && tgt_.lockSnrDb < 1e8 && (!q_.snrValid || q_.snrDb >= tgt_.lockSnrDb) && (!q_.merValid || q_.merDb >= tgt_.lockSnrDb);
    // the quality guard: an upward step that made the decoder worse is undone
    if (guard_ && now - measuredFrom_ >= 1.0 / sp) {
        guard_ = false;
        double v = 0;
        if (score(guardM_, q_, fl_, in.adc, tgt_, tgtH, v) && v < guardMetric_ - 1.0) {
            ceil_ = guardFrom_; ceilUntil_ = now + 60 / sp;
            move(now, guardFrom_, pol, std::string(metricName(guardM_)) + fmt(" fell %.1f dB after raising the gain: undone", guardMetric_ - v), out);
            return true;
        }
    }
    const bool canDown = now - lastDown_ >= 0.8 / sp;
    // recovery after a burst: far under the gain that was cut, with nothing clipping for 1.5 s, the climb is fast (up to 12 dB per step)
    const bool recovering = !ovTimes_.empty() && cur_.totalDb < preOvDb_ - 4 && now - ovTimes_.back() > 1.5 / sp && now - ovTimes_.front() < 60;
    const bool canUp = recovering ? (now - lastUp_ >= 1.1 / sp && now - lastDown_ >= 1.5 / sp) : (now - lastUp_ >= 2.5 / sp && now - lastDown_ >= 2.0 / sp);
    // too hot
    if (e < -win && canDown) {
        const double to = nextStep(cur_.totalDb, -std::min(-e + win * 0.5, 10.0), pol);
        if (to < cur_.totalDb - 0.01) { move(now, to, pol, fmt("headroom %.1f dB (target %.0f): gain down", headAvg_, tgtH), out); phase_ = Phase::Tracking; return true; }
    }
    const double ceiling = std::min(maxGain(), ceilingDb(now));
    const bool wasted = fl_.valid && fl_.adcMarginDb > tgt_.marginGoalDb + 15;   // the antenna's noise is far above the ADC's: more gain buys nothing
    const bool needMargin = fl_.valid && fl_.adcMarginDb < tgt_.marginGoalDb - 2;
    // above what it may use (the maximum was lowered, the AirspyHF+ preamp range without permission): down to it
    if (cur_.totalDb > maxGain() + 0.01 && canDown) {
        const StagePlan p = planStages(radio_, maxGain(), pol, set_.stageMask, cur_.db, cur_.ampOn);
        if (p.totalDb < cur_.totalDb - 0.01) { move(now, p.totalDb, pol, fmt("above the allowed maximum (%.0f dB): gain down", maxGain()), out); return true; }
    }
    // locked on good data: hold still, unless the ADC is far too quiet for the radio's own noise (then the quality guard watches the step)
    if (lockedGood && e >= -win && !(needMargin && e > 3 * win)) { phase_ = Phase::Frozen; return false; }
    phase_ = Phase::Tracking;
    if (canUp && cur_.totalDb < ceiling - 0.01 && !wasted) {
        double want = 0;
        if (e > win) want = std::min(e - win * 0.5, recovering ? std::max({12.0, tgt_.maxStepDb, preOvDb_ - 3.0 - cur_.totalDb}) : tgt_.maxStepDb);   // recovering: straight back to just under the gain that clipped
        else if (needMargin && e > 1.0) want = std::min({3.0, e - 0.5, tgt_.marginGoalDb - fl_.adcMarginDb});
        // hysteresis: right after coming down, only a clearly larger error turns it around
        if (want > 0 && dir_ < 0 && now - dirAt_ < 8 / sp && e < 1.5 * win && !recovering) want = 0;   // (not while climbing back after a burst)
        if (want > 0.5) {
            const double to = std::min(nextStep(cur_.totalDb, want, pol), ceiling);
            double snapTo = planStages(radio_, to, pol, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
            // the next step lies over the ceiling: the highest gain the radio reaches under it (3.2: a coarse stage table used to stop the
            // climb short of the ceiling for good)
            if (snapTo > ceiling + 0.01) {
                snapTo = cur_.totalDb;
                for (double g : totalSteps(radio_, pol, set_.stageMask, cur_.ampOn)) if (g <= ceiling + 0.01 && g > snapTo + 0.01) snapTo = g;
            }
            if (snapTo > cur_.totalDb + 0.01 && snapTo <= ceiling + 0.01) {
                double v = 0;
                const Metric gm = resolvedMetric(q_);
                if ((gm == Metric::Mer || gm == Metric::Snr || gm == Metric::Ber) && score(gm, q_, fl_, in.adc, tgt_, tgtH, v)) { guard_ = true; guardFrom_ = cur_.totalDb; guardMetric_ = v; guardM_ = gm; }
                move(now, snapTo, pol, needMargin && e <= win ? fmt("noise floor only %.1f dB over the ADC's: gain up", fl_.adcMarginDb) : fmt("headroom %.1f dB (target %.0f): gain up", headAvg_, tgtH), out);
                return true;
            }
        }
    }
    // gain that buys nothing but risk: down slowly
    // (only when the floor stays above the "wasted" line after the step, so the loop does not climb straight back: a 6 dB-step radio)
    if (fl_.valid && fl_.adcMarginDb > tgt_.marginGoalDb + 18 && now - lastDown_ >= 4 / sp && now - lastUp_ >= 4 / sp) {
        const double to = nextStep(cur_.totalDb, -std::min(6.0, fl_.adcMarginDb - (tgt_.marginGoalDb + 16)), pol);
        const double drop = cur_.totalDb - to;
        if (drop > 0.01 && fl_.adcMarginDb - drop > tgt_.marginGoalDb + 15.5) { move(now, to, pol, fmt("noise floor %.0f dB over the ADC's: gain wasted, down", fl_.adcMarginDb), out); return true; }
    }
    // the amp: only with the user's permission, only for a weak signal (low channel SNR, or a decoder that does not lock) with room for its
    // 14 dB and nothing strong near; the amp lowers the noise figure, which more LNA / VGA gain cannot
    const bool weakSig = fl_.valid && (fl_.snrDb < 12 || (tgt_.lockSnrDb < 1e8 && !q_.locked));
    if (set_.ampAllowed && radio_.hasAmp && radio_.id != "airspyhf" && !cur_.ampOn && weakSig && e > -win && !strongNb && cur_.totalDb >= minGain() + 14 &&
        ovTimes_.empty() && now - lastUp_ >= 5 / sp && now - lastDown_ >= 5 / sp) {
        const double t = cur_.totalDb;
        cur_.ampOn = true;
        move(now, t, Policy::Sensitivity, "weak signal at full gain: amp on (allowed by you)", out);
        return true;
    }
    // policy change at the same gain (a neighbour appeared / went): re-split
    if (pol != pol_ && now - changedAt_ > 5 / sp) {
        StagePlan p = planStages(radio_, cur_.totalDb, pol, set_.stageMask, cur_.db, cur_.ampOn);
        if (!(p == cur_)) { move(now, cur_.totalDb, pol, std::string("stages re-split for ") + policyName(pol), out); return true; }
        pol_ = pol;
    }
    // the optional IMD probe
    if (set_.imdProbeAuto && in.spectrum && !in.spectrum->empty() && !lockedGood && now - probeAt_ > 90 / sp && e > 3 && cur_.totalDb < maxGain() - 2.9) {
        probeAt_ = now; probe_ = 1; probeFrom_ = cur_.totalDb; probeLo_ = *in.spectrum;
        move(now, nextStep(cur_.totalDb, 3, pol), pol, "intermodulation check: +3 dB", out);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------------------------------------------------------------------ Once / Safe
bool AutoGainController::beginTrial(double now, double total, Policy pol, AgCommand& out) {
    AgTrial t;
    t.pol = pol;
    t.plan = planStages(radio_, total, pol, set_.stageMask, cur_.db, cur_.ampOn);
    trials_.push_back(t);
    ti_ = (int)trials_.size() - 1;
    const bool same = t.plan == cur_;
    move(now, total, pol, fmt(mode_ == AgMode::Safe ? "safe start: trying %.1f dB" : "optimise: trying %.1f dB", t.plan.totalDb) + (pol != tgt_.policy ? std::string(" (") + policyName(pol) + ")" : ""), out);
    if (same) changedAt_ = now - settleTime() + 0.05;   // nothing to wait for (still a short pause)
    return !same;
}

void AutoGainController::accumulate(AgTrial& t, const AgInput& in) {
    t.n++;
    double v = 0;
    const Metric mm = set_.metric != Metric::Auto ? set_.metric : tgt_.metric;
    if ((mm == Metric::Mer || mm == Metric::Snr || mm == Metric::Ber || mm == Metric::Lock) && score(mm, in.q, fl_, in.adc, tgt_, headroomTarget(), v) &&
        (mm == Metric::Lock || in.q.locked)) { t.metricSum += v; t.nMetric++; }
    if (in.q.locked) t.nLocked++;
    if (fl_.valid) { t.floorSum += fl_.floorDbfs; t.nFloor++; t.snrSum += fl_.snrDb; t.nSnr++; }
    t.headroomMin = std::min(t.headroomMin, in.adc.headroomDb);
    t.clipMax = std::max(t.clipMax, in.adc.clipFraction);
    if (in.spectrum && !in.spectrum->empty()) {   // a mean spectrum in dB (good enough for comparing lines)
        if (t.spec.size() != in.spectrum->size()) t.spec = *in.spectrum;
        else for (size_t k = 0; k < t.spec.size(); k++) t.spec[k] += ((*in.spectrum)[k] - t.spec[k]) / (float)std::min(t.n, 8);
    }
}

int AutoGainController::chooseTrial(Metric& used, double minHead, double maxMargin) const {
    const double tgtH = headroomTarget();
    auto marginOf = [&](const AgTrial& t) { return t.nFloor ? t.floor() + (6.02 * radio_.adcBits + 1.76) + 10 * std::log10((double)std::max<size_t>(t.spec.size(), 64)) - 1.76 : -1e9; };
    auto safe = [&](const AgTrial& t) { return t.done && t.n > 0 && t.clipMax <= tgt_.clipLimit && !t.imd && t.headroomMin >= minHead && marginOf(t) <= maxMargin; };
    bool anyMetric = false, anySnr = false;
    for (const auto& t : trials_) if (safe(t)) { if (t.nMetric > 0 && t.nMetric * 2 >= t.n) anyMetric = true; if (t.nSnr > 0) anySnr = true; }
    const Metric want = set_.metric != Metric::Auto ? set_.metric : tgt_.metric;
    if (anyMetric && (want == Metric::Mer || want == Metric::Snr || want == Metric::Ber || want == Metric::Lock)) used = want;
    else if (want == Metric::Headroom) used = Metric::Headroom;
    else if (anySnr) used = want == Metric::Sensitivity ? Metric::Sensitivity : Metric::ChannelSnr;
    else used = Metric::Headroom;
    std::vector<double> sc(trials_.size(), -1e9);
    double best = -1e9;
    for (size_t i = 0; i < trials_.size(); i++) {
        const AgTrial& t = trials_[i];
        if (!safe(t)) continue;
        double s = -1e9;
        switch (used) {
        case Metric::Mer: case Metric::Snr: case Metric::Ber: case Metric::Lock:
            if (t.nMetric > 0 && t.nMetric * 2 >= t.n) s = t.metric();
            break;
        case Metric::ChannelSnr: if (t.nSnr) s = t.chanSnr(); break;
        case Metric::Sensitivity:
            if (t.nFloor) {
                // the floor over the ADC's own noise: worth nothing past goal + 6 dB; less headroom than the target costs
                const double qPerBin = -(6.02 * radio_.adcBits + 1.76) - 10 * std::log10((double)std::max<size_t>(t.spec.size(), 64)) + 1.76;
                s = std::min(t.floor() - qPerBin, tgt_.marginGoalDb + 6) - 2.0 * std::max(0.0, tgtH - t.headroomMin);
            }
            break;
        default: s = -std::fabs(t.headroomMin - tgtH);
        }
        sc[i] = s;
        best = std::max(best, s);
    }
    if (best <= -1e8) {   // nothing was safe: the lowest gain tried
        int lo = -1;
        for (size_t i = 0; i < trials_.size(); i++) if (trials_[i].done && (lo < 0 || trials_[i].plan.totalDb < trials_[(size_t)lo].plan.totalDb)) lo = (int)i;
        used = Metric::Auto;
        return lo;
    }
    // within 0.5 dB of the best counts as equal (a decoder's MER levels off once the radio's noise is not the limit): of those, the one whose
    // ADC headroom is closest to the mode's target (less headroom than the target counts double: strong signals come and go); on a tie the
    // lower gain (less intermodulation)
    const double tol = used == Metric::Headroom ? 1.0 : 0.5;
    int pick = -1;
    double pickCost = 1e18;
    for (size_t i = 0; i < trials_.size(); i++) {
        if (sc[i] < best - tol) continue;
        const double h = trials_[i].headroomMin;
        const double cost = (h < tgtH ? 2.0 : 1.0) * std::fabs(h - tgtH);
        if (pick < 0 || cost < pickCost - 0.25 || (std::fabs(cost - pickCost) <= 0.25 && trials_[i].plan.totalDb < trials_[(size_t)pick].plan.totalDb - 0.01)) { pick = (int)i; pickCost = cost; }
    }
    return pick;
}

void AutoGainController::finish(double now, int idx, Metric used, AgCommand& out, bool& changed) {
    phase_ = Phase::Done;
    queue_.clear();
    if (idx < 0) { phase_ = Phase::Failed; result_ = "no measurement: the gain was left as it was"; if (!(cur_ == startPlan_)) { out.plan = startPlan_; cur_ = startPlan_; changed = true; } return; }
    const AgTrial& t = trials_[(size_t)idx];
    used_ = used;
    bestMetric_ = used == Metric::Mer || used == Metric::Snr || used == Metric::Ber || used == Metric::Lock ? t.metric() : used == Metric::ChannelSnr ? t.chanSnr() : t.headroomMin;
    if (!(t.plan == cur_)) {
        const StagePlan from = cur_;
        out.plan = t.plan; out.why = "result";
        cur_ = t.plan;
        changedAt_ = now; nAvg_ = 0; changes_++;
        logEvent(now, from.totalDb, cur_.totalDb, "result");
        changed = true;
    }
    if (used == Metric::Auto) result_ = fmt("every step overloaded the ADC: the lowest gain (%.1f dB) was kept", cur_.totalDb);
    else result_ = fmt("%.1f dB", cur_.totalDb) + " (" + metricName(used) + " " + fmt("%.1f", bestMetric_) + (used == Metric::Headroom ? " dB headroom" : used == Metric::Lock ? "" : " dB") + ")";
    logEvent(now, cur_.totalDb, cur_.totalDb, "done: " + result_);
}

bool AutoGainController::tickOnce(const AgInput& in, AgCommand& out) {
    const double now = in.now;
    if (ti_ < 0) {   // the first step
        if (queue_.empty()) { bool ch = false; finish(now, -1, Metric::Auto, out, ch); return ch; }
        const auto q = queue_.front(); queue_.erase(queue_.begin());
        return beginTrial(now, q.first, q.second, out) || true;
    }
    AgTrial& t = trials_[(size_t)ti_];
    if (now < changedAt_ + settleTime()) return false;
    accumulate(t, in);
    const bool bad = in.adc.clipFraction > tgt_.clipLimit * 5 || in.adc.headroomDb < 0.1;   // clearly overloaded: no need to measure the full time
    if (now - measuredFrom_ < set_.measureSec && !bad) return false;
    t.done = true;
    // the intermodulation check against the step below it (same split)
    for (int k = ti_ - 1; k >= 0; k--) {
        const AgTrial& p = trials_[(size_t)k];
        if (!p.done || p.pol != t.pol || p.plan.totalDb >= t.plan.totalDb - 0.5 || p.spec.size() != t.spec.size() || p.spec.empty()) continue;
        const ImdReport r = imdCompare(p.spec, t.spec, t.plan.totalDb - p.plan.totalDb);
        if (r.valid && r.overload && p.headroomMin > 1) { t.imd = true; imdSeen_ = imdSeen_ || r.spurs > 0; compSeen_ = compSeen_ || r.compressed; }
        break;
    }
    const bool over = t.clipMax > tgt_.clipLimit || t.imd || t.headroomMin < 0.5;
    if (over && phase_ == Phase::Sweeping) {   // the steps above it are worse still: drop them
        std::vector<std::pair<double, Policy>> keep;
        for (const auto& q : queue_) if (q.first < t.plan.totalDb - 0.01 || q.second != t.pol) keep.push_back(q);
        queue_.swap(keep);
    }
    if (queue_.empty() && !refined_) {
        refined_ = true;
        phase_ = Phase::Refining;
        Metric used;
        const int b = chooseTrial(used);
        if (b >= 0 && used != Metric::Auto) {
            const double bt = trials_[(size_t)b].plan.totalDb;
            const Policy bp = trials_[(size_t)b].pol;
            // halfway to the neighbours that were tried, and the other stage splits at the best gain (a radio with several stages)
            double below = -1e9, above = 1e9;
            for (const auto& x : trials_) if (x.pol == bp) { if (x.plan.totalDb < bt - 0.01) below = std::max(below, x.plan.totalDb); if (x.plan.totalDb > bt + 0.01) above = std::min(above, x.plan.totalDb); }
            auto tried = [&](double g, Policy p) {
                const double s = planStages(radio_, g, p, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
                for (const auto& x : trials_) if (x.pol == p && std::fabs(x.plan.totalDb - s) < 0.05) return true;
                for (const auto& x : queue_) if (x.second == p && std::fabs(x.first - s) < 0.05) return true;
                return false;
            };
            if (below > -1e8 && bt - below > 1.5 && !tried((bt + below) / 2, bp)) queue_.push_back({(bt + below) / 2, bp});
            if (above < 1e8 && above - bt > 1.5 && !tried((bt + above) / 2, bp)) queue_.push_back({(bt + above) / 2, bp});
            if (radio_.settableCount() >= 2)
                for (Policy p : {Policy::Sensitivity, Policy::Linearity, Policy::Balanced})
                    if (p != bp && !(planStages(radio_, bt, p, set_.stageMask, cur_.db, cur_.ampOn) == trials_[(size_t)b].plan) && !tried(bt, p)) queue_.push_back({bt, p});
        }
    }
    if (!queue_.empty()) {
        const auto q = queue_.front(); queue_.erase(queue_.begin());
        beginTrial(now, q.first, q.second, out);
        return true;
    }
    Metric used;
    const int b = chooseTrial(used);
    bool ch = false;
    finish(now, b, used, out, ch);
    return ch;
}

bool AutoGainController::tickSafe(const AgInput& in, AgCommand& out) {
    const double now = in.now;
    const double step = set_.stepDb > 0 ? set_.stepDb : 3.0;
    const double rise = set_.safeRiseDb >= 0 ? set_.safeRiseDb : tgt_.floorRiseDb;
    if (ti_ < 0) return beginTrial(now, minGain(), tgt_.policy, out) || true;
    AgTrial& t = trials_[(size_t)ti_];
    if (now < changedAt_ + settleTime()) return false;
    accumulate(t, in);
    const bool bad = in.adc.clipFraction > tgt_.clipLimit || in.adc.headroomDb < 0.3;
    if (now - measuredFrom_ < set_.measureSec && !bad) return false;
    t.done = true;
    if (ti_ > 0) {
        const AgTrial& p = trials_[(size_t)ti_ - 1];
        if (!p.spec.empty() && p.spec.size() == t.spec.size() && t.plan.totalDb > p.plan.totalDb + 0.5) {
            const ImdReport r = imdCompare(p.spec, t.spec, t.plan.totalDb - p.plan.totalDb);
            if (r.valid && r.overload && p.headroomMin > 1) { t.imd = true; imdSeen_ = imdSeen_ || r.spurs > 0; compSeen_ = compSeen_ || r.compressed; }
        }
    }
    bool ch = false;
    const double tgtH = headroomTarget();
    if (t.clipMax > tgt_.clipLimit || t.headroomMin < tgtH - tgt_.windowDb || t.imd) {   // too far: one step back (two after intermodulation)
        int back = ti_ - (t.imd ? 2 : 1);
        back = std::max(0, back);
        if (ti_ == 0) back = 0;
        finish(now, back, Metric::Headroom, out, ch);
        result_ = fmt("%.1f dB: ", cur_.totalDb) + (t.imd ? "intermodulation above it" : t.clipMax > tgt_.clipLimit ? "the ADC clipped above it" : "the headroom ran out above it");
        logEvent(now, cur_.totalDb, cur_.totalDb, "safe start: " + result_);
        return ch;
    }
    if (t.nFloor) {
        if (ti_ == 0) safeBase_ = t.floor();
        if (t.floor() - safeBase_ >= rise) {
            finish(now, ti_, Metric::Sensitivity, out, ch);
            result_ = fmt("%.1f dB: the noise floor rose %.1f dB (the antenna's noise is above the radio's)", cur_.totalDb, t.floor() - safeBase_);
            logEvent(now, cur_.totalDb, cur_.totalDb, "safe start: " + result_);
            return ch;
        }
    } else if (t.headroomMin <= tgtH + 0.5) {   // no spectrum: stop at the headroom target
        finish(now, ti_, Metric::Headroom, out, ch);
        result_ = fmt("%.1f dB: the ADC reached its headroom target", cur_.totalDb);
        return ch;
    }
    const double next = nextStep(cur_.totalDb, step, tgt_.policy);
    if (next <= cur_.totalDb + 0.01 || next > maxGain() + 0.01) {
        finish(now, ti_, Metric::Sensitivity, out, ch);
        result_ = fmt("%.1f dB: the top of the range, the noise floor rose only %.1f dB", cur_.totalDb, t.nFloor ? t.floor() - safeBase_ : 0);
        logEvent(now, cur_.totalDb, cur_.totalDb, "safe start: " + result_);
        return ch;
    }
    return beginTrial(now, next, tgt_.policy, out) || true;
}

double AutoGainController::progress() const {
    if (mode_ == AgMode::Once) {
        int done = 0;
        for (const auto& t : trials_) if (t.done) done++;
        const int total = (int)trials_.size() + (int)queue_.size() + (phase_ == Phase::Sweeping ? 3 : 0);
        return phase_ == Phase::Done ? 1.0 : total ? std::min(0.99, (double)done / total) : 0;
    }
    if (mode_ == AgMode::Safe) {
        if (phase_ == Phase::Done) return 1;
        const double lo = minGain(), hi = maxGain();
        return hi > lo ? std::max(0.0, std::min(0.99, (cur_.totalDb - lo) / (hi - lo))) : 0;
    }
    return 0;
}

std::vector<AgAdvice> AutoGainController::advice() const {
    std::vector<AgAdvice> v;
    const double tgtH = headroomTarget();
    if (met_.clipping) v.push_back({Advice::Overload, "ADC overload: the signal is too strong for this gain; reduce the gain"});
    if (fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20)
        v.push_back({Advice::StrongNeighbour, "Strong signal nearby: reduce the gain or add a filter (band-pass, or an FM / GSM notch)"});
    if (imdSeen_) v.push_back({Advice::Intermod, "Spurs that move when the gain changes: the front end is overloading; lower the LNA (or the amp) or add a filter"});
    if (compSeen_) v.push_back({Advice::Compressed, "The front end is compressing (power no longer follows the gain): lower the gain"});
    const bool atMax = cur_.totalDb >= maxGain() - 0.01;
    const bool noisy = met_.floorValid && met_.adcMarginDb > tgt_.marginGoalDb;   // the radio's noise is already above the ADC's: more gain will not help
    const bool weakSig = met_.floorValid && (met_.chanSnrDb < 12 || (tgt_.lockSnrDb < 1e8 && !q_.locked)) && !met_.clipping;
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    if (weakSig && (atMax || noisy) && !strongNb) {
        if (radio_.hasAmp && !cur_.ampOn) v.push_back({Advice::EnableAmp, "Weak signal: switching the amp on may help (it lowers the noise figure; only you switch it: strong signals can damage it)"});
        else if (radio_.hasBias) v.push_back({Advice::CheckAntenna, "Weak signal: check the antenna; an active antenna or LNA needs the bias-tee (antenna power) on"});
        else if (atMax) v.push_back({Advice::AtMax, "Weak signal at full gain: a better antenna or an external LNA would help"});
    }
    if (cur_.totalDb <= minGain() + 0.01 && met_.valid && met_.headroomDb < tgtH - tgt_.windowDb)
        v.push_back({Advice::AtMin, "Too strong even at the lowest gain: add an attenuator"});
    return v;
}

// ------------------------------------------------------------------------------------------------------------------------------ memory
std::string GainMemory::key(const std::string& radioKey, int stdMode, double f) {
    const double bandMhz = f < 30 ? 1 : f < 300 ? 10 : f < 1000 ? 25 : 100;
    const long band = (long)std::floor(f / bandMhz) * (long)bandMhz;
    return radioKey + "|" + std::to_string(stdMode) + "|" + std::to_string(band);
}
void GainMemory::put(const std::string& k, const GainMemoryEntry& e) { m_[k] = e; }
void GainMemory::forget(const std::string& k) { m_.erase(k); }
const GainMemoryEntry* GainMemory::find(const std::string& k) const {
    const auto it = m_.find(k);
    if (it != m_.end()) return &it->second;
    const size_t p = k.rfind('|');
    if (p == std::string::npos) return nullptr;
    const std::string pre = k.substr(0, p + 1);
    const double band = atof(k.c_str() + p + 1);
    const GainMemoryEntry* best = nullptr;
    double bd = 1e18;
    for (const auto& kv : m_)
        if (kv.first.compare(0, pre.size(), pre) == 0) {
            const double d = std::fabs(atof(kv.first.c_str() + pre.size()) - band);
            if (d < bd) { bd = d; best = &kv.second; }
        }
    return best;
}
std::string GainMemory::serialise() const {
    std::ostringstream o;
    for (const auto& kv : m_) {
        const auto& e = kv.second;
        o << kv.first << '\t' << e.plan.totalDb << '\t' << (e.plan.ampOn ? 1 : 0) << '\t' << e.metric << '\t' << e.metricName << '\t' << e.when << '\t';
        for (size_t i = 0; i < e.plan.db.size(); i++) o << (i ? "," : "") << e.plan.db[i];
        o << '\n';
    }
    return o.str();
}
bool GainMemory::parse(const std::string& s) {
    m_.clear();
    std::istringstream in(s);
    std::string line;
    bool ok = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t a = 0;
        for (;;) { const size_t b = line.find('\t', a); f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (f.size() < 7) { ok = false; continue; }
        GainMemoryEntry e;
        e.plan.totalDb = atof(f[1].c_str()); e.plan.ampOn = f[2] == "1"; e.metric = atof(f[3].c_str()); e.metricName = f[4]; e.when = atoll(f[5].c_str());
        size_t c = 0;
        while (c < f[6].size()) { const size_t d = f[6].find(',', c); e.plan.db.push_back(atof(f[6].substr(c, d == std::string::npos ? std::string::npos : d - c).c_str())); if (d == std::string::npos) break; c = d + 1; }
        m_[f[0]] = e;
    }
    return ok;
}

// ------------------------------------------------------------------------------------------------------------------------------ runner
AgRunResult autogainRun(AgRadioIo& io, const RadioProfile& r, const ModeTarget& t, AgSettings s, double startDb, bool ampOn, double timeoutSec) {
    AgRunResult res;
    if (s.mode != AgMode::Safe) s.mode = AgMode::Once;
    AutoGainController c;
    c.configure(r, t, s);
    const double t0 = io.now();
    c.start(t0, startDb, ampOn);
    while (!io.cancelled() && io.now() - t0 < timeoutSec) {
        AgInput in;
        if (!io.measure(in)) { io.sleep(0.02); continue; }
        in.now = io.now();
        AgCommand cmd;
        if (c.tick(in, cmd)) io.apply(cmd);
        if (!c.active()) break;
    }
    if (c.active()) {   // cancelled or out of time: back to where it started
        AgCommand back; back.plan = c.startPlan(); back.why = "stopped";
        io.apply(back);
        res.plan = c.startPlan();
        res.text = io.cancelled() ? "cancelled: the gain was put back" : "out of time: the gain was put back";
    } else {
        res.ok = c.phase() == Phase::Done;
        res.plan = c.current();
        res.text = c.resultText();
        res.metric = c.usedMetric();
        res.metricValue = c.bestMetric();
    }
    res.log.assign(c.log().begin(), c.log().end());
    return res;
}

void applyPlan(const RadioProfile& r, const StagePlan& p, TuneSettings& tune) {
    for (size_t i = 0; i < r.stages.size() && i < p.db.size(); i++) {
        const GainStage& s = r.stages[i];
        if (!s.settable) continue;
        if (!s.settingKey.empty()) { tune.radio[s.settingKey] = stageSettingValue(s, p.db[i]); continue; }
        if (s.id == "lna") tune.lnaDb = (int)std::lround(p.db[i]);
        else if (s.id == "vga") tune.vgaDb = (int)std::lround(p.db[i]);
        else if (s.id == "amp") tune.ampOn = p.ampOn;
        else if (s.id == "overall") tune.gainDb = p.db[i];
    }
}

StagePlan planFromTune(const RadioProfile& r, const TuneSettings& tune) {
    StagePlan p;
    p.db.assign(r.stages.size(), 0.0);
    p.ampOn = r.hasAmp && tune.ampOn && r.stageIndex("amp") >= 0;
    for (size_t i = 0; i < r.stages.size(); i++) {
        const GainStage& s = r.stages[i];
        double v = s.minDb();
        if (!s.settingKey.empty()) {
            const auto it = tune.radio.find(s.settingKey);
            if (it == tune.radio.end() || it->second == "auto") v = s.maxDb();
            else if (s.settingIsIndex) {
                const int k = atoi(it->second.c_str());
                const int idx = s.reduction ? (int)s.steps.size() - 1 - k : k;
                v = s.steps[(size_t)std::max(0, std::min((int)s.steps.size() - 1, idx))];
            } else v = s.reduction ? s.maxDb() - atof(it->second.c_str()) : atof(it->second.c_str());
        } else if (s.id == "lna") v = tune.lnaDb;
        else if (s.id == "vga") v = tune.vgaDb;
        else if (s.id == "amp") v = p.ampOn ? s.maxDb() : 0;
        else if (s.id == "overall") v = tune.gainDb;
        p.db[i] = v;
        if (s.settable) p.totalDb += v;
    }
    cascade(r, p);
    return p;
}

EngineAgIo::EngineAgIo(Engine& e, TuneSettings& tune, const RadioProfile& r, double bw, double off) : e_(e), tune_(tune), r_(r), bw_(bw), off_(off) {}
double EngineAgIo::now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void EngineAgIo::sleep(double s) { std::this_thread::sleep_for(std::chrono::microseconds((long long)(s * 1e6))); }
bool EngineAgIo::apply(const AgCommand& c) {
    applyPlan(r_, c.plan, tune_);
    return e_.retune(tune_);
}
bool EngineAgIo::measure(AgInput& in) {
    // the engine thread's GainMeter: every sample, the same feed as Auto (the display analyser's statistics are not used any more)
    BlockStats a;
    if (!e_.gainMeter().take(a, spec_)) return false;
    RxTelemetry t;
    if (e_.latestRx(t, rxSeq_)) { rxSeq_ = t.seq; rx_ = t; haveRx_ = true; }
    in.adc = a;
    in.spectrum = &spec_;
    in.rateHz = e_.radioRate() > 0 ? e_.radioRate() : e_.sampleRate();
    in.chanBwHz = bw_;
    in.offsetHz = off_;
    if (haveRx_) in.q = qualityFrom(rx_);
    return true;
}

AgRunResult autogainRun(Engine& e, TuneSettings& tune, const DeviceInfo& radio, int stdMode, AgMode how, AgSettings s, double timeoutSec, const std::atomic<bool>* cancel) {
    const RadioProfile r = radioProfileFor(radio);
    const double bw = tune.bandwidthMhz * 1e6;
    const ModeTarget t = modeTargetFor(stdMode, bw, e.sampleRate() > 0 ? e.sampleRate() : tune.sampleRate);
    s.mode = how == AgMode::Safe ? AgMode::Safe : AgMode::Once;
    EngineAgIo io(e, tune, r, bw);
    io.setCancelFlag(cancel);
    const StagePlan p = planFromTune(r, tune);
    return autogainRun(io, r, t, s, p.totalDb, p.ampOn, timeoutSec);
}

std::string autogainJson(const AutoGainController& c, double now) {
    auto esc = [](const std::string& s) {
        std::string o;
        for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; if ((unsigned char)ch >= 0x20) o += ch; }
        return o;
    };
    auto num = [](double v) { char b[32]; snprintf(b, sizeof b, "%.2f", std::isfinite(v) ? v : 0.0); return std::string(b); };
    const AgMeters& m = c.meters();
    std::string o = "{\"mode\":\"" + std::string(agModeName(c.mode())) + "\",\"phase\":\"" + phaseName(c.phase()) + "\",\"radio\":\"" + esc(c.radio().name) +
                    "\",\"target\":\"" + modeKindName(c.target().kind) + "\",\"gainDb\":" + num(c.current().totalDb) + ",\"amp\":" + (c.current().ampOn ? "true" : "false") +
                    ",\"headroomTargetDb\":" + num(c.headroomTarget()) + ",\"progress\":" + num(c.progress());
    o += ",\"meters\":{\"valid\":" + std::string(m.valid ? "true" : "false") + ",\"headroomDb\":" + num(m.headroomDb) + ",\"peakDbfs\":" + num(m.peakDbfs) +
         ",\"rmsDbfs\":" + num(m.rmsDbfs) + ",\"crestDb\":" + num(m.crestDb) + ",\"clipFraction\":" + num(m.clipFraction * 100) + ",\"floorDbfs\":" +
         (m.floorValid ? num(m.floorDbfs) : std::string("null")) + ",\"adcMarginDb\":" + (m.floorValid ? num(m.adcMarginDb) : std::string("null")) +
         ",\"channelSnrDb\":" + (m.floorValid ? num(m.chanSnrDb) : std::string("null")) + ",\"overload\":" + (m.overload ? "true" : "false") +
         ",\"intermod\":" + (m.imd ? "true" : "false") + ",\"overloads\":" + std::to_string(m.overloads) + "}";
    o += ",\"stages\":[";
    for (size_t i = 0; i < c.radio().stages.size() && i < c.current().db.size(); i++) {
        const GainStage& s = c.radio().stages[i];
        if (!s.settable) continue;
        if (o.back() != '[') o += ',';
        o += "{\"id\":\"" + esc(s.id) + "\",\"db\":" + num(c.current().db[i]) + "}";
    }
    o += "],\"advice\":[";
    bool first = true;
    for (const auto& a : c.advice()) { if (!first) o += ','; first = false; o += "\"" + esc(a.text) + "\""; }
    o += "],\"result\":\"" + esc(c.resultText()) + "\",\"log\":[";
    first = true;
    const auto& lg = c.log();
    for (size_t i = lg.size() > 8 ? lg.size() - 8 : 0; i < lg.size(); i++) {
        if (!first) o += ',';
        first = false;
        o += "{\"ago\":" + num(now - lg[i].t) + ",\"fromDb\":" + num(lg[i].fromDb) + ",\"toDb\":" + num(lg[i].toDb) + ",\"why\":\"" + esc(lg[i].why) + "\"}";
    }
    return o + "]}";
}

} // namespace dect2::ag

// ------------------------------------------------------------------------------------------------------------------------------ quick search
namespace dect2::ag {

// 4-6 gains around where the headroom would meet its target (a clean front end gains 1 dB per dB), measured for bracketSec each, ascending: the low
// ones are safe, a clip or an intermodulation product stops the climb. The best of them by the mode's metric (as Optimise once chooses) is applied.
void AutoGainController::beginBracket(const AgInput& in) {
    wantBracket_ = false;
    trials_.clear(); queue_.clear(); ti_ = -1; refined_ = false;
    const double tgtH = bracketHead();
    const double star = havePrior_ ? prior_.totalDb + 3.0 : cur_.totalDb + (headAvg_ - tgtH);
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    const Policy pol = (in.now < linearUntil_ || (strongNb && !set_.v2) || (imdSeen_ && !set_.v2)) ? Policy::Linearity : (set_.v2 ? pol_ : tgt_.policy);
    const double hi = std::min(maxGain(), ceilingDb(in.now)), lo = minGain();
    double last = -1e9;
    const double top = std::clamp(star + 3.0, lo, std::max(lo, hi));   // when the estimate is beyond the range the window slides down to its end
    // a remembered gain is a prior: five gains within +/-6 dB of it (the world may have changed); else the wide search around the headroom estimate
    const bool priorMode = havePrior_;
    const std::vector<double> offs = havePrior_ ? std::vector<double>{-9.0, -6.0, -3.0, 0.0, 3.0} : std::vector<double>{-14.0, -9.0, -5.0, -2.0, 0.0, 3.0};
    havePrior_ = false;
    for (double off : offs) {
        const double g = std::clamp(top + off - 3.0, lo, std::max(lo, hi));
        const double snapped = planStages(radio_, g, pol, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
        if (snapped > last + 0.4) { queue_.push_back({snapped, pol}); last = snapped; }
    }
    if (queue_.size() < 3) { queue_.clear(); return; }   // too coarse a radio to bracket: the loop does its normal work
    br_ = 1; brExtra_ = priorMode ? 4 : 0;   // a prior: the search stays within a few dB of it (the climb goes on from there)
    phase_ = Phase::Sweeping;
    logEvent(in.now, cur_.totalDb, cur_.totalDb, fmt("quick search: %.0f gains around %.0f dB", (double)queue_.size(), star));
}

bool AutoGainController::tickBracket(const AgInput& in, AgCommand& out) {
    const double now = in.now;
    if (queue_.empty() && ti_ < 0) { br_ = 0; phase_ = Phase::Tracking; return false; }
    if (ti_ >= 0) {
        AgTrial& t = trials_[(size_t)ti_];
        const double ready = changedAt_ + radio_.settleSec + 0.05;
        if (now < ready) return false;
        if (t.n == 0) brFrom_ = now;
        accumulate(t, in);
        const bool bad = in.adc.clipFraction > tgt_.clipLimit * 3 || in.adc.headroomDb < 0.1;
        if (now - brFrom_ < set_.bracketSec && !bad) return false;
        t.done = true;
        for (int k = ti_ - 1; k >= 0; k--) {   // intermodulation: against the step below (a product rises about 3 dB per dB)
            const AgTrial& p = trials_[(size_t)k];
            if (!p.done || p.pol != t.pol || p.plan.totalDb >= t.plan.totalDb - 0.5 || p.spec.size() != t.spec.size() || p.spec.empty()) continue;
            const ImdReport r = imdCompare(p.spec, t.spec, t.plan.totalDb - p.plan.totalDb);
            if (r.valid && r.overload && p.headroomMin > 1) { t.imd = true; imdSeen_ = imdSeen_ || r.spurs > 0; compSeen_ = compSeen_ || r.compressed; }
            break;
        }
        if (t.clipMax > tgt_.clipLimit || t.imd || t.headroomMin < 0.5) {   // the gains above it are worse still
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const std::pair<double, Policy>& q) { return q.first >= t.plan.totalDb - 0.01; }), queue_.end());
            if (t.clipMax > tgt_.clipLimit) { met_.overloads++; }
        }
    }
    if (queue_.empty() && ti_ >= 0 && brExtra_ < 4) {
        // still room above the last gain although nothing overloaded: the ADC's own noise hid the signal at the start, so the headroom
        // estimate was too low; the search climbs further (a jump of 70 % of the missing headroom, 4-16 dB), not past the range
        const AgTrial& l = trials_[(size_t)ti_];
        const bool over = l.clipMax > tgt_.clipLimit || l.imd || l.headroomMin < 0.5;
        const double hi = std::min(maxGain(), ceilingDb(now));
        if (!over && l.headroomMin > bracketHead() + 1.5 && l.plan.totalDb < hi - 0.5) {
            brExtra_++;
            double g = l.plan.totalDb, lastQ = g;
            const double jump = std::clamp(0.7 * (l.headroomMin - bracketHead()), 2.0, 16.0);
            for (int k = 1; k <= 3; k++) {
                const double snapped = planStages(radio_, std::min(hi, l.plan.totalDb + jump * k), l.pol, set_.stageMask, cur_.db, cur_.ampOn).totalDb;
                if (snapped > lastQ + 0.4) { queue_.push_back({snapped, l.pol}); lastQ = snapped; }
            }
            (void)g;
        }
    }
    // a gain that would (1 dB per dB from the last one measured) leave less than the mode's headroom is not worth a clip: the search ends below it
    if (!queue_.empty() && ti_ >= 0) {
        const AgTrial& l = trials_[(size_t)ti_];
        const double floorHead = std::max(1.0, bracketHead() - tgt_.windowDb - 3.0);
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const std::pair<double, Policy>& q) {
            return q.second == l.pol && l.headroomMin - (q.first - l.plan.totalDb) < floorHead && q.first > l.plan.totalDb; }), queue_.end());
    }
    if (!queue_.empty()) {
        const auto q = queue_.front(); queue_.erase(queue_.begin());
        beginTrial(now, q.first, q.second, out);
        return true;
    }
    // done: apply the best
    Metric used;
    // Auto keeps the mode's headroom (within its dead band) wherever it can: raw SNR with the ADC near full scale is not the aim
    // and does not buy gain that only costs headroom (the floor already far above the ADC's own noise: the loop would take it down again)
    const double minHead = set_.v2 ? std::max(0.5, tgt_.minHeadroomDb - 1.0) : std::max(0.5, headroomTarget() - tgt_.windowDb - 1.0);   // a dB of allowance for the short measurement
    int b = chooseTrial(used, minHead, tgt_.marginGoalDb + 15);
    double wastedTo = -1;   // every gain tried was already "wasted": the floor sits that far above the ADC's own noise at the lowest one: go down by the excess
    if (b < 0 || used == Metric::Auto) {
        b = chooseTrial(used, minHead);
        if (b >= 0) {
            int lo = -1;
            for (size_t i = 0; i < trials_.size(); i++) if (trials_[i].done && trials_[i].nFloor && trials_[i].clipMax <= tgt_.clipLimit && (lo < 0 || trials_[i].plan.totalDb < trials_[(size_t)lo].plan.totalDb)) lo = (int)i;
            if (lo >= 0) {
                const AgTrial& L = trials_[(size_t)lo];
                const double qPerBin = -(6.02 * radio_.adcBits + 1.76) - 10 * std::log10((double)std::max<size_t>(L.spec.size(), 64)) + 1.76;
                const double margin = L.floor() - qPerBin;
                if (margin > tgt_.marginGoalDb + 15) { wastedTo = L.plan.totalDb - (margin - (tgt_.marginGoalDb + 13)); b = lo; }
            }
        }
    }
    if (b < 0 || used == Metric::Auto) b = chooseTrial(used);
    // a search must never end on a gain that clipped hard: the highest clean one instead (v2)
    if (set_.v2 && b >= 0 && trials_[(size_t)b].clipMax > tgt_.clipLimit * 3) {
        int alt = -1;
        for (size_t i = 0; i < trials_.size(); i++) if (trials_[i].done && trials_[i].clipMax <= tgt_.clipLimit * 3 && (alt < 0 || trials_[i].plan.totalDb > trials_[(size_t)alt].plan.totalDb)) alt = (int)i;
        if (alt < 0) {   // every gain tried clipped: the lowest of them, and the loop goes on from there
            for (size_t i = 0; i < trials_.size(); i++) if (trials_[i].done && (alt < 0 || trials_[i].plan.totalDb < trials_[(size_t)alt].plan.totalDb)) alt = (int)i;
        }
        b = alt;
    }
    br_ = 0;
    phase_ = Phase::Tracking;
    bracketDone_ = true;
    if (b < 0) { out.plan = startPlan_; out.why = "quick search: nothing measured, gain put back"; cur_ = startPlan_; changedAt_ = now; nAvg_ = 0; changes_++; explain(out.why); return true; }
    const AgTrial best = trials_[(size_t)b];
    used_ = used;
    double unsafeAbove = 1e9;   // the lowest gain above the best that overloaded: the loop does not climb into it for a while
    for (const auto& t : trials_) if (t.done && (t.clipMax > tgt_.clipLimit || t.imd || t.headroomMin < 0.5) && t.plan.totalDb > best.plan.totalDb + 0.01) unsafeAbove = std::min(unsafeAbove, t.plan.totalDb);
    if (unsafeAbove < 1e8) { ceil_ = unsafeAbove - 1; ceilUntil_ = now + 60 / std::max(0.25, set_.loopSpeed); }
    const std::string why = fmt("quick search: %.0f dB gave the best ", best.plan.totalDb) + metricName(used);
    if (wastedTo > -1) {
        const double to = std::max(minGain(), wastedTo);
        move(now, nextStep(best.plan.totalDb, to - best.plan.totalDb, best.pol), best.pol, "quick search: the noise floor is far above the ADC's at every gain tried, so the lowest useful gain", out);
        explain(out.why);
        return true;
    }
    move(now, best.plan.totalDb, best.pol, why, out);
    guard_ = false;
    explain(why);
    return true;
}

void AutoGainController::explain(const std::string& lead) {
    reasons_.clear();
    if (tgt_.bandNote && *tgt_.bandNote) reasons_.push_back(tgt_.bandNote);
    const bool strongNb = fl_.valid && fl_.strongestDbfs > -25 && fl_.strongestDbfs > fl_.chanDbfs + 20;
    if (strongNb) reasons_.push_back("a strong signal nearby, kept headroom");
    if (imdSeen_) reasons_.push_back("intermodulation seen: linear stage split");
    if (compSeen_) reasons_.push_back("front end compressing: gain kept down");
    if (!ovTimes_.empty()) reasons_.push_back("backed off after an overload");
    if (fl_.valid && fl_.snrDb < 12 && !strongNb && ovTimes_.empty()) reasons_.push_back("weak signal: gain for sensitivity");
    if (fl_.valid && fl_.adcMarginDb > tgt_.marginGoalDb + 15) reasons_.push_back("the antenna's noise is far above the ADC's: more gain would only cost headroom");
    if (reasons_.empty()) reasons_.push_back(fmt("headroom of about %.0f dB kept for the signal peaks", headroomTarget()));
    reasons_.push_back(std::string("last change: ") + lead);
    expl_ = fmt("Gain set to %.0f dB: ", cur_.totalDb) + reasons_.front();
    if (reasons_.size() > 2) expl_ += ", " + reasons_[1];
}

} // namespace dect2::ag
