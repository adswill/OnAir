// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// My station: see station.h.
#include "dect2/station/station.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace dect2::station {

namespace {

constexpr double kBandMargin = 0.10;      // an antenna still works this far outside its stated band (Partial is for further out)
constexpr double kUsable = 0.8;           // share of the span that is usable (the edges roll off)

std::string clean(std::string s) {        // a field of the text format: no tabs or line ends
    for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> f;
    size_t p = 0;
    for (;;) {
        const size_t q = line.find('\t', p);
        if (q == std::string::npos) { f.push_back(line.substr(p)); return f; }
        f.push_back(line.substr(p, q - p));
        p = q + 1;
    }
}

double num(const std::vector<std::string>& f, size_t i) { return i < f.size() ? std::strtod(f[i].c_str(), nullptr) : 0.0; }

bool radioTunes(const Radio& r, double hz) { return hz <= 0 || r.maxHz <= 0 || (hz >= r.minHz && hz <= r.maxHz); }

bool antennaCovers(const Antenna& a, double hz, double margin) {
    if (hz <= 0 || a.hiHz <= 0) return true;
    return hz >= a.loHz * (1 - margin) && hz <= a.hiHz * (1 + margin);
}

// the need of an antenna kind, in the order it is reported
void kindMissing(uint8_t need, uint8_t have, std::vector<Missing>& m) {
    if ((need & ADish) && !(have & ADish)) m.push_back(Missing::Dish);
    if ((need & ACable) && !(have & ACable)) m.push_back(Missing::CableFeed);
    if ((need & AActive) && !(have & AActive)) m.push_back(Missing::ActiveAntenna);
}

struct PairEval {
    int radio = -1, ant = -1;
    Level level = Level::Cannot;
    std::vector<Missing> missing;
    double score = -1e18;
};

// One radio with one antenna (ant = -1: the station lists no antenna for it)
PairEval evalPair(const Station& s, const ModeNeed& n, int ri, int ai) {
    const Radio& r = s.radios[(size_t)ri];
    PairEval e; e.radio = ri; e.ant = ai;
    const bool feedNeed = (n.flags & (ADish | ACable)) != 0;
    const Antenna* a = ai >= 0 ? &s.antennas[(size_t)ai] : nullptr;
    const bool feedAnt = a && (a->flags & (ADish | ACable));
    bool hardFail = false;
    // the radio
    if (!radioTunes(r, n.freqHz)) { e.missing.push_back(Missing::RadioRange); hardFail = true; }
    if (n.minRateHz > 0 && r.maxRateHz > 0 && r.maxRateHz + 1 < n.minRateHz) { e.missing.push_back(Missing::RadioRate); hardFail = true; }
    double score = 0;
    // the antenna
    if (s.antennas.empty()) {
        // nothing entered: the antenna is not judged, except for what only special hardware can do
        if (n.flags & ADish) e.missing.push_back(Missing::Dish);
        if (n.flags & ACable) e.missing.push_back(Missing::CableFeed);
        if (n.flags & AActive) e.missing.push_back(Missing::ActiveAntenna);
    } else if (!a) {
        kindMissing(n.flags, 0, e.missing);
        if (!feedNeed) e.missing.push_back(Missing::AntennaBand);
    } else {
        kindMissing(n.flags, a->flags, e.missing);
        if (feedAnt != feedNeed) {
            // a dish or a cable feed is no use for a terrestrial mode, nor the other way round
            if (feedAnt && !feedNeed) e.missing.push_back(Missing::AntennaBand);
        } else if (!feedNeed && !antennaCovers(*a, n.freqHz, kBandMargin)) {
            e.missing.push_back(Missing::AntennaBand);
        }
        if ((a->flags & AActive) && (n.flags & AActive) && !r.biasTee) e.missing.push_back(Missing::NoAntennaPower);   // it may have its own supply: a note
        if (!a->radioId.empty() && a->radioId == r.id) score += 40;   // already connected
        else if (!a->radioId.empty()) score -= 25;                    // would have to be moved
        if (n.freqHz > 0 && a->hiHz > a->loHz && a->loHz > 0 && !feedAnt) score += 20.0 / (1.0 + std::log2(a->hiHz / a->loHz));   // narrower is more tuned
    }
    e.level = hardFail ? Level::Cannot : e.missing.empty() ? Level::Ready : Level::Partial;
    // a radio much wider than the mode needs is left free for the modes that need it
    if (n.minRateHz > 0 && r.maxRateHz > 0) score -= 5.0 * std::log2(std::max(1.0, r.maxRateHz / n.minRateHz));
    if (!r.present) score -= 1000;
    e.score = score - 10.0 * (double)e.missing.size();
    return e;
}

bool better(const PairEval& a, const PairEval& b) {
    if (a.level != b.level) return (int)a.level > (int)b.level;
    return a.score > b.score;
}

PairEval bestForRadio(const Station& s, const ModeNeed& n, int ri) {
    PairEval best = evalPair(s, n, ri, -1);
    bool any = false;
    for (int ai = 0; ai < (int)s.antennas.size(); ai++) {
        const PairEval e = evalPair(s, n, ri, ai);
        if (!any || better(e, best)) { best = e; any = true; }
    }
    return best;
}

Verdict toVerdict(const PairEval& e) {
    Verdict v;
    v.level = e.level; v.radio = e.radio; v.antenna = e.ant; v.missing = e.missing; v.score = e.score;
    return v;
}

bool isBusy(const std::vector<bool>& busy, int i) { return i < (int)busy.size() && busy[(size_t)i]; }

}  // namespace

std::string toText(const Station& s) {
    std::ostringstream o;
    o.precision(15);
    for (const Radio& r : s.radios)
        o << "R\t" << clean(r.id) << '\t' << clean(r.name) << '\t' << r.minHz << '\t' << r.maxHz << '\t' << r.maxRateHz << '\t' << (r.biasTee ? 1 : 0) << '\n';
    for (const Antenna& a : s.antennas)
        o << "A\t" << clean(a.name) << '\t' << a.loHz << '\t' << a.hiHz << '\t' << (int)a.flags << '\t' << clean(a.radioId) << '\n';
    return o.str();
}

Station fromText(const std::string& text) {
    Station s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = split(line);
        if (f[0] == "R" && f.size() >= 3) {
            Radio r;
            r.id = f[1]; r.name = f[2]; r.minHz = num(f, 3); r.maxHz = num(f, 4); r.maxRateHz = num(f, 5); r.biasTee = num(f, 6) != 0;
            r.present = false;   // the app marks the ones it finds
            s.radios.push_back(r);
        } else if (f[0] == "A" && f.size() >= 2) {
            Antenna a;
            a.name = f[1]; a.loHz = num(f, 2); a.hiHz = num(f, 3); a.flags = (uint8_t)num(f, 4);
            if (f.size() > 5) a.radioId = f[5];
            s.antennas.push_back(a);
        }
    }
    return s;
}

const std::vector<AntennaPreset>& antennaPresets() {
    static const std::vector<AntennaPreset> p = {
        {"Telescopic whip (radio's own)", 70e6, 1000e6, 0},
        {"Discone", 25e6, 1300e6, 0},
        {"Long wire (HF)", 0.1e6, 30e6, 0},
        {"Active loop (HF)", 0.01e6, 30e6, AActive},
        {"VHF/UHF collinear", 140e6, 450e6, 0},
        {"Airband dipole", 108e6, 137e6, 0},
        {"Marine VHF", 156e6, 163e6, 0},
        {"1090 MHz collinear", 1030e6, 1100e6, 0},
        {"137 MHz QFH / V-dipole", 130e6, 145e6, 0},
        {"GNSS active patch", 1160e6, 1610e6, AActive},
        {"L-band active patch", 1520e6, 1660e6, AActive},
        {"Dish with LNB or feed", 950e6, 2150e6, ADish},
        {"Cable TV feed", 47e6, 862e6, ACable},
    };
    return p;
}

Verdict assessRadio(const Station& s, const ModeNeed& n, int radio) {
    if (radio < 0 || radio >= (int)s.radios.size()) return Verdict();
    return toVerdict(bestForRadio(s, n, radio));
}

Verdict assess(const Station& s, const ModeNeed& n, const std::vector<bool>& busy) {
    Verdict none;
    if (s.radios.empty()) return none;
    PairEval best, fallback;
    bool haveBest = false, haveFallback = false;
    for (int ri = 0; ri < (int)s.radios.size(); ri++) {
        const PairEval e = bestForRadio(s, n, ri);
        if (!haveFallback || better(e, fallback)) { fallback = e; haveFallback = true; }
        if (isBusy(busy, ri) || !s.radios[(size_t)ri].present) continue;
        if (!haveBest || better(e, best)) { best = e; haveBest = true; }
    }
    if (haveBest) return toVerdict(best);
    // every radio is busy or unplugged: say what the best of them would do, without offering it
    Verdict v = toVerdict(fallback);
    v.radio = -1; v.antenna = -1;
    if (std::find(v.missing.begin(), v.missing.end(), Missing::NoRadioPlugged) == v.missing.end()) v.missing.insert(v.missing.begin(), Missing::NoRadioPlugged);
    v.level = Level::Cannot;
    return v;
}

int pickRadio(const Station& s, const ModeNeed& n, int current, const std::vector<bool>& busy) {
    if (current < 0 || current >= (int)s.radios.size()) return current;
    const Verdict best = assess(s, n, busy);
    if (best.radio < 0 || best.radio == current) return current;
    if (isBusy(busy, current) || !s.radios[(size_t)current].present) return best.radio;
    const PairEval cur = bestForRadio(s, n, current);
    return (int)best.level > (int)cur.level ? best.radio : current;   // only a clearly better radio: same level keeps the user's choice
}

std::vector<int> assignRadios(const Station& s, const std::vector<ModeNeed>& wanted, const std::vector<bool>& busy) {
    std::vector<int> out(wanted.size(), -1);
    const int R = std::min((int)s.radios.size(), 16);
    if (R == 0 || wanted.empty()) return out;
    const int M = (int)wanted.size();
    // value of a mode on a radio: Ready far above Partial far above score, Cannot is not an option; earlier modes get a little more
    std::vector<std::vector<double>> val((size_t)M, std::vector<double>((size_t)R, -1));
    for (int m = 0; m < M; m++)
        for (int r = 0; r < R; r++) {
            if (isBusy(busy, r) || !s.radios[(size_t)r].present) continue;
            const PairEval e = bestForRadio(s, wanted[(size_t)m], r);
            if (e.level == Level::Cannot) continue;
            const double priority = (double)(M - m) * 1e3;   // beats any score difference
            val[(size_t)m][(size_t)r] = (e.level == Level::Ready ? 1e9 : 1e6) + priority + std::clamp(e.score, -400.0, 400.0);
        }
    const int full = 1 << R;
    const double kNone = -std::numeric_limits<double>::infinity();
    std::vector<std::vector<double>> dp((size_t)M + 1, std::vector<double>((size_t)full, kNone));
    std::vector<std::vector<int>> from((size_t)M + 1, std::vector<int>((size_t)full, -2));
    dp[0][0] = 0;
    for (int m = 0; m < M; m++)
        for (int mask = 0; mask < full; mask++) {
            const double d = dp[(size_t)m][(size_t)mask];
            if (d == kNone) continue;
            if (d > dp[(size_t)m + 1][(size_t)mask]) { dp[(size_t)m + 1][(size_t)mask] = d; from[(size_t)m + 1][(size_t)mask] = -1; }   // this mode gets nothing
            for (int r = 0; r < R; r++) {
                if ((mask >> r) & 1) continue;
                const double v = val[(size_t)m][(size_t)r];
                if (v < 0) continue;
                const int nm = mask | (1 << r);
                if (d + v > dp[(size_t)m + 1][(size_t)nm]) { dp[(size_t)m + 1][(size_t)nm] = d + v; from[(size_t)m + 1][(size_t)nm] = r; }
            }
        }
    int bestMask = 0;
    for (int mask = 1; mask < full; mask++) if (dp[(size_t)M][(size_t)mask] > dp[(size_t)M][(size_t)bestMask]) bestMask = mask;
    for (int m = M, mask = bestMask; m > 0; m--) {
        const int r = from[(size_t)m][(size_t)mask];
        out[(size_t)m - 1] = r;
        if (r >= 0) mask &= ~(1 << r);
    }
    return out;
}

CapturePlan planCapture(const std::vector<CaptureChannel>& ch, double maxRateHz, double minRateHz) {
    CapturePlan p;
    if (ch.empty()) return p;
    // the channels sorted by frequency; the best window is the contiguous run with the most channels that fits
    std::vector<int> order(ch.size());
    for (size_t i = 0; i < ch.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return ch[(size_t)a].freqHz < ch[(size_t)b].freqHz; });
    auto lo = [&](int i) { return ch[(size_t)order[(size_t)i]].freqHz - ch[(size_t)order[(size_t)i]].widthHz / 2; };
    auto hi = [&](int i) { return ch[(size_t)order[(size_t)i]].freqHz + ch[(size_t)order[(size_t)i]].widthHz / 2; };
    const int n = (int)order.size();
    const double maxSpan = maxRateHz > 0 ? maxRateHz * kUsable : std::numeric_limits<double>::infinity();
    int bestI = -1, bestJ = -1;
    long bestPri = -1;
    double bestSpan = 0;
    for (int i = 0; i < n; i++) {
        double top = hi(i);
        for (int j = i; j < n; j++) {
            top = std::max(top, hi(j));
            const double span = top - lo(i);
            if (span > maxSpan) break;
            // more channels first, then the earlier channels of the caller's list, then the narrower window
            long pri = (long)(j - i + 1) * 1000000L;
            for (int k = i; k <= j; k++) pri += std::max(0, 1000 - order[(size_t)k]);
            if (pri > bestPri || (pri == bestPri && span < bestSpan)) { bestPri = pri; bestI = i; bestJ = j; bestSpan = span; }
        }
    }
    if (bestI < 0) {   // not even one channel fits
        for (size_t i = 0; i < ch.size(); i++) p.outside.push_back((int)i);
        return p;
    }
    double top = hi(bestI);
    for (int k = bestI; k <= bestJ; k++) top = std::max(top, hi(k));
    const double bottom = lo(bestI);
    p.rateHz = std::max(minRateHz, (top - bottom) / kUsable);
    if (maxRateHz > 0) p.rateHz = std::min(p.rateHz, maxRateHz);
    p.centerHz = (top + bottom) / 2;
    // keep the radio's DC spike out of a channel when the span has room to move
    const double halfUsable = p.rateHz * kUsable / 2;
    const double slack = halfUsable - (top - bottom) / 2;
    auto onChannel = [&](double c) {
        for (int k = bestI; k <= bestJ; k++) if (c >= lo(k) && c <= hi(k)) return true;
        return false;
    };
    if (onChannel(p.centerHz) && slack > 0) {
        double bestC = p.centerHz, bestMove = std::numeric_limits<double>::infinity();
        for (int k = bestI; k <= bestJ; k++)
            for (double c : {lo(k) - 1000.0, hi(k) + 1000.0}) {
                const double move = std::fabs(c - p.centerHz);
                if (move <= slack && move < bestMove && !onChannel(c)) { bestMove = move; bestC = c; }
            }
        p.centerHz = bestC;
    }
    for (size_t i = 0; i < ch.size(); i++) {
        bool in = false;
        for (int k = bestI; k <= bestJ; k++) if (order[(size_t)k] == (int)i) in = true;
        if (!in) p.outside.push_back((int)i);
    }
    p.fits = p.outside.empty();
    return p;
}

} // namespace dect2::station
