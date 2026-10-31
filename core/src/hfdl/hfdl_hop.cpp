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

}  // namespace dect2
