// Band plans in the interface: the plan picker with import / export / delete (scan tab), the TV channel selector is tvchan_ui.cpp (toolbar) and the scan
// ranges taken from the active plan. The plans, their files and their validation are in core (dect2/bandplan/bandplan.h); the format is in docs/band-plans.md.
#include "../app.h"
#include "../platform/platform.h"
#include "../ui/ui_common.h"
#include "dect2/bandplan/bandplan.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bp = dect2::bandplan;

namespace {
bp::Registry& reg() {
    static bp::Registry r = [] { bp::Registry x; x.setUserDir(plat::dataDir() + "/bandplans"); for (const std::string& l : x.log()) std::fprintf(stderr, "band plans: %s\n", l.c_str()); return x; }();
    return r;
}
// "" = automatic (from the language and the system's region), "none" = no plan: the built-in ranges
const bp::Plan* activePlan(const App& a) {
    if (a.bandPlan == "none") return nullptr;
    if (!a.bandPlan.empty()) if (const bp::Plan* p = reg().find(a.bandPlan)) return p;
    const std::string id = reg().defaultFor(a.uiLang.empty() ? plat::uiLanguage() : a.uiLang, bp::Registry::localeFromEnv());
    return id.empty() ? nullptr : reg().find(id);
}
// the names the plans use for the standard the TV mode decodes
std::vector<std::string> tvModes(const App& a) {
    if (a.family == 7) return {"DTMB"};
    if (a.isdbtMode) return {"ISDB-T"};
    if (a.atsc3Mode) return {"ATSC 3.0"};
    if (a.atscMode) return {"ATSC"};
    return {"DVB-T2", "DVB-T"};
}
std::vector<const bp::Band*> tvBands(const App& a) {
    std::vector<const bp::Band*> o;
    const bp::Plan* p = activePlan(a);
    if (!p) return o;
    for (const bp::Band& b : p->bands) {
        if (b.service != "tv" || b.count() == 0) continue;
        for (const std::string& m : tvModes(a)) if (std::find(b.modes.begin(), b.modes.end(), m) != b.modes.end()) { o.push_back(&b); break; }
    }
    return o;
}
struct Msg { std::string text; bool bad = false; double t = 0; } gMsg;
void say(const std::string& s, bool bad = false) { gMsg.text = s; gMsg.bad = bad; gMsg.t = ImGui::GetTime(); }
std::string lastSlash(const std::string& p) { const size_t i = p.find_last_of("/\\"); return i == std::string::npos ? p : p.substr(i + 1); }
} // namespace

std::vector<TvRange> tvBandRanges(App& a) {
    std::vector<TvRange> o;
    for (const bp::Band* b : tvBands(a)) {
        const std::vector<bp::Channel> ch = b->list(2000);
        if (ch.empty()) continue;
        TvRange r;
        r.startMhz = ch.front().centreHz / 1e6;
        r.stopMhz = ch.back().centreHz / 1e6;
        r.stepMhz = b->stepHz > 0 ? b->stepHz / 1e6 : ch.size() > 1 ? (ch[1].centreHz - ch[0].centreHz) / 1e6 : (b->bandwidthHz > 0 ? b->bandwidthHz / 1e6 : 1);
        r.bwMhz = b->bandwidthHz > 0 ? b->bandwidthHz / 1e6 : r.stepMhz;
        char l[160];
        const std::string first = ch.front().label.empty() ? std::string() : " ch " + ch.front().label + (ch.size() > 1 ? "-" + ch.back().label : std::string());
        std::snprintf(l, sizeof l, "%s%s (%.0f-%.0f MHz, %g MHz)", b->name.c_str(), first.c_str(), b->startHz / 1e6, b->endHz / 1e6, r.bwMhz);
        r.label = l;
        o.push_back(r);
    }
    return o;
}

void fmBandRange(const App& a, double& loMhz, double& hiMhz) {
    loMhz = 87.5; hiMhz = 108.0;
    if (const bp::Plan* p = activePlan(a))
        for (const bp::Band& b : p->bands)
            if (b.service == "fm" && b.startHz >= 30e6 && b.endHz <= 200e6) { loMhz = std::min(loMhz, b.startHz / 1e6); hiMhz = std::max(hiMhz, b.endHz / 1e6); }
}

// the active plan as labels on the spectrum: the numbered channels of the rasters (TV, DAB, FM ...) and the names of the plain ranges (ham, ISM ...)
void bandPlanLabelSource(const App& a, std::vector<autolabel::Label>& out) {
    const bp::Plan* p = activePlan(a);
    if (!p) return;
    auto modeKey = [](const bp::Band& b) -> std::string {
        for (const std::string& m : b.modes) {
            if (m == "DVB-T2" || m == "DVB-T") return "dvb";
            if (m == "ATSC") return "atsc";
            if (m == "ATSC 3.0") return "atsc3";
            if (m == "ISDB-T") return "isdbt";
            if (m == "FM") return "fm";
            if (m == "DAB" || m == "DAB+") return "dab";
        }
        return std::string();
    };
    for (const bp::Band& b : p->bands) {
        const int n = b.count();
        if (n == 0) {
            if (b.endHz <= b.startHz || b.name.empty()) continue;
            autolabel::Label l;
            l.mhz = (b.startHz + b.endHz) / 2e6; l.text = b.name; l.detail = b.note; l.src = autolabel::Band; l.prio = -0.5f; l.mode = modeKey(b); l.bwMhz = (b.endHz - b.startHz) / 1e6; l.allocation = true;
            out.push_back(l);
            continue;
        }
        if (n > 2000) continue;
        const std::string mk = modeKey(b);
        for (const bp::Channel& c : b.list(2000)) {
            autolabel::Label l;
            l.mhz = c.centreHz / 1e6;
            char t[48];
            if (c.label.empty()) std::snprintf(t, sizeof t, "%.4g", l.mhz);
            else std::snprintf(t, sizeof t, "%s%s", b.service == "tv" ? "CH " : "", c.label.c_str());
            l.text = t;
            char d[160]; std::snprintf(d, sizeof d, "%s  %.3f MHz", b.name.c_str(), l.mhz);
            l.detail = d; l.src = autolabel::Band; l.mode = mk; l.bwMhz = b.bandwidthHz > 0 ? b.bandwidthHz / 1e6 : 0;
            out.push_back(l);
        }
    }
}

bool bandPlanPicker(App& a) {
    bool changed = false;
    const bp::Plan* cur = activePlan(a);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Band plan"));
    ImGui::SameLine(0, 5 * gUi);
    std::string label = a.bandPlan == "none" ? tr("None (built-in ranges)") : cur ? cur->name : tr("None (built-in ranges)");
    if (a.bandPlan.empty() && cur) label = std::string(tr("Automatic")) + ": " + cur->name;
    ImGui::SetNextItemWidth(std::min(250 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##bandplan", label.c_str())) {
        auto pick = [&](const char* id) { a.bandPlan = id; savePrefs(a); changed = true; };
        if (ImGui::Selectable(tr("Automatic"), a.bandPlan.empty())) pick("");
        if (ImGui::Selectable(tr("None (built-in ranges)"), a.bandPlan == "none")) pick("none");
        ImGui::Separator();
        std::vector<const bp::Plan*> mine, built;
        for (const bp::Plan& p : reg().plans()) (p.builtin ? built : mine).push_back(&p);
        std::sort(built.begin(), built.end(), [](const bp::Plan* x, const bp::Plan* y) { return x->name < y->name; });
        for (const bp::Plan* p : built) if (ImGui::Selectable(p->name.c_str(), a.bandPlan == p->id)) pick(p->id.c_str());
        if (!mine.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("My band plans"));
            for (const bp::Plan* p : mine) if (ImGui::Selectable(p->name.c_str(), a.bandPlan == p->id)) pick(p->id.c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("The channel raster of your country: TV channels for the scan and the channel list, and more. Automatic picks a plan from the interface language and the system's region."));
    flowNext();
    if (ImGui::Button(tr("Import..."))) {
        const std::string path = openFileDialog();
        if (!path.empty()) {
            std::string id, err;
            bool replaced = false;
            if (reg().importFile(path, id, err, &replaced)) {
                const bp::Plan* p = reg().find(id);
                char b[200];
                const char* fmt = replaced ? tr("Replaced band plan \"%s\" (%zu bands).") : tr("Imported band plan \"%s\" (%zu bands).");
                std::snprintf(b, sizeof b, fmt, p ? p->name.c_str() : id.c_str(), p ? p->bands.size() : (size_t)0);
                a.bandPlan = id; savePrefs(a); changed = true;
                say(b);
            } else say(std::string(tr("Import failed: ")) + lastSlash(path) + ": " + err, true);
        }
    }
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Reads a band plan from a CSV or JSON file (format in docs/band-plans.md), checks it and keeps it in your settings folder. Nothing is changed when the file has a mistake; the message says where."));
    flowNext();
    ImGui::BeginDisabled(!cur);
    if (ImGui::Button(tr("Export..."))) {
        const std::string path = saveFileDialog((cur->id + ".json").c_str());
        if (!path.empty()) {
            std::string err;
            const bool csv = path.size() > 4 && (path.compare(path.size() - 4, 4, ".csv") == 0 || path.compare(path.size() - 4, 4, ".CSV") == 0);
            if (reg().exportFile(cur->id, path, csv, err)) say(std::string(tr("Exported to ")) + path);
            else say(err, true);
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapTooltip(380 * gUi, tr("Saves the plan as a JSON file, or as CSV when the name ends in .csv. A built-in plan makes a good starting point for your own."));
    flowNext();
    static std::string confirm;   // the first click asks, the second deletes
    static double confirmT = 0;
    const bool mineSel = cur && !cur->builtin;
    ImGui::BeginDisabled(!mineSel);
    const bool asking = mineSel && confirm == cur->id && ImGui::GetTime() - confirmT < 4;
    if (ImGui::Button(asking ? tr("Really delete?") : tr("Delete"))) {
        if (asking) {
            std::string err;
            const std::string name = cur->name;
            if (reg().remove(cur->id, err)) { a.bandPlan.clear(); savePrefs(a); changed = true; { char b[200]; std::snprintf(b, sizeof b, tr("Deleted band plan %s."), name.c_str()); say(b); }; }
            else say(err, true);
            confirm.clear();
        } else { confirm = cur->id; confirmT = ImGui::GetTime(); }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapTooltip(320 * gUi, tr("Deletes one of your own band plans. The built-in plans stay."));
    flowEnd();
    if (!gMsg.text.empty() && ImGui::GetTime() - gMsg.t < 30) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(gMsg.bad ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", gMsg.text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (cur && !cur->verified) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(tr("Not fully verified: %s"), cur->note.c_str());
        ImGui::PopTextWrapPos();
    }
    return changed;
}

// The active plan's channels between loHz and hiHz, of the bands that suggest a mode of the current family: the whole-span detector finds
// carriers on them at a lower threshold (core/include/dect2/engine/wide_modes.h)
std::vector<double> bandPlanChannels(const App& a, double loHz, double hiHz) {
    std::vector<double> out;
    const bp::Plan* p = activePlan(a);
    if (!p) return out;
    std::vector<std::string> names;
    for (const auto& m : allModes()) if (m.family == a.family && m.name) names.push_back(m.name);
    for (const bp::Band& b : p->bands) {
        if (b.endHz < loHz || b.startHz > hiHz) continue;
        bool fits = false;
        for (const std::string& bm : b.modes) for (const std::string& n : names) if (bm == n) fits = true;
        if (!fits) continue;
        for (const bp::Channel& c : b.list(4000)) if (c.centreHz >= loHz && c.centreHz <= hiHz) out.push_back(c.centreHz);
    }
    return out;
}
