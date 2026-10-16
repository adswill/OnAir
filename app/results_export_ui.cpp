// GUI half of the results export (dialog + button); the pure half is results_export.cpp.
#include "results_export.h"
#include "app.h"
#include "icons.h"
#include "dect2/u8file.h"
#include <ctime>
#include <fstream>

namespace resexp {

static const double kStart = (double)std::time(nullptr);   // when OnAir started: the "since start" cut

bool saveButton(const Table& t, const char* kind, std::string& note, const char* id) {
    static int fmt = 0, scope = 1;   // Fmt order below; scope 0 = since start, 1 = all
    static const Fmt order[] = {Fmt::Csv, Fmt::Json, Fmt::Adif, Fmt::Kml, Fmt::Gpx};
    ImGui::PushID(id);
    bool hit = iconFlat(Ic::Save, "Save results: CSV, JSON, ADIF logbook (ham modes), KML or GPX (anything with positions)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save results...")) hit = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::Combo("##fmt", &fmt, "CSV\0JSON\0ADIF\0KML\0GPX\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##scope", &scope, "Since start\0All\0");
    ImGui::PopID();
    if (!hit) return false;
    const Fmt f = order[fmt];
    Table w = t;
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

}  // namespace resexp
