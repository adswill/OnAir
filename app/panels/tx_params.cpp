// The parameters of the transmit panel, drawn from the core table (dect2/tx/tx_params.h, one place for every mode): the Simple rows under "What to send"
// (simpleRows), the Advanced rows in their groups Modulation, Audio, Filters, Framing, Output and Timing (advancedPanel), with presets and "Reset to default".
// A row is drawn by one function (paramRow) from its type, range, unit and help, or by the mode's own widget when it is a custom row (customRow,
// tx_payload_ui.cpp). Every row reaches the generator: tests/tx/test_tx_param_model.cpp checks it. Nothing here limits a value to what is lawful.
#include "tx_state.h"
#include "dect2/audio/audio_control.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace txui;

namespace {

using P = PDef;

std::string sectionName(TxPGroup g) { return tr(txParamGroupName(g)); }

// the slider format of a row: its own format and its unit ("%" written as "%%")
std::string fmtOf(const P& d) {
    std::string f = d.fmt && *d.fmt ? d.fmt : "%.2f";
    if (d.unit && *d.unit) {
        f += ' ';
        for (const char* c = d.unit; *c; c++) { if (*c == '%') f += "%%"; else f += *c; }
    }
    return f;
}

// the TxUi buffer a text row edits (shared between the modes and saved as before), nullptr for a field without one
char* textBuf(TxUi& s, TxPField f, size_t& cap) {
    switch (f) {
    case TxPField::Callsign: cap = sizeof s.callsign; return s.callsign;
    case TxPField::Grid: cap = sizeof s.grid; return s.grid;
    case TxPField::Text: cap = sizeof s.text; return s.text;
    case TxPField::Dest: cap = sizeof s.dest; return s.dest;
    case TxPField::Path: cap = sizeof s.path; return s.path;
    case TxPField::File: cap = sizeof s.file; return s.file;
    case TxPField::Name: cap = sizeof s.name; return s.name;
    case TxPField::Network: cap = sizeof s.network; return s.network;
    default: cap = 0; return nullptr;
    }
}
const char* prefKey(TxPField f) {
    switch (f) {
    case TxPField::Callsign: return "tx.callsign";
    case TxPField::Grid: return "tx.grid";
    case TxPField::Text: return "tx.text";
    default: return nullptr;
    }
}

// "k=v,k=v" of the values, and back
std::string pack(const TxUi& s) {
    std::string o;
    char b[96];
    for (const auto& kv : s.pv) { snprintf(b, sizeof b, "%s=%.6g", kv.first.c_str(), kv.second); if (!o.empty()) o += ','; o += b; }
    return o;
}
void unpack(TxUi& s, const std::string& txt) {
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find(',', pos);
        if (e == std::string::npos) e = txt.size();
        const std::string item = txt.substr(pos, e - pos);
        pos = e + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq);
        if (key[0] == '@') continue;
        if (s.pv.count(key)) s.pv[key] = atof(item.c_str() + eq + 1);
    }
}

std::string lastKey(const TxUi& s) { return "tx.pv." + s.modeId; }
std::string presetKey(const TxUi& s) { return "tx.presets." + s.modeId; }

struct Preset { std::string name, data; };
std::vector<Preset> loadPresets(const TxUi& s) {
    std::vector<Preset> v;
    const std::string txt = plat::basePrefs().getS(presetKey(s).c_str(), "");
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find('|', pos);
        if (e == std::string::npos) e = txt.size();
        const std::string item = txt.substr(pos, e - pos);
        pos = e + 1;
        const size_t t = item.find('~');
        if (t == std::string::npos || t == 0) continue;
        v.push_back({item.substr(0, t), item.substr(t + 1)});
    }
    return v;
}
void savePresets(const TxUi& s, const std::vector<Preset>& v) {
    std::string o;
    for (const auto& p : v) { if (!o.empty()) o += '|'; o += p.name + "~" + p.data; }
    plat::basePrefs().setS(presetKey(s).c_str(), o);
}

void defaults(TxUi& s) {
    for (const auto& d : s.params) if (!d.custom && d.type != TxPType::Text && d.type != TxPType::File) s.pv[d.key] = d.def;
}

} // namespace

namespace txui {

TxParamCtx paramCtx(const TxUi& s, const TxModeSpec& m) {
    TxParamCtx c;
    c.analogVoice = s.analogVoice;
    c.variant = m.id && std::string(m.id) == "qo100" && s.variant == 1 ? 1 : 0;
    c.subId = s.subId;
    return c;
}

const std::vector<PDef>& paramsFor(TxUi& s, const TxModeSpec& m) {
    const TxParamCtx c = paramCtx(s, m);
    const std::string key = std::string(m.id) + (c.analogVoice ? "+a" : "") + (c.variant == 1 ? "+v" : "") + "/" + c.subId;
    if (key != s.paramsKey) {
        s.paramsKey = key;
        s.params = txParams(m, c);
        for (const auto& d : s.params) if (!s.pv.count(d.key) && !d.custom && d.type != TxPType::Text && d.type != TxPType::File) s.pv[d.key] = d.def;
    }
    return s.params;
}

// the mode was chosen: its parameters start from the defaults and then from the values it had last time
void paramsEnter(TxUi& s, const TxModeSpec& m) {
    s.pv.clear();
    s.paramsKey.clear();
    paramsFor(s, m);
    defaults(s);
    unpack(s, plat::basePrefs().getS(lastKey(s).c_str(), ""));
    s.pvMode = s.modeId;
}

// the values into the request: every number row of the table (by its field, its setter or opt[key]); then every request field no row of this mode
// writes is cleared, so nothing typed for another mode reaches this one's generator
void paramsApply(const TxUi& s, TxRequest& r) {
    const TxModeSpec* m = txModeById(r.modeId.empty() ? s.modeId : r.modeId);
    if (!m) return;
    const TxParamCtx c = paramCtx(s, *m);
    const auto& rows = txParams(*m, c);
    txParamsApplyNumbers(rows, s.pv, r);
    txParamsSanitize(*m, c, r);
}

namespace {
// one row: the label on the left (tooltip: what it does and the default), the control on the right; true when the value changed.
// Text and file rows edit the shared buffer of their field; number rows s.pv[key]. A changed value is shown in the normal colour, a default dimmed.
bool paramRow(TxUi& s, const PDef& d, bool running) {
    (void)running;
    const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.38f);
    const bool isText = d.type == TxPType::Text || d.type == TxPType::File;
    double& v = s.pv[d.key];
    size_t cap = 0;
    char* buf = isText ? textBuf(s, d.field, cap) : nullptr;
    if (isText && !buf) return false;
    ImGui::AlignTextToFramePadding();
    const bool changed0 = isText ? buf[0] != 0 : std::fabs(v - d.def) > 1e-9;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(changed0 ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(tr(d.label));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        char def[96] = "";
        if (d.type == TxPType::Choice) snprintf(def, sizeof def, "%s", tr(d.choices[(size_t)std::max(0.0, std::min(d.hi, d.def))]));
        else if (d.type == TxPType::Bool) snprintf(def, sizeof def, "%s", d.def > 0.5 ? tr("on") : tr("off"));
        else if (isText) snprintf(def, sizeof def, "%s", tr("empty: the mode's demo content"));
        else snprintf(def, sizeof def, fmtOf(d).c_str(), d.def);
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320 * gUi);
        ImGui::TextUnformatted(tr(d.help));
        ImGui::TextDisabled(tr("Default: %s"), def);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::SameLine(labW);
    ImGui::PushID(d.key);
    bool ch = false;
    if (isText) {
        const bool isFile = d.type == TxPType::File;
        ImGui::SetNextItemWidth(isFile ? std::max(80.f * gUi, ImGui::GetContentRegionAvail().x - 80 * gUi) : -1);
        if (ImGui::InputText("##t", buf, cap)) { ch = true; if (const char* k = prefKey(d.field)) saveStr(k, buf); }
        if (isFile) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Browse…"))) { const std::string f = openFileDialog(); if (!f.empty()) { snprintf(buf, cap, "%s", f.c_str()); ch = true; } }
        }
        ImGui::PopID();
        return ch;
    }
    ImGui::SetNextItemWidth(-1);
    const std::string f = fmtOf(d);
    if (d.type == TxPType::Float) {
        const bool typed = d.hi - d.lo > 20000 || (d.unit && !strcmp(d.unit, "°"));   // a wide range or a position is typed, not dragged
        if (typed) {
            double x = v;
            if (ImGui::InputDouble("##v", &x, 0, 0, f.c_str())) { v = std::max(d.lo, std::min(d.hi, x)); ch = true; }
        } else {
            float x = (float)v;
            if (ImGui::SliderFloat("##v", &x, (float)d.lo, (float)d.hi, f.c_str(), ImGuiSliderFlags_AlwaysClamp)) { v = x; ch = true; }
        }
    } else if (d.type == TxPType::Int) {
        if (d.hi - d.lo > 1000) {
            double x = v;
            if (ImGui::InputDouble("##v", &x, 1, 100, "%.0f")) { v = std::max(d.lo, std::min(d.hi, std::round(x))); ch = true; }
        } else {
            int n = (int)std::lround(v);
            std::string fi = "%d";
            if (d.unit && *d.unit) { fi += ' '; for (const char* c = d.unit; *c; c++) { if (*c == '%') fi += "%%"; else fi += *c; } }
            if (ImGui::SliderInt("##v", &n, (int)d.lo, (int)d.hi, fi.c_str())) { v = n; ch = true; }
        }
    } else if (d.type == TxPType::Hex) {   // typed as hex digits: a sync word, an address, a PI code
        long long n = std::llround(v);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputScalar("##v", ImGuiDataType_S64, &n, nullptr, nullptr, "%llX", ImGuiInputTextFlags_CharsHexadecimal)) {
            v = std::max(d.lo, std::min(d.hi, (double)n));
            ch = true;
        }
    } else if (d.type == TxPType::Bool) {
        bool b = v > 0.5;
        if (ImGui::Checkbox("##v", &b)) { v = b ? 1 : 0; ch = true; }
    } else {
        const int cur = std::max(0, std::min((int)d.choices.size() - 1, (int)std::lround(v)));
        if (ImGui::BeginCombo("##v", tr(d.choices[(size_t)cur]))) {
            for (int i = 0; i < (int)d.choices.size(); i++) if (ImGui::Selectable(tr(d.choices[(size_t)i]), i == cur)) { v = i; ch = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::PopID();
    return ch;
}

// a row is drawn here: shown by its condition, not hidden, and (a custom row) by the mode's own widget
bool drawRow(TxUi& s, const TxModeSpec& mode, const PDef& d, bool running) {
    if (!txParamShown(d, s.params, s.pv)) return false;
    if (d.custom) { ImGui::PushID(d.key); customRow(s, mode, d, running); ImGui::PopID(); return false; }
    return paramRow(s, d, running);
}
} // namespace

// What to send: the Simple rows of the table, in their order (the payload, and the few settings that matter most)
void simpleRows(TxUi& s, const TxModeSpec& mode, bool running) {
    if (s.pvMode != s.modeId) paramsEnter(s, mode);
    paramsFor(s, mode);
    bool changed = false, any = false;
    for (const auto& d : s.params) {
        if (d.level != TxPLevel::Simple) continue;
        any = true;
        changed |= drawRow(s, mode, d, running);
    }
    if (!any) ImGui::TextDisabled("%s", tr("Nothing to type: this generator sends its built-in test signal."));
    if (changed) plat::basePrefs().setS(lastKey(s).c_str(), pack(s));
}

// The Advanced block of the panel: presets and reset on top, then the sections Modulation, Audio, Filters, Output, Timing
void advancedPanel(TxUi& s, const TxModeSpec& mode, bool running) {
    if (s.pvMode != s.modeId) paramsEnter(s, mode);
    paramsFor(s, mode);
    bool changed = false;

    // presets of this signal (the frequency and level are part of a preset)
    {
        std::vector<Preset> pr = loadPresets(s);
        ImGui::SetNextItemWidth(std::max(140.f * gUi, ImGui::GetContentRegionAvail().x - 240 * gUi));
        if (ImGui::BeginCombo("##txpreset", s.presetName[0] ? s.presetName : tr("Presets"))) {
            for (const auto& p : pr) {
                if (ImGui::Selectable(p.name.c_str(), p.name == s.presetName)) {
                    defaults(s);
                    unpack(s, p.data);
                    const size_t a = p.data.find("@mhz="), g = p.data.find("@gain=");
                    if (a != std::string::npos) s.freqMhz = atof(p.data.c_str() + a + 5);
                    if (g != std::string::npos) { s.gainDb = (float)atof(p.data.c_str() + g + 6); s.gainSet = true; }
                    snprintf(s.presetName, sizeof s.presetName, "%s", p.name.c_str());
                    changed = true;
                }
            }
            if (pr.empty()) ImGui::TextDisabled("%s", tr("No presets saved for this signal yet."));
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Reset to default"))) { defaults(s); resetControl(s); s.presetName[0] = 0; changed = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One click: every setting below back to its default (the filter and the gain stages to automatic, no schedule). The radio's calibration is kept."));
        ImGui::SetNextItemWidth(std::max(120.f * gUi, ImGui::GetContentRegionAvail().x - 190 * gUi));
        ImGui::InputTextWithHint("##txpname", tr("Name for a new preset"), s.presetNew, sizeof s.presetNew);
        ImGui::SameLine();
        ImGui::BeginDisabled(!s.presetNew[0]);
        if (ImGui::Button(tr("Save preset"))) {
            std::string nm = s.presetNew;
            for (char& c : nm) if (c == '|' || c == '~' || c == ',' || c == '=') c = '-';
            char extra[96];
            snprintf(extra, sizeof extra, ",@mhz=%.6f,@gain=%.2f", s.freqMhz, (double)s.gainDb);
            const std::string data = pack(s) + extra;
            bool found = false;
            for (auto& p : pr) if (p.name == nm) { p.data = data; found = true; }
            if (!found) pr.push_back({nm, data});
            savePresets(s, pr);
            snprintf(s.presetName, sizeof s.presetName, "%s", nm.c_str());
            s.presetNew[0] = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Saves the settings below, the frequency and the level under this name."));
        if (s.presetName[0]) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Delete"))) {
                pr.erase(std::remove_if(pr.begin(), pr.end(), [&](const Preset& p) { return p.name == s.presetName; }), pr.end());
                savePresets(s, pr);
                s.presetName[0] = 0;
            }
        }
    }

    ImGui::BeginDisabled(running);
    for (TxPGroup sec : {TxPGroup::Modulation, TxPGroup::Audio, TxPGroup::Filters, TxPGroup::Framing, TxPGroup::Output, TxPGroup::Timing}) {
        bool any = sec == TxPGroup::Output || sec == TxPGroup::Timing;
        for (const auto& d : s.params) if (d.level == TxPLevel::Advanced && d.group == sec && !d.hidden) any = true;
        if (!any) continue;
        ImGui::Spacing();
        ImGui::TextDisabled("%s", sectionName(sec).c_str());
        ImGui::Separator();
        for (const auto& d : s.params) if (d.level == TxPLevel::Advanced && d.group == sec) changed |= drawRow(s, mode, d, running);
        if (sec == TxPGroup::Output) {   // the offset belongs to the radio, not to the signal
            const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("Frequency offset"));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("This is added to the frequency sent to the radio, for a radio that radiates off frequency."));
            ImGui::SameLine(labW);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputDouble("##txoff", &s.offsetMhz, 0.0, 0.0, "%.6f MHz")) plat::basePrefs().setD("tx.offsetMhz", s.offsetMhz);
            const TxModeSpec* tm = txModeById(s.modeId);
            const double modeOff = tm ? txModeTuneOffsetHz(*tm) / 1e6 : 0;   // the radio is tuned this far above the dial, the signal sits that far below the radio's centre (#351)
            if (s.offsetMhz != 0 || modeOff != 0) ImGui::TextDisabled(tr("radio is told %.6f MHz"), s.freqMhz + s.offsetMhz + modeOff);
        }
        if (sec == TxPGroup::Timing) {
            const float labW = std::max(140.f * gUi, ImGui::GetContentRegionAvail().x * 0.42f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("Repeat until stopped"));
            ImGui::SameLine(labW);
            ImGui::Checkbox("##txloop", &s.loop);
            if (!s.loop) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr("seconds"));
                ImGui::SameLine(labW);
                ImGui::SetNextItemWidth(-1);
                ImGui::SliderInt("##txonce", &s.onceSeconds, 1, 3600);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(tr("TX sample rate"));
            ImGui::SameLine(labW);
            ImGui::SetNextItemWidth(100 * gUi);
            if (ImGui::BeginCombo("##txfsmode", s.autoSampleRate ? tr("Auto") : tr("Manual"))) {
                if (ImGui::Selectable(tr("Auto"), s.autoSampleRate)) s.autoSampleRate = true;
                if (ImGui::Selectable(tr("Manual"), !s.autoSampleRate)) s.autoSampleRate = false;
                ImGui::EndCombo();
            }
            if (!s.autoSampleRate) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140 * gUi);
                ImGui::InputDouble("##txfs", &s.sampleRateMsps, 0, 0, "%.6f Msps");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Requested transmit IQ sample rate. For a shared RX/TX radio, set the same hardware rate on both sides. Stop transmitting before changing it."));
            }
        }
    }
    ImGui::EndDisabled();
    if (changed) plat::basePrefs().setS(lastKey(s).c_str(), pack(s));
}

} // namespace txui
