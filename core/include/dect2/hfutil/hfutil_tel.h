// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF utility receivers (families 430 .. 439, 603 .. 606) telemetry, shared by the receiver and the panel. Keep the first members (copied into RxTelemetry).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kHfuFamilyFirst = 430, kHfuKinds = 10;
// 3.9 kinds after the first ten, in their own family numbers (docs/HF-MIL-3.9.md): 10 = MIL-STD-188-110C Appendix C / STANAG 4539 (family 603),
// 11 = 3G-ALE (family 604), 12 = Link 11 CLEW (family 605), 13 = STANAG 4481 FSK (family 606). Family f has active standard f + 1 and engine
// standard f + 2.
constexpr int kHfuKindHdr = 10, kHfuKindAle3g = 11, kHfuKindLink11 = 12, kHfuKind4481 = 13, kHfuKindsAll = 14;
constexpr int kHfuHdrFamily = 603, kHfuAle3gFamily = 604, kHfuLink11Family = 605, kHfu4481Family = 606;
constexpr int kHfuExtraActiveFirst = kHfuHdrFamily + 1, kHfuExtraActiveLast = kHfuHdrFamily + kHfuKindsAll - kHfuKinds;
inline int hfuFamilyOf(int k) { return k < kHfuKinds ? kHfuFamilyFirst + k : kHfuHdrFamily + (k - kHfuKinds); }
inline const char* hfuName(int k) {
    static const char* n[kHfuKindsAll] = {"MIL-STD-188-110 serial", "STANAG 4285", "ARQ-E", "ARQ-N", "AUTOSPEC", "SPREAD", "G-TOR", "EFR teleswitch", "Piccolo Mk VI", "Coquelet-8",
                                          "STANAG 4539 / 110C high rate", "3G-ALE", "Link 11 CLEW", "STANAG 4481 FSK"};
    return k >= 0 && k < kHfuKindsAll ? n[k] : "HF utility";
}

struct HfUtilEvent {
    int64_t utc = 0;
    std::string text;
};

struct HfUtilTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 searching, 1 signal found, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;
    float levelDb = -200;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;
    int kind = 0;
    std::string mode;            // rate / interleaver, or the found baud and shift
    double baud = 0, centreHz = 0, shiftHz = 0;
    std::string text;            // decoded text, newest last (kept to a few thousand characters)
    std::vector<HfUtilEvent> events;   // newest first
    uint64_t pdus = 0;           // STANAG 5066 D_PDUs seen with a good header CRC
};

inline std::string hfUtilSummary(const HfUtilTelemetry& t) {
    char b[200];
    if (t.state == 0) snprintf(b, sizeof b, "%s: searching", hfuName(t.kind));
    else if (t.mode.empty()) snprintf(b, sizeof b, "%s: signal found", hfuName(t.kind));
    else snprintf(b, sizeof b, "%s: %s", hfuName(t.kind), t.mode.c_str());
    return b;
}

}  // namespace dect2
