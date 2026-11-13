// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CIS HF modes (families 620 - 626): telemetry shared by the receiver and the screens. The first members are copied into RxTelemetry (history
// plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kCisHfFamily0 = 620;    // family of kind 0 (cishf::Kind order: BEE, CIS-14, CIS-12, CIS-36, OFDM HDR, CIS-128, CIS-3000)
constexpr int kCisHfKinds = 7;
constexpr size_t kCisHfEventsMax = 60;
constexpr size_t kCisHfTextMax = 12000;

struct CisHfEvent {
    int64_t utc = 0;
    double timeSec = 0;
    std::string text;
};

struct CisHfTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 signal found, 2 frames / symbols being read
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    int kind = 0;                // cishf::Kind
    bool identifyOnly = false;   // the kinds whose frame format is not published
    std::string mode;            // one line: what was found
    std::vector<std::pair<std::string, std::string>> params;
    std::string text;            // the newest decoded text (characters, frames or phase steps), at most kCisHfTextMax
    std::vector<CisHfEvent> events;   // newest first
    double timeSec = 0, inputRate = 0;
    double forcedBaud = 0, forcedShift = 0;
};

inline const char* cisHfName(int kind) {
    static const char* n[kCisHfKinds] = {"CIS-36-50 / 50-50 (BEE)", "CIS-14", "CIS-12 (MS5)", "CIS-36", "CIS OFDM HDR", "CIS-128", "CIS-3000"};
    return kind >= 0 && kind < kCisHfKinds ? n[kind] : "CIS";
}
inline bool cisHfIdentifyOnly(int kind) { return kind >= 3; }

inline std::string cisHfSummary(const CisHfTelemetry& t) {
    std::string s = std::string(cisHfName(t.kind)) + ": " + (t.mode.empty() ? "searching" : t.mode);
    if (s.size() > 120) s.resize(120);
    return s;
}

} // namespace dect2
