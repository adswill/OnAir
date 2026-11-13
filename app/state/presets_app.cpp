// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "presets_app.h"
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/engine/mode_tuning.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

using namespace dect2::presets;

namespace presetsapp {
namespace {

std::vector<Provider>& providers() { static std::vector<Provider> v; return v; }
Provider* providerFor(const std::string& k) { for (auto& p : providers()) if (p.key == k) return &p; return nullptr; }

std::string storePath() { return plat::dataDir() + "/presets.json"; }

RadioCaps capsOf(const App& a) {
    RadioCaps c;
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) {
        const DeviceInfo& d = a.devices[a.devIdx];
        if (d.isRadio()) {
            if (d.maxRateHz > 0) { c.maxRate = d.maxRateHz; c.minRate = d.minRateHz; }
            if (d.gainMaxDb > d.gainMinDb) { c.minGainDb = d.gainMinDb; c.maxGainDb = d.gainMaxDb; }
            c.hasBiasTee = d.hasBiasTee;
        }
    }
    return c;
}

}  // namespace

void registerProvider(Provider p) {
    if (Provider* q = providerFor(p.key)) *q = std::move(p); else providers().push_back(std::move(p));
}

std::string modeKey(const App& a) {
    if (const dect2::ModeTuning* t = dect2::modeTuning(engineStd(a))) if (t->id && *t->id) return t->id;
    switch (engineStd(a)) {
    case 2: return "dvbt";
    case 1: return "dvbt2";
    case 3: return "atsc";
    case 4: return "dab";
    case 5: return "atsc3";
    case 6: return "isdbt";
    case 7: return "fm";
    default: return "mode" + std::to_string(engineStd(a));
    }
}

Preset capture(App& a, const std::string& name) {
    Preset p;
    p.name = name; p.mode = modeKey(a);
    p.values["freqMhz"] = Value::n(a.freqMhz);
    p.values["sampleRate"] = Value::n(a.tune.sampleRate);
    p.values["bandwidthMhz"] = Value::n(a.tune.bandwidthMhz);
    p.values["gainDb"] = Value::n(a.tune.gainDb);
    p.values["lnaDb"] = Value::n(a.tune.lnaDb);
    p.values["vgaDb"] = Value::n(a.tune.vgaDb);
    p.values["ampOn"] = Value::f(a.tune.ampOn);
    p.values["biasTee"] = Value::f(a.tune.biasTee);
    p.values["spec.avg"] = Value::n(std::lround(a.specAvgSec * 1000.f));   // milliseconds
    for (auto& pr : providers()) {
        Value v;
        if (pr.get && pr.get(a, v) && validate(pr.key, v).empty()) p.values[pr.key] = v;
    }
    // drop anything that would not pass validation (a value left over from another radio, say)
    for (auto it = p.values.begin(); it != p.values.end();) it = validate(it->first, it->second).empty() ? std::next(it) : p.values.erase(it);
    return p;
}

ApplyResult apply(App& a, const Preset& p) {
    ApplyResult r = applyPreset(p, capsOf(a));
    bool retune = false;
    std::map<std::string, Value> done;
    for (const auto& [k, v] : r.apply) {
        if (Provider* pr = providerFor(k)) { if (pr->set) pr->set(a, v); continue; }
        if (k == "freqMhz") { tuneFreq(a, v.num); retune = false; }   // tuneFreq retunes a running receiver itself
        else if (k == "sampleRate") { a.tune.sampleRate = v.num; retune = true; }
        else if (k == "bandwidthMhz") { a.tune.bandwidthMhz = v.num; retune = true; }
        else if (k == "gainDb") { a.tune.gainDb = v.num; retune = true; }
        else if (k == "lnaDb") { a.tune.lnaDb = (int)v.num; retune = true; }
        else if (k == "vgaDb") { a.tune.vgaDb = (int)v.num; retune = true; }
        else if (k == "ampOn") { a.tune.ampOn = v.flag; retune = true; }
        else if (k == "biasTee") { a.tune.biasTee = v.flag; retune = true; }
        else if (k == "spec.avg") a.specAvgSec = (float)(v.num / 1000.0);
        else { r.notes.push_back("'" + k + "' is not used by this version of the program"); ++r.skipped; }
    }
    if (retune && a.engine.running()) a.engine.retune(a.tune);
    return r;
}

Store& store() {
    static Store s;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::string err;
        if (!loadStoreFile(storePath(), s, err)) s = Store{};   // a damaged file: start empty (the file is only replaced on the next save)
    }
    return s;
}

void save() {
    std::string err;
    saveStoreFile(storePath(), store(), err);
}

void resetModeToDefaults(App& a) {
    const dect2::ModeTuning* t = dect2::modeTuning(engineStd(a));
    if (!t) return;
    Preset p; p.mode = modeKey(a); p.name = "defaults";
    p.values["freqMhz"] = Value::n(t->defMhz);
    p.values["sampleRate"] = Value::n(t->sampleRate);
    p.values["bandwidthMhz"] = Value::n(t->bandwidthMhz);
    apply(a, p);
}

void applyModeDefault(App& a) {
    Store& s = store();
    auto it = s.defaultFor.find(modeKey(a));
    if (it == s.defaultFor.end()) return;
    if (const Preset* p = s.find(it->second)) { apply(a, *p); s.touch(p->id); }
}

void toolbarButton(App& a) {
    Store& s = store();
    static char name[64] = "";
    static std::string msg;
    if (ImGui::Button(tr("Presets"))) ImGui::OpenPopup("##presets");
    if (!ImGui::BeginPopup("##presets")) return;
    const std::string mode = modeKey(a);
    auto run = [&](const Preset& p) {
        const ApplyResult r = apply(a, p);
        s.touch(p.id); save();
        msg = r.notes.empty() ? "" : r.notes.front() + (r.notes.size() > 1 ? " (+" + std::to_string(r.notes.size() - 1) + " more)" : "");
    };
    ImGui::SetNextItemWidth(160 * gUi);
    ImGui::InputTextWithHint("##pname", tr("name"), name, sizeof name);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save as...")) && name[0]) {
        s.add(capture(a, name)); save(); name[0] = 0; msg.clear();
    }
    const auto here = s.forMode(mode);
    if (ImGui::BeginMenu(tr("Presets for this mode"), !here.empty())) {
        for (const Preset* p : here) {
            std::string lbl = (s.isFavourite(p->id) ? "* " : "") + p->name + (s.defaultFor.count(mode) && s.defaultFor[mode] == p->id ? "  (default)" : "");
            if (ImGui::MenuItem(lbl.c_str())) run(*p);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Recent"), !s.recent.empty())) {
        for (const auto& id : std::vector<std::string>(s.recent)) if (const Preset* p = s.find(id)) if (ImGui::MenuItem((p->name + "  [" + p->mode + "]").c_str())) run(*p);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Favourites"), !s.favourite.empty())) {
        for (const Preset* p : s.favourites()) if (ImGui::MenuItem((p->name + "  [" + p->mode + "]").c_str())) run(*p);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(tr("Manage"), !here.empty())) {
        for (const Preset* p : here) {
            if (!ImGui::BeginMenu(p->name.c_str())) continue;
            const bool fav = s.isFavourite(p->id);
            if (ImGui::MenuItem(fav ? tr("Remove from favourites") : tr("Add to favourites"))) { s.setFavourite(p->id, !fav); save(); }
            const bool isDef = s.defaultFor.count(mode) && s.defaultFor[mode] == p->id;
            if (ImGui::MenuItem(isDef ? tr("Not the default for this mode") : tr("Default for this mode"))) { s.setDefault(mode, isDef ? "" : p->id); save(); }
            if (!p->builtin) {
                if (ImGui::MenuItem(tr("Rename to the name above"), nullptr, false, name[0] != 0)) { if (s.rename(p->id, name)) { name[0] = 0; save(); } else msg = tr("That name is taken"); }
                if (ImGui::MenuItem(tr("Update with the current settings"))) {
                    Preset np = capture(a, p->name); np.id = p->id; mergeUnknown(np, *p);
                    for (auto& q : s.user) if (q.id == p->id) q = np;
                    save();
                }
            }
            if (ImGui::MenuItem(tr("Copy as .onairpreset text"))) ImGui::SetClipboardText(exportText(*p).c_str());
            if (ImGui::MenuItem(tr("Copy with my location"))) ImGui::SetClipboardText(exportText(*p, ExportOptions{true}).c_str());
            if (ImGui::MenuItem(tr("Save as a file"))) {
                const std::string path = plat::dataDir() + "/" + suggestedFileName(*p);
                std::ofstream f(path, std::ios::binary);
                f << exportText(*p);
                msg = std::string(tr("Saved ")) + path;
            }
            if (!p->builtin && ImGui::MenuItem(tr("Delete"))) { const std::string id = p->id; ImGui::EndMenu(); s.remove(id); save(); break; }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(tr("Import from the clipboard"))) {
        Preset p; std::string err;
        const char* t = ImGui::GetClipboardText();
        if (t && importText(t, p, err)) { s.add(p); save(); msg = err.empty() ? tr("Imported") : err; }
        else msg = err.empty() ? tr("Nothing to import") : err;
    }
    if (ImGui::MenuItem(tr("Reset mode to defaults"))) { resetModeToDefaults(a); msg.clear(); }
    if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
    ImGui::EndPopup();
}

}  // namespace presetsapp
