// Mesh (LoRa) settings: band plan and region, protocols and presets, channel keys, the custom LoRa setting of each entry,
// and the MeshCore preset list (with its optional refresh from the internet).
// Split from mesh_ui.cpp; the shared state is declared in mesh_internal.h.
#include "dect2/common/thread_name.h"
#include "mesh_internal.h"
#include "dect2/mesh/mesh_channel_url.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

using namespace meshui;

namespace meshui {

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

bool planFits(const App& a, int i) {
    const int m = subMask(a);
    return !m || ((m & 3) ? i <= 1 || i >= 8 : m == 4 ? (i >= 2 && i <= 4) : m == 8 ? i == 5 : (i == 6 || i == 7));
}

int effRegion(const App& a) {   // the selected plan, or the entry's first when the selected one does not carry it
    if (planFits(a, S.region)) return S.region;
    for (int i = 0; i < kNumPlans; i++) if (planFits(a, i)) return i;
    return 0;
}

bool planCombo(App& a, const char* id, int& r, bool shortNames) {   // the band plans this entry can use
    bool ch = false;
    if (ImGui::BeginCombo(id, shortNames ? kPlans[r].shortName : kPlans[r].name)) {
        for (int i = 0; i < kNumPlans; i++) if (planFits(a, i) && ImGui::Selectable(shortNames ? kPlans[i].shortName : kPlans[i].name, i == r)) { r = i; ch = true; }
        ImGui::EndCombo();
    }
    return ch;
}

void applyRegion(App& a, int region) {
    S.region = region;
    plat::prefs().setI("meshRegion", region);
    a.freqMhz = kPlans[region].mhz;                    // the plan's channel (Meshtastic LongFast, LoRa APRS, MeshCom)
    if (subMask(a) == 2) a.freqMhz = region == 1 ? 910.525 : region == 8 ? 915.8 : region == 9 ? 915.075 : 869.618;   // MeshCore's own channel (AU or Mid in ANZ)
    a.tune.synth.modeOpt[1] = kPlans[region].synthRegion;
}

// ---------------------------------------------------------------- custom LoRa setting, Meshtastic region

// the standard LoRa bandwidths (SX126x), then the 2.4 GHz ones (SX128x, Meshtastic LORA_24)
const double kBws[] = {7812.5, 10416.667, 15625, 20833.333, 31250, 41666.667, 62500, 125000, 250000, 500000, 812500, 1625000};

const char* kBwNames[] = {"7.8 kHz", "10.4 kHz", "15.6 kHz", "20.8 kHz", "31.25 kHz", "41.7 kHz", "62.5 kHz", "125 kHz", "250 kHz", "500 kHz",
                          "812.5 kHz (2.4 GHz)", "1625 kHz (2.4 GHz)"};

constexpr int kNumBws = (int)(sizeof(kBws) / sizeof(kBws[0]));

State::Custom& customOf(const App& a) {
    const int m = subMask(a);
    auto it = S.custom.find(m);
    if (it != S.custom.end()) return it->second;
    State::Custom c;
    const std::string v = plat::prefs().getS(("meshCustom" + std::to_string(m)).c_str(), "");
    int on = 0;
    if (!v.empty() && sscanf(v.c_str(), "%d %lf %lf %d %d %d %d", &on, &c.mhz, &c.bwHz, &c.sf, &c.cr, &c.sync, &c.preamble) == 7) c.on = on != 0;
    else {   // the entry's usual network as the starting point
        if (m == 2) { c.mhz = 869.618; c.bwHz = 62500; c.sf = 8; c.cr = 8; c.sync = 0x12; c.preamble = 32; }
        else if (m == 4) { c.mhz = 433.775; c.bwHz = 125000; c.sf = 12; c.cr = 5; c.sync = 0x12; c.preamble = 8; }
        else if (m == 8) { c.mhz = 433.175; c.bwHz = 250000; c.sf = 11; c.cr = 6; c.sync = 0x2B; c.preamble = 8; }
    }
    c.sf = std::max(5, std::min(12, c.sf)); c.cr = std::max(5, std::min(8, c.cr)); c.sync &= 0xFF; c.preamble = std::max(6, std::min(65535, c.preamble));
    return S.custom[m] = c;
}

void saveCustom(const App& a) {
    const State::Custom& c = customOf(a);
    char b[128];
    snprintf(b, sizeof b, "%d %.6f %.3f %d %d %d %d", c.on ? 1 : 0, c.mhz, c.bwHz, c.sf, c.cr, c.sync, c.preamble);
    plat::prefs().setS(("meshCustom" + std::to_string(subMask(a))).c_str(), b);
}

// ---------------------------------------------------------------- MeshCore radio presets

std::string mcFile() { return plat::dataDir() + "/meshcore-presets.json"; }

void mcLoad() { S.mcList = dect2::meshCorePresets(mcFile(), &S.mcFromFile); S.mcListed = true; }

// the refresh runs on its own thread, only after a click; the file replaces the saved one only when it parses
void mcRefresh() {
    if (S.mcFetch == 1) return;
    if (S.mcThread.joinable()) S.mcThread.join();
    S.mcFetch = 1;
    S.mcThread = dect2::namedThread("onair-mesh", [] {
        const std::string tmp = mcFile() + ".part";
        std::vector<dect2::MeshCorePresetEntry> v;
        bool ok = plat::fetchUrl("https://api.meshcore.nz/api/v1/config", tmp, std::string("OnAir/") + ONAIR_VERSION) && dect2::loadMeshCorePresetFile(tmp, v);
        if (ok) { std::remove(mcFile().c_str()); ok = std::rename(tmp.c_str(), mcFile().c_str()) == 0; }
        if (!ok) std::remove(tmp.c_str());
        S.mcFetch = ok ? 2 : 3;
    });
}

// a preset sets the custom LoRa setting (MeshCore sync word and preamble) and the tuning frequency
void mcApply(App& a, const dect2::MeshCorePresetEntry& p) {
    State::Custom& c = customOf(a);
    const dect2::MeshLoraSettings l = dect2::meshCorePresetToLora(p);
    c.on = true; c.mhz = p.mhz; c.bwHz = l.bwHz; c.sf = l.sf; c.cr = l.cr; c.sync = l.syncWord; c.preamble = l.preamble;
    a.freqMhz = p.mhz;
    saveCustom(a);
}

// ---------------------------------------------------------------- settings


// the LoRa setting: the band plan's presets (with the Meshtastic region and modem preset) or a custom one, remembered per entry
// the MeshCore radio presets of the community (meshcore.nz): a list per region and a refresh from the internet on request
static void meshCorePresets(App& a) {
    if (!S.mcListed) mcLoad();
    if (S.mcFetch == 2 || S.mcFetch == 3) { if (S.mcThread.joinable()) S.mcThread.join(); if (S.mcFetch == 2) mcLoad(); S.mcFetch = S.mcFetch == 2 ? 4 : 5; }
    const dect2::MeshCorePresetEntry* cur = nullptr;
    for (const auto& p : S.mcList) if (p.title == S.mcPreset) cur = &p;
    ImGui::TextDisabled("MeshCore radio preset");
    ImGui::SetNextItemWidth(340 * gUi);
    if (ImGui::BeginCombo("##mcpre", cur ? cur->title.c_str() : "Custom")) {
        if (ImGui::Selectable("Custom", !cur)) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); }
        for (const auto& reg : dect2::meshCoreRegions(S.mcList)) {
            ImGui::TextDisabled("%s", reg.c_str());
            for (const auto& p : S.mcList) {
                if (p.region != reg) continue;
                const std::string lab = p.title + "   " + dect2::meshCorePresetSummary(p) + "##mcp" + p.title;
                if (ImGui::Selectable(lab.c_str(), cur && cur->title == p.title)) {
                    S.mcPreset = p.title; plat::prefs().setS("meshCorePreset", p.title);
                    mcApply(a, p);
                    if (a.engine.running()) startReceiver(a);
                }
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The radio settings the MeshCore app suggests for each region (frequency, bandwidth, spreading factor, coding rate). A preset fills in the\ncustom LoRa setting and tunes to its frequency. Custom keeps the values you enter below. %s", S.mcFromFile ? "The list is the one you refreshed from meshcore.nz." : "The list is the one built into this version.");
    if (cur) ImGui::TextDisabled("%s", dect2::meshCorePresetSummary(*cur).c_str());
    ImGui::BeginDisabled(S.mcFetch == 1);
    if (ImGui::SmallButton("Refresh from meshcore.nz")) mcRefresh();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contacts the internet (api.meshcore.nz), only when you click, and nothing about you is sent. Saves the list of presets and uses it instead of the built-in one.");
    if (S.mcFetch == 1) { ImGui::SameLine(); ImGui::TextDisabled("fetching..."); }
    else if (S.mcFetch == 4) { ImGui::SameLine(); ImGui::TextDisabled("%zu presets loaded", S.mcList.size()); }
    else if (S.mcFetch == 5) { ImGui::SameLine(); ImGui::TextDisabled("could not fetch (the previous list stays)"); }
    ImGui::Spacing();
}

// Meshtastic channel link (https://meshtastic.org/e/#...): import sets the region, the modem preset (or a custom setting) and the channel keys in one go;
// "Copy link" makes one from the keys and the region / preset in use (mesh_channel_url.h)
void channelLink(App& a) {
    static char url[1024];
    static std::string msg;
    ImGui::TextDisabled("%s", tr("Meshtastic channel link"));
    ImGui::SetNextItemWidth(std::min(420 * gUi, ImGui::GetContentRegionAvail().x - 160 * gUi));
    ImGui::InputTextWithHint("##mturl", "https://meshtastic.org/e/#...", url, sizeof url, ImGuiInputTextFlags_Password);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Paste the link the Meshtastic app shares (Channels, Share, or the text of its QR code). It holds the channel keys, so it is hidden here."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Import##mturl"))) {
        dect2::MeshtasticChannelSet cs;
        std::string err;
        if (!dect2::meshtasticUrlDecode(url, cs, err)) msg = err;
        else {
            int added = 0;
            for (size_t i = 0; i < cs.channels.size(); i++) {
                const std::string name = dect2::meshtasticUrlChannelName(cs, i), key = dect2::meshtasticUrlChannelKey(cs.channels[i]);
                if (key == "AQ==" && (name == "LongFast" || cs.channels[i].name.empty())) continue;   // the built-in default channel
                bool have = false;
                for (const auto& k : S.keys) if (k.proto == 1 && k.name == name && k.secret == key) have = true;
                if (!have && a.engine.mesh().addMeshtasticChannel(name, key)) { S.keys.push_back({1, name, key}); added++; }
            }
            saveKeys();
            msg = std::to_string(added) + " channel key(s) added";
            dect2::MeshLoraSettings l;
            std::string le;
            if (cs.hasLora && dect2::meshtasticUrlLora(cs, l, le)) {
                State::Custom& c = customOf(a);
                if (cs.usePreset && cs.overrideFrequency <= 0 && cs.channelNum == 0 && cs.channels[0].name.empty()) {   // the usual slot: the region and preset settings
                    c.on = false;
                    S.mtRegion = dect2::meshtasticRegionFromEnum(cs.region);
                    S.mtPreset = dect2::meshtasticPresetFromEnum(cs.modemPreset);
                    plat::prefs().setS("meshMtRegion", S.mtRegion);
                    plat::prefs().setS("meshMtPreset", S.mtPreset);
                } else {   // a private primary name, a channel number or a frequency of its own: the custom setting on that frequency
                    c.on = true; c.mhz = l.freqHz / 1e6; c.bwHz = l.bwHz; c.sf = l.sf; c.cr = l.cr; c.sync = 0x2B; c.preamble = 16;
                }
                saveCustom(a);
                a.freqMhz = l.freqHz / 1e6;
                char b[160];
                std::snprintf(b, sizeof b, ", %s %s, %.4f MHz", dect2::meshtasticRegionFromEnum(cs.region).c_str(), l.name.c_str(), l.freqHz / 1e6);
                msg += b;
                if (a.engine.running()) startReceiver(a);
            } else if (cs.hasLora) msg += " (radio setting not used: " + le + ")";
            pushKeys(a);
            url[0] = 0;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy link##mturl"))) {
        dect2::MeshtasticChannelSet cs;
        dect2::MeshtasticUrlChannel prim;
        prim.psk = {1};
        cs.channels.push_back(prim);
        for (const auto& k : S.keys) {
            if (k.proto != 1) continue;
            dect2::MeshtasticUrlChannel c;
            c.name = k.name;
            std::vector<uint8_t> raw;
            if (!dect2::meshcrypto::base64Decode(k.secret, raw) && !dect2::meshcrypto::hexDecode(k.secret, raw)) continue;
            c.psk = raw;
            cs.channels.push_back(c);
        }
        cs.hasLora = true; cs.usePreset = true; cs.hopLimit = 3; cs.txEnabled = true;
        cs.modemPreset = std::max(0, dect2::meshtasticPresetToEnum(S.mtPreset));
        cs.region = std::max(0, dect2::meshtasticRegionToEnum(!S.mtRegion.empty() ? S.mtRegion : planRegionName(kPlans[effRegion(a)].rxRegion)));
        ImGui::SetClipboardText(dect2::meshtasticUrlEncode(cs).c_str());
        msg = "link copied (default channel first, then your Meshtastic keys)";
    }
    if (!msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", msg.c_str()); ImGui::PopTextWrapPos(); }
}

void loraSettings(App& a) {
    State::Custom& c = customOf(a);
    if (subMask(a) == 2) meshCorePresets(a);
    ImGui::TextDisabled("LoRa setting");
    int mode = c.on ? 1 : 0;
    ImGui::SetNextItemWidth(220 * gUi);
    static const char* modes[] = {"Presets of the band plan", "Custom"};
    if (ImGui::Combo("##mlmode", &mode, modes, 2)) {
        c.on = mode == 1;
        if (!c.on) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); }
        if (c.on) a.freqMhz = c.mhz;
        else applyRegion(a, effRegion(a));
        saveCustom(a);
        if (a.engine.running()) startReceiver(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Custom: one LoRa setting of your own (bandwidth, spreading factor, coding rate, sync word, preamble) on the\nfrequency below, for networks that do not use the usual presets. Only that setting is searched. Kept for each mode entry.");
    if (!c.on) {
        if (!meshtasticShown(a)) return;
        // Meshtastic: any firmware region and modem preset; the frequency box goes to the preset's slot
        const auto regions = dect2::meshtasticRegions();
        const auto presets = dect2::meshtasticPresetNames();
        bool ch = false;
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("Meshtastic region##mtreg", S.mtRegion.empty() ? "from the band plan" : S.mtRegion.c_str())) {
            if (ImGui::Selectable("from the band plan", S.mtRegion.empty())) { S.mtRegion.clear(); ch = true; }
            for (const auto& r : regions) if (ImGui::Selectable(r.c_str(), r == S.mtRegion)) { S.mtRegion = r; ch = true; }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The region set in the Meshtastic app (Config > LoRa > Region). The channel slot follows from the region,\nthe modem preset and the channel name, as the firmware computes it. LORA_24 is 2.4 GHz LoRa (812.5 kHz and wider).");
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo("Modem preset##mtpre", S.mtPreset.c_str())) {
            for (const auto& p : presets) if (ImGui::Selectable(p.c_str(), p == S.mtPreset)) { S.mtPreset = p; ch = true; }
            ImGui::EndCombo();
        }
        if (ch) {
            plat::prefs().setS("meshMtRegion", S.mtRegion);
            plat::prefs().setS("meshMtPreset", S.mtPreset);
            dect2::MeshLoraSettings s;
            if (!S.mtRegion.empty() && dect2::meshtasticRegionPreset(S.mtRegion, S.mtPreset, s)) a.freqMhz = s.freqHz / 1e6;
            else if (S.mtRegion.empty()) applyRegion(a, effRegion(a));
            if (a.engine.running()) startReceiver(a);
        }
        dect2::MeshLoraSettings s;
        const std::string reg = !S.mtRegion.empty() ? S.mtRegion : planRegionName(kPlans[effRegion(a)].rxRegion);
        if (dect2::meshtasticRegionPreset(reg, S.mtPreset, s))
            ImGui::TextDisabled("%s %s: %.4f MHz, SF%d, %g kHz, 4/%d", reg.c_str(), S.mtPreset.c_str(), s.freqHz / 1e6, s.sf, s.bwHz / 1e3, s.cr);
        else ImGui::TextDisabled("%s has no slot for %s (the region is narrower than the preset)", reg.c_str(), S.mtPreset.c_str());
        return;
    }
    bool ch = false, retune = false;
    ImGui::SetNextItemWidth(220 * gUi);
    double mhz = a.freqMhz;
    if (ImGui::InputDouble("Frequency (MHz)##mcf", &mhz, 0.025, 1.0, "%.4f", ImGuiInputTextFlags_EnterReturnsTrue) && mhz > 0) {   // any frequency: the radio is the limit
        a.freqMhz = mhz; retune = true;
    }
    if (S.customEntry == subMask(a) && std::fabs(c.mhz - a.freqMhz) > 1e-7) { c.mhz = a.freqMhz; ch = true; }   // the frequency box is the custom frequency
    int bi = 7;
    for (int i = 0; i < kNumBws; i++) if (std::fabs(kBws[i] - c.bwHz) < 1) bi = i;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("Bandwidth##mcb", &bi, kBwNames, kNumBws)) {
        const bool rateChange = (kBws[bi] > 500e3) != (c.bwHz > 500e3);
        c.bwHz = kBws[bi]; ch = true; retune |= rateChange;
    }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::SliderInt("Spreading factor##mcs", &c.sf, 5, 12, "SF%d")) { c.sf = std::max(5, std::min(12, c.sf)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("SF5 and SF6 follow the SX126x frame layout (two more symbols after the sync word); they are not checked against a real radio yet.");
    int ci = c.cr - 5;
    static const char* crs[] = {"4/5", "4/6", "4/7", "4/8"};
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("Coding rate##mcc", &ci, crs, 4)) { c.cr = ci + 5; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Each frame's header carries its coding rate: frames at any rate decode.");
    char sw[8];
    snprintf(sw, sizeof sw, "%02X", c.sync);
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::InputText("##mcw", sw, sizeof sw, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) { c.sync = (int)strtol(sw, nullptr, 16) & 0xFF; ch = true; }
    ImGui::SameLine(0, 6 * gUi); ImGui::TextUnformatted("Sync word (hex)");
    if (ImGui::SmallButton("0x2B Meshtastic / MeshCom")) { c.sync = 0x2B; ch = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::SmallButton("0x12 MeshCore / LoRa APRS")) { c.sync = 0x12; ch = true; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::SmallButton("0x34 LoRaWAN")) { c.sync = 0x34; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputInt("Preamble (symbols)##mcp", &c.preamble, 1, 4)) { c.preamble = std::max(6, std::min(65535, c.preamble)); ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Meshtastic 16, MeshCore 32 (SF8 and below) or 16, LoRa APRS and LoRaWAN 8.");
    ImGui::TextDisabled("Low data rate optimisation: %s (on when a symbol lasts 16 ms or more)", dect2::meshLoraLdro(c.sf, c.bwHz) ? "on" : "off");
    if (ch) { saveCustom(a); if (!S.mcPreset.empty()) { S.mcPreset.clear(); plat::prefs().setS("meshCorePreset", ""); } }
    if (retune && a.engine.running()) startReceiver(a);
}

void settingsView(App& a) {
    loadState();
    ImGui::TextDisabled("Band plan");
    ImGui::SetNextItemWidth(340 * gUi);
    int r = effRegion(a);
    if (planCombo(a, "##mreg", r, false)) {
        applyRegion(a, r);
        if (a.engine.running()) startReceiver(a);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the tuning frequency (and the Meshtastic / MeshCore channel plan). Meshtastic and MeshCore use different\nfrequencies in each region; the US pair is 3.65 MHz apart and needs a radio rate of about 8 Msps or more (ANZ: 4.1 MHz).\nLoRa APRS and MeshCom are on 70 cm: every known LoRa APRS and MeshCom channel inside the captured band is searched.");
    ImGui::Spacing();
    if (lorawanEntry(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("EU868: 868.1, 868.3 and 868.5 MHz (every network), and 867.1 - 867.9 MHz when the capture is wide enough, at SF7 - SF12, 125 kHz, "
                            "and SF7 250 kHz on 868.3 MHz. US915: the 125 kHz uplink channels (902.3 + 0.2 n MHz, SF7 - SF10) and the 500 kHz ones inside the capture. "
                            "Sync word 0x34 (public networks). Every spreading factor of each channel is searched at once. Downlinks (inverted chirps) are not received.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        lorawanPrivacyNote();
        return;
    }
    if (!subMask(a)) {
    ImGui::TextDisabled("Networks");
    bool mt = (S.protocols & 1) != 0, mc = (S.protocols & 2) != 0, la = (S.protocols & 4) != 0, mm = (S.protocols & 8) != 0, lw = (S.protocols & 16) != 0, ch = false;
    if (ImGui::Checkbox("Meshtastic", &mt)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCore", &mc)) ch = true;
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("LoRa APRS", &la)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRa APRS (lora-aprs / CA2RXU trackers and iGates): 125 kHz, sync word 0x12;\n433.775 MHz SF12 4/5 (EU), 434.855 MHz SF9 4/7 (PL), 439.9125 MHz SF12 (UK), 915 MHz SF12.");
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("MeshCom", &mm)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("MeshCom 4 (icssw.org): sync word 0x2B, 4/6; 433.175 MHz SF11 250 kHz (EU) and the firmware's other country profiles.");
    ImGui::SameLine(0, 14 * gUi);
    if (ImGui::Checkbox("LoRaWAN", &lw)) ch = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("LoRaWAN headers (EU868 / US915, SF7 - SF12): one demodulator per channel and spreading factor, so it uses much more CPU.\nOnly the clear headers are shown; payloads stay encrypted.");
    if (ch) { S.protocols = (mt ? 1 : 0) | (mc ? 2 : 0) | (la ? 4 : 0) | (mm ? 8 : 0) | (lw ? 16 : 0); plat::prefs().setI("meshProtocols", S.protocols); }
    }
    if (!subMask(a) || subMask(a) == 1)    if (ImGui::Checkbox("Search every Meshtastic preset of the region", &S.allPresets)) plat::prefs().setB("meshPresets", S.allPresets);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: LongFast only (the default of every new node). On: also MediumFast, MediumSlow, ShortFast, ShortSlow, LongModerate\nand LongSlow, each on its own channel slot. Uses more CPU, and only the slots inside the captured band are searched.");
    {
        static const char* sz[] = {"standard", "5 times", "20 times"};
        int cur = S.tableScale == 20 ? 2 : S.tableScale == 5 ? 1 : 0;
        ImGui::TextUnformatted(tr("Table size")); ImGui::SameLine(0, 8 * gUi);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::BeginCombo("##meshsz", sz[cur])) {
            for (int i = 0; i < 3; i++) if (ImGui::Selectable(sz[i], cur == i)) { S.tableScale = i == 2 ? 20 : i == 1 ? 5 : 1; plat::prefs().setI("meshTableScale", S.tableScale); }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many rows the lists keep before the oldest leave: standard is 100 packets, 150 nodes, 100 messages, 150 APRS stations, 200 LoRaWAN devices. Larger sizes use more memory and a little more CPU; Save results exports what is kept."));
    }
    ImGui::Spacing();
    loraSettings(a);
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
    channelLink(a);
    ImGui::Spacing();
    { void meshWatchSettings(); meshWatchSettings(); }   // node watch (mesh_watch_ui.cpp)
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Keys are kept in this computer's settings, never sent anywhere."); ImGui::PopTextWrapPos(); }
}

}  // namespace meshui
