// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
namespace dect2::railway {
// OnAir uses three numbering domains: the UI family, Engine::setStandard
// (family + 2), and RxTelemetry/feed/poll/reset (engine standard - 1).
inline constexpr int kKlwFamily = 146;
inline constexpr int kTdcsFamily = 147;
inline constexpr int kKlwStandard = kKlwFamily + 2;
inline constexpr int kTdcsStandard = kTdcsFamily + 2;
inline constexpr int kKlwActiveStandard = kKlwStandard - 1;
inline constexpr int kTdcsActiveStandard = kTdcsStandard - 1;

// During a mode switch the last snapshot can still belong to the old receiver.
inline bool matchesMode(int family, int activeStandard) {
    return (family == kKlwFamily && activeStandard == kKlwActiveStandard) ||
           (family == kTdcsFamily && activeStandard == kTdcsActiveStandard);
}
} // namespace dect2::railway
