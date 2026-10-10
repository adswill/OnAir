// Mesh (LoRa) screens: node table, APRS station table (LoRa APRS, MeshCom), map of node and station positions, chat per channel,
// packet log with the raw payload, settings (band plan, protocols, presets, channel keys).
#include "app.h"
#include "adsb_map.h"
#include "dect2/packet_aprs.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace {

struct Key { int proto; std::string name, secret; };

struct State {
    bool loaded = false;
    int view = 0;                                // 0 nodes, 1 map, 2 chat, 3 packets, 4 settings, 5 APRS stations
    bool rawView = false;                        // packets: the raw payload instead of the detail line
    adsbmap::View map;
    bool mapFit = true;                          // centre on the nodes until the user drags the map
    std::string sel;                             // selected node: protocol digit + id
    int chatChannel = 0;                         // index into the channel list built each frame (0 = all)
    int region = 0, protocols = 15;              // region: index into kPlans
    bool allPresets = false;
    std::vector<Key> keys;
    bool keysPushed = false;
    int pushedRegion = -1, pushedProtocols = -1, pushedPresets = -1;
    double pushedTuned = 0;
    char keyName[40] = "", keySecret[96] = "";
    int keyProto = 0;
    std::string keyMsg;
    uint64_t lastSeq = 0;
    std::deque<float> snr;                       // SNR of the last good frame per report, for the history plot
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 21; }

ImVec4 protoColour(int proto) {
    switch (proto) {
    case 1: return ImVec4(0.46f, 0.84f, 0.70f, 1);
    case 2: return ImVec4(0.96f, 0.68f, 0.34f, 1);
    case 3: return ImVec4(0.55f, 0.70f, 0.98f, 1);
    default: return ImVec4(0.86f, 0.58f, 0.92f, 1);
    }
}
const char* protoName(int proto) { return proto == 1 ? "Meshtastic" : proto == 2 ? "MeshCore" : proto == 3 ? "LoRa APRS" : "MeshCom"; }
const char* protoTag(int proto) { return proto == 1 ? "Mt" : proto == 2 ? "Mc" : proto == 3 ? "APRS" : "MCom"; }

// band plans: the receiver's region (Meshtastic / MeshCore channel plan), the frequency to tune and the test signal's scene
struct Plan { const char* name; const char* shortName; double mhz; int rxRegion, synthRegion; };
const Plan kPlans[] = {
    {"EU 868  (Meshtastic LongFast 869.525 MHz)", "EU 868  869.525 MHz", 869.525, 0, 0},
    {"US 915  (Meshtastic LongFast 906.875 MHz)", "US 915  906.875 MHz", 906.875, 1, 1},
    {"LoRa APRS EU  433.775 MHz  SF12", "APRS EU  433.775 MHz", 433.775, 0, 2},
    {"LoRa APRS PL  434.855 MHz  SF9", "APRS PL  434.855 MHz", 434.855, 0, 2},
    {"LoRa APRS UK  439.9125 MHz  SF12", "APRS UK  439.9125 MHz", 439.9125, 0, 2},
    {"MeshCom EU  433.175 MHz (+ LoRa APRS 433.775)", "MeshCom EU  433.175 MHz", 433.175, 0, 2},
};
constexpr int kNumPlans = (int)(sizeof(kPlans) / sizeof(kPlans[0]));
const char* planName(void*, int i) { return kPlans[i].name; }
const char* planShort(void*, int i) { return kPlans[i].shortName; }

std::string nodeKey(const dect2::MeshNode& n) { return std::to_string(n.protocol) + n.id; }

std::string nodeLabel(const dect2::MeshNode& n) {
    if (!n.longName.empty()) return n.longName;
    return n.id;
}

std::string clock(double s) {
    char b[24];
    const int t = (int)std::max(0.0, s);
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string ago(double now, double then) {
    if (then < 0) return "-";
    const int d = (int)std::max(0.0, now - then);
    char b[24];
    if (d < 90) snprintf(b, sizeof b, "%d s", d);
    else if (d < 5400) snprintf(b, sizeof b, "%d min", (d + 30) / 60);
    else snprintf(b, sizeof b, "%.1f h", d / 3600.0);
    return b;
}

// the saved keys: one line each, "protocol<TAB>name<TAB>secret"
void saveKeys() {
    std::string s;
    for (const auto& k : S.keys) s += std::to_string(k.proto) + "\t" + k.name + "\t" + k.secret + "\n";
    plat::prefs().setS("meshKeys", s);
}

void pushKeys(App& a) {
    a.engine.mesh().clearUserChannels();
    for (const auto& k : S.keys) {
        if (k.proto == 1) a.engine.mesh().addMeshtasticChannel(k.name, k.secret);
        else a.engine.mesh().addMeshCoreChannel(k.name, k.secret);
    }
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("meshZoom", 10);
    S.view = (int)d.getI("meshView", 0);
    S.region = std::max(0, std::min(kNumPlans - 1, (int)d.getI("meshRegion", 0)));
    S.protocols = (int)d.getI("meshProtocols", 15);
    S.allPresets = d.getB("meshPresets", false);
    const std::string s = d.getS("meshKeys", "");
    size_t pos = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        const std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        const size_t t1 = line.find('\t');
        const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        S.keys.push_back({atoi(line.c_str()), line.substr(t1 + 1, t2 - t1 - 1), line.substr(t2 + 1)});
    }
}

void applyRegion(App& a, int region) {
    S.region = region;
    plat::prefs().setI("meshRegion", region);
    a.freqMhz = kPlans[region].mhz;                    // the plan's channel (Meshtastic LongFast, LoRa APRS, MeshCom)
    a.tune.synth.modeOpt[1] = kPlans[region].synthRegion;
}

void tick(App& a) {
    loadState();
    if (a.modePreset == 2) {   // picked as "LoRa APRS / MeshCom" in the mode list: the LoRa APRS EU plan, LoRa APRS decoding on, the APRS stations view
        a.modePreset = 0;
        applyRegion(a, 2);
        S.protocols |= 4 | 8;
        plat::prefs().setI("meshProtocols", S.protocols);
        S.view = 5;
        plat::prefs().setI("meshView", S.view);
        savePrefs(a);   // the plan's frequency
    }
    dect2::MeshReceiver& r = a.engine.mesh();
    if (!S.keysPushed) { pushKeys(a); S.keysPushed = true; }
    if (S.pushedRegion != S.region) { r.setRegion(kPlans[S.region].rxRegion); S.pushedRegion = S.region; }
    if (S.pushedProtocols != S.protocols) { r.setProtocols(S.protocols); S.pushedProtocols = S.protocols; }
    if (S.pushedPresets != (int)S.allPresets) { r.setPresetSearch(S.allPresets); S.pushedPresets = (int)S.allPresets; }
    if (S.pushedTuned != a.freqMhz) { r.setTunedHz(a.freqMhz * 1e6); S.pushedTuned = a.freqMhz; }   // the engine does not pass the user's frequency
    if (!a.engine.running()) { S.lastSeq = 0; return; }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        if (a.rx.mesh.dataValid) { S.snr.push_back(a.rx.mesh.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
        if (S.mapFit) {
            double la = 0, lo = 0; int n = 0;
            for (const auto& nd : a.rx.mesh.nodes) if (nd.hasPosition) { la += nd.lat; lo += nd.lon; n++; }
            for (const auto& st : a.rx.mesh.aprsStations) if (st.st.hasPos) { la += st.st.lat; lo += st.st.lon; n++; }
            if (n) { S.map.lat = la / n; S.map.lon = lo / n; }
        }
    }
}

// ---------------------------------------------------------------- tables

std::vector<const dect2::MeshNode*> sortedNodes(const dect2::MeshTelemetry& t) {
    std::vector<const dect2::MeshNode*> v;
    for (const auto& n : t.nodes) v.push_back(&n);
    std::sort(v.begin(), v.end(), [](const dect2::MeshNode* x, const dect2::MeshNode* y) { return x->lastHeard > y->lastHeard; });
    return v;
}

// the width of a fixed column: its header or its widest usual value, whichever is wider (an auto-fit column was sized to its cells
// only, and cut its header short)
float hdrW(const char* header, const char* sample) { return std::max(ImGui::CalcTextSize(header).x, ImGui::CalcTextSize(sample).x); }

void nodeTable(const App& a, bool compact) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const int cols = compact ? 3 : 11;
    if (!ImGui::BeginTable(compact ? "##mn_s" : "##mn_f", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | (compact ? 0 : ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX),
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (compact) {
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR");
    } else {
        ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160 * gUi); ImGui::TableSetupColumn("Short");
        ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 110 * gUi); ImGui::TableSetupColumn("Hardware", ImGuiTableColumnFlags_WidthFixed, 170 * gUi);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 90 * gUi); ImGui::TableSetupColumn("Heard");
        ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5")); ImGui::TableSetupColumn("Hops", ImGuiTableColumnFlags_WidthFixed, hdrW("Hops", "3/7"));
        ImGui::TableSetupColumn("Battery", ImGuiTableColumnFlags_WidthFixed, hdrW("Battery", "100%")); ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthFixed, hdrW("Packets", "99999"));
    }
    ImGui::TableHeadersRow();
    if (on) {
        for (const dect2::MeshNode* n : sortedNodes(t)) {
            ImGui::TableNextRow();
            const bool sel = S.sel == nodeKey(*n);
            ImGui::TableNextColumn();
            ImGui::PushID(nodeKey(*n).c_str());
            if (compact) {
                if (ImGui::Selectable(nodeLabel(*n).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
            } else {
                if (ImGui::Selectable(protoTag(n->protocol), sel, ImGuiSelectableFlags_SpanAllColumns)) S.sel = nodeKey(*n);
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(n->protocol), "%s", n->longName.empty() ? "(no name yet)" : n->longName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->shortName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->hwModel.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(n->role.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, n->lastHeard).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", n->lastSnrDb);
                ImGui::TableNextColumn(); if (n->hopsAway >= 0) ImGui::Text("%d", n->hopsAway); else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn();
                if (n->batteryPct >= 0) ImGui::Text("%.0f %%", n->batteryPct);
                else if (n->voltage > 0) ImGui::Text("%.2f V", n->voltage);
                else ImGui::TextUnformatted("-");
                ImGui::TableNextColumn(); ImGui::Text("%u", n->packets);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || t.nodes.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see nodes" : "no node heard yet: a Meshtastic node announces itself every few minutes"); ImGui::PopTextWrapPos(); }
}

void packetTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    ImGui::Checkbox("raw payload", &S.rawView);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the frame's bytes as text (other bytes as \\xNN) in the last column, for LoRa APRS, MeshCom\nand frames that did not parse. Hover a row's last column to see it either way.");
    if (!ImGui::BeginTable("##mp", 11, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Content"); ImGui::TableSetupColumn("Hops"); ImGui::TableSetupColumn("SNR dB");
    ImGui::TableSetupColumn("SF / BW"); ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        for (size_t i = t.packets.size(); i-- > 0;) {
            const dect2::MeshPacket& p = t.packets[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(p.protocol), "%s", protoTag(p.protocol));
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextColored(pal::badRed(), "CRC error");
            else ImGui::TextUnformatted(p.type.empty() ? "?" : p.type.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.from.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.to.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", p.size);
            ImGui::TableNextColumn();
            if (!p.crcOk) ImGui::TextDisabled("-");
            else if (p.decrypted) ImGui::TextColored(pal::okGreen(), "decrypted");
            else ImGui::TextColored(pal::warnAmber(), "%s", p.note.empty() ? "encrypted" : p.note.c_str());
            ImGui::TableNextColumn();
            if (p.hopStart >= 0 && p.hopLimit >= 0) ImGui::Text("%d", p.hopStart - p.hopLimit);
            else if (!p.path.empty() && p.protocol >= 3) ImGui::TextUnformatted(p.path.c_str());
            else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            ImGui::TableNextColumn(); ImGui::Text("%d / %.1f", p.sf, p.bwHz / 1e3);
            ImGui::TableNextColumn();
            if (S.rawView && !p.raw.empty()) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopFont(); }
            else ImGui::TextUnformatted(p.detail.c_str());
            if (!p.raw.empty() && ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(560 * gUi); ImGui::TextUnformatted(p.raw.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
        }
    }
    ImGui::EndTable();
    if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("start the receiver to see packets"); ImGui::PopTextWrapPos(); }
}

std::string aprsKey(const dect2::MeshAprsStation& s) { return "A" + std::to_string(s.protocol) + s.st.call; }

std::string symbolText(const dect2::PacketStation& s) {
    if (!s.symTable) return "-";
    char b[4] = {s.symTable, s.symCode, 0, 0};
    return b;
}

void aprsTable(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    if (!ImGui::BeginTable("##maprs", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthFixed, hdrW("Call", "A61ABC-15")); ImGui::TableSetupColumn("Sym");
    ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, hdrW("Position", "25.0805N 155.1403E")); ImGui::TableSetupColumn("Last");
    ImGui::TableSetupColumn("Heard"); ImGui::TableSetupColumn("SNR dB", ImGuiTableColumnFlags_WidthFixed, hdrW("SNR dB", "-12.5"));
    ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, hdrW("Frames", "9999")); ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthFixed, 320 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        std::vector<const dect2::MeshAprsStation*> v;
        for (const auto& x : t.aprsStations) v.push_back(&x);
        std::sort(v.begin(), v.end(), [](const dect2::MeshAprsStation* x, const dect2::MeshAprsStation* y) { return x->st.lastHeardSec > y->st.lastHeardSec; });
        for (const auto* s : v) {
            ImGui::TableNextRow();
            ImGui::PushID(aprsKey(*s).c_str());
            ImGui::TableNextColumn();
            if (ImGui::Selectable(protoTag(s->protocol), S.sel == aprsKey(*s), ImGuiSelectableFlags_SpanAllColumns)) S.sel = aprsKey(*s);
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(s->protocol), "%s%s", s->st.call.c_str(), s->st.isObject ? " (obj)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(symbolText(s->st).c_str());
            ImGui::TableNextColumn();
            if (s->st.hasPos) ImGui::Text("%s %s", dect2::aprs::fmtLat(s->st.lat).c_str(), dect2::aprs::fmtLon(s->st.lon).c_str()); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->lastType.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, s->st.lastHeardSec).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f", s->lastSnrDb);
            ImGui::TableNextColumn(); ImGui::Text("%u", s->st.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s->st.isObject ? ("by " + s->st.via).c_str() : s->path.c_str());
            ImGui::TableNextColumn();
            if (s->batteryPct >= 0) { ImGui::Text("battery %d %%", s->batteryPct); ImGui::SameLine(0, 6 * gUi); }
            ImGui::TextUnformatted(s->st.comment.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    if (!on || t.aprsStations.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !on ? "start the receiver to see stations" : "no LoRa APRS or MeshCom station heard yet: pick a 70 cm plan (433.775 or 433.175 MHz) in the tuner");
        ImGui::PopTextWrapPos();
    }
}

// ---------------------------------------------------------------- map

void mapView(App& a, ImVec2 size) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFit = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    const dect2::MeshNode* tip = nullptr; float tipD = 14 * gUi;
    int shown = 0;
    if (on) {
        for (const auto& n : t.nodes) {
            if (!n.hasPosition) continue;
            shown++;
            const ImVec2 q = adsbmap::project(n.lat, n.lon);
            const bool sel = S.sel == nodeKey(n);
            const ImVec4 c = protoColour(n.protocol);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
            dl->AddCircleFilled(q, 6 * gUi, col, 20);
            dl->AddCircle(q, 6 * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), 20, sel ? 2.f : 1.2f);
            const std::string lb = !n.shortName.empty() ? n.shortName : !n.longName.empty() ? n.longName : n.id;
            const ImVec2 ts = ImGui::CalcTextSize(lb.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), lb.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tip = &n; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = nodeKey(n);
        }
    }
    const dect2::MeshAprsStation* tipA = nullptr;
    if (on) {
        for (const auto& s : t.aprsStations) {
            if (!s.st.hasPos) continue;
            shown++;
            const ImVec2 q = adsbmap::project(s.st.lat, s.st.lon);
            const bool sel = S.sel == aprsKey(s);
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(protoColour(s.protocol));
            const float r = 6 * gUi;
            // a diamond: APRS stations and objects, apart from the round mesh nodes
            dl->AddQuadFilled(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), col);
            dl->AddQuad(ImVec2(q.x, q.y - r), ImVec2(q.x + r, q.y), ImVec2(q.x, q.y + r), ImVec2(q.x - r, q.y), sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(20, 24, 26, 220), sel ? 2.f : 1.2f);
            const ImVec2 ts = ImGui::CalcTextSize(s.st.call.c_str());
            dl->AddRectFilled(ImVec2(q.x + 8 * gUi, q.y - ts.y * 0.5f - 1), ImVec2(q.x + 12 * gUi + ts.x, q.y + ts.y * 0.5f + 1), IM_COL32(10, 12, 14, 190), 2.f);
            dl->AddText(ImVec2(q.x + 10 * gUi, q.y - ts.y * 0.5f), IM_COL32(230, 233, 236, 255), s.st.call.c_str());
            const float d = std::hypot(q.x - mouse.x, q.y - mouse.y);
            if (d < tipD) { tipD = d; tipA = &s; tip = nullptr; }
            if (clicked && std::hypot(q.x - click.x, q.y - click.y) < 14 * gUi) S.sel = aprsKey(s);
        }
    }
    dl->PopClipRect();
    if (tipA && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s  %s  %s\n%.5f, %.5f\n%s\nSNR %.1f dB, heard %s ago", tipA->st.call.c_str(), protoName(tipA->protocol), symbolText(tipA->st).c_str(), tipA->st.lat, tipA->st.lon,
                          tipA->st.comment.c_str(), tipA->lastSnrDb, ago(t.timeSec, tipA->st.lastHeardSec).c_str());
    }
    if (tip && ImGui::IsWindowHovered()) {
        ImGui::SetTooltip("%s  %s\n%s  %s\n%.5f, %.5f  %.0f m\nSNR %.1f dB, heard %s ago", nodeLabel(*tip).c_str(), tip->id.c_str(), protoName(tip->protocol),
                          tip->hwModel.c_str(), tip->lat, tip->lon, tip->altM, tip->lastSnrDb, ago(t.timeSec, tip->lastHeard).c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1);
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFit = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never a node's position");
    plat::prefs().setI("meshZoom", S.map.zoom);
    if (!on || shown == 0) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
        adsbmap::legend(size.x, "%s", on ? "no node or station has sent its position yet" : "start the receiver");
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

void nodeCard(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const dect2::MeshNode* n = nullptr;
    if (live(a)) for (const auto& x : t.nodes) if (S.sel == nodeKey(x)) n = &x;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(96 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    const dect2::MeshAprsStation* as = nullptr;
    if (live(a)) for (const auto& x : t.aprsStations) if (S.sel == aprsKey(x)) as = &x;
    if (as) {
        ImGui::TextColored(protoColour(as->protocol), "%s", as->st.call.c_str());
        ImGui::TextDisabled("%s%s", protoName(as->protocol), as->st.isObject ? " object" : " station");
        ImGui::Spacing();
        kv("symbol", "%s", symbolText(as->st).c_str());
        if (as->st.isObject) kv("sent by", "%s", as->st.via.c_str());
        kv("heard", "%s ago, %u frames", ago(t.timeSec, as->st.lastHeardSec).c_str(), as->st.count);
        kv("last", "%s", as->lastType.c_str());
        kv("last SNR", "%.1f dB", as->lastSnrDb);
        if (!as->path.empty()) kv("path", "%s", as->path.c_str());
        if (as->st.hasPos) kv("position", "%.5f %.5f", as->st.lat, as->st.lon);
        if (as->st.hasAlt) kv("altitude", "%.0f m", as->st.altM);
        if (as->st.hasCourse || as->st.hasSpeed) kv("course", "%d deg, %.0f km/h", as->st.courseDeg, as->st.speedKnots * 1.852);
        if (as->batteryPct >= 0) kv("battery", "%d %%", as->batteryPct);
        if (!as->st.comment.empty()) kv("comment", "%s", as->st.comment.c_str());
        return;
    }
    if (!n) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Select a node or station in a table or on the map."); ImGui::PopTextWrapPos(); } return; }
    ImGui::TextColored(protoColour(n->protocol), "%s", nodeLabel(*n).c_str());
    ImGui::TextDisabled("%s", protoName(n->protocol));
    ImGui::Spacing();
    kv("id", "%s", n->id.c_str());
    if (!n->shortName.empty()) kv("short name", "%s", n->shortName.c_str());
    if (!n->hwModel.empty()) kv("hardware", "%s", n->hwModel.c_str());
    if (!n->role.empty()) kv("role", "%s", n->role.c_str());
    kv("heard", "%s ago, %u packets", ago(t.timeSec, n->lastHeard).c_str(), n->packets);
    kv("last SNR", "%.1f dB", n->lastSnrDb);
    if (n->hopsAway >= 0) kv("hops away", "%d", n->hopsAway);
    if (n->hasPosition) {
        kv("position", "%.5f %.5f", n->lat, n->lon);
        kv("altitude", "%.0f m", n->altM);
    }
    if (n->batteryPct >= 0) kv("battery", "%.0f %%", n->batteryPct);
    if (n->voltage > 0) kv("voltage", "%.2f V", n->voltage);
    if (n->channelUtilPct >= 0) kv("channel use", "%.1f %%", n->channelUtilPct);
    if (n->airUtilTxPct >= 0) kv("air time tx", "%.2f %%", n->airUtilTxPct);
    if (n->uptimeS >= 0) kv("uptime", "%s", clock((double)n->uptimeS).c_str());
    if (n->hasEnv) {
        kv("temperature", "%.1f C", n->tempC);
        kv("humidity", "%.0f %%", n->humidity);
        kv("pressure", "%.0f hPa", n->pressureHpa);
    }
}

// ---------------------------------------------------------------- chat

void chatView(const App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    std::vector<std::string> chans;      // "<protocol digit><channel>"
    if (on) {
        for (const auto& m : t.messages) {
            const std::string k = std::to_string(m.protocol) + m.channel;
            if (std::find(chans.begin(), chans.end(), k) == chans.end()) chans.push_back(k);
        }
        std::sort(chans.begin(), chans.end());
    }
    if (S.chatChannel > (int)chans.size()) S.chatChannel = 0;
    std::string cur = "All channels";
    if (S.chatChannel > 0) cur = std::string(protoName(chans[S.chatChannel - 1][0] - '0')) + ": " + chans[S.chatChannel - 1].substr(1);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##mchan", cur.c_str())) {
        if (ImGui::Selectable("All channels", S.chatChannel == 0)) S.chatChannel = 0;
        for (size_t i = 0; i < chans.size(); i++) {
            const std::string l = std::string(protoName(chans[i][0] - '0')) + ": " + chans[i].substr(1);
            if (ImGui::Selectable(l.c_str(), S.chatChannel == (int)i + 1)) S.chatChannel = (int)i + 1;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("%zu messages", t.messages.size());
    ImGui::BeginChild("##mchat", ImVec2(0, 0), ImGuiChildFlags_Borders);
    bool any = false;
    if (on) {
        for (const auto& m : t.messages) {
            if (S.chatChannel > 0 && std::to_string(m.protocol) + m.channel != chans[S.chatChannel - 1]) continue;
            any = true;
            ImGui::TextDisabled("%s", clock(m.timeSec).c_str());
            ImGui::SameLine(0, 8 * gUi);
            ImGui::TextColored(protoColour(m.protocol), "%s", protoTag(m.protocol));
            ImGui::SameLine(0, 8 * gUi);
            if (S.chatChannel == 0) { ImGui::TextDisabled("[%s]", m.channel.c_str()); ImGui::SameLine(0, 6 * gUi); }
            const std::string who = m.fromName.empty() ? m.from : m.fromName;
            ImGui::TextColored(ImVec4(0.9f, 0.92f, 0.94f, 1), "%s", who.c_str());
            if ((m.protocol == 1 && m.to != "^all" && !m.to.empty()) || (m.protocol >= 3 && !m.to.empty() && m.to != "*")) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("to %s", m.to.c_str()); }
            if (m.hops > 0) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("(%d hop%s)", m.hops, m.hops == 1 ? "" : "s"); }
            ImGui::PushTextWrapPos(0);
            ImGui::Indent(18 * gUi);
            ImGui::TextUnformatted(m.text.c_str());
            ImGui::Unindent(18 * gUi);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }
    }
    if (!any) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", !on ? "start the receiver to see messages" : "no text message yet"); ImGui::PopTextWrapPos(); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- settings


void settingsView(App& a) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(340 * gUi);
    int r = S.region;
    if (ImGui::Combo("##mreg", &r, planName, nullptr, kNumPlans)) {
        applyRegion(a, r);
        if (a.engine.running()) startReceiver(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the tuning frequency (and the Meshtastic / MeshCore channel plan). Meshtastic and MeshCore use different\nfrequencies in each region; the US pair is 3.65 MHz apart and needs a radio rate of about 8 Msps or more.\nLoRa APRS and MeshCom are on 70 cm: every known LoRa APRS and MeshCom channel inside the captured band is searched.");
    ImGui::Spacing();
    ImGui::TextDisabled("Networks");
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, ch = false;
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS (lora-aprs / CA2RXU trackers and iGates): 125 kHz, sync word 0x12;\n433.775 MHz SF12 4/5 (EU), 434.855 MHz SF9 4/7 (PL), 439.9125 MHz SF12 (UK), 915 MHz SF12.");
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MeshCom 4 (icssw.org): sync word 0x2B, 4/6; 433.175 MHz SF11 250 kHz (EU) and the firmware's other country profiles.");
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
    if (ImGui::Checkbox("Search every Meshtastic preset of the region", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: LongFast only (the default of every new node). On: also MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate\nand LongSlow, each on its own channel slot. Uses more CPU, and only the slots inside the captured band are searched.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Channel keys");
    ImGui::TextWrapped("The default keys are built in (Meshtastic default channel, MeshCore Public). Add a key for a private channel: Meshtastic takes the channel name and the key in base64; MeshCore takes the channel name and the secret in hex or base64.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(120 * gUi);
    static const char* protos[] = {"Meshtastic", "MeshCore"};
    ImGui::Combo("##mkp", &S.keyProto, protos, 2);
    ImGui::SameLine(0, 6 * gUi); ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputTextWithHint("##mkn", "channel name", S.keyName, sizeof S.keyName);
    ImGui::SameLine(0, 6 * gUi); ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputTextWithHint("##mks", "key (base64 or hex)", S.keySecret, sizeof S.keySecret, ImGuiInputTextFlags_Password);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("Add")) {
        const int proto = S.keyProto == 0 ? 1 : 2;
        const bool ok = proto == 1 ? a.engine.mesh().addMeshtasticChannel(S.keyName, S.keySecret) : a.engine.mesh().addMeshCoreChannel(S.keyName, S.keySecret);
        if (ok) {
            S.keys.push_back({proto, S.keyName, S.keySecret});
            saveKeys();
            S.keyName[0] = 0; S.keySecret[0] = 0;
            S.keyMsg = "key added";
        } else {
            S.keyMsg = "this key is not valid";
        }
    }
    if (!S.keyMsg.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", S.keyMsg.c_str()); }
    for (size_t i = 0; i < S.keys.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::TextColored(protoColour(S.keys[i].proto), "%s", protoName(S.keys[i].proto));
        ImGui::SameLine(110 * gUi); ImGui::TextUnformatted(S.keys[i].name.c_str());
        ImGui::SameLine(300 * gUi);
        if (ImGui::SmallButton("remove")) {
            S.keys.erase(S.keys.begin() + (long)i);
            saveKeys(); pushKeys(a);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (S.keys.empty()) ImGui::TextDisabled("no key added");
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Keys are kept in this computer's settings, never sent anywhere."); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    subNav("meshv", S.view, {"Nodes", "Map", "Chat", "Packets", "Settings", "APRS"});
    plat::prefs().setI("meshView", S.view);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 0) { nodeTable(a, false); return; }
    if (S.view == 2) { chatView(a); return; }
    if (S.view == 3) { packetTable(a); return; }
    if (S.view == 4) { settingsView(a); return; }
    if (S.view == 5) { aprsTable(a); return; }
    const float cw = std::min(300.f * gUi, W * 0.32f);
    ImGui::BeginChild("##mesh_l", ImVec2(W - cw - 8 * gUi, H));
    mapView(a, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##mesh_r", ImVec2(0, H));
    nodeCard(a);
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see nodes"); ImGui::PopTextWrapPos(); } return; }
    const dect2::MeshTelemetry& t = a.rx.mesh;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu nodes, %zu APRS stations, %zu messages", t.nodes.size(), t.aprsStations.size(), t.messages.size()); ImGui::PopTextWrapPos(); }
    nodeTable(a, true);
}

void receiver(App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("tuned", "%.4f MHz, %.3f Msps, %s plan", t.tunedHz / 1e6, t.inputRate / 1e6, t.region == 0 ? "EU" : "US");
    kv("frames", "%llu good, %llu CRC errors, %llu bad headers", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.headerBad);
    kv("preambles", "%llu", (unsigned long long)t.preambles);
    kv("signal time", "%s", clock(t.timeSec).c_str());
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("LoRa settings searched"); ImGui::PopTextWrapPos(); }
    if (ImGui::BeginTable("##mdec", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX, ImVec2(0, std::max(60.f * gUi, ImGui::GetContentRegionAvail().y * 0.5f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthFixed, hdrW("Setting", "LoRa APRS EU")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("SF"); ImGui::TableSetupColumn("BW kHz");
        ImGui::TableSetupColumn("CR"); ImGui::TableSetupColumn("Sync"); ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, hdrW("Frames", "out of band")); ImGui::TableSetupColumn("Bad", ImGuiTableColumnFlags_WidthFixed, hdrW("Bad", "99999"));
        ImGui::TableSetupColumn("Last SNR", ImGuiTableColumnFlags_WidthFixed, hdrW("Last SNR", "-12.5"));
        ImGui::TableHeadersRow();
        for (const auto& d : t.decoders) {
            if (!d.inBand && d.protocol >= 3) continue;    // the 70 cm list is long: only the channels in the capture
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(d.protocol), "%s", ellipsize(d.preset, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.preset.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f", d.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%d", d.sf);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", d.bwHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("4/%d", d.cr);
            ImGui::TableNextColumn(); ImGui::Text("0x%02X", d.syncWord);
            ImGui::TableNextColumn(); if (d.inBand) ImGui::Text("%llu", (unsigned long long)d.frames); else ImGui::TextDisabled("out of band");
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)(d.crcBad + d.headerBad + d.syncBad));
            ImGui::TableNextColumn(); if (d.frames) ImGui::Text("%.1f", d.lastSnrDb); else ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Packets by type");
    // at least a few rows: in a short tab the pane scrolls to it instead (it was left a sliver of its header)
    if (ImGui::BeginTable("##mcnt", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, 5 * ImGui::GetTextLineHeightWithSpacing())))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, hdrW("Count", "99999"));
        ImGui::TableHeadersRow();
        for (const auto& c : t.counts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextColored(protoColour(c.protocol), "%s", protoTag(c.protocol));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.type.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", c.count);
        }
        ImGui::EndTable();
    }
}

void panels(App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    const float w1 = std::max(200.f, (W - 3 * gap) * 0.45f), w2 = std::max(120.f, (W - 3 * gap) * 0.25f), w3 = std::max(120.f, W - w1 - w2 - 3.5f * gap);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(w1, "Last packets");
    if (ImGui::BeginTable("##mpl", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(w1, plotH))) {
        ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Net"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("SNR");
        if (on) {
            for (size_t i = t.packets.size(); i-- > 0;) {
                const dect2::MeshPacket& p = t.packets[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(p.timeSec).c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(p.protocol), "%s", protoTag(p.protocol));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(p.crcOk ? (p.type.empty() ? "?" : p.type) : "CRC error", ImGui::GetContentRegionAvail().x).c_str());   // the stretched column of a narrow table
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p.from.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f", p.snrDb);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w2, "Packets by type");
    if (ImGui::BeginTable("##mcn", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(w2, plotH))) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("n");
        if (on) {
            for (const auto& c : t.counts) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(protoColour(c.protocol), "%s", c.type.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%u", c.count);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(w3, "SNR of good frames (dB)");
    std::vector<float> v(S.snr.begin(), S.snr.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines("##msn", v.data(), (int)v.size(), 0, nullptr, -25.f, 30.f, ImVec2(w3, plotH));
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::MeshTelemetry& t = a.rx.mesh;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("LoRa", !on ? 0 : t.state >= 1 ? 1 : 2); flowNext(12 * gUi);
    lamp("Decoding", !on ? 0 : t.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Signal" : "Searching");
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Frames ok / bad", b);
    int mt = 0, mc = 0;
    for (const auto& n : t.nodes) (n.protocol == 1 ? mt : mc)++;
    snprintf(b, sizeof b, "%d / %d", mt, mc); ro("Nodes Mt / Mc", b);
    if (!t.aprsStations.empty()) { snprintf(b, sizeof b, "%zu", t.aprsStations.size()); ro("APRS stations", b); }
    snprintf(b, sizeof b, "%zu", t.messages.size()); ro("Messages", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Mesh (LoRa)";
    if (live(a)) l2 = dect2::meshSummary(a.rx.mesh);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    int r = S.region;
    if (ImGui::Combo("##mreg_t", &r, planShort, nullptr, kNumPlans)) { applyRegion(a, r); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("EU / US: the Meshtastic LongFast channel; MeshCore sits 93 kHz above it in the EU (one capture holds both).\nAPRS / MeshCom: 70 cm LoRa APRS and MeshCom; MeshCom EU at 433.175 MHz also holds LoRa APRS at 433.775 MHz.\nThe frequency box can be changed for other slots: the receiver follows it.");
}

void decoder(App& a, bool&) {
    loadState();
    (void)a;
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, ch = false;
    auto boxW = [](const char* l) { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(l).x; };   // on the next line when the panel is narrow
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    sameLineIf(boxW("MeshCore"), 8 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    sameLineIf(boxW("LoRa APRS"), 8 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    sameLineIf(boxW("MeshCom"), 8 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
    sameLineIf(boxW("all presets"), 8 * gUi);
    if (ImGui::Checkbox("all presets", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also search MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate and LongSlow. Uses more CPU.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("simulated mesh");
    flowNext(); ImGui::SetNextItemWidth(100 * gUi);
    const bool p70 = sc.modeOpt[1] == 2;
    int prot = sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 15;
    // the scene of the band: Meshtastic / MeshCore on 868 or 915, LoRa APRS / MeshCom on 70 cm
    static const char* names868[] = {"", "Meshtastic", "MeshCore", "both"};
    static const char* names70[] = {"", "", "", "", "LoRa APRS", "", "", "", "MeshCom", "", "", "", "both"};
    const int sel = p70 ? ((prot & 12) ? (prot & 12) : 12) : ((prot & 3) ? (prot & 3) : 3);
    if (ImGui::BeginCombo("##msyp", p70 ? names70[sel] : names868[sel])) {
        if (p70) { for (int i : {4, 8, 12}) if (ImGui::Selectable(names70[i], sel == i)) { sc.modeOpt[0] = i; changed = true; } }
        else for (int i = 1; i <= 3; i++) if (ImGui::Selectable(names868[i], sel == i)) { sc.modeOpt[0] = i; changed = true; }
        ImGui::EndCombo();
    }
    flowNext(); ImGui::TextDisabled("SNR"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##msnr", &snr, -20.f, 30.f, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("SNR of the strongest node. The weakest is 20 dB lower unless set otherwise.");
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##mcfo", &cfo, -25, 25, "%.1f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio's frequency error at 869 MHz (a 1 ppm TCXO is off by about 0.9 kHz).");
    flowNext();
    bool us = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("US plan", &us)) { applyRegion(a, us ? 1 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LongFast at 906.875 MHz and MeshCore at 910.525 MHz. MeshCore needs a rate of about 8 Msps to be inside the capture.");
    flowNext();
    bool b70 = p70;
    if (ImGui::Checkbox("70 cm", &b70)) { applyRegion(a, b70 ? 5 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS stations on 433.775 MHz and MeshCom nodes on 433.175 MHz (tunes to MeshCom EU, which holds both).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    out.push_back({"NODES", "%.0f", (double)t.nodes.size(), 0, 20, t.nodes.empty() ? 0 : 1});
    out.push_back({"FRAMES OK", "%.0f", (double)t.blocksOk, 0, 100, t.blocksOk ? 1 : 0});
    out.push_back({"SNR  dB", "%.1f", (double)t.snrDb, -20, 20, t.snrDb >= 0 ? 1 : t.snrDb >= -10 ? 2 : 3});
}

} // namespace

extern const ModeUi kMeshUi;
const ModeUi kMeshUi = {
    .sideTitle = "NODES",
    .tabName = "Mesh",
    .tabIcon = Ic::Link,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
