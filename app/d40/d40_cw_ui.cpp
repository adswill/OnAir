// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW: callsigns heard and who is calling CQ, built from the text of every signal. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/ham/ham_callsign.h"
#include <cstdio>
#include <ctime>
#include <map>

namespace {
dect2::HamCallHeard heard;
std::map<int, uint64_t> fedChars;   // per signal id: characters already fed
std::map<int, std::string> pending; // text after the last space, not yet fed
double lastTime = 0;
bool cqOnly = false, loaded = false;
}  // namespace

void d40CwCard(App& a, const dect2::CwTelemetry& t) {
    if (!loaded) { loaded = true; cqOnly = plat::prefs().getI("d40CwHeardCq", 0) != 0; }
    if (t.timeSec < lastTime) { heard = dect2::HamCallHeard(); fedChars.clear(); pending.clear(); }
    lastTime = t.timeSec;
    const double now = (double)std::time(nullptr);
    for (const auto& s : t.signals) {
        uint64_t& done = fedChars[s.id];
        if (s.chars <= done) continue;
        const size_t add = (size_t)std::min<uint64_t>(s.chars - done, s.text.size());
        done = s.chars;
        std::string& p = pending[s.id];
        p += s.text.substr(s.text.size() - add);
        const size_t sp = p.find_last_of(' ');   // only finished words: a call split over two readings is read when complete
        if (sp != std::string::npos) { heard.feed(p.substr(0, sp), now); p.erase(0, sp + 1); }
        if (p.size() > 160) p.clear();
    }
    if (!typo::collapsingHeader(trw("Stations heard and CQ callers"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Callsigns found in the decoded text of all signals in the window, newest first, with the number of times seen. CQ marks a station that was read calling CQ. Garbled text can give false calls: check by ear."));
    if (ImGui::Checkbox(tr("CQ callers only##d40cwh"), &cqOnly)) plat::prefs().setI("d40CwHeardCq", cqOnly ? 1 : 0);
    ImGui::SameLine(); ImGui::TextDisabled("%zu %s", heard.size(), tr("stations heard"));
    int shown = 0;
    for (const auto& e : heard.list()) {
        if (cqOnly && !e.cq) continue;
        if (++shown > 10) break;
        ImGui::Text("%-10s %3d x  %s", e.call.c_str(), e.count, e.cq ? "CQ" : "");
    }
    (void)a;
}
