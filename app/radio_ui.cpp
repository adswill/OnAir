// The "Radio settings" section under the gain controls: the radio's own settings its driver lists (DeviceInfo::settings: frequency
// correction, direct sampling, notch filters, gain modes, ...), only those the selected radio has. The values live in a.tune.radio, are
// saved per radio (app_state.cpp: syncRadioSettings / saveRadioSettings) and reach a running radio at once (a retune); one that takes
// effect when the radio is opened restarts a running receiver. A value set back to its default is dropped, so the driver does what it always did.
#include "app.h"
#include <cmath>
#include <map>

namespace {

std::string numText(double v) {
    char b[48];
    snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// one setting; true when its value changed (the new value is in v, "" = back to the default)
bool settingRow(const RadioSetting& r, std::string& v, bool vertical) {
    const std::string cur = v.empty() ? r.def : v;
    bool changed = false;
    ImGui::PushID(r.key.c_str());
    const float w = (vertical ? 150 : 170) * gUi;
    switch (r.type) {
    case RadioSetting::Bool: {
        bool on = cur == "1";
        if (ImGui::Checkbox(r.label.c_str(), &on)) { v = on ? "1" : "0"; changed = true; }
        break;
    }
    case RadioSetting::Choice: {
        size_t at = 0;
        for (size_t i = 0; i < r.values.size(); i++) if (r.values[i] == cur) at = i;
        ImGui::TextDisabled("%s", r.label.c_str());
        if (ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
        ImGui::SetNextItemWidth(w);
        if (ImGui::BeginCombo("##v", at < r.names.size() ? r.names[at].c_str() : cur.c_str())) {
            for (size_t i = 0; i < r.values.size(); i++)
                if (ImGui::Selectable(i < r.names.size() ? r.names[i].c_str() : r.values[i].c_str(), i == at) && i != at) { v = r.values[i]; changed = true; }
            ImGui::EndCombo();
        }
        break;
    }
    case RadioSetting::Number: {
        // typed values are taken when the field is left (not at every key); the step buttons take effect at once
        static std::map<std::string, double> editing;
        auto it = editing.find(r.key);
        double val = it != editing.end() ? it->second : atof(cur.c_str());
        ImGui::TextDisabled("%s", r.label.c_str());
        if (ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
        ImGui::SetNextItemWidth(w);
        char fmt[32];
        const int dec = r.step >= 1 ? 0 : r.step >= 0.1 ? 1 : r.step >= 0.01 ? 2 : 3;
        snprintf(fmt, sizeof fmt, "%%.%df%s%s", dec, r.unit.empty() ? "" : " ", r.unit.c_str());
        if (ImGui::InputDouble("##v", &val, r.step > 0 ? r.step : 0.0, r.step > 0 ? r.step * 10 : 0.0, fmt)) editing[r.key] = val;
        it = editing.find(r.key);
        if (it != editing.end() && !ImGui::IsItemActive()) {
            double x = it->second;
            if (r.maxV > r.minV) x = std::min(r.maxV, std::max(r.minV, x));
            if (r.step >= 1) x = std::round(x);
            editing.erase(it);
            const std::string nv = numText(x);
            if (nv != numText(atof(cur.c_str()))) { v = nv; changed = true; }
        }
        break;
    }
    }
    if (r.type == RadioSetting::Bool && ImGui::IsItemHovered() && !r.help.empty()) ImGui::SetTooltip("%s", r.help.c_str());
    ImGui::PopID();
    if (changed && (v == r.def || (r.type == RadioSetting::Number && numText(atof(v.c_str())) == numText(atof(r.def.c_str()))))) v.clear();
    return changed;
}

}   // namespace

void radioSettingsUi(App& a, bool vertical) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    const DeviceInfo& d = a.devices[a.devIdx];
    if (!d.isRadio() || d.settings.empty()) return;
    syncRadioSettings(a);
    int changedN = 0;
    for (const auto& r : d.settings) if (a.tune.radio.count(r.key)) changedN++;
    char title[64];
    snprintf(title, sizeof title, changedN ? "Radio settings (%d changed)###radioset" : "Radio settings###radioset", changedN);
    bool open = false;
    if (vertical) {
        ImGui::SetNextItemOpen(false, ImGuiCond_FirstUseEver);
        open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth);
    } else {   // a one-line bar: a button with the settings in a popup (the caller places it)
        if (ImGui::SmallButton(changedN ? "Radio settings*" : "Radio settings")) ImGui::OpenPopup("##radiosetpop");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: frequency correction and the other settings of this radio", title);
        open = ImGui::BeginPopup("##radiosetpop");
    }
    if (!open) return;
    bool restart = false, retune = false;
    for (const auto& r : d.settings) {
        auto it = a.tune.radio.find(r.key);
        std::string v = it != a.tune.radio.end() ? it->second : std::string();
        if (!settingRow(r, v, vertical)) continue;
        if (v.empty()) a.tune.radio.erase(r.key); else a.tune.radio[r.key] = v;
        saveRadioSettings(a);
        if (r.restart) restart = true; else retune = true;
    }
    if (d.kind == DeviceInfo::Soapy && d.settings.size() <= 1) ImGui::TextDisabled("The driver's own settings show up here\nonce this radio has been started.");
    if (vertical) ImGui::TreePop(); else ImGui::EndPopup();
    if (!a.engine.running()) return;   // the next start takes them
    if (restart) { a.engine.log("radio setting changed: restarting the radio"); startReceiver(a); }
    else if (retune) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
}
