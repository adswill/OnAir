// Mesh node watch in the app: the watched nodes (right-click a node), the limits, and the lora.node_* events every 10 s. Core: mesh_watch.h.
#include "app.h"
#include "dect2/automation/engine.h"
#include "dect2/mesh/mesh_watch.h"
#include "i18n/i18n.h"
#include <ctime>
#include <sstream>

void meshWatchTick(App& a, bool live);
bool meshWatchIs(const std::string& id);
void meshWatchToggle(const std::string& id);
void meshWatchSettings();

namespace {
struct Mw { bool loaded = false; dect2::MeshWatchConfig cfg; dect2::MeshWatch w; double last = 0; };
Mw W;
void load() {
    if (W.loaded) return;
    W.loaded = true;
    plat::Prefs& p = plat::prefs();
    std::stringstream ss(p.getS("meshWatchIds", ""));
    std::string id;
    while (std::getline(ss, id, ',')) if (!id.empty()) W.cfg.watched.insert(id);
    W.cfg.silentHours = p.getD("meshWatchSilentH", 6);
    W.cfg.batteryPct = p.getD("meshWatchBattery", 20);
    W.cfg.newWithinKm = p.getD("meshWatchNewKm", 0);
    W.w.parse(p.getS("meshWatchState", ""));
}
void save() {
    plat::Prefs& p = plat::prefs();
    std::string ids;
    for (const auto& x : W.cfg.watched) ids += (ids.empty() ? "" : ",") + x;
    p.setS("meshWatchIds", ids); p.setD("meshWatchSilentH", W.cfg.silentHours); p.setD("meshWatchBattery", W.cfg.batteryPct); p.setD("meshWatchNewKm", W.cfg.newWithinKm);
}
}  // namespace

bool meshWatchIs(const std::string& id) { load(); return W.cfg.watched.count(id) != 0; }
void meshWatchToggle(const std::string& id) { load(); if (!W.cfg.watched.erase(id)) W.cfg.watched.insert(id); save(); }

void meshWatchTick(App& a, bool live) {
    load();
    if (!live) return;
    const double now = (double)time(nullptr);
    if (now - W.last < 10) return;
    W.last = now;
    W.cfg.hasHome = receiverLocationSet();
    if (W.cfg.hasHome) receiverLocation(W.cfg.homeLat, W.cfg.homeLon);
    auto ev = W.w.update(a.rx.mesh.nodes, W.cfg, now);
    if (!W.cfg.watched.empty()) plat::prefs().setS("meshWatchState", W.w.serialize());
    namespace au = dect2::automation;
    if (au::wants(au::FLora)) for (auto& e : ev) au::emit(std::move(e));
}

void meshWatchSettings() {
    load();
    ImGui::TextDisabled("%s", tr("Node watch (right-click a node in the node table to watch it)"));
    bool ch = false;
    float h = (float)W.cfg.silentHours, b = (float)W.cfg.batteryPct, k = (float)W.cfg.newWithinKm;
    ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputFloat(tr("silent after (h, 0 off)##mw"), &h, 1, 6, "%.0f")) { W.cfg.silentHours = std::max(0.f, h); ch = true; }
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputFloat(tr("battery below (%, 0 off)##mw"), &b, 5, 10, "%.0f")) { W.cfg.batteryPct = std::max(0.f, std::min(100.f, b)); ch = true; }
    ImGui::SetNextItemWidth(90 * gUi); if (ImGui::InputFloat(tr("new node within (km, 0 off)##mw"), &k, 1, 10, "%.0f")) { W.cfg.newWithinKm = std::max(0.f, k); ch = true; }
    ImGui::TextDisabled(tr("%zu nodes watched. The alerts are the automations Mesh node silent, Mesh node battery low and New mesh node near me (off until you switch them on)."), W.cfg.watched.size());
    if (ch) save();
}
