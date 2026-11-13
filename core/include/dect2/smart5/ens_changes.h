// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ensemble / multiplex change log (3.9, docs/SMART5-broadcast-3.9.md item 6). Feed the service list each time the decoder has a
// complete one (DAB FIC, or a DVB SDT/PMT view); every difference from the last list becomes a timed event: a service added or
// removed, a label change, a bitrate or protection change, a move to another sub-channel. A short absence (the list being
// rebuilt) is not reported: a service must be missing in `missingLists` consecutive lists before it counts as removed.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct EnsService {
    unsigned id = 0;          // DAB SId, DVB service_id
    std::string label;
    int bitrateKbps = 0;      // 0 = not known
    int subch = -1;           // DAB sub-channel, DVB: PMT PID; -1 = not known
    std::string protection;   // "EEP 3-A", "UEP 3", ...
    std::string kind;         // "DAB+", "DAB", "data", "TV", ...
};

struct EnsEvent { double t = 0; unsigned id = 0; std::string what, text; };

class EnsChangeLog {
public:
    explicit EnsChangeLog(int missingLists = 3) : miss_(missingLists) {}
    void update(const std::vector<EnsService>& list, double now);
    const std::vector<EnsEvent>& events() const { return ev_; }
    size_t services() const { return cur_.size(); }
    std::string csv() const;
    void reset() { cur_.clear(); missing_.clear(); ev_.clear(); first_ = true; }
private:
    int miss_;
    bool first_ = true;
    std::map<unsigned, EnsService> cur_;
    std::map<unsigned, int> missing_;
    std::vector<EnsEvent> ev_;
};

} // namespace smart5
} // namespace dect2
