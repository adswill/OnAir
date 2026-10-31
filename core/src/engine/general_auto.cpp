// General / Auto state machine (dect2/engine/general_auto.h).
#include "dect2/engine/general_auto.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace genauto {

bool fromResult(const sigid::Result& r, double rfHint, Detection& out) {
    if (!r.f.valid || r.cands.empty() || r.verdict == sigid::VerdictUnknown) return false;
    const sigid::Candidate& c = r.cands[0];
    out = Detection();
    out.rfHz = r.f.rfHz > 0 ? r.f.rfHz : rfHint;
    out.bwHz = r.f.obwHz;
    out.snrDb = r.f.snrDb;
    out.name = c.name; out.modeId = c.modeId; out.subId = c.subId; out.family = c.family;
    out.confidence = c.confidence;
    out.verify = c.verify;
    out.likely = r.verdict == sigid::VerdictLikely;
    out.bursty = r.f.duty < 0.9;
    out.mem = sigid::toMemory(c, r.f);
    out.mem.bandwidthHz = 0;   // as the Identify window opens it: the mode's own width, the measured one is in its options
    out.mem.tags.clear(); out.mem.notes.clear();
    return true;
}

bool fromFound(const sigid::Capture::Found& f, double rfOfCentreHz, Detection& out) {
    return fromResult(f.res, rfOfCentreHz + f.region.centreHz, out);
}

double releaseFor(const Config& c, double bwHz) {
    if (c.releaseHz > 0) return c.releaseHz;
    return std::max(c.minReleaseHz, c.releaseWidthShare * std::max(0.0, bwHz));
}

namespace {
double matchTol(double a, double b) {
    const double w = std::min(a > 0 ? a : 12.5e3, b > 0 ? b : 12.5e3);
    return std::max(2.5e3, 0.3 * w);
}
bool decodable(const Detection& d) { return !d.mem.mode.empty(); }
} // namespace

const Label* GeneralAuto::find(int id) const {
    if (id <= 0) return nullptr;
    for (const Label& l : labels_) if (l.id == id) return &l;
    return nullptr;
}

std::vector<const Label*> GeneralAuto::visible() const {
    std::vector<const Label*> v;
    for (const Label& l : labels_) if (l.shown && l.inView && !l.ignored && !l.decoding) v.push_back(&l);
    std::sort(v.begin(), v.end(), [](const Label* a, const Label* b) { return a->d.snrDb > b->d.snrDb; });
    return v;
}

bool GeneralAuto::isIgnored(const Detection& d) const {
    for (const Ign& g : ignored_) if (g.name == d.name && std::fabs(g.rfHz - d.rfHz) <= g.tolHz) return true;
    return false;
}

bool GeneralAuto::wantScan(double nowSec) const {
    return state_ == State::General && !scanning_ && nowSec >= settleUntil_ && nowSec >= nextScan_;
}
void GeneralAuto::scanStarted(double) { scanning_ = true; }

void GeneralAuto::retuned(double nowSec) {
    nextScan_ = nowSec + 0.8;   // sooner than the regular period: the view is new, but the radio needs a moment to settle
}

void GeneralAuto::expire(double nowSec) {
    labels_.erase(std::remove_if(labels_.begin(), labels_.end(), [&](const Label& l) { return !l.inView && !l.decoding && nowSec - l.lastSeenSec > cfg.forgetSec; }), labels_.end());
    while (labels_.size() > cfg.maxLabels) {   // too many: the oldest unshown, else the oldest
        size_t worst = labels_.size();
        for (size_t i = 0; i < labels_.size(); i++) {
            if (labels_[i].decoding) continue;
            if (worst == labels_.size()) { worst = i; continue; }
            const Label& a = labels_[i]; const Label& b = labels_[worst];
            if ((!a.shown && b.shown) || (a.shown == b.shown && a.lastSeenSec < b.lastSeenSec)) worst = i;
        }
        if (worst == labels_.size()) break;
        labels_.erase(labels_.begin() + (long)worst);
    }
}

void GeneralAuto::scanDone(double nowSec, double spanLoHz, double spanHiHz, const std::vector<Detection>& found) {
    scanning_ = false;
    nextScan_ = nowSec + cfg.scanPeriodSec;
    if (state_ == State::Decoding) return;   // a late result: the radio is on a decoder now
    for (Label& l : labels_) l.inView = l.d.rfHz >= spanLoHz && l.d.rfHz <= spanHiHz;
    std::vector<char> seen(labels_.size(), 0);
    std::vector<Label> added;
    for (const Detection& d : found) {
        if (d.confidence < cfg.minConfidence || isIgnored(d)) continue;
        // the nearest label that was not matched yet
        int best = -1; double bestD = 1e18;
        for (size_t i = 0; i < labels_.size(); i++) {
            if (seen[i] || labels_[i].decoding) continue;
            const double dist = std::fabs(labels_[i].d.rfHz - d.rfHz);
            if (dist <= matchTol(labels_[i].d.bwHz, d.bwHz) && dist < bestD) { bestD = dist; best = (int)i; }
        }
        if (best < 0) {
            Label l; l.id = nextId_++; l.d = d; l.confidence = d.confidence; l.hits = 1; l.misses = 0; l.inView = true;
            l.firstSec = l.lastSeenSec = nowSec;
            l.shown = cfg.confirmScans <= 1 || (d.confidence >= cfg.instantConfidence && d.verify != sigid::VerifyFailed && d.likely);
            added.push_back(l);
            continue;
        }
        Label& l = labels_[(size_t)best];
        seen[(size_t)best] = 1;
        l.hits++; l.misses = 0; l.inView = true; l.lastSeenSec = nowSec;
        l.confidence = 0.5 * l.confidence + 0.5 * d.confidence;
        const bool sameName = d.name == l.d.name && d.subId == l.d.subId;
        bool adopt = sameName;
        if (!sameName) {
            if (l.pendName == d.name + "/" + d.subId) l.pendCount++; else { l.pendName = d.name + "/" + d.subId; l.pendCount = 1; }
            // a different name needs two scans in a row, or a clearly better number
            if (l.pendCount >= 2 || d.confidence > l.confidence + 0.25) { adopt = true; l.confidence = d.confidence; }
        } else { l.pendName.clear(); l.pendCount = 0; }
        if (adopt) { l.d = d; l.pendName.clear(); l.pendCount = 0; }
        else { l.d.rfHz = d.rfHz; l.d.bwHz = d.bwHz; l.d.snrDb = d.snrDb; }   // where it is, not what it is
        if (!l.shown && (l.hits >= cfg.confirmScans || (l.confidence >= cfg.instantConfidence && l.d.verify != sigid::VerifyFailed && l.d.likely))) l.shown = true;
    }
    // labels whose place was in view and which were not found
    for (size_t i = 0; i < seen.size(); i++) {
        Label& l = labels_[i];
        if (seen[i] || !l.inView || l.decoding) continue;
        l.misses++;
        if (!l.shown) l.hits = 0;
    }
    labels_.erase(std::remove_if(labels_.begin(), labels_.end(), [&](const Label& l) {
        if (l.decoding || !l.inView || l.misses == 0) return false;
        return !l.shown || l.misses >= (l.d.bursty ? cfg.dropScansBursty : cfg.dropScans);
    }), labels_.end());
    for (Label& l : added) labels_.push_back(l);
    expire(nowSec);
}

Step GeneralAuto::decode(double nowSec, int labelId) {
    Step s;
    const Label* cl = find(labelId);
    if (!cl) { s.note = "That signal is gone."; return s; }
    if (!decodable(cl->d)) { s.note = "OnAir has no decoder for " + cl->d.name + "."; return s; }
    for (Label& l : labels_) l.decoding = false;
    Label* l = nullptr;
    for (Label& x : labels_) if (x.id == labelId) l = &x;
    l->decoding = true;
    state_ = State::Decoding; activeId_ = l->id;
    lockRfHz_ = l->d.rfHz; lockBwHz_ = l->d.bwHz; lockName_ = l->d.name;
    lockReleaseHz_ = releaseFor(cfg, l->d.bwHz);
    settleUntil_ = nowSec + cfg.settleSec;
    haveSuppress_ = false;
    for (Label& x : labels_) x.inView = false;   // the radio moves: the old view is gone until the next scan
    s.act = Action::Decode; s.labelId = l->id; s.mem = l->d.mem; s.mem.freqHz = (uint64_t)std::llround(l->d.rfHz);
    s.note = "Decoding " + l->d.name;
    return s;
}

Step GeneralAuto::stop(double nowSec) {
    Step s;
    if (state_ != State::Decoding) return s;
    haveSuppress_ = true; suppressRf_ = lockRfHz_; suppressTol_ = lockReleaseHz_;
    for (Label& l : labels_) l.decoding = false;
    state_ = State::General; activeId_ = 0;
    settleUntil_ = nowSec + cfg.settleSec; nextScan_ = settleUntil_;
    s.act = Action::ReturnToGeneral; s.note = "Back to General";
    return s;
}

void GeneralAuto::abandon() {
    for (Label& l : labels_) l.decoding = false;
    state_ = State::General; activeId_ = 0; haveSuppress_ = false;
    nextScan_ = 0; settleUntil_ = 0; scanning_ = false;
}

Step GeneralAuto::tick(double nowSec, double tunedHz) {
    Step s;
    if (state_ == State::Decoding) {
        if (nowSec >= settleUntil_ && std::fabs(tunedHz - lockRfHz_) > lockReleaseHz_) {
            const std::string name = lockName_;
            for (Label& l : labels_) l.decoding = false;
            state_ = State::General; activeId_ = 0; haveSuppress_ = false;
            settleUntil_ = nowSec + cfg.settleSec; nextScan_ = settleUntil_;
            s.act = Action::ReturnToGeneral; s.note = "Tuned away from " + name + ": back to General";
        }
        return s;
    }
    if (haveSuppress_ && std::fabs(tunedHz - suppressRf_) > suppressTol_) haveSuppress_ = false;
    if (!cfg.autoDecode || haveSuppress_ || nowSec < settleUntil_) return s;
    const Label* best = nullptr;
    for (const Label& l : labels_) {
        if (!l.shown || l.ignored || !decodable(l.d) || l.confidence < cfg.autoConfidence || l.d.verify == sigid::VerifyFailed) continue;
        if (std::fabs(tunedHz - l.d.rfHz) > releaseFor(cfg, l.d.bwHz)) continue;
        if (!best || std::fabs(tunedHz - l.d.rfHz) < std::fabs(tunedHz - best->d.rfHz)) best = &l;
    }
    if (!best) return s;
    s = decode(nowSec, best->id);
    if (s.act == Action::Decode) s.note = "Auto-decode: " + best->d.name;
    return s;
}

void GeneralAuto::ignore(int labelId) {
    for (size_t i = 0; i < labels_.size(); i++) {
        if (labels_[i].id != labelId) continue;
        const Label& l = labels_[i];
        ignored_.push_back({l.d.rfHz, std::max(5e3, 0.5 * l.d.bwHz), l.d.name});
        if (activeId_ == labelId) { state_ = State::General; activeId_ = 0; }
        labels_.erase(labels_.begin() + (long)i);
        return;
    }
}
void GeneralAuto::clearIgnored() { ignored_.clear(); }

void GeneralAuto::reset() {
    labels_.clear(); ignored_.clear();
    state_ = State::General; activeId_ = 0; haveSuppress_ = false;
    nextScan_ = 0; settleUntil_ = 0; scanning_ = false;
}

} // namespace genauto
} // namespace dect2
