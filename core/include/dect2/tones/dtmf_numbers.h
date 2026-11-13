// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DTMF number assembler (4.0): turns the stream of DTMF keys heard on a channel into dialled numbers and codes. Keys more than gapS apart start a new
// entry; an entry ending in '#' is a complete code; one starting with '*' is a feature / access code; an entry of 7 or more digits is a phone number;
// an entry heard again (the same digits within repeatS) is counted, not listed twice (a repeater's courtesy tone or a keypad bounce). Pure code.
// Why: a log of single keys is unreadable; the user wants "0037061234567" and "*21#" as lines.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct DtmfEntry { std::string keys; double firstS = 0, lastS = 0; int times = 1; std::string kind; };   // kind: "number", "code", "star", "short"

class DtmfNumbers {
public:
    double gapS = 2.5, repeatS = 120;
    void key(char k, double tS) {
        if (!cur_.empty() && tS - lastKey_ > gapS) close();
        if (cur_.empty()) start_ = tS;
        cur_ += k; lastKey_ = tS;
        if (k == '#') close();
    }
    // closes the entry in progress when the silence has lasted gapS (call now and then)
    void tick(double nowS) { if (!cur_.empty() && nowS - lastKey_ > gapS) close(); }
    const std::vector<DtmfEntry>& entries() const { return e_; }
private:
    void close() {
        if (cur_.empty()) return;
        DtmfEntry en; en.keys = cur_; en.firstS = start_; en.lastS = lastKey_;
        const bool star = cur_[0] == '*', hash = cur_.back() == '#';
        int digits = 0; for (char c : cur_) if (c >= '0' && c <= '9') digits++;
        en.kind = hash ? "code" : star ? "star" : digits >= 7 ? "number" : "short";
        for (auto it = e_.rbegin(); it != e_.rend(); ++it) {
            if (en.firstS - it->lastS > repeatS) break;
            if (it->keys == en.keys) { it->times++; it->lastS = en.lastS; cur_.clear(); return; }
        }
        e_.push_back(en); cur_.clear();
    }
    std::vector<DtmfEntry> e_; std::string cur_; double start_ = 0, lastKey_ = 0;
};

} // namespace dect2
