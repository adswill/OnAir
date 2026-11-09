// My station window (View > My station): the user's radios and antennas, entered once. Radios the program finds are added by themselves; an antenna is one
// click from the list of common ones, its details under Advanced. The verdicts come from core/src/station/station.cpp (docs/STATION-3.8.md); this file keeps the
// data in the settings, maps it to the device list, answers the mode list, and moves a receiver to a better free radio when its mode is chosen.
#include "../app.h"
#include "dect2/engine/device_claims.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/station/station.h"
#include "../ui/mode_picker_logic.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace dect2;
namespace st = dect2::station;

namespace {

st::Station S;
bool gLoaded = false;
bool gAuto = true;     // Advanced: "Choose the radio automatically"
std::string gNote;

// One program-wide station, whichever receiver asks (the settings of RX 2 and up carry a prefix)
struct FirstRx { int old; FirstRx() : old(plat::gPrefRx) { plat::gPrefRx = 1; } ~FirstRx() { plat::gPrefRx = old; } };

void load() {
    if (gLoaded) return;
    gLoaded = true;
    FirstRx scope;
    S = st::fromText(plat::prefs().getS("myStation", ""));
    gAuto = plat::prefs().getB("myStationAuto", true);
}

void save() {
    FirstRx scope;
    plat::prefs().setS("myStation", st::toText(S));
    plat::prefs().setB("myStationAuto", gAuto);
}

std::string radioKey(const DeviceInfo& d) { std::string k = deviceClaimKey(d); return k.empty() ? d.name : k; }

// Adds the radios of the device list that are new and marks which are connected now. Saved only when one was added.
void sync(const App& a) {
    load();
    bool added = false;
    for (st::Radio& r : S.radios) r.present = false;
    for (const DeviceInfo& d : a.devices) {
        if (!d.isRadio()) continue;
        const std::string key = radioKey(d);
        auto it = std::find_if(S.radios.begin(), S.radios.end(), [&](const st::Radio& r) { return r.id == key; });
        if (it == S.radios.end()) {
            st::Radio r; r.id = key;
            S.radios.push_back(r);
            it = S.radios.end() - 1;
            added = true;
        }
        const RateLimits L = rateLimitsOf(d);
        it->name = d.name; it->minHz = d.minFreqHz; it->maxHz = d.maxFreqHz; it->maxRateHz = L.maxHz; it->biasTee = d.hasBiasTee; it->present = true;
    }
    if (added) save();
}

int deviceOf(const App& a, const st::Radio& r) {
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[(size_t)i].isRadio() && radioKey(a.devices[(size_t)i]) == r.id) return i;
    return -1;
}

int stationIndexOfDevice(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[(size_t)a.devIdx].isRadio()) return -1;
    const std::string key = radioKey(a.devices[(size_t)a.devIdx]);
    for (int i = 0; i < (int)S.radios.size(); i++) if (S.radios[(size_t)i].id == key) return i;
    return -1;
}

std::vector<bool> busyFor(const App& a) {
    std::vector<bool> b(S.radios.size(), false);
    for (size_t i = 0; i < S.radios.size(); i++) {
        const int di = deviceOf(a, S.radios[i]);
        if (di < 0) continue;
        const int h = deviceHolder(a.devices[(size_t)di]);
        b[i] = h != 0 && h != a.rxId;
    }
    return b;
}

st::ModeNeed needOf(const ModeDef& m) {
    st::ModeNeed n;
    n.id = m.name;
    n.freqHz = m.bandMinMhz * 1e6;
    n.minRateHz = m.minRateMsps * 1e6;
    if (m.needs & modepick::NDish) n.flags |= st::ADish;
    if (m.needs & modepick::NActiveAnt) n.flags |= st::AActive;
    if (m.needs & modepick::NCable) n.flags |= st::ACable;
    return n;
}

const char* missingText(st::Missing m) {
    switch (m) {
    case st::Missing::RadioRange: return tr("no radio here reaches this frequency");
    case st::Missing::RadioRate: return tr("needs a radio with a higher sample rate");
    case st::Missing::NoRadioPlugged: return tr("no free radio connected");
    case st::Missing::AntennaBand: return tr("no antenna for this band");
    case st::Missing::ActiveAntenna: return tr("needs an active antenna");
    case st::Missing::Dish: return tr("needs a dish");
    case st::Missing::CableFeed: return tr("needs a cable feed");
    case st::Missing::NoAntennaPower: return tr("the active antenna needs power (this radio has no bias tee)");
    }
    return "";
}

std::string missingLine(const st::Verdict& v) {
    std::string s;
    for (st::Missing m : v.missing) { if (!s.empty()) s += ", "; s += missingText(m); }
    return s;
}

std::string pairName(const st::Verdict& v) {
    if (v.radio < 0) return std::string();
    std::string s = S.radios[(size_t)v.radio].name;
    if (v.antenna >= 0) s += " + " + S.antennas[(size_t)v.antenna].name;
    return s;
}

bool entered() { return !S.antennas.empty() || S.radios.size() >= 2; }

const ModeDef* currentMode(const App& a) {
    for (const auto& m : allModes()) if (modeSelected(a, m)) return &m;
    return nullptr;
}

void assignAll(App& a) {
    std::vector<App*> rx = receiversAll(), stopped;
    std::vector<st::ModeNeed> wanted;
    std::vector<bool> busy(S.radios.size(), false);
    modeInfoFill();
    for (App* r : rx) {
        if (r->engine.running()) {
            for (size_t i = 0; i < S.radios.size(); i++) {
                const int di = deviceOf(a, S.radios[i]);
                if (di >= 0 && deviceHolder(a.devices[(size_t)di]) == r->rxId) busy[i] = true;
            }
            continue;
        }
        const ModeDef* m = currentMode(*r);
        if (!m) continue;
        stopped.push_back(r);
        wanted.push_back(needOf(*m));
    }
    const std::vector<int> got = st::assignRadios(S, wanted, busy);
    int moved = 0;
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i] < 0) continue;
        App* r = stopped[i];
        const int di = deviceOf(*r, S.radios[(size_t)got[i]]);
        if (di < 0 || di == r->devIdx) continue;
        r->devIdx = di; r->tune.biasTee = false;
        moved++;
    }
    char b[120];
    snprintf(b, sizeof b, tr("%d receivers moved to a better radio."), moved);
    gNote = b;
}

void addAntennaPopup() {
    if (ImGui::Button(tr("Add antenna"))) ImGui::OpenPopup("##addant");
    if (ImGui::BeginPopup("##addant")) {
        for (const st::AntennaPreset& p : st::antennaPresets()) {
            if (ImGui::MenuItem(tr(p.name))) {
                st::Antenna n; n.name = p.name; n.loHz = p.loHz; n.hiHz = p.hiHz; n.flags = p.flags;
                S.antennas.push_back(n);
                save();
            }
        }
        ImGui::EndPopup();
    }
}

}  // namespace

bool stationFit(const App& a, const ModeDef& m, ModeFit& out) {
    load();
    if (!entered() || a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[(size_t)a.devIdx].isRadio()) return false;
    const st::Verdict v = st::assess(S, needOf(m), busyFor(a));
    out.ok = v.level == st::Level::Ready;
    out.reason = v.level == st::Level::Ready ? std::string() : missingLine(v);
    return true;
}

std::string stationUseLine(const App& a, const ModeDef& m) {
    load();
    if (!entered() || a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[(size_t)a.devIdx].isRadio()) return std::string();
    return pairName(st::assess(S, needOf(m), busyFor(a)));
}

void stationAutoPick(App& a, bool onlyIfBusy) {
    sync(a);
    if (!gAuto || S.radios.size() < 2) return;
    modeInfoFill();
    const ModeDef* m = currentMode(a);
    const int cur = stationIndexOfDevice(a);
    if (!m || cur < 0) return;   // the test signal or a file stay what they are
    const std::vector<bool> busy = busyFor(a);
    if (onlyIfBusy && !(cur < (int)busy.size() && busy[(size_t)cur])) return;   // Start never overrules a radio that is free
    const st::ModeNeed n = needOf(*m);
    const int want = st::pickRadio(S, n, cur, busy);
    if (want == cur) return;
    const int di = deviceOf(a, S.radios[(size_t)want]);
    if (di < 0) return;
    a.devIdx = di;
    a.tune.biasTee = false;   // antenna power set for another radio is never carried over
    a.engine.log("My station: using " + S.radios[(size_t)want].name + " for this mode");
}

void stationWindow(App& a) {
    if (!a.stationOpen) return;
    sync(a);
    firstWindowSize(640 * gUi, 480 * gUi);
    if (!ImGui::Begin(trw("My station"), &a.stationOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Enter your antennas once. Modes then use the right radio and antenna by themselves, and the mode list shows what can be received here and what is missing."));
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Radios"));
    ImGui::Separator();
    if (S.radios.empty()) ImGui::TextDisabled("%s", tr("No radio found yet. Connect one and it appears here."));
    int removeRadio = -1;
    for (int i = 0; i < (int)S.radios.size(); i++) {
        const st::Radio& r = S.radios[(size_t)i];
        ImGui::PushID(i);
        if (r.present) ImGui::TextUnformatted(r.name.c_str());
        else { ImGui::TextDisabled("%s (%s)", r.name.c_str(), tr("not connected")); ImGui::SameLine(); if (ImGui::SmallButton(tr("Forget"))) removeRadio = i; }
        ImGui::PopID();
    }
    if (removeRadio >= 0) { S.radios.erase(S.radios.begin() + removeRadio); save(); }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Antennas"));
    ImGui::Separator();
    int removeAnt = -1;
    bool changed = false;
    for (int i = 0; i < (int)S.antennas.size(); i++) {
        st::Antenna& n = S.antennas[(size_t)i];
        ImGui::PushID(1000 + i);
        ImGui::Text("%s  (%.4g - %.5g MHz)", n.name.c_str(), n.loHz / 1e6, n.hiHz / 1e6);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Remove"))) removeAnt = i;
        ImGui::PopID();
    }
    if (removeAnt >= 0) { S.antennas.erase(S.antennas.begin() + removeAnt); changed = true; }
    addAntennaPopup();
    ImGui::Spacing();
    const ModeDef* cur = currentMode(a);
    if (cur && !S.radios.empty()) {
        modeInfoFill();
        const st::Verdict v = st::assess(S, needOf(*cur), busyFor(a));
        ImGui::TextUnformatted(tr("This mode"));
        ImGui::Separator();
        const std::string pair = pairName(v);
        if (v.level == st::Level::Ready) ImGui::TextColored(ImVec4(.45f, .85f, .5f, 1), "%s: %s", cur->name, pair.c_str());
        else ImGui::TextColored(ImVec4(1, .7f, .3f, 1), "%s: %s%s%s", cur->name, pair.c_str(), pair.empty() ? "" : ", ", missingLine(v).c_str());
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(tr("Advanced"))) {
        if (ImGui::Checkbox(tr("Choose the radio automatically"), &gAuto)) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Selecting a mode moves to another free radio only when the selected one cannot do it. A radio that works is left alone."));
        if (receiversAll().size() > 1 && ImGui::Button(tr("Give every stopped receiver its best free radio"))) assignAll(a);
        if (!gNote.empty()) ImGui::TextDisabled("%s", gNote.c_str());
        for (int i = 0; i < (int)S.antennas.size(); i++) {
            st::Antenna& n = S.antennas[(size_t)i];
            ImGui::PushID(2000 + i);
            ImGui::SeparatorText(n.name.c_str());
            char nm[96];
            snprintf(nm, sizeof nm, "%s", n.name.c_str());
            if (ImGui::InputText(tr("Name"), nm, sizeof nm)) { n.name = nm; changed = true; }
            double lo = n.loHz / 1e6, hi = n.hiHz / 1e6;
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::InputDouble(tr("From MHz"), &lo, 0, 0, "%.3f")) { n.loHz = std::max(0.0, lo) * 1e6; changed = true; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::InputDouble(tr("To MHz"), &hi, 0, 0, "%.3f")) { n.hiHz = std::max(0.0, hi) * 1e6; changed = true; }
            bool act = n.flags & st::AActive, dish = n.flags & st::ADish, cable = n.flags & st::ACable;
            if (ImGui::Checkbox(tr("Active (has an amplifier)"), &act)) { n.flags = (uint8_t)((n.flags & ~st::AActive) | (act ? st::AActive : 0)); changed = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("Dish"), &dish)) { n.flags = (uint8_t)((n.flags & ~st::ADish) | (dish ? st::ADish : 0)); changed = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox(tr("Cable feed"), &cable)) { n.flags = (uint8_t)((n.flags & ~st::ACable) | (cable ? st::ACable : 0)); changed = true; }
            std::string curName = tr("Any radio");
            for (const st::Radio& r : S.radios) if (r.id == n.radioId) curName = r.name;
            if (ImGui::BeginCombo(tr("Connected to"), curName.c_str())) {
                if (ImGui::Selectable(tr("Any radio"), n.radioId.empty())) { n.radioId.clear(); changed = true; }
                for (const st::Radio& r : S.radios) if (ImGui::Selectable(r.name.c_str(), r.id == n.radioId)) { n.radioId = r.id; changed = true; }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }
    if (changed) save();
    ImGui::End();
}
