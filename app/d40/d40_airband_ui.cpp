// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airband: ATIS and VOLMET channels from the live transcripts: the information letter, how old it is, a "new ATIS" notice, the airports a VOLMET names. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../transcribe/transcribe_app.h"
#include "../ui/typography.h"
#include "dect2/airband/airband_atis.h"
#include <cstdio>
#include <ctime>
#include <set>

namespace {
dect2::AtisTracker tracker;
std::set<std::string> volmet;
int64_t lastWall = 0;
std::string lastChange;
}  // namespace

void d40AirbandCard() {
    if (!typo::collapsingHeader(trw("ATIS letter tracker and VOLMET"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Reads the live transcripts of the airband channels (Transcripts need the speech model). A channel whose label says ATIS or VOLMET, or that read an information letter twice, is an ATIS channel: the current letter, its age, and a notice when the letter changes. A VOLMET read-out lists the airports it named."));
    std::string status; int level = 0; status = tx::statusLine(&level);
    const int64_t now = (int64_t)std::time(nullptr);
    for (const auto& l : tx::airbandSince(lastWall)) {
        if (l.wall <= lastWall) continue;
        lastWall = l.wall;
        if (tracker.feed(l.freqHz, l.channel, l.text, (double)l.wall)) { char b[96]; snprintf(b, sizeof b, "%.3f MHz: new ATIS", l.freqHz / 1e6); lastChange = b; }
        for (const auto& s : dect2::volmetStations(l.text)) volmet.insert(s);
    }
    const auto ch = tracker.atisChannels();
    if (ch.empty()) { ImGui::TextDisabled("%s", tr("No ATIS channel found yet.")); ImGui::TextDisabled("%s", status.c_str()); }
    for (const auto& c : ch) {
        if (c.letter) ImGui::Text("%.3f MHz: %s %c, %.0f %s", c.freqHz / 1e6, tr("information"), c.letter, ((double)now - c.sinceS) / 60.0, tr("minutes ago"));
        else ImGui::Text("%.3f MHz: %s", c.freqHz / 1e6, tr("letter not read yet"));
    }
    if (!lastChange.empty()) ImGui::TextColored(pal::warnAmber(), "%s", lastChange.c_str());
    if (!volmet.empty()) {
        std::string s;
        for (const auto& v : volmet) s += (s.empty() ? "" : ", ") + v;
        ImGui::PushTextWrapPos(0); ImGui::Text("%s: %s", tr("VOLMET airports"), s.c_str()); ImGui::PopTextWrapPos();
    }
}
