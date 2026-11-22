// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/engine/star_prompt.h"
namespace dect2 {

bool starShouldShow(StarState& s, StarEvent e) {
    if (e == StarEvent::Launch) s.launches++;
    else if (e == StarEvent::Decode) s.decoded = true;
    if (!s.enabled || s.choice != 0 || s.shownThisSession) return false;
    if (s.launches < 2) return false;   // never on the first launch, whatever happened in it
    bool due;
    if (s.shown == 0) due = s.launches >= 3 || s.decoded;   // after a few launches, or after the first good decode
    else due = s.later >= 1 && s.later <= 2 && s.today - s.lastShownDay >= (s.later == 1 ? 30 : 60);   // much later, and at most twice
    if (!due) return false;
    s.shown++;
    s.shownThisSession = true;
    s.lastShownDay = s.today;
    return true;
}

void starOnChoice(StarState& s, StarChoice c) {
    if (c == StarChoice::Star) s.choice = 1;
    else if (c == StarChoice::Never) s.choice = 2;
    else if (++s.later > 2) s.choice = 2;
}

const char* starUrl() { return "https://github.com/adswill/OnAir"; }
}
