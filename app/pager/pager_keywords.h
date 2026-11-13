// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pagers: keyword alerts. A list of keywords (one per line) is matched, without regard to case, against the message text and the capcode's name
// (pager_alias.h). A plain line is a piece of text; "re:" before it makes it a regular expression (ECMAScript, a bad one is reported and skipped).
// Lines that start with # are comments. Pure code: the window highlights the rows, shows the live ticker and raises the optional notification.
//   fire
//   re:\bstructure fire\b|\bMVA\b
//   Fire Station 3          (matches the name of a capcode too)
#pragma once
#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace dect2 {

class PagerKeywords {
public:
    static constexpr size_t kMaxKeywords = 200, kMaxPattern = 200, kMaxScan = 4000;

    // Replaces the list. Returns the number of lines that could not be used (bad regular expression, too long).
    int parse(const std::string& text) {
        rows_.clear();
        int bad = 0;
        size_t s = 0;
        while (s <= text.size() && rows_.size() < kMaxKeywords) {
            size_t e = text.find('\n', s);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(s, e - s);
            s = e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = 0; while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) b++;
            line.erase(0, b);
            if (line.empty() || line[0] == '#') continue;
            Row r; r.shown = line;
            if (line.compare(0, 3, "re:") == 0) {
                const std::string p = line.substr(3);
                if (p.empty() || p.size() > kMaxPattern || nestedQuantifier(p)) { bad++; continue; }
                try { r.re = std::regex(p, std::regex::ECMAScript | std::regex::icase | std::regex::optimize); r.isRegex = true; }
                catch (const std::regex_error&) { bad++; continue; }
            } else {
                r.lower = lower(line);
            }
            rows_.push_back(std::move(r));
            if (e >= text.size()) break;
        }
        return bad;
    }
    bool empty() const { return rows_.empty(); }
    size_t size() const { return rows_.size(); }

    // The first keyword that matches the text or the name; nullptr when none. `where` (optional) gets 0 for the text, 1 for the name.
    const std::string* match(const std::string& text, const std::string& name, int* where = nullptr) const {
        if (rows_.empty()) return nullptr;
        const std::string t = text.size() > kMaxScan ? text.substr(0, kMaxScan) : text;
        const std::string tl = lower(t), nl = lower(name.size() > kMaxScan ? name.substr(0, kMaxScan) : name);
        for (const Row& r : rows_) {
            const bool inT = r.isRegex ? safeSearch(r.re, t) : tl.find(r.lower) != std::string::npos;
            if (inT) { if (where) *where = 0; return &r.shown; }
            if (nl.empty()) continue;
            const bool inN = r.isRegex ? safeSearch(r.re, name) : nl.find(r.lower) != std::string::npos;
            if (inN) { if (where) *where = 1; return &r.shown; }
        }
        return nullptr;
    }

private:
    struct Row { std::string shown, lower; std::regex re; bool isRegex = false; };
    static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
    // A quantified group that holds a quantifier, like (a+)+, can take forever on a long text: refuse it.
    static bool nestedQuantifier(const std::string& p) {
        std::vector<bool> inner;
        for (size_t i = 0; i < p.size(); i++) {
            const char c = p[i];
            if (c == '\\') { i++; continue; }
            if (c == '(') inner.push_back(false);
            else if (c == '*' || c == '+' || c == '{') { if (!inner.empty()) inner.back() = true; }
            else if (c == ')' && !inner.empty()) {
                const bool had = inner.back();
                inner.pop_back();
                if (had && i + 1 < p.size() && (p[i + 1] == '*' || p[i + 1] == '+' || p[i + 1] == '{')) return true;
                if (had && !inner.empty()) inner.back() = true;
            }
        }
        return false;
    }
    static bool safeSearch(const std::regex& re, const std::string& s) {
        try { return std::regex_search(s, re); } catch (const std::regex_error&) { return false; }
    }
    std::vector<Row> rows_;
};

} // namespace dect2
