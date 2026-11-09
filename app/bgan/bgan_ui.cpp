// Inmarsat BGAN screens: one click (Scan 1525 - 1559 MHz) walks the band and lists every forward bearer found, with bearer type, satellite, spot beam, network and
// load, plus the plain-language log of the system information the network broadcasts. Signalling and structure only; see docs/BGAN-3.7.md.
#include "app.h"
#include "i18n/i18n.h"
#include "dect2/bgan/bgan_rx.h"
#include "dect2/bgan/bgan_gen.h"
#include "platform/platform.h"
#include "ui/kit/kit.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/mode_panel.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace {

struct State {
    bool scanning = false, wasRunning = false;
    int step = 0, steps = 0;
    double stepStart = 0, stepMhz = 1.5, dwell = 7;
    double oldMhz = 0;
};
State S;

bool live(const App& a) { return a.rx.standard == 246; }

void notRunning(const App& a) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); }

void intro() {
    ImGui::TextWrapped("%s", tr("BGAN is the Inmarsat data service. This lists its forward carriers (the satellite to terminal side) and reads the system information each one broadcasts: which satellite, which spot beam, which network, how busy the return channels are. It does not decode voice or data, does not show who is connected, and has only been tested on signals made by the test generator, never on a real BGAN recording."));
}

void startScan(App& a) {
    const dect2::ModeTuning mt = dect2::bganTuning();
    (void)mt;
    double msps = a.engine.running() ? a.rx.bgan.rate / 1e6 : 2.0;
    if (msps <= 0) msps = 2.0;
    S.stepMhz = std::max(0.5, msps * 0.8);
    S.steps = int(std::ceil((1559.0 - 1525.0) / S.stepMhz));
    S.step = 0;
    S.scanning = true;
    S.stepStart = ImGui::GetTime();
    S.oldMhz = a.freqMhz;
    a.freqMhz = 1525.0 + S.stepMhz / 2;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (a.engine.running()) a.engine.retune(a.tune);
    a.engine.bgan().setScanStatus(true, 1, S.steps);
}

void header(App& a) {
    bool run = a.engine.running();
    if (!run) ImGui::BeginDisabled();
    if (S.scanning) {
        if (ImGui::Button(tr("Stop##bganscan"))) { S.scanning = false; a.engine.bgan().setScanStatus(false, 0, 0); }
    } else if (ImGui::Button(tr("Scan 1525 - 1559 MHz##bgan"))) startScan(a);
    if (!run) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Automatic: steps the radio across the band, a few seconds at each step, and lists every BGAN carrier it can identify."));
    flowNext(6 * gUi);
    if (ImGui::Button(tr("Clear##bgan"))) a.engine.bgan().clearResults();
    flowNext(10 * gUi);
    if (live(a)) {
        const dect2::BganTelemetry& t = a.rx.bgan;
        if (S.scanning) ImGui::TextDisabled(tr("scanning step %d of %d"), S.step + 1, S.steps);
        else ImGui::TextDisabled("%.2f Msps, %d carriers", t.rate / 1e6, t.carriers);
    }
}

const char* stateText(int s) { return s == 2 ? "decoded" : s == 1 ? "frame sync" : "seen"; }

void carriersTable(App& a) {
    if (!live(a)) return;
    const dect2::BganTelemetry& t = a.rx.bgan;
    if (t.rows.empty()) { ImGui::TextDisabled("%s", tr("No carriers yet. Press Scan to walk the band.")); return; }
    if (ImGui::BeginTable("##bganrows", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) {
        for (const char* h : { tr("MHz"), tr("Bearer"), tr("State"), tr("C/N0"), tr("Spot"), tr("Beam"), tr("Satellite"), tr("Network"), tr("Load") }) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        char b[64];
        for (const auto& r : t.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); snprintf(b, sizeof b, "%.4f", r.freqHz / 1e6); ImGui::TextUnformatted(r.freqHz > 0 ? b : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.kind == 1 ? (r.bearer + " " + r.subtype).c_str() : r.kind == 2 ? tr("GMR-2 class (IsatPhone type)") : tr("not identified"));
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr(stateText(r.state)));
            ImGui::TableNextColumn(); if (r.cn0Dbhz > 0) ImGui::Text("%.1f", (double)r.cn0Dbhz); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (r.spotBeam >= 0) ImGui::Text("%d", r.spotBeam); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.beamType == 0 ? tr("global") : r.beamType == 1 ? tr("regional") : r.beamType == 2 ? tr("narrow") : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.satellite.empty() ? "-" : r.satellite.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.plmn.empty() ? "-" : r.plmn.c_str());
            ImGui::TableNextColumn(); if (r.load >= 0) ImGui::Text("%.0f%%", r.load * 100); else ImGui::TextUnformatted("-");
            if (ImGui::IsItemHovered() && r.structureOnly) ImGui::SetTooltip("%s", tr("Frame structure only: this bearer type's tables are not published, or it is below the code's limit"));
        }
        ImGui::EndTable();
    }
}

void logList(App& a) {
    if (!live(a)) return;
    const dect2::BganTelemetry& t = a.rx.bgan;
    ImGui::TextDisabled("%s", tr("What the network broadcasts"));
    if (ImGui::BeginChild("##bganlog", ImVec2(0, 0), true)) {
        for (const auto& l : t.log) ImGui::TextUnformatted(l.c_str());
    }
    ImGui::EndChild();
}

void beams(App& a) {
    if (!live(a) || a.rx.bgan.beams.empty()) return;
    ImGui::TextDisabled("%s", tr("Primary bearers announced"));
    for (const auto& b : a.rx.bgan.beams) ImGui::Text("spot %d: %.4f MHz (%s)", b.spot, b.mhz, b.bearer.c_str());
}

void tab(App& a) {
    kit::modeHeader({245, "Inmarsat BGAN", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    intro();
    header(a);
    ImGui::Spacing();
    if (!live(a)) { notRunning(a); return; }
    carriersTable(a);
    ImGui::Spacing();
    logList(a);
}

void command(App& a) {
    kit::modeHeader({245, "Inmarsat BGAN", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    if (!live(a)) { intro(); notRunning(a); return; }
    modepanel::panes({{"##bgancar", tr("Carriers"), [&a] { header(a); ImGui::Spacing(); carriersTable(a); }, 1.2f, 420.f},
                      {"##bganlog2", tr("System information"), [&a] { beams(a); ImGui::Spacing(); logList(a); }, 1.f, 260.f}}, 340.f);
}

void receiver(App& a) {
    if (!live(a)) { notRunning(a); return; }
    const dect2::BganTelemetry& t = a.rx.bgan;
    ImGui::Text("%s: %d", tr("BGAN bearers"), t.bearers);
    if (t.identifiedGmr) ImGui::Text("%s: %d", tr("GMR-2 class carriers"), t.identifiedGmr);
    if (!t.satellites.empty()) ImGui::Text("%s: %s", tr("Satellites"), t.satellites.c_str());
    if (!t.network.empty()) ImGui::Text("%s: %s", tr("Network"), t.network.c_str());
    ImGui::Text("%s: %llu / %llu", tr("FEC blocks good / bad"), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
}

void list(App&) {}
void decoder(App& a, bool&) { header(a); }

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    int o = std::clamp(sc.modeOpt[0], 0, dect2::bgan::bearerCount());
    ImGui::SetNextItemWidth(170 * gUi);
    const char* pv = o == 0 ? "Group of carriers" : dect2::bgan::bearer(o - 1).name;
    if (ImGui::BeginCombo("##bgansyn", tr(pv))) {
        if (ImGui::Selectable(tr("Group of carriers"), o == 0)) { sc.modeOpt[0] = 0; changed = true; }
        for (int i = 0; i < dect2::bgan::bearerCount(); i++) if (ImGui::Selectable(dect2::bgan::bearer(i).name, o == i + 1)) { sc.modeOpt[0] = i + 1; changed = true; }
        ImGui::EndCombo();
    }
    float cn0 = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 70.f;
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::SliderFloat("##bgancn0", &cn0, 40.f, 80.f, "C/N0 %.0f dB-Hz")) { sc.modeVal[0] = cn0; changed = true; }
}

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && S.scanning && ImGui::GetTime() - S.stepStart > S.dwell) {
        S.step++;
        if (S.step >= S.steps) {
            S.scanning = false;
            a.engine.bgan().setScanStatus(false, 0, 0);
        } else {
            a.freqMhz = 1525.0 + S.stepMhz * (S.step + 0.5);
            a.tune.centerHz = a.freqMhz * 1e6;
            a.engine.retune(a.tune);
            a.engine.bgan().setScanStatus(true, S.step + 1, S.steps);
            S.stepStart = ImGui::GetTime();
        }
    }
    if (!run && S.wasRunning) { S.scanning = false; }
    S.wasRunning = run;
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    lamp("IQ", run ? 1 : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("BGAN"), !on ? 0 : a.rx.bgan.dataValid ? 1 : a.rx.bgan.state == 1 ? 2 : 2); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::bganSummary(a.rx.bgan).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Inmarsat BGAN";
    if (live(a)) l2 = dect2::bganSummary(a.rx.bgan);
}

} // namespace

extern const ModeUi kBganUi;
const ModeUi kBganUi = {
    .sideTitle = "BGAN",
    .tabName = "BGAN",
    .tabIcon = Ic::Wave,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .command = command,
};
