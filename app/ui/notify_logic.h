// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Settings > Notifications (4.0): the quiet hours rule, with no ImGui and no App (tests/app/test_settings.cpp).
#pragma once

namespace notifyset {

// Is minute-of-day `now` (0..1439) inside the quiet hours [from, to)? A span past midnight (22:00 to 07:00) wraps; from == to is no quiet time.
inline bool inQuiet(int now, int from, int to) {
    if (from == to) return false;
    if (from < to) return now >= from && now < to;
    return now >= from || now < to;
}

}  // namespace notifyset
