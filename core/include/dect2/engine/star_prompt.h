// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The friendly "star OnAir on GitHub" card. Pure logic, no network and no clock of its own: the app feeds it launches, a first good
// decode and today's day number, and it says when the card may show. It never shows on the first launch, shows once per session, never
// again after Star or Don't ask again, and "Maybe later" brings it back at most twice, 30 and then 60 days later. The browser is opened
// by the app only on a click on "Star on GitHub" (starUrl()). The state is saved by the app as plain numbers in the prefs (starKeys).
#pragma once
namespace dect2 {

enum class StarEvent { Launch, Decode, Frame };   // a start of the app, the first good decode, any later moment (just asks)
enum class StarChoice { Star, Later, Never };

struct StarState {
    bool enabled = true;       // Settings: off = never shows
    int launches = 0;          // starts of the app so far (the one running now included)
    bool decoded = false;      // a good decode has happened at some time
    int choice = 0;            // 0 none yet, 1 starred, 2 never again
    int shown = 0;             // times the card was shown
    int later = 0;             // "Maybe later" clicks
    long lastShownDay = 0;     // the day number (days since 1970) of the last show
    long today = 0;            // set by the app before each call
    bool shownThisSession = false;   // not saved
};

// Records the event and returns true when the card should be shown now (and counts it as shown).
bool starShouldShow(StarState& s, StarEvent e);
// The click: Star and Never end it; Later schedules a return (30 days, then 60), and the third Later ends it too.
void starOnChoice(StarState& s, StarChoice c);
// The address opened on "Star on GitHub".
const char* starUrl();

// The saved numbers, so the app and the tests agree on the pref names.
struct StarKeys { static constexpr const char* on = "starPromptOn"; static constexpr const char* launches = "starLaunches"; static constexpr const char* decoded = "starDecoded";
                  static constexpr const char* choice = "starChoice"; static constexpr const char* shown = "starShown"; static constexpr const char* later = "starLater";
                  static constexpr const char* lastDay = "starLastDay"; };
}
