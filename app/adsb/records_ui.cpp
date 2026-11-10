// Records board: see records_ui.h.
#include "app.h"
#include "adsb/records_ui.h"
#include "dect2/common/records.h"
#include "i18n/i18n.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
struct Rb { bool loaded = false; dect2::RecordsBoard b; double lastSave = 0; std::string note; double noteAt = -100; };
Rb B;
fs::path file() { return fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/stats").c_str())) / "records.txt"; }
void load() {
    if (B.loaded) return;
    B.loaded = true;
    std::ifstream f(file(), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    B.b.parse(ss.str());
}
void save() {
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    std::ofstream o(file(), std::ios::binary | std::ios::trunc);
    o << B.b.serialize();
    B.b.clearDirty();
}
}  // namespace

double recordsDistKm(double lat, double lon) {
    if (!receiverLocationSet()) return -1;
    double la, lo;
    receiverLocation(la, lo);
    const double r = 3.14159265358979 / 180, a = std::sin((lat - la) * r / 2), b = std::sin((lon - lo) * r / 2);
    const double h = a * a + std::cos(la * r) * std::cos(lat * r) * b * b;
    return 6371.0 * 2 * std::atan2(std::sqrt(h), std::sqrt(1 - h));
}

void recordsOffer(const char* cat, const std::string& band, double km, const std::string& id, const std::string& detail) {
    load();
    if (km <= 0) return;
    if (B.b.offer(cat, band, km, id, detail, (int64_t)time(nullptr))) {
        char n[160];
        std::snprintf(n, sizeof n, "New record: %s%s%s %.0f km (%s)", cat, band.empty() ? "" : " ", band.c_str(), km, id.c_str());
        B.note = n;
    }
    const double now = (double)time(nullptr);
    if (B.b.dirty() && now - B.lastSave > 60) { B.lastSave = now; save(); }
}

void recordsPanel() {
    load();
    if (!ImGui::CollapsingHeader(tr("Records board (personal best distances)##recb"))) return;
    if (!B.note.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", B.note.c_str());
    if (!receiverLocationSet()) ImGui::TextDisabled("%s", tr("Set the receiver position (Tuner, Locate): records are distances from it."));
    const auto v = B.b.all();
    if (v.empty()) { ImGui::TextDisabled("%s", tr("No records yet: they fill as the modes hear positions.")); return; }
    if (ImGui::BeginTable("##recbt", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("Mode")); ImGui::TableSetupColumn(tr("Distance")); ImGui::TableSetupColumn(tr("Who")); ImGui::TableSetupColumn(tr("When")); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& r : v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", r.category.c_str(), r.band.empty() ? "" : " ", r.band.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.0f km", r.km);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.id.c_str());
            ImGui::TableNextColumn(); { const time_t t = (time_t)r.utc; struct tm l = *localtime(&t); char b[24]; std::strftime(b, sizeof b, "%Y-%m-%d", &l); ImGui::TextUnformatted(b); }
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.detail.c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::SmallButton(tr("Reset all records##recb"))) ImGui::OpenPopup("##recbreset");
    if (ImGui::BeginPopup("##recbreset")) {
        ImGui::TextUnformatted(tr("Forget every record?"));
        if (ImGui::Button(tr("Reset##recbyes"))) { B.b.clear(""); save(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button(tr("Cancel##recbno"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
