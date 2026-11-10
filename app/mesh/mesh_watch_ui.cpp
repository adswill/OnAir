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

// ---- LoRaWAN receive-only packet forwarder (dect2/mesh/lorawan_fwd.h): the user's own gateway entry, uplinks only, never transmits ----
#include "dect2/mesh/lorawan_fwd.h"
#include <set>
void meshFwdTick(App& a, bool live);
void meshFwdSettings();
namespace {
struct Fw { bool loaded = false, on = false; char host[96] = {}, eui[24] = {}; int port = 1700; dect2::SemtechForwarder f; std::string err; std::set<std::string> sent; bool configured = false; };
Fw F;
void fwLoad() {
    if (F.loaded) return;
    F.loaded = true;
    plat::Prefs& p = plat::prefs();
    F.on = p.getB("lorawanFwdOn", false);
    std::snprintf(F.host, sizeof F.host, "%s", p.getS("lorawanFwdHost", "").c_str());
    std::snprintf(F.eui, sizeof F.eui, "%s", p.getS("lorawanFwdEui", "").c_str());
    F.port = (int)p.getI("lorawanFwdPort", 1700);
}
void fwApply() {
    F.err.clear();
    F.f.configure(F.on ? F.host : "", F.port, F.eui, F.err);
    F.configured = true;
}
}  // namespace

void meshFwdTick(App& a, bool live) {
    fwLoad();
    if (!F.configured) fwApply();
    if (!F.on || !live) return;
    for (const auto& p : a.rx.mesh.packets) {
        if (!dect2::semtechIsUplink(p)) continue;
        char k[64]; std::snprintf(k, sizeof k, "%.3f|", p.timeSec);
        if (!F.sent.insert(k + p.raw).second) continue;
        F.f.forward(p);
    }
    if (F.sent.size() > 5000) F.sent.clear();
    F.f.poll(ImGui::GetTime());
}

void meshFwdSettings() {
    fwLoad();
    ImGui::TextDisabled("%s", tr("LoRaWAN packet forwarder (receive only)"));
    bool ch = ImGui::Checkbox(tr("Forward LoRaWAN uplinks to my network server##lwf"), &F.on);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Semtech UDP packet forwarder: the uplinks OnAir hears go to your own gateway entry on ChirpStack or The Things Network (register a gateway with the EUI below). Downlinks are never transmitted."));
    ImGui::SetNextItemWidth(220 * gUi); ch |= ImGui::InputTextWithHint("##lwfhost", "eu1.cloud.thethings.network", F.host, sizeof F.host);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputInt("##lwfport", &F.port, 0, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(170 * gUi); ch |= ImGui::InputTextWithHint("##lwfeui", tr("gateway EUI, 16 hex digits"), F.eui, sizeof F.eui);
    if (ch) {
        plat::Prefs& p = plat::prefs();
        p.setB("lorawanFwdOn", F.on); p.setS("lorawanFwdHost", F.host); p.setI("lorawanFwdPort", F.port); p.setS("lorawanFwdEui", F.eui);
        fwApply();
    }
    const dect2::SemtechStats st = F.f.stats();
    if (F.on) ImGui::TextDisabled(tr("%llu sent, %llu acknowledged, keep-alive %llu / %llu%s%s"), (unsigned long long)st.pushed, (unsigned long long)st.acked, (unsigned long long)st.pullAcks,
                                  (unsigned long long)st.pulls, F.err.empty() ? "" : "  ", F.err.c_str());
}
