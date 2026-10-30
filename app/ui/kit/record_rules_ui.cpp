#include "record_rules_ui.h"
#include "app.h"
#include "dect2/session/record_rules.h"
#include "dect2/session/session_store.h"
#include "i18n/i18n.h"
#include "ui/scale.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace kit {
namespace {
using namespace dect2::sess;

struct Ed {
    bool open = false;
    uint32_t editId = 0;
    int modeSel = 1;                   // 0 all modes, 1 this mode
    int field = 0;
    char match[160] = "";
    float fmin = 0, fmax = 0;
    bool audio = true, iq = false, emerg = false;
    int maxMin = 60, capMB = 0, iqSec = 10;
};
Ed& ed() { static Ed e; return e; }

void resetForm(Ed& e) {
    e.editId = 0; e.field = 0; e.match[0] = 0; e.fmin = e.fmax = 0; e.audio = true; e.iq = false; e.emerg = false; e.maxMin = 60; e.capMB = 0; e.iqSec = 10;
}

std::string modeLabel(uint16_t m) {
    if (!m) return tr("all modes");
    const std::string n = global().modeName(m);
    return n.empty() ? "mode " + std::to_string(m) : n;
}
}  // namespace

void recordBar(uint16_t mode) {
    RecordRules& rr = rules();
    bool on = rr.enabled();
    if (ImGui::Checkbox(tr("Record"), &on)) {
        rr.setEnabled(on);
        if (on && rr.list().empty()) {   // one click: with no rule yet, "record" means everything unencrypted of this mode
            RecordRule r;
            r.mode = mode;
            r.name = tr("Everything unencrypted");
            rr.add(r);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Saves the audio of calls as they start, by your rules. Off by default. Encrypted calls are never recorded. You are responsible for the law of your country."));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Rules..."))) { ed().open = true; ed().modeSel = 1; }
    if (on) {
        ImGui::SameLine();
        const uint64_t capped = rr.cappedCount();
        ImGui::TextDisabled("%u %s%s", (unsigned)rr.list().size(), tr("rules"), capped ? (std::string(", ") + std::to_string(capped) + " " + tr("held back by a disk cap")).c_str() : "");
    }
}

bool recordThisButton(const Session& s) {
    RecordRules& rr = rules();
    RecordRule r;
    r.mode = s.mode;
    if (!s.who[1].empty()) { r.field = RecField::Target; r.match = s.who[1]; }
    else if (!s.alias[0].empty()) { r.field = RecField::Callsign; r.match = s.alias[0]; }
    else if (!s.who[0].empty()) { r.field = RecField::Source; r.match = s.who[0]; }
    else if (!s.key.empty()) { r.field = RecField::Any; r.match = s.key; }
    else return false;
    const bool had = rr.has(r.mode, r.field, r.match);
    if (had && rr.enabled()) { ImGui::TextDisabled("%s", tr("Already recording this")); return false; }
    if (!ImGui::Button(had ? tr("Turn recording on") : tr("Record this"))) return false;
    if (s.verdict == (uint8_t)Verdict::Encrypted) return false;
    r.name = r.match;
    rr.add(r);
    rr.setEnabled(true);
    return true;
}

void recordRulesDialog(uint16_t mode) {
    Ed& e = ed();
    if (!e.open) return;
    RecordRules& rr = rules();
    const float u = gUi;
    ImGui::SetNextWindowSize(ImVec2(560 * u, 520 * u), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin(tr("Record rules"), &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); if (!open) e.open = false; return; }
    bool on = rr.enabled();
    if (ImGui::Checkbox(tr("Auto-record (off by default)"), &on)) rr.setEnabled(on);
    ImGui::TextWrapped("%s", tr("When a call or track starts and a rule matches, its audio (and optionally a short IQ clip) is saved to your recordings folder. Encrypted or unreadable traffic is never recorded: only the log line is kept. Nothing is ever blocked, and nothing is uploaded. Laws about recording differ by country and by kind of traffic (telephony, pagers, private radio): you are responsible for yours. Files are kept until you delete them."));
    ImGui::Separator();

    if (ImGui::Button(tr("Record everything unencrypted in this mode"))) { RecordRule r; r.mode = mode; r.name = tr("Everything unencrypted"); rr.add(r); rr.setEnabled(true); }
    ImGui::SameLine();
    if (ImGui::Button(tr("in all modes"))) { RecordRule r; r.name = tr("Everything unencrypted, all modes"); rr.add(r); rr.setEnabled(true); }

    ImGui::TextUnformatted(tr("Rules"));
    const std::vector<RecordRule> rl = rr.list();
    if (rl.empty()) ImGui::TextDisabled("%s", tr("No rules yet."));
    uint32_t del = 0;
    for (const RecordRule& r : rl) {
        ImGui::PushID((int)r.id);
        bool en = r.enabled;
        if (ImGui::Checkbox("##en", &en)) { RecordRule x = r; x.enabled = en; rr.update(x); }
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s", modeLabel(r.mode).c_str(), RecordRules::describe(r).c_str());
        if (r.usedBytes) { ImGui::SameLine(); ImGui::TextDisabled("(%llu MB)", (unsigned long long)(r.usedBytes >> 20)); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Edit"))) {
            e.editId = r.id; e.modeSel = r.mode ? 1 : 0; e.field = (int)r.field; snprintf(e.match, sizeof e.match, "%s", r.match.c_str());
            e.fmin = (float)r.freqMinMHz; e.fmax = (float)r.freqMaxMHz; e.audio = r.actions & RecAudio; e.iq = r.actions & RecIq; e.emerg = r.emergencyOnly;
            e.maxMin = (int)(r.maxSeconds / 60); e.capMB = (int)r.capMB; e.iqSec = (int)r.iqSeconds;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Remove"))) del = r.id;
        ImGui::PopID();
    }
    if (del) { rr.remove(del); if (e.editId == del) resetForm(e); }

    ImGui::Separator();
    ImGui::TextUnformatted(e.editId ? tr("Edit rule") : tr("New rule"));
    const std::string thisMode = modeLabel(mode);
    const std::string items = std::string(tr("All modes")) + '\0' + thisMode + '\0';
    ImGui::SetNextItemWidth(160 * u);
    ImGui::Combo(tr("Mode"), &e.modeSel, items.c_str());
    static const char* const fields[] = {"Anything", "Talkgroup / channel / capcode", "Source", "Name or callsign", "Frequency (MHz)"};
    ImGui::SetNextItemWidth(220 * u);
    ImGui::Combo(tr("Match"), &e.field, fields, 5);
    if (e.field == (int)RecField::Frequency) {
        ImGui::SetNextItemWidth(110 * u); ImGui::InputFloat(tr("from"), &e.fmin, 0, 0, "%.4f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(110 * u); ImGui::InputFloat(tr("to (0 = single)"), &e.fmax, 0, 0, "%.4f");
    } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##m", tr("empty = everything; e.g. 91, 100-199, LY*"), e.match, sizeof e.match);
    }
    ImGui::Checkbox(tr("Audio"), &e.audio);
    ImGui::SameLine(); ImGui::Checkbox(tr("IQ clip"), &e.iq);
    ImGui::SameLine(); ImGui::Checkbox(tr("Emergencies only"), &e.emerg);
    ImGui::SetNextItemWidth(90 * u); ImGui::InputInt(tr("Longest call (min, 0 = no limit)"), &e.maxMin);
    ImGui::SetNextItemWidth(90 * u); ImGui::InputInt(tr("Disk cap (MB, 0 = none)"), &e.capMB);
    if (e.iq) {
        ImGui::SetNextItemWidth(90 * u); ImGui::InputInt(tr("IQ clip length (s)"), &e.iqSec);
        ImGui::TextDisabled("%s", tr("An IQ clip is the whole radio band from the start of the call: size = sample rate x 8 bytes x seconds (10 Msps x 10 s = 800 MB). It also contains neighbouring channels."));
    }
    e.maxMin = std::max(0, e.maxMin); e.capMB = std::max(0, e.capMB); e.iqSec = std::max(1, std::min(600, e.iqSec));
    if (ImGui::Button(e.editId ? tr("Save rule") : tr("Add rule"))) {
        RecordRule r;
        r.id = e.editId;
        r.mode = e.modeSel ? mode : 0;
        r.field = (RecField)e.field;
        r.match = e.field == (int)RecField::Frequency ? std::string() : std::string(e.match);
        r.freqMinMHz = e.fmin; r.freqMaxMHz = e.fmax;
        r.actions = (uint8_t)((e.audio ? RecAudio : 0) | (e.iq ? RecIq : 0));
        if (!r.actions) r.actions = RecAudio;
        r.emergencyOnly = e.emerg; r.maxSeconds = (uint32_t)e.maxMin * 60; r.capMB = (uint32_t)e.capMB; r.iqSeconds = (uint32_t)e.iqSec;
        r.name = r.match.empty() ? std::string(recFieldName(r.field)) : r.match;
        if (e.editId) {
            for (const RecordRule& x : rl) if (x.id == e.editId) { r.enabled = x.enabled; r.usedBytes = x.usedBytes; }
            rr.update(r);
        } else {
            rr.add(r);
            rr.setEnabled(true);
        }
        resetForm(e);
    }
    if (e.editId) { ImGui::SameLine(); if (ImGui::Button(tr("Cancel"))) resetForm(e); }
    ImGui::End();
    if (!open) e.open = false;
}

}  // namespace kit
