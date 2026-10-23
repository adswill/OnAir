// Pagers: the "Decoded only" table filter. Pure predicate so a test can check it; the UI applies it to the message table, the Copy export and the side list.
#pragma once
#include "dect2/pager/pager_tel.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

// true when the page carries readable content: not a tone-only page and not one whose text came out empty (FLEX has no tone type; an empty text there is a failed decode).
inline bool pagerIsDecoded(const PagerMessage& m) {
    if (m.type == kPagerTone) return false;
    for (char c : m.text)
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) return true;
    return false;
}

// One row of the "Capcodes" view: an address (POCSAG RIC or FLEX capcode) heard, with how often and when last.
struct PagerHeard {
    uint32_t address = 0;
    bool flex = false;
    int count = 0;
    int64_t lastWall = 0;        // wall clock of the last page
    uint64_t lastSerial = 0;
    int speed = 0;               // PagerSpeed of the last page
    double carrierHz = 0;        // whole-span: the carrier of the last page
    int carrierId = 0;
    std::string lastText;        // the last page that had text (may be empty)
};

// Counts the pages per (protocol, address); busiest first, then the most recent. The same RIC on POCSAG and FLEX stays two rows.
inline std::vector<PagerHeard> pagerHeardTable(const std::vector<PagerMessage>& msgs) {
    std::vector<PagerHeard> rows;
    for (const PagerMessage& m : msgs) {
        auto it = std::find_if(rows.begin(), rows.end(), [&](const PagerHeard& r) { return r.address == m.address && r.flex == m.flex; });
        if (it == rows.end()) { rows.emplace_back(); it = rows.end() - 1; it->address = m.address; it->flex = m.flex; }
        it->count++;
        if (m.serial >= it->lastSerial) {
            it->lastSerial = m.serial; it->lastWall = m.wallTime; it->speed = m.speed; it->carrierHz = m.carrierHz; it->carrierId = m.carrierId;
            if (pagerIsDecoded(m)) it->lastText = m.text;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const PagerHeard& a, const PagerHeard& b) { return a.count != b.count ? a.count > b.count : a.lastSerial > b.lastSerial; });
    return rows;
}

// The capcode watch list (#269): addresses typed as numbers separated by spaces, commas or semicolons, and ranges "1000-1099".
// Anything else is skipped. A range is kept as one entry; at most 256 entries.
struct PagerWatch {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    bool empty() const { return ranges.empty(); }
    bool has(uint32_t a) const {
        for (const auto& r : ranges) if (a >= r.first && a <= r.second) return true;
        return false;
    }
};
inline PagerWatch pagerParseWatch(const std::string& s) {
    PagerWatch w;
    size_t i = 0;
    auto num = [&](uint32_t& v) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        uint64_t x = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { x = x * 10 + (uint64_t)(s[i] - '0'); if (x > 0xFFFFFFFFull) x = 0xFFFFFFFFull; i++; }
        v = (uint32_t)x;
        return true;
    };
    while (i < s.size() && w.ranges.size() < 256) {
        uint32_t a = 0, b = 0;
        if (!num(a)) { i++; continue; }
        b = a;
        if (i < s.size() && s[i] == '-') { i++; if (!num(b)) b = a; }
        if (b < a) std::swap(a, b);
        w.ranges.emplace_back(a, b);
    }
    return w;
}

}  // namespace dect2
