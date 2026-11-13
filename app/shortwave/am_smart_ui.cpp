// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AM on shortwave and medium wave: the census of the capture, what is this station (an estimate), and playing the better of two frequencies. docs/SMART-fmam-3.7.md
#include "app.h"
#include "../ui/typography.h"
#include "transcribe/transcribe_app.h"
#include <mutex>
#include "dect2/general/am_diversity.h"
#include "dect2/general/sw_ident.h"
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace {
dect2::SwIdent gId;
dect2::IntervalDetector gInterval;
std::mutex gIntervalMu;
bool gIdInit = false;
std::map<int, dect2::FadeTracker> gFade;   // per listened kHz
double gLastFade = 0;

void installTap(App& a) { a.engine.general().setAudioTap([](const float* x, size_t n) { std::lock_guard<std::mutex> lk(gIntervalMu); gInterval.push(x, n); }); }

void idInit(App& a) {
    if (gIdInit) return;
    gIdInit = true;
    gInterval.configure(48000);
    // the user's own schedule file (EiBi or HFCC style), never one shipped with the program
    std::string path = plat::prefs().getS("swSchedPath", "");
    if (!path.empty()) { std::ifstream f(path); if (f) { std::stringstream b; b << f.rdbuf(); gId.addSchedule(dect2::parseEibi(b.str())); } }
    // the taught interval signals
    std::ifstream t(plat::dataDir() + "/sw_interval_signals.tsv");
    std::string line;
    while (std::getline(t, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::vector<float> p; std::stringstream s(line.substr(tab + 1)); float v; while (s >> v) p.push_back(v);
        if (!p.empty()) gId.teachInterval(line.substr(0, tab), p);
    }
    installTap(a);
}
}

void amSmartPanel(App& a) {
    if (!typo::collapsingHeader(trw("Band census"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    idInit(a);
    bool on = a.engine.amCensusOn();
    if (ImGui::Checkbox(tr("Find every station in the capture"), &on)) a.engine.setAmCensus(on);
    const double dial = generalListenHz(a);
    const auto rows = a.engine.amCensusRows();
    if (on && !rows.empty() && ImGui::BeginTable("##amcensus", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 200 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("kHz"); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Fading"));
        ImGui::TableSetupColumn(tr("Advice")); ImGui::TableSetupColumn(""); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.khz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::amKindName(r.kind));
            ImGui::TableNextColumn(); ImGui::Text("%.0f dB", r.snrDb);
            ImGui::TableNextColumn(); if (r.fading) ImGui::Text("%.0f dB%s", r.fadeDb, r.selective ? " sel" : ""); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.ecss > 0 ? tr("upper sideband") : r.ecss < 0 ? tr("lower sideband") : "");
            ImGui::TableNextColumn();
            ImGui::PushID((int)r.khz);
            if (r.kind == dect2::AmKind::Drm) {   // a DRM-shaped channel: open the DRM mode on it
                if (ImGui::SmallButton(tr("Decode DRM"))) {
                    const bool was = a.engine.running();
                    if (was) a.engine.stop();
                    ::setMode(a, 10, 0);
                    a.freqMhz = r.khz / 1e3; a.tune.centerHz = a.freqMhz * 1e6;
                    savePrefs(a);
                    if (was) startReceiver(a);
                    ImGui::PopID(); ImGui::EndTable();
                    return;
                }
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(tr("Listen"))) generalSetListenHz(a, r.khz * 1e3);
            ImGui::TableNextColumn();
            if (!a.engine.amDiversityOn() && std::fabs(r.khz * 1e3 - dial) > 500 && ImGui::SmallButton(tr("Also on"))) a.engine.setAmDiversity(true, r.khz * 1e3 - dial);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Play this frequency together with the listened one: the same programme on two frequencies, the clearer one is heard."));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (a.engine.amDiversityOn()) {
        auto& d = a.engine.amDiversity();
        dect2::GeneralTelemetry t1, t2;
        a.engine.general().telemetry(t1, 0); a.engine.generalExtra(0).telemetry(t2, 0);
        d.setSnr(t1.amActive ? t1.amSnrDb : t1.snrDb, t2.amActive ? t2.amSnrDb : t2.snrDb);
        ImGui::Text("%s: %s  (%d %s, %.0f ms)", tr("Two frequencies"), d.active() == 0 ? tr("first") : tr("second"), d.switches(), tr("changes"), d.delaySamples() / 48.0);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Stop"))) { a.engine.setAmDiversity(false); installTap(a); }
    }
    // the fade of the listened carrier and when the next one is due
    {
        const double now = ImGui::GetTime();
        dect2::GeneralTelemetry t;
        if (now - gLastFade >= 0.25 && a.engine.running() && a.engine.general().telemetry(t, 0) && t.amActive) {
            gLastFade = now;
            auto& ft = gFade[(int)std::lround(dial / 1e3)];
            ft.push(t.amSnrDb);
        }
        auto it = gFade.find((int)std::lround(dial / 1e3));
        if (it != gFade.end() && it->second.depthDb() > 3) {
            const auto& f = it->second;
            ImGui::TextDisabled("%s %.0f dB%s", tr("Fading"), f.depthDb(), f.bestMoment() ? tr(", good moment now") : "");
            if (f.nextFadeIn() >= 0) { ImGui::SameLine(); ImGui::TextDisabled("(%s %.0f s)", tr("next dip in"), f.nextFadeIn()); }
        }
    }
    // what is this station: an estimate, from the audio and what the user installed
    {
        std::vector<float> print; bool rep = false; float strength = 0;
        { std::lock_guard<std::mutex> lk(gIntervalMu); rep = gInterval.repeating(); strength = gInterval.strength(); if (rep) print = gInterval.fingerprint(); }
        float prob = 0; const std::string lang = tx::latestLanguage(dial, &prob);
        dect2::SwIdentInput in;
        in.khz = dial / 1e3;
        const std::time_t now = std::time(nullptr);
        std::tm u = *std::gmtime(&now);
        in.utcMinute = u.tm_hour * 60 + u.tm_min; in.weekdayMon0 = (u.tm_wday + 6) % 7;
        in.language = lang; in.languageProb = prob; in.intervalSignal = rep; in.intervalPrint = print;
        const auto g = gId.guess(in);
        if (!g.empty()) ImGui::Text("%s: %s  (%s, %s)", tr("Probably"), g[0].station.c_str(), g[0].why.c_str(), tr("estimated"));
        if (rep) { ImGui::SameLine(); ImGui::TextDisabled("%s %.0f%%", tr("interval signal"), 100 * strength); }
        if (ImGui::TreeNode(trw("Identify: settings"))) {
            static char path[512]; static bool loaded = false;
            if (!loaded) { loaded = true; snprintf(path, sizeof path, "%s", plat::prefs().getS("swSchedPath", "").c_str()); }
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", tr("No station list ships with the program. Give your own schedule file (EiBi or HFCC style). The language comes from your installed speech model."));
            ImGui::PopTextWrapPos();
            ImGui::InputText(tr("Schedule file"), path, sizeof path);
            if (ImGui::SmallButton(tr("Load"))) { plat::prefs().setS("swSchedPath", path); std::ifstream f(path); if (f) { std::stringstream b; b << f.rdbuf(); auto e = dect2::parseEibi(b.str()); gId.addSchedule(std::move(e)); } }
            ImGui::SameLine(); ImGui::TextDisabled("%zu %s", gId.scheduleSize(), tr("entries"));
            static char name[64] = "";
            ImGui::InputText(tr("Name of this interval signal"), name, sizeof name);
            ImGui::BeginDisabled(!rep || !name[0]);
            if (ImGui::SmallButton(tr("Learn"))) {
                gId.teachInterval(name, print);
                std::ofstream f(plat::dataDir() + "/sw_interval_signals.tsv", std::ios::app);
                if (f) { f << name << "\t"; for (float v : print) f << v << " "; f << "\n"; }
                name[0] = 0;
            }
            ImGui::EndDisabled();
            ImGui::TreePop();
        }
    }
}
