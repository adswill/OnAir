#include "dect2/geo/sat_plan.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

unsigned bandOf(double hz) {
    if (hz >= 130e6 && hz <= 174e6) return BandVhf;
    if (hz >= 400e6 && hz <= 470e6) return BandUhf;
    if (hz >= 1.4e9 && hz <= 1.8e9) return BandL;
    return BandOther;
}

bool linkHearable(const SatLink& l, const RadioCaps& radio, unsigned antennaMask) {
    if (!(bandOf(l.hz) & antennaMask)) return false;
    if (radio.minHz > 0 && l.hz < radio.minHz) return false;
    if (radio.maxHz > 0 && l.hz > radio.maxHz) return false;
    if (radio.maxRateHz > 0 && l.needRateHz > 0 && l.needRateHz > radio.maxRateHz) return false;
    return true;
}

int bestLink(const std::vector<SatLink>& links, const RadioCaps& radio, unsigned antennaMask, bool* hearable) {
    int best = -1, bestScore = -1;
    for (int i = 0; i < (int)links.size(); i++) {
        const int score = (linkHearable(links[i], radio, antennaMask) ? 2 : 0) + (links[i].decoder ? 1 : 0);
        if (best < 0 || score > bestScore || (score == bestScore && links[i].hz < links[best].hz)) { best = i; bestScore = score; }
    }
    if (hearable) *hearable = best >= 0 && linkHearable(links[best], radio, antennaMask);
    return best;
}

bool passVisible(unsigned tags, bool retired, const std::vector<SatLink>& links, const Pass& p, const PassFilter& f) {
    if (!(tags & f.tagMask)) return false;
    if (retired && !f.showRetired) return false;
    if (p.maxElDeg < f.minPeakDeg) return false;
    if (f.onlyHearable) {
        bool any = false;
        for (const SatLink& l : links) any = any || linkHearable(l, f.radio, f.antennaMask);
        if (!any) return false;
    }
    return true;
}

std::vector<Pass> findPassesPeak(const Sgp4& s, const Observer& o, double t0, double t1, double horizonDeg, double minPeakDeg) {
    std::vector<Pass> v = findPasses(s, o, t0, t1, horizonDeg);
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Pass& p) { return p.maxElDeg < minPeakDeg; }), v.end());
    return v;
}

double tleAgeDays(const Tle& t, double nowSec) { return unixToJd(nowSec) - t.epochJd; }

TleFresh tleFreshness(double ageDays) {
    if (ageDays < -2) return TleFresh::Expired;   // an epoch in the future: a wrong clock or wrong elements
    if (ageDays < 3) return TleFresh::Fresh;
    if (ageDays < 14) return TleFresh::Ageing;
    if (ageDays < 30) return TleFresh::Stale;
    return TleFresh::Expired;
}

double tleTimingErrorSec(double ageDays) {
    const double d = std::max(0.0, std::fabs(ageDays));
    return 1.0 + 1.5 * d + 0.05 * d * d;
}

void mergeNewestTle(std::map<int, Tle>& into, const std::vector<Tle>& add) {
    for (const Tle& t : add) {
        auto it = into.find(t.norad);
        if (it == into.end() || t.epochJd >= it->second.epochJd) into[t.norad] = t;
    }
}

int matchPass(const Pass& armed, const std::vector<Pass>& candidates, double tolSec) {
    int best = -1;
    double bestD = tolSec;
    for (int i = 0; i < (int)candidates.size(); i++) {
        const double d = std::fabs(candidates[i].tca - armed.tca);
        if (d <= bestD) { bestD = d; best = i; }
    }
    return best;
}

// ---- the run of one pass

void PassRunner::tick(double now, const RunJob* job, PassHost& h, std::vector<PassResult>* finished) {
    if (active() && now >= cur_.end) { finish(now, PassOutcome::Completed, h, finished); return; }
    if (active() && (!job || job->norad != cur_.norad || !samePass(job->pass, cur_.pass))) { finish(now, PassOutcome::Cancelled, h, finished); return; }
    if (active() && job) {   // new elements: the same pass with a little different times
        cur_.pass = job->pass; cur_.start = job->start; cur_.end = job->end;
    }
    if (!active()) {
        if (!job || now >= job->end) return;
        if (haveDone_ && job->norad == doneNorad_ && samePass(job->pass, donePass_)) return;
        begin(now, *job, h, finished);
        if (!active()) return;
    }
    if (following_ && (!h.running() || h.family() != cur_.family)) {   // the user changed the mode or stopped the receiver during the pass: it is theirs now
        following_ = false;
        owned_ = false; restoreMode_ = false;
        if (weRecord_) { h.stopRecording(); weRecord_ = false; }
        h.log("pass of " + cur_.name + ": the receiver was changed by hand, no longer following it");
    }
    if (phase_ == Phase::Warmup && now >= cur_.start) {
        phase_ = Phase::Receiving;
        if (!aosDone_) { aosDone_ = true; h.log("pass of " + cur_.name + " begins (AOS)"); h.onAos(cur_); }
    }
    if (phase_ == Phase::Receiving && following_ && cur_.record && !recTried_ && h.running() && h.canRecord() && !h.recording()) {
        recTried_ = true;
        std::string err;
        if (h.startRecording(iqPath_, err)) { weRecord_ = true; h.log("pass of " + cur_.name + ": recording IQ to " + iqPath_); }
        else { iqPath_.clear(); h.log("recording failed: " + err); }
    }
    if (following_) doppler(now, h, false);
}

void PassRunner::begin(double now, const RunJob& j, PassHost& h, std::vector<PassResult>* finished) {
    cur_ = j;
    following_ = false; owned_ = false; restoreMode_ = false; weRecord_ = false; recTried_ = false; aosDone_ = false;
    iqPath_.clear(); lastHz_ = 0; lastTune_ = -1e18; retunes_ = 0; startedAt_ = now;
    prevRunning_ = h.running(); prevFamily_ = h.family(); prevHz_ = h.tunedHz();
    if (h.transmitting()) { phase_ = Phase::Warmup; finish(now, PassOutcome::SkippedTx, h, finished, "transmitting"); return; }
    if (prevRunning_ && prevFamily_ == j.family) {
        following_ = true;   // the receiver is already in this mode: tune and record, leave it running afterwards
    } else if (prevRunning_) {
        if (!j.takeOver) {   // scheduled by itself: a receiver the user is using stays as it is
            phase_ = Phase::Warmup;
            h.log("pass of " + j.name + " skipped: the receiver is in use in another mode");
            finish(now, PassOutcome::SkippedBusy, h, finished, "receiver in use");
            return;
        }
        h.stop();
        h.setMode(j.family);
        h.tune(j.hz + (j.radioFollowsDoppler ? h.dopplerHz(j, now) : 0.0));
        h.start();
        following_ = h.running();
        owned_ = true; restoreMode_ = true;
    } else if (j.startReceiver) {
        if (prevFamily_ != j.family) h.setMode(j.family);
        h.tune(j.hz + (j.radioFollowsDoppler ? h.dopplerHz(j, now) : 0.0));
        h.start();
        following_ = h.running();
        owned_ = true; restoreMode_ = prevFamily_ != j.family;
    }
    phase_ = Phase::Warmup;
    if ((j.startReceiver || prevRunning_) && !following_) { finish(now, PassOutcome::Failed, h, finished, "the receiver did not start"); return; }
}

void PassRunner::doppler(double now, PassHost& h, bool force) {
    if (!cur_.radioFollowsDoppler) return;
    if (!force && now - lastTune_ < 1.0) return;
    lastTune_ = now;
    const double target = cur_.hz + h.dopplerHz(cur_, now);
    if (force || lastHz_ == 0 || std::fabs(target - lastHz_) >= cur_.retuneTolHz) { h.tune(target); lastHz_ = target; retunes_++; }
}

void PassRunner::finish(double now, PassOutcome why, PassHost& h, std::vector<PassResult>* finished, const std::string& detail) {
    const bool reached = why == PassOutcome::Completed;
    if (aosDone_) h.onLos(cur_, reached);
    if (weRecord_) { h.stopRecording(); weRecord_ = false; }
    if (owned_ && h.running()) h.stop();
    if (restoreMode_) {
        h.setMode(prevFamily_);
        h.tune(prevHz_);
        if (prevRunning_) h.start();
    } else if (following_ && !owned_) {
        h.tune(cur_.hz);   // the receiver stays in this mode: back on the nominal frequency
    } else if (owned_) {
        h.tune(prevHz_);
    }
    if (reached || why == PassOutcome::Cancelled) h.log("pass of " + cur_.name + (reached ? " ends (LOS)" : ": cancelled"));
    PassResult r;
    r.job = cur_; r.outcome = why; r.iqPath = iqPath_; r.detail = detail; r.startedAt = startedAt_; r.endedAt = now;
    if (finished) finished->push_back(std::move(r));
    phase_ = Phase::Idle;
    following_ = false;
    haveDone_ = true; doneNorad_ = cur_.norad; donePass_ = cur_.pass;
}

void PassRunner::cancel(double now, PassHost& h, std::vector<PassResult>* finished) {
    if (active()) finish(now, PassOutcome::Cancelled, h, finished);
}

}  // namespace dect2
