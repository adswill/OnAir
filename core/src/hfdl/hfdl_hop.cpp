#include "dect2/hfdl/hfdl_hop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {

const std::vector<HfdlBand>& hfdlBands() {
    static const std::vector<HfdlBand> b = {
        {"5.5 MHz (5.45 - 5.72)", 5.58, 5450, 5725, true},       {"6.6 MHz (6.53 - 6.71)", 6.62, 6525, 6715, true},
        {"8.9 MHz (8.82 - 8.98)", 8.90, 8820, 8980, true},       {"10.06 MHz (10.03 - 10.09)", 10.06, 10020, 10100, true},
        {"11.3 MHz (11.18 - 11.39)", 11.29, 11180, 11390, true}, {"13.3 MHz (13.26 - 13.36)", 13.31, 13260, 13360, true},
        {"17.94 MHz (17.90 - 17.99)", 17.945, 17900, 17990, true}, {"21.96 MHz (21.93 - 22.00)", 21.963, 21920, 22000, true},
        {"3.455 MHz", 3.455, 3450, 3460, false}, {"3.9 MHz", 3.90, 3895, 3905, false},
        {"4.67 MHz (4.66 - 4.69)", 4.675, 4655, 4695, false}, {"15.03 MHz", 15.025, 15020, 15030, false}};
    return b;
}

int hfdlBandOfKhz(int khz) {
    const auto& b = hfdlBands();
    for (size_t i = 0; i < b.size(); i++) if (khz >= b[i].loKhz && khz <= b[i].hiKhz) return (int)i;
    return -1;
}

int hfdlBandOfMhz(double mhz) {
    const auto& b = hfdlBands();
    const int k = hfdlBandOfKhz((int)std::lround(mhz * 1000.0));
    if (k >= 0) return k;
    for (size_t i = 0; i < b.size(); i++) if (std::fabs(mhz - b[i].centreMhz) <= 0.1) return (int)i;
    return -1;
}

void HfdlBandHopper::reset() { b_.clear(); cur_ = -1; pending_ = -1; started_ = false; status_.clear(); lastGood = 0; }

double HfdlBandHopper::rate(int band) const { return band >= 0 && (size_t)band < b_.size() && b_[(size_t)band].visited ? b_[(size_t)band].rate : 0.0; }
bool HfdlBandHopper::visited(int band) const { return band >= 0 && (size_t)band < b_.size() && b_[(size_t)band].visited; }
std::string HfdlBandHopper::status() const { return status_; }

// what the memory of a band's rate is worth now: it fades while the band is not watched
double HfdlBandHopper::decayed(int i, double now) const {
    const B& x = b_[(size_t)i];
    if (!x.visited) return c_.unvisitedGuess;
    return x.rate * std::exp(-std::max(0.0, now - x.seenSec) / c_.memorySec);
}

int HfdlBandHopper::update(const Obs& o) {
    const int n = (int)hfdlBands().size();
    if ((int)b_.size() != n) b_.assign((size_t)n, B());
    const double now = o.nowSec;
    // the announcements: remember how many distinct active frequencies each band was said to have
    if (!o.announcedKhz.empty()) {
        std::vector<std::vector<int>> seen((size_t)n);
        for (int k : o.announcedKhz) { const int bi = hfdlBandOfKhz(k); if (bi >= 0 && std::find(seen[(size_t)bi].begin(), seen[(size_t)bi].end(), k) == seen[(size_t)bi].end()) seen[(size_t)bi].push_back(k); }
        for (int i = 0; i < n; i++) if (!seen[(size_t)i].empty()) { b_[(size_t)i].hint = (int)seen[(size_t)i].size(); b_[(size_t)i].hintSec = now; }
    }
    for (auto& x : b_) if (x.hint && now - x.hintSec > c_.hintAgeSec) x.hint = 0;

    if (o.band < 0 || o.band >= n) { cur_ = -1; started_ = false; status_ = "not on an HFDL band"; return -1; }
    if (o.goodPdus < lastGood) lastGood = o.goodPdus;   // the counters restarted
    if (pending_ >= 0 && o.band != pending_ && now >= pendingSec_ && now - pendingSec_ < 6) return -1;   // the radio is still retuning
    if (o.band == pending_ || now - pendingSec_ >= 6) pending_ = -1;
    if (!started_ || o.band != cur_ || now < lastSec) {   // arrived (our hop or the user's tuning): a new look begins
        if (started_ && cur_ >= 0 && (size_t)cur_ < b_.size()) b_[(size_t)cur_].seenSec = lastSec;
        cur_ = o.band; enteredSec = now; lastSec = now; lastGood = o.goodPdus; started_ = true;
        B& x = b_[(size_t)cur_];
        if (!x.visited) { x.visited = true; x.rate = 0; }   // the first look starts from nothing: the rate is what is measured now
        x.seenSec = now;
        char s[96]; snprintf(s, sizeof s, "%s: looking", hfdlBands()[(size_t)cur_].name);
        status_ = s;
        return -1;
    }
    if (now - lastSec < 0.5) return -1;   // calls closer than half a second add nothing (and would make the rate jump)
    // measure the current band
    B& cb = b_[(size_t)cur_];
    const double dt = now - lastSec;
    if (dt > 0) {
        const double inst = (double)(o.goodPdus - lastGood) * 60.0 / dt;
        const double a = 1.0 - std::exp(-dt / c_.rateTauSec);
        // right after arriving the estimate follows the data quickly (a fresh band has no history)
        const double a2 = (now - enteredSec) < 3 * c_.rateTauSec && cb.rate == 0 ? std::max(a, 0.5) : a;
        cb.rate += (inst - cb.rate) * a2;
        cb.seenSec = now;
        lastSec = now; lastGood = o.goodPdus;
    }
    const double here = cb.rate;
    const double dwell = here > 0.5 ? c_.longDwellSec : c_.dwellSec;
    const double waited = now - enteredSec;
    char s[128];
    if (here >= c_.goodPerMin) { snprintf(s, sizeof s, "%s: %.0f PDU/min, staying", hfdlBands()[(size_t)cur_].name, here); status_ = s; return -1; }
    if (waited < dwell) {
        snprintf(s, sizeof s, "%s: %s, deciding in %.0f s", hfdlBands()[(size_t)cur_].name, here > 0.05 ? "some traffic" : "silent", dwell - waited);
        status_ = s; return -1;
    }
    // choose the best other band
    int best = -1; double bestScore = -1, bestAge = -1;
    for (int i = 0; i < n; i++) {
        if (i == cur_) continue;
        if (!hfdlBands()[(size_t)i].common && b_[(size_t)i].hint == 0 && !(b_[(size_t)i].visited && b_[(size_t)i].rate > 0.05)) continue;   // a rare band: only when announced
        const double score = decayed(i, now) + c_.hintWeight * std::min(b_[(size_t)i].hint, c_.hintMax);
        const double age = b_[(size_t)i].visited ? now - b_[(size_t)i].seenSec : 1e9;   // tie: the band looked at longest ago
        if (score > bestScore + 1e-9 || (std::fabs(score - bestScore) <= 1e-9 && age > bestAge)) { best = i; bestScore = score; bestAge = age; }
    }
    if (best < 0) return -1;
    // a silent band is always left (the silence is the evidence); a band with some traffic needs a clearly better one
    const double hereScore = here + c_.hintWeight * std::min(cb.hint, c_.hintMax);
    if (here > 0.05 && bestScore < hereScore + c_.margin) { snprintf(s, sizeof s, "%s: %.1f PDU/min, no better band known", hfdlBands()[(size_t)cur_].name, here); status_ = s; enteredSec = now; return -1; }
    snprintf(s, sizeof s, "%s: %s, moving to %s", hfdlBands()[(size_t)cur_].name, here > 0.05 ? "weak" : "silent", hfdlBands()[(size_t)best].name);
    status_ = s;
    pending_ = best; pendingSec_ = now;
    return best;
}

// ---------------------------------------------------------------- busiest band (survey, stay, survey again)

void HfdlBusiestBand::reset() {
    phase_ = Phase::Idle; sc_.clear(); cand_.clear(); surveyed_.clear(); cur_ = -1; best_ = -1; pending_ = -1; pendingSec_ = 0;
    stayStartSec_ = 0; hintKhz_.clear(); hintSec_ = -1e18; w_ = Win(); status_.clear(); asked_ = -1;
}

const HfdlBusiestBand::Score& HfdlBusiestBand::score(int band) const {
    static const Score none;
    return band >= 0 && (size_t)band < sc_.size() ? sc_[(size_t)band] : none;
}

double HfdlBusiestBand::scoreOf(const Score& s) const {
    return c_.wPdu * s.perMin + c_.wAircraft * s.aircraft + c_.wStation * s.stations + (s.perMin > 0 ? c_.wSnr * std::max(0.0, s.snrDb) : 0.0);
}

namespace {
std::vector<std::pair<uint32_t, double>> baseOf(const std::vector<HfdlBusiestBand::Heard>& v) {
    std::vector<std::pair<uint32_t, double>> b;
    b.reserve(v.size());
    for (const auto& h : v) b.push_back({h.key, h.lastSec});
    std::sort(b.begin(), b.end());
    return b;
}
// keys heard since the base was taken: new keys, or a later last-heard time
void heardSince(const std::vector<std::pair<uint32_t, double>>& base, const std::vector<HfdlBusiestBand::Heard>& now, std::vector<uint32_t>& out) {
    for (const auto& h : now) {
        auto it = std::lower_bound(base.begin(), base.end(), std::make_pair(h.key, -1e300));
        const bool changed = it == base.end() || it->first != h.key || h.lastSec > it->second;
        if (changed && std::find(out.begin(), out.end(), h.key) == out.end()) out.push_back(h.key);
    }
}
}  // namespace

void HfdlBusiestBand::startWindow(const Obs& o) {
    w_ = Win();
    w_.startSec = w_.lastSec = o.nowSec;
    w_.startPdus = w_.lastPdus = o.goodPdus;
    w_.acBase = baseOf(o.aircraft);
    w_.gsBase = baseOf(o.stations);
}

void HfdlBusiestBand::feedWindow(const Obs& o) {
    if (o.goodPdus < w_.lastPdus) { w_.startPdus = o.goodPdus; w_.lastPdus = o.goodPdus; }   // the counters restarted
    if (o.goodPdus > w_.lastPdus && std::isfinite(o.snrDb)) { w_.snrSum += o.snrDb; w_.snrN++; }
    w_.lastPdus = o.goodPdus;
    w_.lastSec = o.nowSec;
    heardSince(w_.acBase, o.aircraft, w_.ac);
    heardSince(w_.gsBase, o.stations, w_.gs);
}

HfdlBusiestBand::Score HfdlBusiestBand::closeWindow(double nowSec) const {
    Score s;
    s.measured = true;
    s.atSec = nowSec;
    const double dt = std::max(1.0, nowSec - w_.startSec);
    s.perMin = (double)(w_.lastPdus - w_.startPdus) * 60.0 / dt;
    s.aircraft = (int)w_.ac.size();
    s.stations = (int)w_.gs.size();
    s.snrDb = w_.snrN ? w_.snrSum / w_.snrN : 0.0;
    s.score = scoreOf(s);
    return s;
}

std::vector<int> HfdlBusiestBand::makeCandidates(int current) const {
    const auto& B = hfdlBands();
    const int n = (int)B.size();
    std::vector<double> w((size_t)n, 0.0);
    for (size_t r = 0; r < hintKhz_.size(); r++) { const int bi = hfdlBandOfKhz(hintKhz_[r]); if (bi >= 0) w[(size_t)bi] += 1.0 / (1.0 + 0.15 * (double)r); }   // the nearest stations' frequencies weigh most
    std::vector<int> out;
    if (current >= 0 && current < n) out.push_back(current);   // where the radio is: measured first (no retune)
    std::vector<int> hinted;
    for (int i = 0; i < n; i++) if (w[(size_t)i] > 0 && i != current) hinted.push_back(i);
    std::stable_sort(hinted.begin(), hinted.end(), [&](int a, int b) { return w[(size_t)a] > w[(size_t)b]; });
    out.insert(out.end(), hinted.begin(), hinted.end());
    if (hinted.empty() || !c_.onlyHinted)
        for (int i = 0; i < n; i++) if (B[(size_t)i].common && std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
    return out;
}

int HfdlBusiestBand::nextSurveyBand() const {
    for (int b : cand_) if (b != cur_ && (size_t)b < surveyed_.size() && !surveyed_[(size_t)b]) return b;
    return -1;
}

int HfdlBusiestBand::update(const Obs& o) {
    const auto& B = hfdlBands();
    const int n = (int)B.size();
    if ((int)sc_.size() != n) { sc_.assign((size_t)n, Score()); surveyed_.assign((size_t)n, false); }
    const double now = o.nowSec;
    if (!o.hintKhz.empty()) { hintKhz_ = o.hintKhz; hintSec_ = now; }
    else if (!hintKhz_.empty() && now - hintSec_ > c_.hintAgeSec) hintKhz_.clear();
    char s[160];
    auto name = [&](int b) { return B[(size_t)b].name; };
    auto hop = [&](int b, const char* whyText) {
        const std::string why = whyText;   // may be s itself
        pending_ = b; pendingSec_ = now; asked_ = b;
        snprintf(s, sizeof s, "%s, going to %s", why.c_str(), name(b));
        status_ = s;
        return b;
    };
    // a hop asked for and not yet arrived
    if (pending_ >= 0 && o.band != pending_ && now - pendingSec_ < c_.retuneSec) return -1;
    if (pending_ >= 0) {
        pending_ = -1;
        if (o.band == cur_ && o.band >= 0 && phase_ != Phase::Idle) { asked_ = -1; startWindow(o); status_ = std::string(name(cur_)) + ": the radio did not retune, measuring again"; return -1; }
    }
    if (o.band < 0 || o.band >= n) { phase_ = Phase::Idle; cur_ = -1; asked_ = -1; status_ = "not on an HFDL band"; return -1; }

    if (phase_ == Phase::Idle || o.band != cur_) {   // arrived
        const bool ours = o.band == asked_ && phase_ != Phase::Idle;
        asked_ = -1;
        cur_ = o.band;
        if (phase_ == Phase::Idle) {
            phase_ = Phase::Survey;
            cand_ = makeCandidates(cur_);
            surveyed_.assign((size_t)n, false);
        } else if (!ours) {   // the user tuned to another HFDL band: stay there
            phase_ = Phase::Stay;
            stayStartSec_ = now;
        }
        startWindow(o);
        snprintf(s, sizeof s, "%s: %s", name(cur_), phase_ == Phase::Survey ? "measuring" : "staying");
        status_ = s;
        return -1;
    }
    feedWindow(o);
    const double el = now - w_.startSec;
    if (el < c_.dwellSec) {
        if (phase_ == Phase::Survey) {
            int done = 0; for (int b : cand_) done += (size_t)b < surveyed_.size() && surveyed_[(size_t)b];
            snprintf(s, sizeof s, "Survey %d of %zu: %s, %d PDU, %zu aircraft, %.0f s left", done + 1, cand_.size(), name(cur_),
                     (int)(w_.lastPdus - w_.startPdus), w_.ac.size(), c_.dwellSec - el);
        } else {
            const Score& k = sc_[(size_t)cur_];
            if (k.measured) snprintf(s, sizeof s, "%s: busiest (score %.0f: %.1f PDU/min, %d aircraft, %d stations), next survey in %.0f min", name(cur_), k.score,
                                     k.perMin, k.aircraft, k.stations, std::max(0.0, (c_.rescanSec - (now - stayStartSec_)) / 60.0));
            else snprintf(s, sizeof s, "%s: staying, measuring", name(cur_));
        }
        status_ = s;
        return -1;
    }
    // a window is complete
    sc_[(size_t)cur_] = closeWindow(now);
    const Score& here = sc_[(size_t)cur_];
    auto beginSurvey = [&]() {
        phase_ = Phase::Survey;
        cand_ = makeCandidates(cur_);
        surveyed_.assign((size_t)n, false);
        surveyed_[(size_t)cur_] = true;   // just measured
    };
    if (phase_ == Phase::Stay) {
        const bool silent = here.score <= 0;
        if (silent || now - stayStartSec_ >= c_.rescanSec) {
            beginSurvey();
            const int nx = nextSurveyBand();
            if (nx >= 0) return hop(nx, silent ? "Nothing heard for a while: surveying the bands" : "Surveying the bands again");
            phase_ = Phase::Stay; stayStartSec_ = now;   // no other band to try
        }
        startWindow(o);
        return -1;
    }
    // survey
    surveyed_[(size_t)cur_] = true;
    const int nx = nextSurveyBand();
    if (nx >= 0) { snprintf(s, sizeof s, "%s scored %.0f", name(cur_), here.score); return hop(nx, s); }
    // the survey is done: the best band of this survey (the current one wins a tie: no retune)
    int best = cur_; double bs = here.score;
    for (int b : cand_) if ((size_t)b < surveyed_.size() && surveyed_[(size_t)b] && sc_[(size_t)b].measured && sc_[(size_t)b].score > bs + 1e-9) { best = b; bs = sc_[(size_t)b].score; }
    best_ = best;
    if (bs <= 0) {   // every band silent: survey again (the bands open and close with the time of day)
        beginSurvey();
        const int again = nextSurveyBand();
        if (again >= 0) return hop(again, "Every band was silent, surveying again");
        startWindow(o);
        status_ = std::string(name(cur_)) + ": silent, the only candidate";
        return -1;
    }
    phase_ = Phase::Stay;
    stayStartSec_ = now;
    if (best != cur_) { snprintf(s, sizeof s, "Survey done: %s is busiest (score %.0f)", name(best), bs); return hop(best, s); }
    startWindow(o);
    snprintf(s, sizeof s, "Survey done: %s is busiest (score %.0f), staying", name(cur_), bs);
    status_ = s;
    return -1;
}

}  // namespace dect2
