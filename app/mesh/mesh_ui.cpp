// Mesh (LoRa) mode entry: the screen state, the mode's tick, and the entry points the shell calls (tab, list, receiver,
// panels, status, summary, tuner, decoder, synth, meters). The tables, views, LoRaWAN screens and settings are in the other mesh_*.cpp.
#include "mesh_internal.h"
#include "ui/kit/mode_frame.h"
#include "ui/kit/session_list.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

using namespace meshui;

namespace meshui {

State S;

bool live(const App& a) { return a.rx.standard == 21; }

ImVec4 protoColour(int proto) {
    switch (proto) {
    case 1: return ImVec4(0.46f, 0.84f, 0.70f, 1);
    case 2: return ImVec4(0.96f, 0.68f, 0.34f, 1);
    case 3: return ImVec4(0.55f, 0.70f, 0.98f, 1);
    case 5: return ImVec4(0.96f, 0.84f, 0.40f, 1);
    default: return ImVec4(0.86f, 0.58f, 0.92f, 1);
    }
}

const char* protoName(int proto) { return proto == 1 ? "Meshtastic" : proto == 2 ? "MeshCore" : proto == 3 ? "LoRa APRS" : proto == 4 ? "MeshCom" : "LoRaWAN"; }

const char* protoTag(int proto) { return proto == 1 ? "Mt" : proto == 2 ? "Mc" : proto == 3 ? "APRS" : proto == 4 ? "MCom" : "LWAN"; }

const char* planRegionName(int rxRegion) { return rxRegion == 1 ? "US" : rxRegion >= 2 ? "ANZ" : "EU_868"; }   // the plan's Meshtastic region

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

// Every position a node or station sends goes to the log once (Meshtastic POSITION_APP, MeshCom / LoRa APRS positions); the packets
// already in the table when the receiver starts are not logged again
void logPositions(App& a) {
    for (const auto& p : a.rx.mesh.packets) {
        if (p.type.find("POSITION") == std::string::npos && p.type.find("Position") == std::string::npos) continue;
        char k[96];
        snprintf(k, sizeof k, "%d %s %08x %.3f", p.protocol, p.from.c_str(), (unsigned)p.packetId, p.timeSec);
        if (!S.logged.insert(k).second) continue;
        S.loggedOrder.push_back(k);
        if (S.loggedOrder.size() > 400) { S.logged.erase(S.loggedOrder.front()); S.loggedOrder.pop_front(); }
        if (!S.logPrimed) continue;
        std::string who = p.from;
        for (const auto& n : a.rx.mesh.nodes) if (n.protocol == p.protocol && n.id == p.from && !n.longName.empty()) { who += " " + n.longName; break; }
        char b[64];
        snprintf(b, sizeof b, ", SNR %.1f dB, %.4f MHz", p.snrDb, p.freqHz / 1e6);
        a.engine.log("Mesh: position from " + who + ": " + (p.detail.empty() ? std::string(p.decrypted || p.protocol != 1 ? "?" : "encrypted") : p.detail) + b);
    }
    S.logPrimed = true;
}

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.map.zoom = (int)d.getI("meshZoom", 10);
    if (receiverLocationSet()) { S.map.lat = d.getD("adsbLat", S.map.lat); S.map.lon = d.getD("adsbLon", S.map.lon); }   // until a node sends its position
    S.view = (int)d.getI("meshView", 0);
    S.region = std::max(0, std::min(kNumPlans - 1, (int)d.getI("meshRegion", 0)));
    S.protocols = (int)d.getI("meshProtocols", 15);
    S.allPresets = d.getB("meshPresets", false);
    S.mtRegion = d.getS("meshMtRegion", "");
    S.mtPreset = d.getS("meshMtPreset", "LongFast");
    S.mcPreset = d.getS("meshCorePreset", "");
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

// a protocol entry (Meshtastic, MeshCore, LoRa APRS, MeshCom): its networks, and the band plans that carry it
int subMask(const App& a) { const SubMode* sm = curSub(a); return sm ? sm->decoders : 0; }

// a node's or station's protocol (1 Meshtastic, 2 MeshCore, 3 LoRa APRS, 4 MeshCom, 5 LoRaWAN) shown in this entry: a protocol entry shows only its own
bool shows(const App& a, int proto) { const int m = subMask(a); return !m || (m & (1 << (proto - 1))); }

bool lorawanEntry(const App& a) { return subMask(a) == 16; }

bool stationEntry(const App& a) { const int m = subMask(a); return m == 4 || m == 8; }   // LoRa APRS, MeshCom: stations, not nodes

int effProtocols(const App& a) { const int m = subMask(a); return m ? m : S.protocols; }

bool customActive(const App& a) { return !lorawanEntry(a) && customOf(a).on; }

bool meshtasticShown(const App& a) { const int m = subMask(a); return !m || m == 1; }

}  // namespace meshui

double meshSampleRate(App& a) {
    double bw = 0;
    if (customActive(a)) bw = customOf(a).bwHz;
    else if (meshtasticShown(a) && !S.mtRegion.empty()) {
        dect2::MeshLoraSettings s;
        if (dect2::meshtasticRegionPreset(S.mtRegion, S.mtPreset, s)) bw = s.bwHz;
    }
    return bw > 500e3 ? 4e6 : 2e6;
}

namespace meshui {

void tick(App& a) {
    loadState();
    dect2::MeshReceiver& r = a.engine.mesh();
    {
        const std::string mreg = meshtasticShown(a) ? S.mtRegion : "";
        const std::string k = mreg + "|" + S.mtPreset;
        if (k != S.pushedMt) { r.setMeshtasticPlan(mreg, S.mtPreset); S.pushedMt = k; }
        const State::Custom& c = customOf(a);
        const bool on = customActive(a);
        if (S.customEntry != subMask(a)) {   // an entry with its custom setting on starts on its custom frequency
            S.customEntry = subMask(a);
            if (on) a.freqMhz = c.mhz;
        }
        char b[128];
        snprintf(b, sizeof b, "%d %.3f %d %d %d %d", on ? 1 : 0, c.bwHz, c.sf, c.cr, c.sync, c.preamble);
        if (b != S.pushedCustom) {
            dect2::MeshLoraSettings s;
            s.name = "Custom"; s.freqHz = 0;          // the tuned frequency (the custom frequency is put in the frequency box)
            s.sf = c.sf; s.bwHz = c.bwHz; s.cr = c.cr; s.syncWord = (uint8_t)c.sync; s.preamble = c.preamble; s.ldro = dect2::meshLoraLdro(c.sf, c.bwHz);
            r.setCustom(on, s);
            S.pushedCustom = b;
        }
    }
    if (!S.keysPushed) { pushKeys(a); S.keysPushed = true; }
    if (S.pushedRegion != effRegion(a)) { S.pushedRegion = effRegion(a); r.setRegion(kPlans[S.pushedRegion].rxRegion); }
    if (S.pushedProtocols != effProtocols(a)) { S.pushedProtocols = effProtocols(a); r.setProtocols(S.pushedProtocols); }   // a protocol entry: only its own network
    if (S.pushedPresets != (int)S.allPresets) { r.setPresetSearch(S.allPresets); S.pushedPresets = (int)S.allPresets; }
    if (S.pushedTuned != a.freqMhz) { r.setTunedHz(a.freqMhz * 1e6); S.pushedTuned = a.freqMhz; }   // the engine does not pass the user's frequency
    if (!a.engine.running()) { S.lastSeq = 0; S.logPrimed = false; return; }
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        logPositions(a);
        if (a.rx.mesh.dataValid) { S.snr.push_back(a.rx.mesh.snrDb); if (S.snr.size() > 240) S.snr.pop_front(); }
        if (S.mapFit) {
            double la = 0, lo = 0; int n = 0;
            for (const auto& nd : a.rx.mesh.nodes) if (nd.hasPosition) { la += nd.lat; lo += nd.lon; n++; }
            for (const auto& st : a.rx.mesh.aprsStations) if (st.st.hasPos) { la += st.st.lat; lo += st.st.lon; n++; }
            if (n) { S.map.lat = la / n; S.map.lon = lo / n; }
        }
    }
}

// ---------------------------------------------------------------- the hooks

void tab(App& a) {
    loadState();
    kit::modeHeader({20, "LoRa mesh", live(a) ? 1 : a.engine.running() ? 2 : 0, -1000.f, a.freqMhz * 1e6});
    if (const int m = subMask(a)) {   // a protocol entry: only its own views (LoRa APRS: stations instead of nodes and chat; MeshCom: stations and chat; LoRaWAN: devices and frames)
        static const int aprsIds[] = {5, 1, 3, 4, 9}, comIds[] = {5, 1, 2, 3, 4, 9}, meshIds[] = {0, 1, 2, 3, 8, 4, 9}, lwIds[] = {6, 7, 4, 9};
        const bool ap = m == 4, com = m == 8, lw = m == 16;
        const int* ids = ap ? aprsIds : com ? comIds : lw ? lwIds : meshIds;
        const int n = ap ? 5 : lw ? 4 : com ? 6 : 7;
        int k = 0;
        for (int i = 0; i < n; i++) if (ids[i] == S.view) k = i;
        if (ap) subNav("meshva", k, {"Stations", "Map", "Packets", "Settings", tr("Tracks")});
        else if (com) subNav("meshvc", k, {"Stations", "Map", "Chat", "Packets", "Settings", tr("Tracks")});
        else if (lw) subNav("meshvl", k, {"Devices", "Frames", "Settings", tr("Tracks")});
        else subNav("meshvm", k, {"Nodes", "Map", "Chat", "Packets", "Telemetry", "Settings", tr("Tracks")});
        S.view = ids[k];
    } else {
        if (S.view == 7) S.view = 3;
        static const int ids[] = {0, 1, 2, 3, 8, 4, 5, 6, 9};   // the views in the order of the tabs (7, the LoRaWAN frames, has no tab here)
        int k = 0;
        for (int i = 0; i < 9; i++) if (ids[i] == S.view) k = i;
        subNav("meshv", k, {"Nodes", "Map", "Chat", "Packets", "Telemetry", "Settings", "APRS", "LoRaWAN", tr("Tracks")});
        S.view = ids[k];
    }
    plat::prefs().setI("meshView", S.view);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    if (S.view == 9) { kit::sessionItems(20, 3, "mesh"); return; }
    if (S.view == 0) { nodeTable(a, false); return; }
    if (S.view == 2) { chatView(a); return; }
    if (S.view == 3) { packetTable(a); return; }
    if (S.view == 8) { telemetryTable(a); return; }
    if (S.view == 4) { settingsView(a); return; }
    if (S.view == 5) { aprsTable(a); return; }
    if (S.view == 6) { lorawanDevices(a, false); return; }
    if (S.view == 7) { lorawanFrames(a); return; }
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
    if (resultsClearButton("mesh")) a.engine.mesh().clearResults();
    const dect2::MeshTelemetry& t = a.rx.mesh;
    size_t msgs = 0;
    for (const auto& m : t.messages) msgs += shows(a, m.protocol) ? 1 : 0;
    ImGui::PushTextWrapPos(0);
    if (lorawanEntry(a)) ImGui::TextDisabled("%zu LoRaWAN devices", t.lorawanDevices.size());
    else if (stationEntry(a)) ImGui::TextDisabled("%zu stations, %zu messages", sortedStations(a).size(), msgs);
    else if (subMask(a)) ImGui::TextDisabled("%zu nodes, %zu messages", sortedNodes(a).size(), msgs);
    else ImGui::TextDisabled("%zu nodes, %zu APRS stations, %zu messages", t.nodes.size(), t.aprsStations.size(), t.messages.size());
    ImGui::PopTextWrapPos();
    if (lorawanEntry(a)) lorawanDevices(a, true);
    else if (stationEntry(a)) aprsTable(a, true);
    else nodeTable(a, true);
}

void receiver(App& a) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    if (t.custom) kv("tuned", "%.4f MHz, %.3f Msps, custom LoRa setting", t.tunedHz / 1e6, t.inputRate / 1e6);
    else kv("tuned", "%.4f MHz, %.3f Msps, %s plan, Meshtastic %s", t.tunedHz / 1e6, t.inputRate / 1e6, t.region == 1 ? "US" : t.region == 2 ? "ANZ" : t.region == 3 ? "ANZ Mid" : "EU", t.mtRegion.c_str());
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
    if (!lorawanEntry(a)) { snprintf(b, sizeof b, "%d / %d", mt, mc); ro("Nodes Mt / Mc", b); }
    if (!t.aprsStations.empty()) { snprintf(b, sizeof b, "%zu", t.aprsStations.size()); ro("APRS stations", b); }
    if (!t.lorawanDevices.empty() || lorawanEntry(a)) { snprintf(b, sizeof b, "%zu", t.lorawanDevices.size()); ro("LoRaWAN devices", b); }
    snprintf(b, sizeof b, "%zu", t.messages.size()); ro("Messages", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const SubMode* sm = curSub(a);
    l1 = sm ? sm->name : "Mesh (LoRa)";
    if (live(a)) l2 = dect2::meshSummary(a.rx.mesh);
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    int r = effRegion(a);
    if (planCombo(a, "##mreg_t", r, true)) { applyRegion(a, r); retune = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("EU / US / ANZ: the Meshtastic LongFast channel; MeshCore sits 93 kHz above it in the EU (one capture holds both).\nAPRS / MeshCom: 70 cm LoRa APRS and MeshCom; MeshCom EU at 433.175 MHz also holds LoRa APRS at 433.775 MHz.\nThe frequency box can be changed for other slots: the receiver follows it.");
}

void decoder(App& a, bool&) {
    loadState();
    if (customActive(a)) {
        const State::Custom& c = customOf(a);
        ImGui::TextDisabled("custom: SF%d %g kHz 4/%d, sync 0x%02X", c.sf, c.bwHz / 1e3, c.cr, c.sync);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A custom LoRa setting is on (Settings tab of the mode).");
        return;
    }
    if (const int m = subMask(a)) {   // a protocol entry decodes only its own network
        if (m == 1) { if (ImGui::Checkbox("all presets", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also search MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate and LongSlow. Uses more CPU."); }
        else ImGui::TextDisabled("%s only", curSub(a)->name);
        if (m == 16 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Every spreading factor (SF7 - SF12) of every channel in the capture at once.\nOnly the headers every gateway receives in the clear; payloads stay encrypted, nothing is decrypted.");
        return;
    }
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, ch = false;
    auto boxW = [](const char* l) { return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(l).x; };   // on the next line when the panel is narrow
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    sameLineIf(boxW("MeshCore"), 8 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    sameLineIf(boxW("LoRa APRS"), 8 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    sameLineIf(boxW("MeshCom"), 8 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    bool lw = (S.protocols & 16) != 0;
    sameLineIf(boxW("LoRaWAN"), 8 * gUi);
    if (ImGui::Checkbox("LoRaWAN", &lw)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRaWAN headers: one demodulator per channel and spreading factor (much more CPU). Payloads stay encrypted.");
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0) | (lw ? 16 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
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
    if (subMask(a)) ImGui::TextDisabled("%s", curSub(a)->name);   // a protocol entry plays only its own network
    else if (ImGui::BeginCombo("##msyp", p70 ? names70[sel] : names868[sel])) {
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
    if (subMask(a)) return;   // the band follows the entry
    flowNext();
    bool us = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("US plan", &us)) { applyRegion(a, us ? 1 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LongFast at 906.875 MHz and MeshCore at 910.525 MHz. MeshCore needs a rate of about 8 Msps to be inside the capture.");
    flowNext();
    bool anz = sc.modeOpt[1] == 5;
    if (ImGui::Checkbox("ANZ plan", &anz)) { applyRegion(a, anz ? 8 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LongFast at 919.875 MHz and MeshCore AU at 915.8 MHz. MeshCore needs a rate of about 10 Msps to be inside the capture.");
    flowNext();
    bool b70 = p70;
    if (ImGui::Checkbox("70 cm", &b70)) { applyRegion(a, b70 ? 5 : 0); sc.modeOpt[0] = 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS stations on 433.775 MHz and MeshCom nodes on 433.175 MHz (tunes to MeshCom EU, which holds both).");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::MeshTelemetry& t = a.rx.mesh;
    if (lorawanEntry(a)) out.push_back({"DEVICES", "%.0f", (double)t.lorawanDevices.size(), 0, 50, t.lorawanDevices.empty() ? 0 : 1});
    else out.push_back({"NODES", "%.0f", (double)t.nodes.size(), 0, 20, t.nodes.empty() ? 0 : 1});
    out.push_back({"FRAMES OK", "%.0f", (double)t.blocksOk, 0, 100, t.blocksOk ? 1 : 0});
    out.push_back({"SNR  dB", "%.1f", (double)t.snrDb, -20, 20, t.snrDb >= 0 ? 1 : t.snrDb >= -10 ? 2 : 3});
}

}  // namespace meshui

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
