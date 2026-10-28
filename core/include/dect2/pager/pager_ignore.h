// Pagers: the Ignore list (feat34-c). Addresses (POCSAG RIC or FLEX capcode) whose pages the user does not want to see: single numbers, ranges
// ("836000-836999") and decimal wildcards ("8360*", "83?00"). One list per protocol. Pure and header-only, so a test can check it.
//
// The pages themselves are never thrown away: the receiver keeps them in its list, the interface hides the ignored ones (with a counter and a
// "show ignored" switch) and the receiver does not send them to the automations (notifications, auto-record, webhooks, the message database).
//
// Text form (prefs, import, export): entries separated by spaces, commas, semicolons or line breaks; '#' starts a comment to the end of the line.
// Anything that is not an entry is skipped. Overlapping and touching ranges are merged; the list has no size limit (lookup is a binary search).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct PagerIgnore {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;   // sorted, merged, inclusive
    std::vector<std::string> patterns;                   // decimal wildcards: digits, '*' (any digits, also none) and '?' (one digit)

    bool empty() const { return ranges.empty() && patterns.empty(); }

    static bool wildMatch(const std::string& pat, const std::string& s) {   // iterative '*' / '?' match
        size_t p = 0, i = 0, star = std::string::npos, mark = 0;
        while (i < s.size()) {
            if (p < pat.size() && (pat[p] == '?' || pat[p] == s[i])) { p++; i++; }
            else if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
            else if (star != std::string::npos) { p = star + 1; i = ++mark; }
            else return false;
        }
        while (p < pat.size() && pat[p] == '*') p++;
        return p == pat.size();
    }

    bool has(uint32_t a) const {
        // first range whose end is >= a
        auto it = std::lower_bound(ranges.begin(), ranges.end(), a, [](const std::pair<uint32_t, uint32_t>& r, uint32_t v) { return r.second < v; });
        if (it != ranges.end() && it->first <= a) return true;
        if (patterns.empty()) return false;
        const std::string s = std::to_string(a);
        for (const std::string& p : patterns) if (wildMatch(p, s)) return true;
        return false;
    }

    // Sorts and merges the ranges (overlapping or touching) and drops duplicate patterns.
    void normalise() {
        std::sort(ranges.begin(), ranges.end());
        std::vector<std::pair<uint32_t, uint32_t>> out;
        out.reserve(ranges.size());
        for (const auto& r : ranges) {
            if (!out.empty() && (r.first <= out.back().second || r.first - out.back().second == 1)) out.back().second = std::max(out.back().second, r.second);
            else out.push_back(r);
        }
        ranges.swap(out);
        std::sort(patterns.begin(), patterns.end());
        patterns.erase(std::unique(patterns.begin(), patterns.end()), patterns.end());
    }

    void add(uint32_t a, uint32_t b) { if (b < a) std::swap(a, b); ranges.emplace_back(a, b); normalise(); }

    // Removes one address from the numbers and ranges (a range is split). A wildcard pattern that matches it stays: false is returned then.
    bool remove(uint32_t a) {
        for (size_t i = 0; i < ranges.size(); i++) {
            if (a < ranges[i].first || a > ranges[i].second) continue;
            const auto r = ranges[i];
            ranges.erase(ranges.begin() + (long)i);
            if (a < r.second) ranges.insert(ranges.begin() + (long)i, {a + 1, r.second});
            if (a > r.first) ranges.insert(ranges.begin() + (long)i, {r.first, a - 1});
            break;
        }
        return !has(a);
    }

    size_t entries() const { return ranges.size() + patterns.size(); }
};

// Parses the text form. Numbers above 4294967295 are clamped; a reversed range is turned round; "5-" is the number 5.
inline PagerIgnore pagerParseIgnore(const std::string& s) {
    PagerIgnore w;
    size_t i = 0;
    auto num = [&](uint32_t& v) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        uint64_t x = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { x = x * 10 + (uint64_t)(s[i] - '0'); if (x > 0xFFFFFFFFull) x = 0xFFFFFFFFull; i++; }
        v = (uint32_t)x;
        return true;
    };
    while (i < s.size()) {
        if (s[i] == '#') { while (i < s.size() && s[i] != '\n') i++; continue; }
        const size_t start = i;
        // a wildcard token: digits with at least one * or ?
        size_t j = i; bool wild = false;
        while (j < s.size() && (std::isdigit((unsigned char)s[j]) || s[j] == '*' || s[j] == '?')) { if (s[j] == '*' || s[j] == '?') wild = true; j++; }
        if (wild && (j >= s.size() || s[j] != '-')) {
            w.patterns.push_back(s.substr(start, j - start));
            i = j;
            continue;
        }
        uint32_t a = 0, b = 0;
        if (!num(a)) { i++; continue; }
        b = a;
        if (i < s.size() && s[i] == '-') { i++; if (!num(b)) b = a; }
        if (b < a) std::swap(a, b);
        w.ranges.emplace_back(a, b);
    }
    w.normalise();
    return w;
}

// The canonical text form, one entry per line, ready to save, to export or to put back in prefs (parsing it gives the same list).
inline std::string pagerIgnoreText(const PagerIgnore& w) {
    std::string o;
    for (const auto& r : w.ranges) {
        o += std::to_string(r.first);
        if (r.second != r.first) o += "-" + std::to_string(r.second);
        o += "\n";
    }
    for (const std::string& p : w.patterns) o += p + "\n";
    return o;
}

// How many of the given addresses the list hides.
template <class It> size_t pagerIgnoreCount(const PagerIgnore& w, It first, It last) {
    size_t n = 0;
    for (; first != last; ++first) if (w.has(*first)) n++;
    return n;
}

// ---- the lists the receivers read (one for POCSAG, one for FLEX). Set by the interface, read from the receiver threads.
namespace pager_ignore_detail {
struct Store {
    std::mutex mu;
    std::shared_ptr<const PagerIgnore> list[2];   // 0 POCSAG, 1 FLEX
};
inline Store& store() { static Store s; return s; }
}  // namespace pager_ignore_detail

inline void pagerIgnoreSet(bool flex, const PagerIgnore& w) {
    auto& st = pager_ignore_detail::store();
    auto p = std::make_shared<const PagerIgnore>(w);
    std::lock_guard<std::mutex> lk(st.mu);
    st.list[flex ? 1 : 0] = std::move(p);
}
inline bool pagerIgnored(bool flex, uint32_t address) {
    auto& st = pager_ignore_detail::store();
    std::shared_ptr<const PagerIgnore> p;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        p = st.list[flex ? 1 : 0];
    }
    return p && p->has(address);
}

}  // namespace dect2
