// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Project Horus balloon telemetry: the mode numbers. OnAir has three numbering domains: the app family, the engine standard
// (Engine::setStandard, family + 2) and the active standard (RxTelemetry::standard, feed / poll / reset, engine standard - 1).
#pragma once
namespace dect2::horus {
inline constexpr int kFamily = 156;                     // docs/MODE-NUMBERS.md (was 148 on horus-030, which NFC uses)
inline constexpr int kStandard = kFamily + 2;          // 158
inline constexpr int kActiveStandard = kStandard - 1;  // 157, also the poll slot (below Engine::kPollSeqSlots)
// During a mode switch the last snapshot can still belong to the old receiver.
inline bool matchesMode(int family, int activeStandard) { return family == kFamily && activeStandard == kActiveStandard; }
} // namespace dect2::horus
