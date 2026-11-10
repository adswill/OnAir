// Insights (View > Insights; docs/SMART5-ham-3.9.md, SMART5-voice-3.9.md, SMART5-pager-3.9.md, SMART5-hfutil-3.9.md): what the smart analysers of
// core/include/dect2/insight found in this receiver's own decodes. On by default; they are fed through the automation analyser hook and the FT8 view,
// and every finding also goes on as an automation event "insight.<kind>". Offline; per talkgroup or system, never per radio.
#include "../app.h"
#include "dect2/insight/live.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <set>

namespace in = dect2::insight;

namespace {

struct State {
    bool started = false, on = true;
    double nextTick = 0, nextSave = 0, lastFtx = 0;
    std::deque<in::Insight> rows;   // newest last
    std::set<std::pair<double, std::string>> ftxSeen;   // FT8 decodes already handed to the hub (slot, message)
} S;

std::string heatPath() { return plat::dataDir() + "/insight_heatmap.txt"; }

int tzOffset() {
    const time_t now = time(nullptr);
    tm lt = *localtime(&now);
    tm gt = *gmtime(&now);
    gt.tm_isdst = lt.tm_isdst;
    return (int)difftime(mktime(&lt), mktime(&gt));
}

std::string utc(double t, bool date = false) {
    const time_t tt = (time_t)t;
    tm g = *gmtime(&tt);
    char b[32];
    strftime(b, sizeof b, date ? "%Y-%m-%d %H:%M" : "%H:%M", &g);
    return b;
}

void station() {
    in::live().setStation(plat::prefs().getS("ham.call", ""), plat::prefs().getS("ham.grid", ""));
}

void start() {
    S.started = true;
    S.on = plat::prefs().getB("insight.on", true);
    in::live().setTzOffset(tzOffset());
    in::live().loadHeatmap(heatPath());
    station();
    in::live().enable(S.on);
}

void tick(App& a) {
    if (!S.started) start();
    const double now = (double)time(nullptr);
    if (!S.on || now < S.nextTick) return;
    S.nextTick = now + 30;
    station();
    for (auto& i : in::live().tick(now, {a.freqMhz * 1e6})) {
        S.rows.push_back(i);
        if (S.rows.size() > 500) S.rows.pop_front();
    }
    if (now >= S.nextSave) { if (S.nextSave > 0) in::live().saveHeatmap(heatPath()); S.nextSave = now + 600; }
}

ImVec4 sevColour(int s) { return s >= 2 ? pal::badRed() : s == 1 ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_Text); }

void findingsTab() {
    if (S.rows.empty()) { ImGui::TextDisabled("%s", tr("Nothing yet. Findings appear here as the decoders hear things.")); return; }
    if (!ImGui::BeginTable("##insrows", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Time (UTC)"), ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(tr("Area"), ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(tr("Finding"));
    ImGui::TableHeadersRow();
    for (auto it = S.rows.rbegin(); it != S.rows.rend(); ++it) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(utc(it->time, true).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(it->area.c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(sevColour(it->severity), "%s", it->text.c_str());
    }
    ImGui::EndTable();
}

void hamTab(in::Hub& h) {
    if (plat::prefs().getS("ham.call", "").empty())
        ImGui::TextDisabled("%s", tr("Set your call sign and locator in the Ham radio mode to see who hears you."));
    const auto rows = h.whoHearsMe().list();
    ImGui::SeparatorText(tr("Who hears me (FT8, FT4, JS8)"));
    if (rows.empty()) ImGui::TextDisabled("%s", tr("No station has called or answered you yet."));
    else if (ImGui::BeginTable("##whm", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Band")); ImGui::TableSetupColumn(tr("Their report"));
        ImGui::TableSetupColumn(tr("Your SNR")); ImGui::TableSetupColumn(tr("Balance")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Last (UTC)"));
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.call.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.band.c_str());
            ImGui::TableNextColumn(); if (r.hasReport) ImGui::Text("%+d dB", r.theirReport); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%+.0f dB", r.mySnr);
            ImGui::TableNextColumn(); if (r.hasReport) ImGui::Text("%+d dB", r.balance()); else ImGui::TextDisabled("-");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Their report of you minus your SNR of them: above 0 they hear you better than you hear them"));
            ImGui::TableNextColumn(); if (r.distanceKm >= 0) ImGui::Text("%.0f km %s", r.distanceKm, in::compass8(r.bearing)); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(utc(r.lastTime).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(tr("Antenna pattern from decodes"));
    const auto sec = h.antennaPattern().sectors(10);
    ImGui::TextDisabled(tr("%zu located decodes; the distance is taken out, what is left per direction:"), h.antennaPattern().size());
    for (const auto& s : sec) {
        if (s.count < 10) continue;
        ImGui::Text("%3d-%3d deg  %+5.1f dB  +/- %.1f  (%d)", s.fromDeg, s.fromDeg + 30, s.meanDb, s.stdErrDb, s.count);
    }
    ImGui::SeparatorText(tr("APRS network health"));
    int over = 0, path = 0;
    for (const auto& s : h.aprsHealth().stations()) { over += s.overBeacon; path += !s.pathIssue.empty(); }
    ImGui::Text(tr("%d stations beacon too often, %d use an obsolete or long path"), over, path);
    for (const auto& s : h.aprsHealth().stations())
        if (s.overBeacon || !s.pathIssue.empty())
            ImGui::BulletText("%s%s%s", s.call.c_str(), s.overBeacon ? (std::string("  ") + tr("every") + " " + in::AprsHealth::secText(s.medianSec)).c_str() : "",
                              s.pathIssue.empty() ? "" : ("  " + s.pathIssue).c_str());
}

void voiceTab(in::Hub& h, double now) {
    const auto tgs = h.heatmap().talkgroups();
    ImGui::SeparatorText(tr("Talkgroups: busiest hours (local time)"));
    if (tgs.empty()) ImGui::TextDisabled("%s", tr("No talkgroup calls yet (DMR, P25, NXDN, TETRA, trunked systems)."));
    else if (ImGui::BeginTable("##tgs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Talkgroup")); ImGui::TableSetupColumn(tr("Busiest")); ImGui::TableSetupColumn(tr("Next busy"));
        ImGui::TableHeadersRow();
        for (const auto& tg : tgs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tg.c_str());
            ImGui::TableNextColumn();
            std::string b;
            for (const auto& x : h.heatmap().busiest(tg, 3)) {
                char t[48];
                snprintf(t, sizeof t, "%s%s %02d:00 (%.0f s)", b.empty() ? "" : ", ", in::weekdayName(x.first / 24), x.first % 24, x.second);
                b += t;
            }
            ImGui::TextUnformatted(b.c_str());
            ImGui::TableNextColumn();
            const double nb = h.heatmap().nextBusy(tg, now);
            if (nb > 0) ImGui::Text(tr("in %.0f h"), (nb - now) / 3600); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText(tr("Trunk load (Erlang B)"));
    for (const auto& kv : h.trunks()) {
        const auto& hrs = kv.second.hours();
        if (hrs.empty()) { ImGui::BulletText(tr("%s: %d channels seen, the first hour is not over yet"), kv.first.c_str(), kv.second.channels()); continue; }
        const auto& r = hrs.back();
        ImGui::BulletText(tr("%s, hour from %s UTC: %d calls, %.2f Erlang carried, %.2f offered, %d channels, blocking %.1f %%, peak %d at once"), kv.first.c_str(),
                          utc(r.start).c_str(), r.calls, r.carriedErl, r.offeredErl, r.channels, r.blocking * 100, r.peakConcurrent);
    }
}

void pagerTab(in::Hub& h) {
    const auto v = h.pagerPatterns().automaticList();
    ImGui::SeparatorText(tr("Automatic and test pages"));
    if (v.empty()) ImGui::TextDisabled("%s", tr("None found yet: a capcode needs seven pages of the same text at a steady pace."));
    for (const auto& x : v) ImGui::BulletText(tr("%s every %s"), x.first.c_str(), in::PagerPatterns::dur(x.second).c_str());
}

void hfTab(in::Hub& h) {
    const auto v = h.schedule().all();
    ImGui::SeparatorText(tr("Learned broadcast schedules (UTC)"));
    if (v.empty()) ImGui::TextDisabled("%s", tr("None yet: a slot is learned after decodes at the same time on three days."));
    for (const auto& l : v) {
        std::string t;
        for (int m : l.minutes) { char b[24]; snprintf(b, sizeof b, "%s%02d:%02d", t.empty() ? "" : " ", m / 60, m % 60); t += b; }
        ImGui::BulletText("%s %.1f kHz: %s", l.station.c_str(), l.freqHz / 1e3, t.c_str());
    }
}

}  // namespace

// the FT8 / FT4 / JS8 view hands its decodes over (hfdig_ftx_ui.cpp); only the ones newer than the last seen
void insightFeedFtx(double slotUtc, double dialHz, const std::string& msg, float snr) {
    if (!S.on || slotUtc < S.lastFtx - 300) return;   // the list is handed over every frame: older ones were seen before
    if (!S.ftxSeen.insert({slotUtc, msg}).second) return;
    if (slotUtc > S.lastFtx) {
        S.lastFtx = slotUtc;
        while (!S.ftxSeen.empty() && S.ftxSeen.begin()->first < S.lastFtx - 600) S.ftxSeen.erase(S.ftxSeen.begin());
    }
    in::live().onFtx(slotUtc, in::hamBand(dialHz), msg, snr);
}

void insightWindow(App& a) {
    tick(a);
    if (!a.insightOpen) return;
    firstWindowSize(760 * gUi, 460 * gUi);
    if (!ImGui::Begin(trw("Insights"), &a.insightOpen)) { ImGui::End(); return; }
    const double now = (double)time(nullptr);
    if (ImGui::BeginTabBar("##instabs")) {
        if (ImGui::BeginTabItem(tr("Findings"))) { findingsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr("Ham"))) { in::live().with([](in::Hub& h) { hamTab(h); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr("Voice"))) { in::live().with([now](in::Hub& h) { voiceTab(h, now); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr("Pagers"))) { in::live().with([](in::Hub& h) { pagerTab(h); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(tr("HF utility"))) { in::live().with([](in::Hub& h) { hfTab(h); }); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        if (ImGui::Checkbox(tr("Analyse decodes"), &S.on)) { plat::prefs().setB("insight.on", S.on); in::live().enable(S.on); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Who hears me, antenna pattern, APRS health, talkgroup activity, trunk load, automatic pages, overdue broadcasts. All of it stays on this computer."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Clear findings"))) S.rows.clear();
    }
    ImGui::End();
}
