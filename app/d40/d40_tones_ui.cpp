// Tones: dialled numbers and codes assembled from the DTMF keys, and the guess which tone system is on the channel. docs/WIRING-4.0.md
#include "d40_ui.h"
#include "../ui/typography.h"
#include "dect2/tones/dtmf_numbers.h"
#include "dect2/tones/tone_ident.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <ctime>

namespace {
dect2::DtmfNumbers dtmf;
int64_t lastWall = 0;
double lastTime = 0;
std::deque<dect2::HeardTone> heard;   // voice band tones, newest last, 20 s
double openHz = 0, openStart = 0;
}  // namespace

void d40TonesFeed(const dect2::TonesTelemetry& t) {
    const double now = (double)std::time(nullptr);
    if (t.timeSec < lastTime) { dtmf = dect2::DtmfNumbers(); lastWall = 0; heard.clear(); openHz = 0; }
    lastTime = t.timeSec;
    int64_t newest = lastWall;
    for (auto it = t.events.rbegin(); it != t.events.rend(); ++it) {   // oldest first
        if (it->wall <= lastWall || it->kind != dect2::TkDtmf) continue;
        double k = 0;
        for (char c : it->text) { if (c == ' ') continue; dtmf.key(c, (double)it->wall + 0.1 * k); k += 1; }
        newest = std::max(newest, it->wall);
    }
    lastWall = newest;
    dtmf.tick(now);
    // tone tracker: a pure tone that holds still is one HeardTone
    const double ms = t.timeSec * 1000.0;
    if (t.toneHz > 0 && (openHz == 0 || std::fabs(t.toneHz - openHz) > 6.0)) {
        if (openHz > 0) heard.push_back({openHz, ms - openStart, openStart});
        openHz = t.toneHz; openStart = ms;
    } else if (t.toneHz <= 0 && openHz > 0) {
        heard.push_back({openHz, ms - openStart, openStart}); openHz = 0;
    }
    while (!heard.empty() && ms - heard.front().startMs > 20000) heard.pop_front();
    if (heard.size() > 64) heard.pop_front();
}

void d40TonesCard(const dect2::TonesTelemetry& t) {
    if (!typo::collapsingHeader(trw("Tone system and dialled numbers"), 0)) return;
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The tones heard in the last 20 seconds are matched against CTCSS, DTMF, the five-tone plans (ZVEI, CCIR, EEA, EIA, DZVEI) and SELCAL. It is a guess to pick the right decoder, not a lock. Dialled keys are joined into numbers and codes; a repeat within 2 minutes is counted, not listed."));
    std::vector<dect2::HeardTone> v(heard.begin(), heard.end());
    if (t.ctcssIdx >= 0) v.push_back({t.ctcssHz, 1000.0, t.timeSec * 1000.0});
    if (openHz > 0) v.push_back({openHz, t.timeSec * 1000.0 - openStart, openStart});
    const auto g = dect2::identifyToneSystem(v);
    if (g.empty()) ImGui::TextDisabled("%s", tr("No tone system recognised yet."));
    for (size_t i = 0; i < g.size() && i < 3; i++) ImGui::Text("%s: %s (%.0f %%)", g[i].system.c_str(), g[i].detail.c_str(), g[i].score * 100);
    ImGui::Separator();
    const auto& e = dtmf.entries();
    if (e.empty()) { ImGui::TextDisabled("%s", tr("No keys dialled yet.")); return; }
    for (size_t i = e.size(); i-- > 0 && e.size() - i <= 12;) {
        const auto& x = e[i];
        ImGui::Text("%s  [%s]%s", x.keys.c_str(), x.kind.c_str(), x.times > 1 ? (" x" + std::to_string(x.times)).c_str() : "");
    }
}
