#include "dect2/source/gain_cal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::ag {

void GainCalibrator::begin(const RadioProfile& r, const std::string& serial, double now, double freqHz) {
    r_ = r;
    cal_ = GainCal();
    cal_.serial = serial; cal_.radioId = r.id; cal_.freqHz = freqHz;
    done_ = false; started_ = false; clipped_ = false; idx_ = -1; stage_ = -1; text_.clear();
    floors_.clear(); margins_.clear(); nominal_.clear();
    for (size_t i = 0; i < r.stages.size(); i++) if (r.stages[i].settable && !r.stages[i].userOnly) { stage_ = (int)i; break; }
    if (stage_ < 0 || r.settableCount() != 1) { done_ = true; text_ = "this radio has no single overall gain control to calibrate"; return; }
    nominal_ = r.stages[(size_t)stage_].steps;
    cal_.nominal = nominal_;
    settle_ = r.settleSec + 0.3; dwell_ = 0.7;
    changedAt_ = now;
}

bool GainCalibrator::tick(const AgInput& in, AgCommand& out) {
    if (done_) return false;
    auto go = [&](int i) {
        StagePlan p;
        p.db.assign(r_.stages.size(), 0.0);
        for (size_t k = 0; k < r_.stages.size(); k++) p.db[k] = r_.stages[k].settable ? r_.stages[k].minDb() : 0.0;
        p.db[(size_t)stage_] = nominal_[(size_t)i];
        p.totalDb = nominal_[(size_t)i];
        p.ampOn = false;
        out.plan = p; out.why = "calibration step";
        idx_ = i; changedAt_ = in.now; sumFloor_ = sumMargin_ = 0; n_ = 0;
    };
    if (!started_) { started_ = true; go(0); return true; }
    if (in.now < changedAt_ + settle_) return false;
    if (in.spectrum && !in.spectrum->empty()) {
        const FloorEstimate f = estimateFloor(*in.spectrum, in.rateHz, in.chanBwHz, in.offsetHz, r_.adcBits);
        if (f.valid) { sumFloor_ += f.floorDbfs; sumMargin_ += f.adcMarginDb; n_++; }
    }
    if (in.adc.clipFraction > 2e-3) clipped_ = true;
    if (in.now < changedAt_ + settle_ + dwell_) return false;
    floors_.push_back(n_ ? sumFloor_ / n_ : -200.0);
    margins_.push_back(n_ ? sumMargin_ / n_ : -100.0);
    // a clipping ADC: the steps above are not measurable, they keep their nominal difference
    if (clipped_ || idx_ + 1 >= (int)nominal_.size()) { finish(); return false; }
    go(idx_ + 1);
    return true;
}

void GainCalibrator::finish() {
    done_ = true;
    const size_t n = nominal_.size(), m = floors_.size();
    cal_.floorDbfs = floors_;
    cal_.actual.assign(n, 0.0);
    cal_.sure.assign(n, 0);
    cal_.floorDbfs.resize(n, -200.0);
    if (n == 0) return;
    cal_.actual[0] = nominal_[0];
    cal_.sure[0] = m > 0 && margins_[0] >= 6;
    for (size_t i = 1; i < n; i++) {
        const double nomStep = nominal_[i] - nominal_[i - 1];
        // a step is trusted when the floor sits well above the ADC's own noise at both ends (the quantisation noise does not follow the gain)
        const bool ok = i < m && margins_[i] >= 6 && margins_[i - 1] >= 6;
        double step = nomStep;
        if (ok) step = floors_[i] - floors_[i - 1];
        cal_.actual[i] = cal_.actual[i - 1] + step;
        cal_.sure[i] = ok;
        if (ok) cal_.worstStepErrDb = std::max(cal_.worstStepErrDb, std::fabs(step - nomStep));
    }
    // ----- flags
    size_t sureN = 0, wrong = 0; double firstSure = -1, lastSure = -1, nomSpan = 0;
    for (size_t i = 0; i < n; i++) if (cal_.sure[i]) { sureN++; if (firstSure < 0) firstSure = (double)i; lastSure = (double)i; }
    if (sureN >= 2) {
        const size_t a = (size_t)firstSure, b = (size_t)lastSure;
        nomSpan = nominal_[b] - nominal_[a];
        const double real = cal_.actual[b] - cal_.actual[a];
        for (size_t i = a + 1; i <= b; i++) if (cal_.sure[i] && cal_.sure[i - 1] && (cal_.actual[i] - cal_.actual[i - 1]) < -1.0 && nominal_[i] > nominal_[i - 1]) wrong++;
        char buf[200];
        if (nomSpan > 6 && real < 0.3 * nomSpan) {
            snprintf(buf, sizeof buf, "the gain control has almost no effect: %.0f dB of settings moved the noise floor %.1f dB (a dead or stuck tuner, or the tuner's own AGC is on)", nomSpan, real);
            cal_.flags.push_back(buf);
        } else if (std::fabs(real - nomSpan) > 0.25 * nomSpan + 3) {
            snprintf(buf, sizeof buf, "the gain range is %.0f dB, the table says %.0f dB", real, nomSpan);
            cal_.flags.push_back(buf);
        }
        if (wrong > 0) { snprintf(buf, sizeof buf, "%d gain steps lowered the noise instead of raising it", (int)wrong); cal_.flags.push_back(buf); }
        if (cal_.worstStepErrDb > 3.5) { snprintf(buf, sizeof buf, "one step is %.1f dB away from the table", cal_.worstStepErrDb); cal_.flags.push_back(buf); }
    } else {
        cal_.flags.push_back("the noise floor was too close to the ADC's own noise to measure the steps: connect an antenna or a noise source and run it again");
    }
    char t[240];
    snprintf(t, sizeof t, "measured %d of %d gain steps, largest difference from the table %.1f dB%s", (int)sureN, (int)n, cal_.worstStepErrDb, cal_.flags.empty() ? "" : "; this unit looks wrong");
    text_ = t;
}

bool applyCalibration(RadioProfile& r, const GainCal& c) {
    if (!c.valid() || c.radioId != r.id) return false;
    if (r.settableCount() != 1) return false;
    for (auto& s : r.stages) {
        if (!s.settable || s.userOnly) continue;
        if (s.steps.size() != c.nominal.size()) return false;
        for (size_t i = 0; i < s.steps.size(); i++) if (std::fabs(s.steps[i] - c.nominal[i]) > 1e-6) return false;
        s.actualDb = c.actual;
        return true;
    }
    return false;
}

// ---- store: one line per unit: serial|radio|freq|n|nominal,actual,floor,sure;... ; flags after '#'
std::string GainCalStore::serialise() const {
    std::ostringstream o;
    for (const auto& kv : m_) {
        const GainCal& c = kv.second;
        o << c.serial << '|' << c.radioId << '|' << (long long)c.freqHz << '|' << c.nominal.size() << '|';
        for (size_t i = 0; i < c.nominal.size(); i++) {
            char b[96];
            snprintf(b, sizeof b, "%.3f,%.3f,%.2f,%d;", c.nominal[i], i < c.actual.size() ? c.actual[i] : c.nominal[i], i < c.floorDbfs.size() ? c.floorDbfs[i] : -200.0, i < c.sure.size() ? (int)c.sure[i] : 0);
            o << b;
        }
        o << '|';
        for (const auto& f : c.flags) { std::string g = f; for (char& ch : g) if (ch == '|' || ch == '\n' || ch == '#') ch = ' '; o << g << '#'; }
        o << '\n';
    }
    return o.str();
}

bool GainCalStore::parse(const std::string& s) {
    m_.clear();
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t p = 0;
        for (int k = 0; k < 5; k++) { const size_t q = line.find('|', p); if (q == std::string::npos) { f.push_back(line.substr(p)); p = line.size(); break; } f.push_back(line.substr(p, q - p)); p = q + 1; }
        if (f.size() < 5) { if (f.size() == 4) f.push_back(""); else continue; }
        GainCal c;
        c.serial = f[0]; c.radioId = f[1]; c.freqHz = atof(f[2].c_str());
        const size_t n = (size_t)atol(f[3].c_str());
        std::istringstream st(f[4]);
        std::string it;
        while (std::getline(st, it, ';')) {
            double a = 0, b = 0, fl = 0; int sure = 0;
            if (sscanf(it.c_str(), "%lf,%lf,%lf,%d", &a, &b, &fl, &sure) == 4) { c.nominal.push_back(a); c.actual.push_back(b); c.floorDbfs.push_back(fl); c.sure.push_back((char)sure); }
        }
        std::string rest = p < line.size() ? line.substr(p) : "";
        std::istringstream fs(rest);
        std::string g;
        while (std::getline(fs, g, '#')) if (!g.empty()) c.flags.push_back(g);
        if (c.nominal.size() != n || c.serial.empty()) continue;
        for (size_t i = 1; i < c.nominal.size(); i++) if (c.sure[i]) c.worstStepErrDb = std::max(c.worstStepErrDb, std::fabs((c.actual[i] - c.actual[i - 1]) - (c.nominal[i] - c.nominal[i - 1])));
        m_[c.serial] = c;
    }
    return true;
}

} // namespace dect2::ag
