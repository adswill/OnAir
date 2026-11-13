// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 narrowband: signal tracker, SSB conversation manager and callsign finder (qo100_auto.h).
#include "dect2/qo100/qo100_auto.h"
#include "dect2/qo100/qo100_dsp.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <map>

namespace dect2 {

const char* qo100ClassName(int c) {
    static const char* const n[kQoClasses] = {"unknown", "beacon", "carrier", "CW", "SSB", "FT8/JS8", "FT4", "PSK", "RTTY", "SSTV", "wide digital"};
    return c >= 0 && c < kQoClasses ? n[c] : "?";
}
const char* qo100DecoderName(int bit) {
    switch (bit) {
    case kQoDecSsb: return "SSB";
    case kQoDecFt8: return "FT8";
    case kQoDecFt4: return "FT4";
    case kQoDecSstv: return "SSTV";
    case kQoDecCw: return "CW";
    case kQoDecPsk: return "PSK31";
    case kQoDecJs8: return "JS8";
    case kQoDecRtty: return "RTTY";
    default: return "?";
    }
}

namespace {

constexpr double kHistSec = 60;
constexpr double kHoldSec = 150;          // an inactive signal is remembered this long (FT8 stations come back every few slots)
constexpr float kDetectDb = 8;            // over the floor
constexpr size_t kMaxTracks = 64;

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
    return v[v.size() / 2];
}

struct Run { int a = 0, b = 0, pk = 0; double lo = 0, hi = 0, rf = 0, powDb = -200; float peakDb = -200; };

} // namespace

// the decoders that can read a class (Qo100Dec bits), shared with qo100_auto.cpp
int qo100ClassDecoders(int cls) {
    switch (cls) {
    case kQoSlot15: return kQoDecFt8 | kQoDecJs8;
    case kQoFt4: return kQoDecFt4;
    case kQoCw: return kQoDecCw;
    case kQoPsk: case kQoCarrier: return kQoDecPsk;
    case kQoRtty: return kQoDecRtty;
    case kQoSstv: return kQoDecSstv;
    case kQoSsb: return kQoDecSsb;
    default: return 0;
    }
}

// ---- the tracker --------------------------------------------------------------------------------------------------------------------------

struct Qo100Tracker::Impl {
    struct Tr {
        Qo100Track t;
        bool confirmed = false, on = false;
        int hits = 0;
        double curStart = 0, lastPresent = -1e9, lastClass = -1e9, lastSeen = 0;
        std::deque<std::pair<double, double>> runs;      // finished on-periods (start, end)
        std::deque<std::pair<double, float>> lev;        // (time, dB) of frames with the signal
        struct H { double t; float p; uint8_t on; };
        std::deque<H> hist;                              // every frame: time, power over the floor (0 when absent), present
        std::deque<double> widths;
        int idCls = -1;
    };
    std::vector<Tr> tr;
    std::vector<Qo100Track> out;
    int nextId = 1;
    double lastT = -1;
    std::vector<double> lin;

    void reset() { tr.clear(); out.clear(); nextId = 1; lastT = -1; }

    static bool nearBeacon(double rf, int* which = nullptr) {
        const double nb[3] = {qo100::kLowBeaconHz, qo100::kPskBeaconHz, qo100::kHighBeaconHz};
        for (int i = 0; i < 3; i++) if (std::fabs(rf - nb[i]) < 1200) { if (which) *which = i; return true; }
        return false;
    }

    void classify(Tr& r, double now) {
        Qo100Track& t = r.t;
        const double age = now - t.firstSec;
        std::vector<double> w(r.widths.begin(), r.widths.end());
        const double wd = median(w);
        // duty over the last 30 s, the level spread of the frames with the signal (dB), the spread of the power including the gaps (10 s), key flips
        int nOn = 0, nAll = 0, flips = 0, n10 = 0;
        double p1 = 0, p2 = 0;
        uint8_t prev = 2;
        for (const auto& h : r.hist) {
            if (h.t >= now - 30) { nAll++; nOn += h.on; }
            if (h.t >= now - 10) { n10++; p1 += h.p; p2 += (double)h.p * h.p; if (prev != 2 && h.on != prev) flips++; prev = h.on; }
        }
        t.duty = nAll ? (float)nOn / nAll : 0.f;
        const double pm = n10 ? p1 / n10 : 0, cv = pm > 0 ? std::sqrt(std::max(0.0, p2 / n10 - pm * pm)) / pm : 0;
        double sum = 0, sum2 = 0; int ln = 0;
        for (const auto& l : r.lev) if (l.first >= now - 20) { sum += l.second; sum2 += (double)l.second * l.second; ln++; }
        const double sd = ln >= 12 ? std::sqrt(std::max(0.0, sum2 / ln - (sum / ln) * (sum / ln))) : 0;
        // frame to frame change of the level (dB): voice and keying jump about, a block of FSK or PSK stations does not
        double rd = 0; int rn = 0;
        std::vector<double> ad;
        for (size_t i = 1; i < r.lev.size(); i++)
            if (r.lev[i].first >= now - 20 && r.lev[i].first - r.lev[i - 1].first < 0.16) { const double d = (double)r.lev[i].second - r.lev[i - 1].second; rd += d * d; rn++; ad.push_back(std::fabs(d)); }
        const double jit = rn >= 10 ? std::sqrt(rd / rn) : 0;
        const bool steady = rn < 10 || median(ad) < 0.9;
        int cls = kQoUnknown;
        if (nearBeacon(t.rfHz)) cls = kQoBeacon;
        else {
            // slotted: finished on-periods of steady level, spaced by a whole number of slots (a single station or a band of them)
            bool slot15 = false, ft4 = false;
            const size_t m = r.runs.size();
            if (m >= 2 && wd < 4000 && steady && age >= 3) {
                std::vector<double> per, len;
                double prevStart = -1;
                for (size_t i = 0; i < m; i++) {
                    if (r.runs[i].second - r.runs[i].first < 1.0) continue;   // a dropout of a weak signal
                    if (prevStart >= 0) per.push_back(r.runs[i].first - prevStart);
                    prevStart = r.runs[i].first;
                    len.push_back(r.runs[i].second - r.runs[i].first);
                }
                if (per.empty()) per.push_back(0);
                if (len.empty()) len.push_back(0);
                const double P = median(per), L = median(len);
                auto mult = [&](double slot) { const double k = std::round(P / slot); return k >= 1 && k <= 10 && std::fabs(P - k * slot) < 1.0 + 0.1 * k; };
                const bool strong = len.size() >= 3 || (std::fabs(P - 15) < 0.8 && L > 10.5 && L < 14.2);
                if (L >= 8 && L <= 14.8 && mult(15) && strong) slot15 = true;
                else if (L >= 3.2 && L <= 6.4 && mult(7.5) && strong) ft4 = true;
            }
            if (slot15) cls = kQoSlot15;
            else if (ft4) cls = kQoFt4;
            else if (wd >= 700) {
                if (age < 2.5) cls = kQoUnknown;
                else if (!steady) cls = kQoSsb;
                else if (t.duty < 0.9f) cls = kQoUnknown;    // a block of stations on slots, before the slots show
                else if (wd <= 1800 && age >= 6) cls = kQoSstv;
                else if (age >= 6) cls = kQoWide;
            } else if (age >= 6) {
                if (t.snrDb >= 12 && t.duty >= 0.3f && !steady && jit >= 1.5) cls = kQoCw;   // keyed: the level jumps about, unlike a block of FSK or PSK
                else if (t.duty >= 0.8f && age >= 8 && steady) {
                    if (wd >= 250) cls = kQoRtty;
                    else if (sd < 0.8) cls = kQoCarrier;
                    else cls = kQoPsk;
                }
            }
        }
        // the signal identifier's opinion fills a gap, never overrules a clear measurement
        if (cls == kQoUnknown && r.idCls > 0) cls = r.idCls;
        t.cls = cls;
    }

    void update(const float* db, size_t n, double f0Hz, double binHz, float floorDb, double now) {
        if (n < 16 || binHz <= 0) return;
        const double dt = lastT < 0 ? 0.1 : std::max(0.0, now - lastT);
        (void)dt;
        lastT = now;
        for (const Qo100Track& o : out) for (Tr& r : tr) if (r.t.id == o.id) { r.t.conv = o.conv; r.t.decMask = o.decMask; break; }   // what the other managers set
        const double floorLin = std::pow(10.0, floorDb / 10.0);
        lin.resize(n);
        for (size_t k = 0; k < n; k++) lin[k] = std::pow(10.0, (double)db[k] / 10.0);
        // runs of bins over the floor; gaps up to 250 Hz bridged
        const float thr = floorDb + kDetectDb;
        const int gap = std::max(2, (int)(250 / binHz));
        std::vector<Run> runs;
        for (size_t k = 0; k < n && runs.size() < 90;) {
            if (db[k] < thr) { k++; continue; }
            int a = (int)k, b = (int)k, quiet = 0;
            for (size_t j = k + 1; j < n; j++) {
                if (db[j] >= thr) { b = (int)j; quiet = 0; }
                else if (++quiet > gap) break;
            }
            Run r; r.a = a; r.b = b; r.pk = a;
            for (int j = a; j <= b; j++) if (db[j] > db[r.pk]) r.pk = j;
            r.peakDb = db[r.pk];
            const float cut = std::max(thr, r.peakDb - 25.f);
            while (r.a < r.pk && db[r.a] < cut) r.a++;
            while (r.b > r.pk && db[r.b] < cut) r.b--;
            runs.push_back(r);
            k = (size_t)b + 1;
        }
        // speech has holes: a wide run and its neighbour closer than 500 Hz are one signal
        for (size_t i = 0; i + 1 < runs.size();) {
            Run& x = runs[i]; const Run& y = runs[i + 1];
            const double wx = (x.b - x.a + 1) * binHz, wy = (y.b - y.a + 1) * binHz, g = (y.a - x.b - 1) * binHz;
            if (g < 500 && (wx > 400 || wy > 400)) {
                if (y.peakDb > x.peakDb) { x.pk = y.pk; x.peakDb = y.peakDb; }
                x.b = y.b;
                runs.erase(runs.begin() + (long)i + 1);
            } else i++;
        }
        for (Run& r : runs) {
            double p = 0;
            for (int j = r.a; j <= r.b; j++) p += std::max(0.0, lin[(size_t)j] - floorLin);
            r.powDb = 10 * std::log10(p + 1e-20);
            r.lo = f0Hz + (r.a - 0.5) * binHz; r.hi = f0Hz + (r.b + 0.5) * binHz;
            r.rf = 0.5 * (r.lo + r.hi);
            if (r.b - r.a <= 2 && r.pk > 0 && r.pk < (int)n - 1) {   // a carrier: the peak by a parabola
                const double l = db[r.pk - 1], m = db[r.pk], rr = db[r.pk + 1], den = l - 2 * m + rr;
                if (den < 0) r.rf = f0Hz + (r.pk + std::clamp(0.5 * (l - rr) / den, -0.5, 0.5)) * binHz;
            }
        }
        // match runs to tracks: nearest centre among those that overlap (padded by 120 Hz)
        struct Pair { double d; size_t ti, ri; };
        std::vector<Pair> prs;
        for (size_t ti = 0; ti < tr.size(); ti++)
            for (size_t ri = 0; ri < runs.size(); ri++) {
                const Run& r = runs[ri]; const Qo100Track& t = tr[ti].t;
                if (r.hi + 120 < t.loHz || r.lo - 120 > t.hiHz) continue;
                prs.push_back({std::fabs(r.rf - t.rfHz), ti, ri});
            }
        std::sort(prs.begin(), prs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
        std::vector<int> runOf(tr.size(), -1), trOf(runs.size(), -1);
        for (const Pair& p : prs) if (runOf[p.ti] < 0 && trOf[p.ri] < 0) { runOf[p.ti] = (int)p.ri; trOf[p.ri] = (int)p.ti; }
        for (size_t ri = 0; ri < runs.size(); ri++) {
            if (trOf[ri] >= 0) continue;
            if (tr.size() >= kMaxTracks) break;
            Tr r;
            r.t.id = nextId++;
            r.t.firstSec = now; r.t.lastSec = now;
            r.t.rfHz = runs[ri].rf; r.t.loHz = runs[ri].lo; r.t.hiHz = runs[ri].hi;
            tr.push_back(std::move(r));
            runOf.push_back((int)ri);
            trOf[ri] = (int)tr.size() - 1;
        }
        for (size_t ti = 0; ti < tr.size(); ti++) {
            Tr& r = tr[ti];
            Qo100Track& t = r.t;
            const bool present = runOf[ti] >= 0;
            if (present) {
                const Run& u = runs[(size_t)runOf[ti]];
                const bool fresh = r.on || now - r.lastPresent < 1.0;
                const double a = fresh ? 0.35 : 1.0;
                t.loHz += a * (u.lo - t.loHz); t.hiHz += a * (u.hi - t.hiHz);
                t.rfHz += a * (u.rf - t.rfHz);
                t.widthHz = t.hiHz - t.loHz;
                t.levelDb = (float)u.powDb; t.snrDb = u.peakDb - floorDb;
                r.widths.push_back(u.hi - u.lo); if (r.widths.size() > 24) r.widths.pop_front();
                r.lev.push_back({now, (float)u.powDb});
                if (!r.on) { r.on = true; r.curStart = now; }
                r.lastPresent = now; t.lastSec = now; r.hits++;
                if (r.hits >= 4) r.confirmed = true;
            } else if (r.on && now - r.lastPresent >= 0.4) {
                r.on = false;
                r.runs.push_back({r.curStart, r.lastPresent});
                if (r.runs.size() > 12) r.runs.pop_front();
            }
            t.active = r.on;
            r.hist.push_back({now, present ? (float)std::pow(10.0, runs[(size_t)runOf[ti]].powDb / 10.0) : 0.f, (uint8_t)(present ? 1 : 0)});
            while (!r.hist.empty() && r.hist.front().t < now - kHistSec) r.hist.pop_front();
            while (!r.lev.empty() && r.lev.front().first < now - kHistSec) r.lev.pop_front();
            if (r.confirmed && now - r.lastClass >= 0.5) { classify(r, now); r.lastClass = now; }
        }
        // forget: signals that never confirmed, and old ones
        tr.erase(std::remove_if(tr.begin(), tr.end(), [&](const Tr& r) {
            return (!r.confirmed && now - r.lastPresent > 0.6) || (r.confirmed && now - r.lastPresent > kHoldSec);
        }), tr.end());
        out.clear();
        for (const Tr& r : tr) if (r.confirmed) out.push_back(r.t);
        std::sort(out.begin(), out.end(), [](const Qo100Track& a, const Qo100Track& b) { return a.rfHz < b.rfHz; });
    }
};

Qo100Tracker::Qo100Tracker() : p_(new Impl) {}
Qo100Tracker::~Qo100Tracker() = default;
void Qo100Tracker::reset() { p_->reset(); }
void Qo100Tracker::update(const float* db, size_t n, double f0Hz, double binHz, float floorDb, double tSec) { p_->update(db, n, f0Hz, binHz, floorDb, tSec); }
const std::vector<Qo100Track>& Qo100Tracker::tracks() const { return p_->out; }
std::vector<Qo100Track>& Qo100Tracker::mutableTracks() { return p_->out; }
void Qo100Tracker::setDecoderMask(int id, int mask) { for (auto& r : p_->tr) if (r.t.id == id) r.t.decMask = mask; }
void Qo100Tracker::setIdentified(int id, const std::string& name, int cls) {
    for (auto& r : p_->tr) if (r.t.id == id) { r.t.idName = name; r.idCls = cls; }
}

// ---- callsigns -----------------------------------------------------------------------------------------------------------------------------

namespace {

bool callLike(const std::string& s) {
    const size_t n = s.size();
    if (n < 4 || n > 8) return false;
    auto L = [&](size_t i) { return i < n && std::isupper((unsigned char)s[i]); };
    auto D = [&](size_t i) { return i < n && std::isdigit((unsigned char)s[i]); };
    // one or two letters, a digit, one to four letters (DL1ABC, G3ABC, LY2XY); or digit, letter, digit, letters (9A2AB, 4X1AB)
    size_t i = 0;
    if (L(0) && L(1) && D(2)) i = 3;
    else if (L(0) && D(1)) i = 2;
    else if (D(0) && L(1) && D(2)) i = 3;
    else return false;
    size_t k = 0;
    while (L(i + k)) k++;
    return k >= 1 && k <= 4 && i + k == n;
}

const char* const kNato[26] = {"ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT", "GOLF", "HOTEL", "INDIA", "JULIETT", "KILO", "LIMA", "MIKE",
                               "NOVEMBER", "OSCAR", "PAPA", "QUEBEC", "ROMEO", "SIERRA", "TANGO", "UNIFORM", "VICTOR", "WHISKEY", "XRAY", "YANKEE", "ZULU"};
const char* const kDigitWords[10] = {"ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"};

char spokenChar(const std::string& w) {
    for (int i = 0; i < 26; i++) if (w == kNato[i]) return (char)('A' + i);
    if (w == "JULIET") return 'J';
    if (w == "X-RAY" || w == "XRAY") return 'X';
    if (w == "ALFA") return 'A';
    if (w == "NINER") return '9';
    if (w == "TREE") return '3';
    for (int i = 0; i < 10; i++) if (w == kDigitWords[i]) return (char)('0' + i);
    return 0;
}

} // namespace

std::vector<std::string> qo100CallsFromText(const std::string& text, bool spoken) {
    std::vector<std::string> out;
    auto add = [&](const std::string& c) { if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); };
    std::vector<std::string> tok;
    std::string cur;
    for (char ch : text) {
        const unsigned char u = (unsigned char)ch;
        if (std::isalnum(u) || ch == '-') cur += (char)std::toupper(u);
        else if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
    }
    if (!cur.empty()) tok.push_back(cur);
    std::string run;
    for (const std::string& w : tok) {
        if (callLike(w)) add(w);
        if (!spoken) continue;
        const char c = spokenChar(w);
        if (c) run += c;
        else { if (callLike(run)) add(run); run.clear(); }
        // single letters or digits spoken one by one ("l y 2 a b c") join the run too
        if (!c && w.size() == 1 && std::isalnum((unsigned char)w[0])) run += w[0];
    }
    if (spoken && callLike(run)) add(run);
    return out;
}

// ---- conversations -------------------------------------------------------------------------------------------------------------------------

struct Qo100Conversations::Impl {
    struct Spurt { double lo; float lev; };
    struct Cluster { double lo; float lev; int n; };
    struct Conv {
        Qo100Conversation c;
        bool ended = false;
        std::deque<std::pair<double, double>> loHist;
        double lastHistSec = -1e9, lastOn = -1e9, spurtStart = -1;
        std::vector<std::pair<double, float>> spFrames;   // (lo, level) of the current spurt
        std::vector<Cluster> clusters;
        std::map<int, double> onSecOf;
        std::string snippet;
    };
    std::vector<Conv> convs;
    std::vector<Qo100Conversation> pub;
    int nextId = 1;
    double gapSec = 90, lastT = -1;

    void endSpurt(Conv& v) {
        if (v.spFrames.size() >= 6) {
            std::vector<double> lo; std::vector<double> lv;
            for (auto& f : v.spFrames) { lo.push_back(f.first); lv.push_back(f.second); }
            const double l = median(lo); const float e = (float)median(lv);
            bool hit = false;
            for (Cluster& k : v.clusters)
                if (std::fabs(k.lo - l) < 120 && std::fabs(k.lev - e) < 3.5f) { k.lo += (l - k.lo) / (k.n + 1); k.lev += (e - k.lev) / (k.n + 1); k.n++; hit = true; break; }
            if (!hit && v.clusters.size() < 12) v.clusters.push_back({l, e, 1});
        }
        v.spFrames.clear();
    }
};

Qo100Conversations::Qo100Conversations() : p_(new Impl) {}
Qo100Conversations::~Qo100Conversations() = default;
void Qo100Conversations::reset() { p_->convs.clear(); p_->pub.clear(); p_->nextId = 1; p_->lastT = -1; }
void Qo100Conversations::setGapSec(double s) { p_->gapSec = std::clamp(s, 5.0, 3600.0); }
const std::vector<Qo100Conversation>& Qo100Conversations::list() const { return p_->pub; }
Qo100Conversation* Qo100Conversations::find(int id) {
    for (auto& v : p_->convs) if (v.c.id == id) return &v.c;
    return nullptr;
}
void Qo100Conversations::setSnippet(int id, const std::string& text) {
    for (auto& v : p_->convs) if (v.c.id == id) {
        v.snippet += (v.snippet.empty() ? "" : " ") + text;
        if (v.snippet.size() > 600) v.snippet.erase(0, v.snippet.size() - 600);
        v.c.snippet = v.snippet;
        v.c.calls = qo100CallsFromText(v.snippet, true);
    }
}

void Qo100Conversations::update(std::vector<Qo100Track>& tracks, double t, double utc) {
    Impl& m = *p_;
    const double dt = m.lastT < 0 ? 0.1 : std::clamp(t - m.lastT, 0.0, 1.0);
    m.lastT = t;
    auto conv = [&](int id) -> Impl::Conv* { for (auto& v : m.convs) if (v.c.id == id) return &v; return nullptr; };
    for (Qo100Track& tk : tracks) {
        if (tk.conv) {
            Impl::Conv* v = conv(tk.conv);
            if (!v || v->ended || tk.cls != kQoSsb) { if (tk.cls != kQoSsb || tk.active) tk.conv = 0; }
        }
        if (tk.cls != kQoSsb || tk.conv || !tk.active) continue;
        Impl::Conv* best = nullptr; double bd = 1e9;
        for (auto& v : m.convs) {
            if (v.ended) continue;
            const double d = std::fabs(tk.loHz - v.c.loHz);
            if (d <= 1200 && d < bd) { bd = d; best = &v; }
        }
        if (!best) {
            Impl::Conv v;
            v.c.id = m.nextId++;
            v.c.startSec = v.c.lastSec = t; v.c.startUtc = v.c.lastUtc = utc;
            v.c.loHz = tk.loHz;
            m.convs.push_back(std::move(v));
            best = &m.convs.back();
        }
        tk.conv = best->c.id;
    }
    for (Impl::Conv& v : m.convs) {
        if (v.ended) continue;
        Qo100Conversation& c = v.c;
        c.tracks.clear();
        float lev = -200; double lo = 0; bool any = false;
        const Qo100Track* strongest = nullptr;
        for (const Qo100Track& tk : tracks) {
            if (tk.conv != c.id) continue;
            c.tracks.push_back(tk.id);
            if (tk.active) { any = true; if (tk.levelDb > lev) { lev = tk.levelDb; strongest = &tk; } }
        }
        if (any && strongest) {
            lo = strongest->loHz;
            c.loHz += 0.3 * (lo - c.loHz);
            c.lastSec = t; c.lastUtc = utc; v.lastOn = t;
            c.levelDb = lev; c.peakDb = std::max(c.peakDb, lev);
            c.talkSec += dt;
            for (const Qo100Track& tk : tracks) if (tk.conv == c.id && tk.active) v.onSecOf[tk.id] += dt;
            if (v.spurtStart < 0) v.spurtStart = t;
            v.spFrames.push_back({lo, lev});
            if (t - v.lastHistSec >= 1.0) {
                v.loHist.push_back({t, c.loHz}); v.lastHistSec = t;
                while (!v.loHist.empty() && v.loHist.front().first < t - 90) v.loHist.pop_front();
            }
        } else if (v.spurtStart >= 0) { m.endSpurt(v); v.spurtStart = -1; }   // (the track itself was gone for 0.4 s)
        c.active = any || t - v.lastOn < 3.0;
        c.dialHz = std::round((c.loHz - 300) / 10) * 10;
        if (v.loHist.size() >= 8) {
            std::vector<double> tt, vv;
            for (auto& h : v.loHist) if (h.first >= t - 60) { tt.push_back(h.first); vv.push_back(h.second); }
            c.driftValid = tt.size() >= 8 && tt.back() - tt.front() > 15 && qo100::slopePerMin(tt, vv, c.driftHzPerMin);
        }
        int multi = 0;
        for (auto& kv : v.onSecOf) if (kv.second >= 3) multi++;
        int cl = 0;
        for (const auto& k : v.clusters) if (k.n >= 2) cl++;
        if (!cl && !v.clusters.empty()) cl = 1;
        c.stations = std::clamp(std::max({1, cl, multi}), 1, 8);
        if (c.tracks.empty() && c.talkSec < 10 && t - c.startSec > 3) { v.ended = true; v.c.id = -v.c.id; c.active = false; continue; }   // its signals turned out to be something else
        if (!c.active && t - c.lastSec > m.gapSec) {
            if (v.spurtStart >= 0) { m.endSpurt(v); v.spurtStart = -1; }
            v.ended = true; c.active = false;
        }
    }
    // old ended ones go
    while (m.convs.size() > 60) {
        auto it = std::find_if(m.convs.begin(), m.convs.end(), [](const Impl::Conv& v) { return v.ended; });
        if (it == m.convs.end()) break;
        m.convs.erase(it);
    }
    m.convs.erase(std::remove_if(m.convs.begin(), m.convs.end(), [](const Impl::Conv& v) { return v.c.id < 0; }), m.convs.end());
    m.pub.clear();
    for (auto it = m.convs.rbegin(); it != m.convs.rend(); ++it) m.pub.push_back(it->c);
}

} // namespace dect2
