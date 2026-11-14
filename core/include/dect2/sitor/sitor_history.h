// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SITOR text kept across retunes: the receiver shows only the tail of what it decoded and starts over when the radio is retuned, so the
// app appends what is new to a history of its own. A retune adds a separator line with the new frequency and the UTC time.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>

namespace dect2 {

struct SitorHistory {
    std::string text;   // everything kept, newest at the end
    std::string last;   // the receiver text seen at the previous update
    static constexpr size_t kMax = 200000;

    // Append what the receiver text `cur` has beyond the previous update (the receiver text is a sliding tail, so the overlap is found).
    void update(const std::string& cur) {
        if (cur.empty()) { last.clear(); return; }
        if (cur == last) return;
        size_t k = 0;   // the longest end of `last` that is the start of `cur`
        const size_t m = std::min(last.size(), cur.size());
        if (cur.compare(0, last.size(), last) == 0) k = last.size();
        else for (size_t n = m; n > 0; n--) if (last.compare(last.size() - n, n, cur, 0, n) == 0) { k = n; break; }
        text.append(cur, k, std::string::npos);
        last = cur;
        trim();
    }
    // The radio was retuned: the receiver starts over; mark the place in the history.
    void retune(const std::string& marker) {
        last.clear();
        if (!text.empty() && text.back() != '\n') text += '\n';
        text += marker;
        text += '\n';
        trim();
    }
    void clear() { text.clear(); last.clear(); }

private:
    void trim() {
        if (text.size() > kMax) text.erase(0, text.size() - kMax);
    }
};

} // namespace dect2
