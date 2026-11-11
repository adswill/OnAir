// Pager watch (4.0): keyword alerts and automatic RIC groups on decoded POCSAG / FLEX messages. Pure code on PagerMessage.
//  - PagerKeywords: the user's words ("fire", "structure", "ALARM 3"), matched case-insensitively on whole words (a word with spaces is a phrase);
//    a leading '*' matches inside a word. A message returns the words it hit, so the UI colours the row and the alert rings once per message.
//  - PagerGroups: capcodes that are paged together (a fire station's pagers are all called within seconds for one incident) are found from the
//    traffic: two capcodes are linked when they were paged within linkS of each other at least minCo times; linked capcodes form a group. The
//    group list tells "these 14 capcodes are one unit" without the user building the list by hand.
#pragma once
#include "dect2/pager/pager_tel.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

class PagerKeywords {
public:
    void set(const std::vector<std::string>& words) {
        w_.clear();
        for (auto s : words) {
            Kw k;
            if (!s.empty() && s[0] == '*') { k.inside = true; s.erase(0, 1); }
            k.text = lower(trim(s));
            if (!k.text.empty()) { k.shown = (k.inside ? "*" : "") + k.text; w_.push_back(k); }
        }
    }
    std::vector<std::string> match(const std::string& message) const {
        std::vector<std::string> hit;
        const std::string t = lower(message);
        for (const auto& k : w_) {
            size_t p = 0; bool ok = false;
            while ((p = t.find(k.text, p)) != std::string::npos) {
                const bool left = p == 0 || !std::isalnum((unsigned char)t[p - 1]);
                const size_t e = p + k.text.size();
                const bool right = e >= t.size() || !std::isalnum((unsigned char)t[e]);
                if (k.inside || (left && right)) { ok = true; break; }
                p++;
            }
            if (ok) hit.push_back(k.shown);
        }
        return hit;
    }
    size_t size() const { return w_.size(); }
private:
    struct Kw { std::string text, shown; bool inside = false; };
    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
    static std::string trim(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && std::isspace((unsigned char)s[a])) a++; while (b > a && std::isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }
    std::vector<Kw> w_;
};

class PagerGroups {
public:
    double linkS = 20; int minCo = 3; size_t keep = 4000;
    void add(uint32_t capcode, double timeS) {
        if (!capcode) return;
        if (!ev_.empty() && timeS < ev_.back().t - 1e-9) std::sort(ev_.begin(), ev_.end(), [](const E& a, const E& b) { return a.t < b.t; });
        ev_.push_back({timeS, capcode});
        if (ev_.size() > keep) ev_.erase(ev_.begin(), ev_.begin() + (ev_.size() - keep));
    }
    struct Group { std::vector<uint32_t> capcodes; int incidents = 0; };
    // groups of two or more capcodes, the largest first; incidents = how often the group's capcodes were paged inside one linkS burst
    std::vector<Group> groups() const {
        std::vector<E> e = ev_;
        std::sort(e.begin(), e.end(), [](const E& a, const E& b) { return a.t < b.t; });
        // bursts: events within linkS of the previous event
        std::vector<std::set<uint32_t>> bursts;
        for (size_t i = 0; i < e.size(); i++) {
            if (i == 0 || e[i].t - e[i - 1].t > linkS) bursts.emplace_back();
            bursts.back().insert(e[i].cap);
        }
        std::map<std::pair<uint32_t, uint32_t>, int> co;
        for (const auto& b : bursts) {
            if (b.size() < 2 || b.size() > 60) continue;   // one capcode alone, or a roll call that links everything
            for (auto i = b.begin(); i != b.end(); ++i) for (auto j = std::next(i); j != b.end(); ++j) co[{*i, *j}]++;
        }
        // union-find on strong links
        std::map<uint32_t, uint32_t> par;
        auto find = [&](uint32_t x) { while (par.count(x) && par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
        for (const auto& kv : co) if (kv.second >= minCo) {
            for (uint32_t c : {kv.first.first, kv.first.second}) if (!par.count(c)) par[c] = c;
            par[find(kv.first.first)] = find(kv.first.second);
        }
        std::map<uint32_t, Group> g;
        for (auto& kv : par) g[find(kv.first)].capcodes.push_back(kv.first);
        std::vector<Group> out;
        for (auto& kv : g) {
            Group& x = kv.second; std::sort(x.capcodes.begin(), x.capcodes.end());
            for (const auto& b : bursts) { int n = 0; for (uint32_t c : x.capcodes) n += b.count(c) ? 1 : 0; if (n >= 2) x.incidents++; }
            out.push_back(x);
        }
        std::sort(out.begin(), out.end(), [](const Group& a, const Group& b) { return a.capcodes.size() != b.capcodes.size() ? a.capcodes.size() > b.capcodes.size() : a.incidents > b.incidents; });
        return out;
    }
private:
    struct E { double t; uint32_t cap; };
    std::vector<E> ev_;
};

} // namespace dect2
