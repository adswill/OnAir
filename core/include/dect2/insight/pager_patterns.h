// Pager insights (docs/SMART5-pager-3.9.md), from the pages this receiver decodes (POCSAG, FLEX):
//  automatic pages - a capcode that sends near-identical text at a steady interval ("automatic, every 15 min"; can be hidden)
//  paging burst    - a capcode that suddenly sends many pages after being quiet
//  network silent  - a channel that paged regularly and has been silent for many of its usual gaps
// Everything stays in memory; the text is only compared, never stored beyond the last few pages of a capcode.
#pragma once
#include "dect2/insight/insight.h"
#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::insight {

// the words of a page with every digit as '#': "TEST 12:00" and "TEST 12:15" are the same
inline std::set<std::string> pageShape(const std::string& text) {
    std::set<std::string> s;
    std::istringstream is(upper(text));
    std::string w;
    while (is >> w) {
        for (auto& c : w) if (std::isdigit((unsigned char)c)) c = '#';
        s.insert(w);
    }
    return s;
}

inline double jaccard(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() && b.empty()) return 1;
    size_t in = 0;
    for (const auto& x : a) in += b.count(x);
    return (double)in / (double)(a.size() + b.size() - in);
}

struct PagerAutoInfo { bool automatic = false; double intervalSec = 0; int pages = 0; };

class PagerPatterns {
public:
    struct Limits {
        int autoGaps = 6;               // steady gaps needed
        double autoSimilar = 0.8;       // word overlap of the pages (digits ignored)
        double autoJitter = 0.07;       // median absolute deviation of the gaps / the median gap
        double autoMaxDev = 0.25;       // and no gap further than this share from the median
        double autoMinSec = 20;         // faster than this is one page repeated, not a schedule
        int burstPages = 5;             // this many pages ...
        double burstWindowSec = 600;    // ... within this ...
        double burstQuietSec = 3600;    // ... after at least this long without a page of the capcode (or the channel heard this long)
        int silentPages = 20;           // a channel with this many pages ...
        double silentFactor = 10;       // ... silent for this many of its usual gaps ...
        double silentMinSec = 1800;     // ... and at least this long
    };
    PagerPatterns() {}
    explicit PagerPatterns(Limits l) : L(l) {}

    // one page; capcode should include the protocol ("POCSAG 1234567"), channel the frequency or the receiver
    void add(double t, const std::string& capcode, const std::string& channel, const std::string& text, std::vector<Insight>& out) {
        Ch& ch = ch_[channel];
        if (ch.first == 0) ch.first = t;
        if (ch.last > 0 && t > ch.last) { ch.gaps.push_back(t - ch.last); if (ch.gaps.size() > 200) ch.gaps.pop_front(); }
        ch.last = t; ch.pages++; ch.silentSaid = false;

        Cap& c = cap_[capcode];
        const double prevLast = c.last;
        c.pages++;
        // automatic pages
        c.recent.push_back({t, pageShape(text)});
        if ((int)c.recent.size() > L.autoGaps + 2) c.recent.pop_front();
        const bool wasAuto = c.automatic;
        if (wasAuto) {
            // stays automatic while the pages keep their text and their pace (one late page does not end it)
            const auto& n = c.recent.back(), &p = c.recent[c.recent.size() - 2];
            const double g = n.first - p.first, k = std::max(1.0, std::round(g / c.interval));
            c.automatic = jaccard(n.second, p.second) >= L.autoSimilar && std::fabs(g - k * c.interval) < 0.3 * c.interval;
        } else {
            c.automatic = isAutomatic(c, c.interval);
        }
        if (c.automatic && !wasAuto) {
            char b[200];
            snprintf(b, sizeof b, "%s sends automatic pages every %s (\"%s\")", capcode.c_str(), dur(c.interval).c_str(), clip(text).c_str());
            out.push_back(mk(t, "pager_auto", 0, capcode, b));
        }
        // bursts
        c.times.push_back(t);
        while (!c.times.empty() && t - c.times.front() > L.burstWindowSec) c.times.pop_front();
        if (c.inBurst && t - prevLast > L.burstQuietSec / 2) c.inBurst = false;
        if (!c.inBurst && (int)c.times.size() >= L.burstPages && !c.automatic) {
            // the quiet time before the window: since the capcode's page before it, or since the channel was first heard
            const double before = c.beforeWindow(L.burstWindowSec, t, ch.first);
            if (before >= L.burstQuietSec) {
                c.inBurst = true;
                char b[200];
                snprintf(b, sizeof b, "Paging burst: %s sent %d pages in %s after %s quiet", capcode.c_str(), (int)c.times.size(),
                         dur(t - c.times.front()).c_str(), dur(before).c_str());
                out.push_back(mk(t, "pager_burst", 1, capcode, b));
            }
        }
        c.hist.push_back(t);
        if (c.hist.size() > 64) c.hist.pop_front();
        c.last = t;
        if (cap_.size() > 20000) trim(t);
    }

    // channels gone quiet; call now and then
    void tick(double now, std::vector<Insight>& out) {
        for (auto& kv : ch_) {
            Ch& ch = kv.second;
            if (ch.pages < L.silentPages || ch.silentSaid || ch.gaps.empty()) continue;
            const double med = median(ch.gaps), lim = std::max(L.silentMinSec, L.silentFactor * med);
            if (now - ch.last > lim) {
                ch.silentSaid = true;
                char b[200];
                snprintf(b, sizeof b, "Pager channel %s silent for %s (it paged every %s)", kv.first.c_str(), dur(now - ch.last).c_str(), dur(med).c_str());
                out.push_back(mk(now, "pager_silent", 1, kv.first, b));
            }
        }
    }

    PagerAutoInfo info(const std::string& capcode) const {
        PagerAutoInfo i;
        auto it = cap_.find(capcode);
        if (it == cap_.end()) return i;
        i.automatic = it->second.automatic; i.intervalSec = it->second.interval; i.pages = it->second.pages;
        return i;
    }
    bool automatic(const std::string& capcode) const { return info(capcode).automatic; }   // for "Hide automatic pages"
    // the capcodes sending automatic pages, with their interval
    std::vector<std::pair<std::string, double>> automaticList() const {
        std::vector<std::pair<std::string, double>> v;
        for (const auto& kv : cap_) if (kv.second.automatic) v.push_back({kv.first, kv.second.interval});
        return v;
    }
    void clear() { cap_.clear(); ch_.clear(); }

    static std::string dur(double s) {
        char b[32];
        if (s < 120) snprintf(b, sizeof b, "%.0f s", s);
        else if (s < 7200) snprintf(b, sizeof b, "%.0f min", s / 60);
        else snprintf(b, sizeof b, "%.1f h", s / 3600);
        return b;
    }

private:
    struct Cap {
        int pages = 0;
        double last = 0, interval = 0;
        bool automatic = false, inBurst = false;
        std::deque<std::pair<double, std::set<std::string>>> recent;
        std::deque<double> times, hist;
        double beforeWindow(double win, double t, double chFirst) const {
            // the newest page older than the window
            double prev = -1;
            for (double h : hist) if (t - h > win) prev = h;
            if (prev >= 0) {
                // the first page inside the window
                double firstIn = t;
                for (double h : hist) if (t - h <= win) { firstIn = h; break; }
                return firstIn - prev;
            }
            return (times.empty() ? t : times.front()) - chFirst;
        }
    };
    struct Ch { double first = 0, last = 0; int pages = 0; bool silentSaid = false; std::deque<double> gaps; };

    bool isAutomatic(const Cap& c, double& interval) const {
        if ((int)c.recent.size() < L.autoGaps + 1) return c.automatic;
        std::vector<double> g;
        for (size_t i = c.recent.size() - (size_t)L.autoGaps; i < c.recent.size(); i++) {
            g.push_back(c.recent[i].first - c.recent[i - 1].first);
            if (jaccard(c.recent[i].second, c.recent[i - 1].second) < L.autoSimilar) return false;
        }
        const double med = median(g);
        if (med < L.autoMinSec) return false;
        std::vector<double> dev;
        for (double x : g) dev.push_back(std::fabs(x - med));
        if (median(dev) > L.autoJitter * med || *std::max_element(dev.begin(), dev.end()) > L.autoMaxDev * med) return false;
        interval = med;
        return true;
    }
    template <class C> static double median(const C& c) {
        if (c.empty()) return 0;
        std::vector<double> v(c.begin(), c.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
    static std::string clip(const std::string& s) { return s.size() > 40 ? s.substr(0, 40) + "..." : s; }
    void trim(double t) {
        for (auto it = cap_.begin(); it != cap_.end();) it = t - it->second.last > 7 * 86400 ? cap_.erase(it) : std::next(it);
    }
    static Insight mk(double t, const char* kind, int sev, const std::string& key, const std::string& text) {
        Insight i;
        i.time = t; i.area = "pager"; i.kind = kind; i.severity = sev; i.key = key; i.text = text;
        return i;
    }

    Limits L;
    std::map<std::string, Cap> cap_;
    std::map<std::string, Ch> ch_;
};

}  // namespace dect2::insight
