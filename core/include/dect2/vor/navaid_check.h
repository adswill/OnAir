// Navaid signal check (3.9, docs/SMART5-avia-3.9.md): what the VOR / ILS receiver measures, averaged over the last windowSec, against
// the signal-in-space limits of ICAO Annex 10 Volume I.
//
// LIMITS FROM MEMORY of Annex 10 Vol I (3.1.3, 3.1.5, 3.1.7, 3.3.5): the Annex is not freely available and was not checked here.
//   VOR: 30 Hz AM 28-32 %, 9960 Hz subcarrier 28-32 %, subcarrier 9960 Hz +-1 %, FM deviation ratio 16 +-1 (480 +-30 Hz),
//        identification depth up to 10 %. (The 30 Hz frequency is not checked: the receiver does not measure it.)
//   Localizer: 90 and 150 Hz 18-22 % each on the course line, SDM 36-44 % off it; ident 5-15 %.
//   Glide path: 90 and 150 Hz 37.5-42.5 % each on the path, SDM 75-85 % off it.
//   Marker: 91-99 %.
// A receiver measurement is not a flight check: the AGC, the filters and multipath move the figures a little, so a value beyond a limit
// by less than `marginPct` is "marginal", not "out". Pure logic, no clock.
#pragma once
#include "vor_tel.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace dect2 {

struct NavaidCheckItem {
    std::string name;            // "30 Hz AM depth"
    double value = 0, lo = 0, hi = 0;
    std::string unit;            // "%", "Hz"
    int verdict = 0;             // 0 within, 1 marginal, 2 out
};

struct NavaidCheck {
    NavFunction function = NavFunction::None;
    int samples = 0;
    std::vector<NavaidCheckItem> items;
    int worst = 0;               // the worst verdict
    std::string summary;
};

class NavaidChecker {
public:
    double windowSec = 20;
    double marginPct = 2;        // percentage points (or percent of the nominal for frequencies) of tolerance for the measurement
    void add(const VorTelemetry& t) {
        if (!t.dataValid) return;
        if (t.function != fn_ || t.dialHz != dial_) { q_.clear(); fn_ = t.function; dial_ = t.dialHz; }   // another beacon starts over
        if (!q_.empty() && t.timeSec <= q_.back().t) { if (t.timeSec < q_.back().t - 1) q_.clear(); else return; }
        S s; s.t = t.timeSec; s.am30 = t.am30; s.sub = t.sub9960; s.dev = t.fmDevHz; s.subHz = t.subcarrierHz; s.t30 = t.tone30Hz;
        s.m90 = t.m90; s.m150 = t.m150; s.ddm = t.ddm; s.ident = t.identDepth; s.identKey = t.identKey; s.marker = t.markerDepth; s.markerOn = t.markerOn;
        q_.push_back(s);
        while (!q_.empty() && t.timeSec - q_.front().t > windowSec) q_.pop_front();
    }
    void clear() { q_.clear(); }

    NavaidCheck result() const {
        NavaidCheck r; r.function = fn_; r.samples = (int)q_.size();
        if (q_.size() < 8) { r.summary = "measuring"; return r; }
        auto mean = [&](auto f, bool (*use)(const S&) = nullptr) { double s = 0; int n = 0; for (const auto& x : q_) if (!use || use(x)) { s += f(x); n++; } return n ? s / n : NAN; };
        auto item = [&](const char* name, double v, double lo, double hi, const char* unit, double margin) {
            if (!std::isfinite(v)) return;
            NavaidCheckItem it; it.name = name; it.value = v; it.lo = lo; it.hi = hi; it.unit = unit;
            it.verdict = v >= lo && v <= hi ? 0 : (v >= lo - margin && v <= hi + margin ? 1 : 2);
            r.items.push_back(it); r.worst = std::max(r.worst, it.verdict);
        };
        if (fn_ == NavFunction::Vor) {
            item("30 Hz AM depth", 100 * mean([](const S& x) { return (double)x.am30; }), 28, 32, "%", marginPct);
            item("9960 Hz subcarrier depth", 100 * mean([](const S& x) { return (double)x.sub; }), 28, 32, "%", marginPct);
            item("subcarrier frequency", mean([](const S& x) { return (double)x.subHz; }), 9860.4, 10059.6, "Hz", 9960 * marginPct / 100);
            item("FM deviation", mean([](const S& x) { return (double)x.dev; }), 450, 510, "Hz", 480 * marginPct / 100);
            item("identification depth", 100 * mean([](const S& x) { return (double)x.ident; }, [](const S& x) { return x.identKey; }), 0, 10, "%", marginPct);
        } else if (fn_ == NavFunction::Localizer || fn_ == NavFunction::Glideslope) {
            const bool loc = fn_ == NavFunction::Localizer;
            const double ddm = std::fabs(mean([](const S& x) { return x.ddm; }));
            if (ddm < (loc ? 0.02 : 0.04)) {   // on (or close to) the course line / path: each tone
                item("90 Hz depth", 100 * mean([](const S& x) { return x.m90; }), loc ? 18 : 37.5, loc ? 22 : 42.5, "%", marginPct);
                item("150 Hz depth", 100 * mean([](const S& x) { return x.m150; }), loc ? 18 : 37.5, loc ? 22 : 42.5, "%", marginPct);
            } else item("SDM", 100 * mean([](const S& x) { return x.m90 + x.m150; }), loc ? 36 : 75, loc ? 44 : 85, "%", marginPct);
            if (loc) item("identification depth", 100 * mean([](const S& x) { return (double)x.ident; }, [](const S& x) { return x.identKey; }), 5, 15, "%", marginPct);
        } else if (fn_ == NavFunction::Marker) {
            item("marker depth", 100 * mean([](const S& x) { return (double)x.marker; }, [](const S& x) { return x.markerOn; }), 91, 99, "%", marginPct);
        }
        int out = 0, marg = 0; for (const auto& it : r.items) { out += it.verdict == 2; marg += it.verdict == 1; }
        char b[160];
        if (r.items.empty()) snprintf(b, sizeof b, "nothing to check");
        else if (!out && !marg) snprintf(b, sizeof b, "all %zu figures within the Annex 10 limits", r.items.size());
        else snprintf(b, sizeof b, "%d out of limits, %d marginal (receiver measurement, not a flight check)", out, marg);
        r.summary = b;
        return r;
    }

private:
    struct S { double t = 0; float am30 = 0, sub = 0, dev = 0, subHz = 0, t30 = 0, ident = 0, marker = 0; bool identKey = false, markerOn = false; double m90 = 0, m150 = 0, ddm = 0; };
    std::deque<S> q_;
    NavFunction fn_ = NavFunction::None;
    double dial_ = 0;
};

} // namespace dect2
