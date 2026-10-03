#include "dect2/direction.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

const char* compassName(double deg) {
    static const char* n[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    double d = std::fmod(deg, 360.0);
    if (d < 0) d += 360;
    return n[(int)std::floor(d / 22.5 + 0.5) % 16];
}

double angDiff(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180) d -= 360;
    if (d <= -180) d += 360;
    return d;
}

double occupancyDb(const std::vector<float>& dbfs, double fsMhz, double bwMhz) {
    if (dbfs.empty() || fsMhz <= 0) return 0;
    const size_t n = dbfs.size();
    double inb = 0; int nIn = 0;
    std::vector<float> floorBins;
    for (size_t b = 0; b < n; b++) {
        const double f = ((double)b / n - 0.5) * fsMhz, a = std::fabs(f);
        if (a < bwMhz * 0.5 * 0.9) { inb += std::pow(10.0, dbfs[b] / 10.0); nIn++; }
        else if (a > bwMhz * 0.5 * 1.12 && a < fsMhz * 0.5 * 0.94) floorBins.push_back(dbfs[b]);
    }
    if (!nIn || floorBins.empty()) return 0;
    std::sort(floorBins.begin(), floorBins.end());
    return 10 * std::log10(inb / nIn) - floorBins[floorBins.size() / 2];
}

std::string DirectionFinder::targetLabel() const {
    char b[32];
    if (kind_ == AntennaKind::Omni) { snprintf(b, sizeof b, "Spot %d", (int)target_); return b; }
    return compassName(target_);
}

std::string DirectionFinder::instruction() const {
    char b[240];
    if (state_ == State::Done) return rec_.text;
    if (state_ == State::Idle) return "Choose your antenna type and press Start.";
    if (kind_ == AntennaKind::Omni) {
        snprintf(b, sizeof b, "Put the antenna at spot %d (a different window, height or room), keep it still, then press Measure.", (int)target_);
        return b;
    }
    const char* what = kind_ == AntennaKind::Dipole ? "Turn the antenna so its broad side faces" : "Point the front of the antenna towards";
    snprintf(b, sizeof b, "%s %s (%.0f\xC2\xB0). Hold it steady, then press Measure.", what, compassName(target_), target_);
    return b;
}

void DirectionFinder::start(AntennaKind kind) {
    kind_ = kind;
    results_.clear();
    rec_ = DirRecommendation();
    queue_.clear();
    acc_ = Acc();
    spotCount_ = 0;
    diagonalsAdded_ = false;
    if (kind == AntennaKind::Omni) { target_ = 1; spotCount_ = 1; }
    else { queue_ = {90, 180, 270}; target_ = 0; }
    state_ = State::WaitConfirm;
}

void DirectionFinder::stop() { state_ = State::Idle; results_.clear(); rec_ = DirRecommendation(); queue_.clear(); }

void DirectionFinder::confirm(double now) {
    if (state_ != State::WaitConfirm) return;
    acc_ = Acc();
    t0_ = now;
    state_ = State::Measuring;
}

double DirectionFinder::progress(double now) const {
    if (state_ == State::Measuring) return std::min(1.0, std::max(0.0, (now - t0_) / cfg_.measureSec));
    return state_ == State::Done ? 1.0 : 0.0;
}

void DirectionFinder::addSample(double now, const DirSample& s) {
    if (state_ != State::Measuring) return;
    const double el = now - t0_;
    if (el >= cfg_.settleSec) {
        acc_.n++;
        acc_.q += s.qualityPct; acc_.occ += s.occupancyDb; acc_.loss += s.lossPct;
        if (s.locked) { acc_.nLocked++; acc_.snr += s.snrDb; }
        acc_.clip = std::max(acc_.clip, s.clipFraction);
        if (s.multipath != MultipathLevel::Unknown && (int)s.multipath > (int)acc_.mp) acc_.mp = s.multipath;
    }
    if (el >= cfg_.measureSec) finishMeasurement();
}

void DirectionFinder::skip() {
    if (state_ == State::Idle || state_ == State::Done) return;
    state_ = State::WaitConfirm;
    planNext();
}

void DirectionFinder::finishNow() {
    if (state_ == State::Idle || state_ == State::Done) return;
    conclude();
}

void DirectionFinder::finishMeasurement() {
    DirResult r;
    r.heading = target_;
    r.label = targetLabel();
    r.samples = acc_.n;
    if (acc_.n > 0) {
        r.qualityPct = acc_.q / acc_.n;
        r.occupancyDb = acc_.occ / acc_.n;
        r.lossPct = acc_.loss / acc_.n;
        r.lockFraction = (double)acc_.nLocked / acc_.n;
        r.snrDb = acc_.nLocked ? acc_.snr / acc_.nLocked : 0;
    }
    r.multipath = acc_.mp;
    r.overload = acc_.clip > 0.005;
    // quality decides; the spectrum (in-band power over the noise floor) ranks directions that do not lock at all
    const double occScore = std::min(100.0, std::max(0.0, r.occupancyDb * 4.0));
    double score = 0.88 * r.qualityPct + 0.12 * occScore;
    if (r.lockFraction > 0.3) {
        if (r.multipath == MultipathLevel::Mild) score -= 2;
        else if (r.multipath == MultipathLevel::Likely) score -= 5;
        else if (r.multipath == MultipathLevel::Severe) score -= 12;
    }
    if (r.overload) score *= 0.85;
    r.score = std::min(100.0, std::max(0.0, score));
    results_.push_back(r);
    state_ = State::WaitConfirm;
    planNext();
}

void DirectionFinder::planNext() {
    if (kind_ == AntennaKind::Omni) {
        // places: the user decides when to stop (at least two); eight at most
        if (spotCount_ >= 8 && (int)results_.size() >= spotCount_) { conclude(); return; }
        if ((int)results_.size() >= spotCount_) spotCount_ = (int)results_.size() + 1;
        target_ = spotCount_;
        return;
    }
    if ((int)results_.size() >= cfg_.maxMeasurements) { conclude(); return; }
    // the planned cardinal directions come first
    auto measured = [&](double h) { for (auto& r : results_) if (std::fabs(angDiff(r.heading, h)) < 1.0) return true; return false; };
    while (!queue_.empty()) {
        const double h = queue_.front();
        queue_.erase(queue_.begin());
        if (!measured(h)) { target_ = h; return; }
    }
    if (results_.size() < 3) { conclude(); return; }
    int b = 0;
    double mn = 1e9, mx = -1e9;
    for (size_t i = 0; i < results_.size(); i++) { if (results_[i].score > results_[b].score) b = (int)i; mn = std::min(mn, results_[i].score); mx = std::max(mx, results_[i].score); }
    double omn = 1e9, omx = -1e9;
    for (auto& r : results_) { omn = std::min(omn, r.occupancyDb); omx = std::max(omx, r.occupancyDb); }
    if (mx - mn < cfg_.flatSpread && omx - omn < 2.5) {
        // About the same everywhere. With a strong signal that means the direction does not matter. With a weak one a narrow
        // beam may simply have fallen between the first four directions: try the diagonals before giving up.
        if (mx >= 60 || diagonalsAdded_) { conclude(); return; }
        diagonalsAdded_ = true;
        queue_ = {45, 135, 225, 315};
        while (!queue_.empty()) {
            const double h = queue_.front();
            queue_.erase(queue_.begin());
            if (!measured(h)) { target_ = h; if (!queue_.empty()) {} return; }
        }
        conclude();
        return;
    }
    const double hb = results_[b].heading;
    // nearest measured directions on either side of the best one
    double cw = 360, ccw = 360;
    for (size_t i = 0; i < results_.size(); i++) {
        if ((int)i == b) continue;
        const double d = angDiff(results_[i].heading, hb);
        if (d > 0) cw = std::min(cw, d); else ccw = std::min(ccw, -d);
    }
    // first the 45 degree neighbours (towards the better side), then 22.5 degree ones, on both sides of the best direction
    const double want[2] = {45.0, 22.5};
    for (double step : want) {
        const double sides[2] = {cw, ccw};
        // try the side with the higher neighbour score first
        auto neighbourScore = [&](double dir) {
            double best = -1;
            for (auto& r : results_) { const double d = angDiff(r.heading, hb); if (d * dir > 0 && std::fabs(d) < 100) best = std::max(best, r.score); }
            return best;
        };
        const double firstDir = neighbourScore(+1) >= neighbourScore(-1) ? +1 : -1;
        for (int k = 0; k < 2; k++) {
            const double dir = k == 0 ? firstDir : -firstDir;
            const double gap = dir > 0 ? sides[0] : sides[1];
            if (gap > step + 1.0) { target_ = std::fmod(hb + dir * step + 720.0, 360.0); return; }
        }
    }
    if (kind_ == AntennaKind::Dipole) {   // confirm that the opposite end is as good: it tells a figure-8 from a one-sided pattern
        bool opp = false;
        for (auto& r : results_) if (std::fabs(std::fabs(angDiff(r.heading, hb)) - 180.0) < 25.0) opp = true;
        if (!opp) { target_ = std::fmod(hb + 180.0, 360.0); return; }
    }
    conclude();
}

void DirectionFinder::conclude() {
    state_ = State::Done;
    rec_ = DirRecommendation();
    if (results_.empty()) { rec_.text = "No measurements were taken."; return; }
    int b = 0;
    double mn = 1e9, mx = -1e9;
    for (size_t i = 0; i < results_.size(); i++) { if (results_[i].score > results_[b].score) b = (int)i; mn = std::min(mn, results_[i].score); mx = std::max(mx, results_[i].score); }
    double omn = 1e9, omx = -1e9;
    for (auto& r : results_) { omn = std::min(omn, r.occupancyDb); omx = std::max(omx, r.occupancyDb); }
    rec_.valid = true;
    rec_.bestIndex = b;
    char t[320];
    if (kind_ == AntennaKind::Omni) {
        rec_.heading = results_[b].heading;
        rec_.label = results_[b].label;
        rec_.confidence = std::min(1.0, (mx - mn) / 30.0);
        snprintf(t, sizeof t, "%s is the best place (score %.0f of 100%s). The others scored %.0f to %.0f.", rec_.label.c_str(), results_[b].score, results_[b].lockFraction > 0.5 ? "" : ", but the receiver did not lock there", mn, mx);
        if (mx - mn < cfg_.flatSpread) snprintf(t, sizeof t, "All the places are about equally good (scores %.0f to %.0f). Choose by convenience.", mn, mx);
        rec_.text = t;
        rec_.flat = mx - mn < cfg_.flatSpread && omx - omn < 2.5;
        return;
    }
    // interpolate the peak with a parabola through the best measurement and its nearest neighbours on either side
    const double hb = results_[b].heading;
    double bestHeading = hb;
    int cwI = -1, ccwI = -1;
    double cwD = 360, ccwD = 360;
    for (size_t i = 0; i < results_.size(); i++) {
        if ((int)i == b) continue;
        const double d = angDiff(results_[i].heading, hb);
        if (d > 0 && d < cwD) { cwD = d; cwI = (int)i; }
        if (d < 0 && -d < ccwD) { ccwD = -d; ccwI = (int)i; }
    }
    if (cwI >= 0 && ccwI >= 0 && cwD <= 100 && ccwD <= 100) {
        const double x1 = -ccwD, x2 = 0, x3 = cwD, y1 = results_[ccwI].score, y2 = results_[b].score, y3 = results_[cwI].score;
        const double den = (x1 - x2) * (x1 - x3) * (x2 - x3);
        const double A = (x3 * (y2 - y1) + x2 * (y1 - y3) + x1 * (y3 - y2)) / den;
        const double B = (x3 * x3 * (y1 - y2) + x2 * x2 * (y3 - y1) + x1 * x1 * (y2 - y3)) / den;
        if (A < -1e-9) {
            const double xv = -B / (2 * A);
            bestHeading = hb + std::max(-ccwD * 0.5, std::min(cwD * 0.5, xv));
        } else {                                                    // no peak between the neighbours: weight them
            const double w1 = std::max(0.0, y1 - mn), w2 = std::max(0.0, y2 - mn), w3 = std::max(0.0, y3 - mn);
            if (w1 + w2 + w3 > 0) bestHeading = hb + (x1 * w1 + x2 * w2 + x3 * w3) / (w1 + w2 + w3);
        }
    }
    bestHeading = std::fmod(bestHeading + 720.0, 360.0);
    rec_.heading = bestHeading;
    rec_.label = compassName(bestHeading);
    rec_.flat = mx - mn < cfg_.flatSpread && omx - omn < 2.5;
    // a dipole or figure-8 antenna is as good facing the opposite way
    double oppScore = -1;
    for (auto& r : results_) if (std::fabs(std::fabs(angDiff(r.heading, hb)) - 180.0) < 25.0) oppScore = std::max(oppScore, r.score);
    rec_.symmetric = kind_ == AntennaKind::Dipole && oppScore >= 0 && oppScore > results_[b].score - 10;
    // confidence: how far the peak stands out and how well it is bracketed
    double others = 0; int no = 0;
    for (size_t i = 0; i < results_.size(); i++) if ((int)i != b && std::fabs(angDiff(results_[i].heading, hb)) > 60) { others += results_[i].score; no++; }
    const double prom = no ? results_[b].score - others / no : mx - mn;
    rec_.confidence = std::min(1.0, std::max(0.0, prom / 30.0)) * (cwI >= 0 && ccwI >= 0 ? 1.0 : 0.6);
    const bool locked = results_[b].lockFraction > 0.5;
    if (rec_.flat) {
        snprintf(t, sizeof t, "The signal is about the same in every direction (scores %.0f to %.0f), so turning the antenna hardly matters here. Placement (height, window) is more likely to help.", mn, mx);
    } else if (rec_.symmetric) {
        snprintf(t, sizeof t, "Aim the antenna at about %s (%.0f\xC2\xB0), or the opposite way: a dipole works the same from both sides. Best score %.0f of 100.%s", rec_.label.c_str(), bestHeading, results_[b].score, locked ? "" : " The receiver could not lock at the best direction: the signal is weak.");
    } else {
        snprintf(t, sizeof t, "Point the antenna at about %s (%.0f\xC2\xB0). Best score %.0f of 100 (SNR %.1f dB, %.1f%% lost blocks).%s", rec_.label.c_str(), bestHeading, results_[b].score, results_[b].snrDb, results_[b].lossPct, locked ? "" : " The receiver could not lock at the best direction: the signal is weak.");
    }
    rec_.text = t;
}

} // namespace dect2
