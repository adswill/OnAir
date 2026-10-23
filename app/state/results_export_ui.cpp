// GUI half of the results export (dialog + button); the pure half is results_export.cpp.
#include "results_export.h"
#include "../app.h"
#include "../ui/icons.h"
#include "../clips/clips.h"
#include <ctime>
#include <fstream>
#include "dect2/common/u8file.h"

namespace resexp {

static const double kStart = (double)std::time(nullptr);   // when OnAir started: the "since start" cut

static bool saveButtonImpl(const Table* t, const std::function<Table()>* make, const char* kind, std::string& note, const char* id) {
    static int fmt = 0, scope = 1;   // Fmt order below; scope 0 = since start, 1 = all
    static const Fmt order[] = {Fmt::Csv, Fmt::Json, Fmt::Adif, Fmt::Kml, Fmt::Gpx, Fmt::GeoJson};
    if (t) clips::tableLog().observe(kind, *t, (double)std::time(nullptr));   // radio clips: remembers what the table showed (rate limited inside)
    else if (clips::tableLog().due(kind, (double)std::time(nullptr))) clips::tableLog().observe(kind, (*make)(), (double)std::time(nullptr));
    ImGui::PushID(id);
    bool hit = iconFlat(Ic::Save, "Save results: CSV, JSON, ADIF logbook (ham modes), KML, GPX or GeoJSON (anything with positions)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save results...")) hit = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::Combo("##fmt", &fmt, "CSV\0JSON\0ADIF\0KML\0GPX\0GeoJSON\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * gUi);
    {
        const char* scopes[] = {tr("Since start"), tr("All")};
        ImGui::Combo("##scope", &scope, scopes, IM_ARRAYSIZE(scopes));
    }
    ImGui::PopID();
    if (!hit) return false;
    const Fmt f = order[fmt];
    Table w = t ? *t : (*make)();
    const double now = (double)std::time(nullptr);
    w.epoch.resize(w.rows.size(), 0);
    for (double& e : w.epoch) if (e <= 0) e = now;   // a live snapshot: seen now
    if (scope == 0) w = since(w, kStart);
    const std::string nm = std::string(f == Fmt::Adif ? "log-" : "scan-") + kind + "-" + stamp() + fmtExt(f);
    const std::string path = saveFileDialog(nm.c_str());
    if (path.empty()) return true;
    const Fmt pf = fmtFromPath(path);
    const bool typed = path.size() >= 4 && path.rfind('.') != std::string::npos && path.size() - path.rfind('.') <= 5;
    std::ofstream o(dect2::u8file(path), std::ios::binary);
    o << render(w, typed ? pf : f);
    o.flush();
    note = o ? "saved " + path : "could not write " + path;
    return true;
}

bool saveButton(const Table& t, const char* kind, std::string& note, const char* id) { return saveButtonImpl(&t, nullptr, kind, note, id); }
bool saveButtonLazy(const std::function<Table()>& make, const char* kind, std::string& note, const char* id) { return saveButtonImpl(nullptr, &make, kind, note, id); }

}  // namespace resexp
