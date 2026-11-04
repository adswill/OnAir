// Tools > "nRF24 / ESP32 radio": an ESP32 board (with an nRF24L01+ module on its SPI bus) on a USB serial port as a receiver and, behind the Arm switch,
// a transmitter. Receive first: nRF24 packets, BLE advertising, WiFi networks / clients / signal / channel load (observation only). Every parameter is under
// Advanced. The driver is core/src/esp32; the firmware is firmware/esp32. Untested on a real board (docs/ESP32-NRF24-DESIGN.md).
#include "app.h"
#include "state/receivers.h"
#include "i18n/i18n.h"
#include "dect2/common/time_fmt.h"
#include "dect2/esp32/esp32_radio.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace dect2::esp32;
namespace w = onair::wire;

struct State {
    Radio radio;
    std::vector<std::string> ports;
    int portIdx = 0;                 // 0 = automatic
    int baud = 115200;
    std::thread th;
    std::atomic<int> busy{0};        // 1 connecting
    std::mutex mu; std::string msg;
    int kind = 0;                    // 0 nRF24, 1 WiFi, 2 BLE, 3 802.15.4
    bool listening = false;
    Nrf24Params nrf; BleParams ble; WifiParams wifi; Ieee154Params z154;
    int wifiMode = 0;                // 0 listen, 1 scan
    int view = 0;
    int selAp = -1;
    char addrHex[16] = "E7E7E7E7E7";
    char payload[200] = "48454C4C4F";
    bool payloadText = false, noAck = false, armed = false;
    int bleAdvType = 3;
    bool scanned = false, advOpen = false;
};
State S;

void say(const std::string& m) { std::lock_guard<std::mutex> l(S.mu); S.msg = m; }
std::string said() { std::lock_guard<std::mutex> l(S.mu); return S.msg; }

bool parseHex(const char* s, std::vector<uint8_t>& out) {
    out.clear();
    int hi = -1;
    for (const char* c = s; *c; c++) {
        int v;
        if (*c >= '0' && *c <= '9') v = *c - '0'; else if (*c >= 'a' && *c <= 'f') v = *c - 'a' + 10; else if (*c >= 'A' && *c <= 'F') v = *c - 'A' + 10; else if (*c == ' ' || *c == ':') continue; else return false;
        if (hi < 0) hi = v; else { out.push_back((uint8_t)(hi << 4 | v)); hi = -1; }
    }
    return hi < 0;
}

void connectAsync(const std::string& only) {
    if (S.busy) return;
    if (S.th.joinable()) S.th.join();
    S.busy = 1; say(tr("connecting..."));
    const int baud = S.baud;
    S.th = std::thread([only, baud] {
        std::string err;
        if (S.radio.autoConnect(err, only, baud)) say(tr("connected"));
        else say(err);
        S.busy = 0;
    });
}

void applyAndStart() {
    std::string err;
    const int radio = S.kind == 0 ? w::kRadioNrf24 : S.kind == 1 ? w::kRadioWifi : S.kind == 2 ? w::kRadioBle : w::kRadioIeee802154;
    bool ok = false;
    if (S.kind == 0) { std::vector<uint8_t> a; parseHex(S.addrHex, a); for (int i = 0; i < 5; i++) S.nrf.address[i] = i < (int)a.size() ? a[i] : 0; S.nrf.addrWidth = std::max(3, std::min(5, S.nrf.addrWidth)); ok = S.radio.configure(S.nrf, err); }
    else if (S.kind == 1) { S.wifi.scan = S.wifiMode == 1; ok = S.radio.configure(S.wifi, err); }
    else if (S.kind == 2) { ok = S.radio.configure(S.ble, err); }
    else ok = S.radio.configure(S.z154, err);
    if (ok) ok = S.radio.start(radio, err);
    S.listening = ok;
    say(ok ? tr("listening") : err);
}

void stopListening() { std::string e; S.radio.stop(e); S.listening = false; say(tr("stopped")); }

void nrfTables(const Snapshot& sn) {
    subNav("e32n", S.view, {"Frames", "Addresses"});
    const float h = ImGui::GetContentRegionAvail().y;
    if (S.view == 0) {
        if (ImGui::BeginTable("##e32f", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Rate")); ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn("PID"); ImGui::TableSetupColumn("CRC");
            ImGui::TableSetupColumn(tr("Copies")); ImGui::TableSetupColumn(tr("Payload (hex)"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(tr("Text"));
            ImGui::TableHeadersRow();
            for (const auto& f : sn.nrf.frames) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d k", f.rateKbps);
                ImGui::TableNextColumn(); ImGui::Text("%s", f.address.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d%s", f.pid, f.noAck ? " NA" : "");
                ImGui::TableNextColumn(); ImGui::Text("%d%s", f.crcBits, f.confirmed ? "" : "?");
                ImGui::TableNextColumn(); ImGui::Text("%d", f.repeats);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(f.payloadHex.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", f.text.c_str());
            }
            ImGui::EndTable();
        }
    } else if (ImGui::BeginTable("##e32d", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, h))) {
        ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn(tr("Bytes")); ImGui::TableSetupColumn("CRC"); ImGui::TableSetupColumn(tr("Last heard")); ImGui::TableSetupColumn(tr("Frames"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& d : sn.nrf.devices) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", d.address.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", d.addrLen);
            ImGui::TableNextColumn(); ImGui::Text("%d", d.crcBits);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(d.lastSeen).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)d.count);
        }
        ImGui::EndTable();
    }
}

void wifiTables(const Snapshot& sn) {
    const WifiSnapshot& W = sn.wifi;
    ImGui::TextDisabled("%s", tr("Observation only: this radio never transmits WiFi, never deauthenticates and never captures passwords."));
    ImGui::TextDisabled("%zu %s, %zu %s, %llu beacons, %llu probes, %llu deauth", W.aps.size(), tr("networks"), W.clients.size(), tr("clients"), (unsigned long long)W.beacons, (unsigned long long)W.probes, (unsigned long long)W.deauths);
    subNav("e32w", S.view, {"Networks", "Clients", "Signal", "Channels", "Events"});
    const float h = ImGui::GetContentRegionAvail().y;
    const int64_t now = (int64_t)std::time(nullptr);
    if (S.view == 0) {
        if (ImGui::BeginTable("##e32a", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("SSID", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("BSSID"); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn("dBm");
            ImGui::TableSetupColumn(tr("Security")); ImGui::TableSetupColumn(tr("Vendor")); ImGui::TableSetupColumn(tr("Clients")); ImGui::TableSetupColumn(tr("Seen"));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < W.aps.size(); i++) {
                const dect2::esp32::WifiAp& a = W.aps[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(((a.ssid.empty() ? std::string(tr("(hidden)")) : a.ssid) + "##ap" + std::to_string(i)).c_str(), S.selAp == (int)i, ImGuiSelectableFlags_SpanAllColumns)) S.selAp = (int)i;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.bssid.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", a.channel);
                ImGui::TableNextColumn(); ImGui::Text("%d", a.rssi);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(authName(a.auth).c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", a.vendor.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", a.clients);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%llds", (long long)(now - a.lastSeen));
            }
            ImGui::EndTable();
        }
    } else if (S.view == 1) {
        if (ImGui::BeginTable("##e32c", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("MAC"); ImGui::TableSetupColumn(tr("Vendor")); ImGui::TableSetupColumn(tr("Network")); ImGui::TableSetupColumn("dBm"); ImGui::TableSetupColumn(tr("Frames")); ImGui::TableSetupColumn(tr("Asked for"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& c : W.clients) {
                std::string net = c.bssid.empty() ? "-" : c.bssid;
                for (const auto& a : W.aps) if (a.bssid == c.bssid && !a.ssid.empty()) { net = a.ssid; break; }
                std::string asked; for (const auto& s : c.probed) asked += (asked.empty() ? "" : ", ") + s;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(c.mac.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c.vendor.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(net.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", c.rssi);
                ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.count);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", asked.c_str());
            }
            ImGui::EndTable();
        }
    } else if (S.view == 2) {   // signal strength over time of the selected network (click one in Networks), else the five strongest
        const float gh = std::max(60.f * gUi, (h - 60 * gUi) / 5);
        int shown = 0;
        for (size_t i = 0; i < W.aps.size() && shown < 5; i++) {
            if (S.selAp >= 0 && (int)i != S.selAp) continue;
            const dect2::esp32::WifiAp& a = W.aps[i];
            std::vector<float> v(a.history.begin(), a.history.end());
            char lbl[96]; std::snprintf(lbl, sizeof lbl, "%s  %d dBm##sg%zu", a.ssid.empty() ? tr("(hidden)") : a.ssid.c_str(), a.rssi, i);
            ImGui::PlotLines(lbl, v.data(), (int)v.size(), 0, nullptr, -100.f, -20.f, ImVec2(ImGui::GetContentRegionAvail().x * 0.8f, S.selAp >= 0 ? std::max(gh, 160.f * gUi) : gh));
            shown++;
        }
        if (!shown) ImGui::TextDisabled("%s", tr("no networks yet"));
        else if (S.selAp >= 0 && ImGui::Button(tr("Show the strongest five"))) S.selAp = -1;
    } else if (S.view == 3) {   // channel congestion: networks per channel and the strongest signal on it
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float bw = std::max(18.f * gUi, ImGui::GetContentRegionAvail().x / 15.f), bh = std::max(120.f * gUi, h - 50 * gUi);
        int maxN = 1; for (int c = 1; c <= 14; c++) maxN = std::max(maxN, W.chanAps[c]);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int c = 1; c <= 14; c++) {
            const float x = p0.x + (c - 1) * bw, frac = (float)W.chanAps[c] / (float)maxN;
            const ImU32 col = W.chanAps[c] == 0 ? IM_COL32(70, 70, 70, 255) : W.chanAps[c] >= 4 ? IM_COL32(220, 90, 70, 255) : W.chanAps[c] >= 2 ? IM_COL32(230, 180, 60, 255) : IM_COL32(90, 190, 120, 255);
            dl->AddRectFilled(ImVec2(x + 3, p0.y + bh * (1 - frac)), ImVec2(x + bw - 3, p0.y + bh), col);
            char t[24]; std::snprintf(t, sizeof t, "%d", c); dl->AddText(ImVec2(x + bw * 0.4f, p0.y + bh + 2), IM_COL32_WHITE, t);
            if (W.chanAps[c]) { std::snprintf(t, sizeof t, "%d", W.chanAps[c]); dl->AddText(ImVec2(x + bw * 0.4f, p0.y + bh * (1 - frac) - 16), IM_COL32_WHITE, t); }
            if (W.chanBestDb[c]) { std::snprintf(t, sizeof t, "%d", W.chanBestDb[c]); dl->AddText(ImVec2(x + 2, p0.y + bh + 18), IM_COL32(170, 170, 170, 255), t); }
        }
        ImGui::Dummy(ImVec2(bw * 14, bh + 40 * gUi));
        ImGui::TextDisabled("%s", tr("Networks per channel (bar) and the strongest signal seen on it (dBm). Few networks on a channel = a good one to use; channels 1, 6 and 11 do not overlap."));
    } else {
        ImGui::BeginChild("##e32ev", ImVec2(0, h));
        for (auto it = sn.log.rbegin(); it != sn.log.rend(); ++it) ImGui::TextUnformatted(it->c_str());
        ImGui::EndChild();
    }
}

void bleTable(const Snapshot& sn) {
    if (!ImGui::BeginTable("##e32b", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Address")); ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn("dBm"); ImGui::TableSetupColumn("PDU"); ImGui::TableSetupColumn(tr("Seen")); ImGui::TableSetupColumn(tr("Data (hex)"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& b : sn.ble) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.addr.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", b.rssi);
        ImGui::TableNextColumn(); ImGui::Text("%d", b.pduType);
        ImGui::TableNextColumn(); ImGui::Text("%llux", (unsigned long long)b.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(b.dataHex.c_str());
    }
    ImGui::EndTable();
}

void ieeeTable(const Snapshot& sn) {
    if (!ImGui::BeginTable("##e32z", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Ch")); ImGui::TableSetupColumn("dBm"); ImGui::TableSetupColumn(tr("Frame (hex)"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& f : sn.ieee) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(f.utc).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", f.channel);
        ImGui::TableNextColumn(); ImGui::Text("%d", f.rssi);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(f.hex.c_str());
    }
    ImGui::EndTable();
}

void advancedBody(const Snapshot& sn, bool connected) {
    ImGui::TextDisabled("%s", tr("Every parameter of the selected radio. Changes apply with Listen (or Apply while listening)."));
    ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Baud (serial)"), &S.baud, 0, 0);
    if (S.kind == 0) {
        static const int rates[3] = {250, 1000, 2000};
        int ri = S.nrf.rateKbps == 250 ? 0 : S.nrf.rateKbps == 2000 ? 2 : 1;
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Channel (0-125)"), &S.nrf.channel); S.nrf.channel = std::max(0, std::min(125, S.nrf.channel));
        ImGui::SetNextItemWidth(120 * gUi); if (ImGui::Combo(tr("Data rate"), &ri, "250 kbit/s\0" "1 Mbit/s\0" "2 Mbit/s\0")) S.nrf.rateKbps = rates[ri];
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Address width (3-5)"), &S.nrf.addrWidth); S.nrf.addrWidth = std::max(3, std::min(5, S.nrf.addrWidth));
        ImGui::SetNextItemWidth(160 * gUi); ImGui::InputText(tr("Address (hex, first byte first)"), S.addrHex, sizeof S.addrHex);
        int ci = S.nrf.crcBits == 0 ? 0 : S.nrf.crcBits == 8 ? 1 : 2;
        ImGui::SetNextItemWidth(120 * gUi); if (ImGui::Combo("CRC", &ci, "off\0" "CRC-8\0" "CRC-16\0")) S.nrf.crcBits = ci == 0 ? 0 : ci == 1 ? 8 : 16;
        ImGui::Checkbox(tr("Auto acknowledge"), &S.nrf.autoAck);
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Retry delay (x250 us, 0-15)"), &S.nrf.retryDelay); S.nrf.retryDelay = std::max(0, std::min(15, S.nrf.retryDelay));
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Retries (0-15)"), &S.nrf.retryCount); S.nrf.retryCount = std::max(0, std::min(15, S.nrf.retryCount));
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Payload length (0 = dynamic, 1-32)"), &S.nrf.payloadLen); S.nrf.payloadLen = std::max(0, std::min(32, S.nrf.payloadLen));
        ImGui::SetNextItemWidth(120 * gUi); ImGui::Combo(tr("Power"), &S.nrf.power, "-18 dBm\0" "-12 dBm\0" "-6 dBm\0" "0 dBm\0");
        ImGui::Checkbox(tr("Promiscuous (find unknown addresses)"), &S.nrf.promiscuous);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The chip is set to a 2-byte address with no CRC and the raw bytes are searched for frames by OnAir. Sees much less than the SDR mode: only frames the chip's matcher lets through."));
    } else if (S.kind == 1) {
        ImGui::SetNextItemWidth(120 * gUi); ImGui::Combo(tr("Mode"), &S.wifiMode, "Listen (promiscuous)\0" "Scan (active)\0");
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Channel (0 = hop)"), &S.wifi.channel); S.wifi.channel = std::max(0, std::min(14, S.wifi.channel));
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Hop time (ms)"), &S.wifi.hopMs); S.wifi.hopMs = std::max(20, std::min(5000, S.wifi.hopMs));
    } else if (S.kind == 2) {
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Channels (bit mask 37/38/39)"), &S.ble.channelMask); S.ble.channelMask = std::max(1, std::min(7, S.ble.channelMask));
        ImGui::Checkbox(tr("Active scan"), &S.ble.activeScan);
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Power (0-7)"), &S.ble.power); S.ble.power = std::max(0, std::min(7, S.ble.power));
    } else {
        ImGui::SetNextItemWidth(120 * gUi); ImGui::InputInt(tr("Channel (11-26)"), &S.z154.channel); S.z154.channel = std::max(11, std::min(26, S.z154.channel));
    }
    if (connected && S.listening && ImGui::Button(tr("Apply"))) applyAndStart();

    ImGui::Separator();
    ImGui::TextUnformatted(tr("Transmit"));
    if (S.kind == 1) { ImGui::TextDisabled("%s", tr("WiFi is receive only: this radio never transmits WiFi.")); return; }
    if (S.kind == 3) { ImGui::TextDisabled("%s", tr("The firmware does not transmit 802.15.4 yet.")); return; }
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", tr(Radio::txWarning()));
    if (ImGui::Checkbox(tr("Arm"), &S.armed) && connected) { std::string e; if (!S.radio.setTxEnable(S.armed, e)) { S.armed = false; say(e); } }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Text instead of hex"), &S.payloadText);
    if (S.kind == 0) { ImGui::SameLine(); ImGui::Checkbox(tr("No acknowledge"), &S.noAck); }
    if (S.kind == 2) { ImGui::SetNextItemWidth(160 * gUi); ImGui::Combo(tr("Type"), &S.bleAdvType, "ADV_IND\0" "(unused)\0" "ADV_SCAN_IND\0" "ADV_NONCONN_IND\0"); }
    ImGui::SetNextItemWidth(-90 * gUi); ImGui::InputText("##e32p", S.payload, sizeof S.payload);
    ImGui::SameLine();
    if (ImGui::Button(tr("Send")) && connected) {
        std::vector<uint8_t> d;
        if (S.payloadText) d.assign(S.payload, S.payload + strlen(S.payload));
        else if (!parseHex(S.payload, d)) { say(tr("The payload is not valid hex.")); return; }
        std::string e;
        bool ok;
        if (S.kind == 0) ok = S.radio.sendNrf24(d, S.noAck, e);
        else { const uint8_t a[6] = {0xC0, 0x4F, 0x41, 0x49, 0x52, 0x01}; ok = S.radio.sendBle(S.bleAdvType, a, d, e); }
        say(ok ? tr("sent") : e);
    }
    (void)sn;
}

void advanced(const Snapshot& sn, bool connected) {
    S.advOpen = ImGui::CollapsingHeader(trw("Advanced"));
    if (!S.advOpen) return;
    ImGui::BeginChild("##e32adv", ImVec2(0, 0));
    advancedBody(sn, connected);
    ImGui::EndChild();
}

void windowPanel(App& a) {
    firstWindowSize(900 * gUi, 720 * gUi);
    if (!ImGui::Begin(trw("nRF24 / ESP32 radio"), &a.esp32Open)) { ImGui::End(); return; }
    if (!S.scanned) { S.ports = candidatePorts(); S.scanned = true; }
    const bool connected = S.radio.connected();
    const Snapshot sn = S.radio.snapshot();
    ImGui::TextWrapped("%s", tr("An ESP32 board on USB (with an nRF24L01+ module for 2.4 GHz packets) as a receiver: nRF24 packets, Bluetooth LE advertising and WiFi networks. Firmware: firmware/esp32. Not yet tried on a real board."));
    // device
    if (!connected) {
        std::string cur = S.portIdx == 0 || S.portIdx > (int)S.ports.size() ? std::string(tr("Automatic")) : S.ports[S.portIdx - 1];
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::BeginCombo("##e32port", cur.c_str())) {
            if (ImGui::Selectable(tr("Automatic"), S.portIdx == 0)) S.portIdx = 0;
            for (size_t i = 0; i < S.ports.size(); i++) if (ImGui::Selectable(S.ports[i].c_str(), S.portIdx == (int)i + 1)) S.portIdx = (int)i + 1;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Rescan"))) S.ports = candidatePorts();
        ImGui::SameLine();
        ImGui::BeginDisabled(S.busy != 0);
        if (ImGui::Button(tr("Connect"))) connectAsync(S.portIdx == 0 || S.portIdx > (int)S.ports.size() ? "" : S.ports[S.portIdx - 1]);
        ImGui::EndDisabled();
        ImGui::TextDisabled("%s", said().c_str());
        if (S.ports.empty()) ImGui::TextDisabled("%s", tr("No USB serial port found. Plug the ESP32 in (a USB-serial driver may be needed)."));
        ImGui::End();
        return;
    }
    if (S.busy == 0 && S.th.joinable()) S.th.join();
    char info[200];
    std::snprintf(info, sizeof info, "%s on %s, firmware %d.%d, protocol %d", sn.info.name.c_str(), sn.info.port.c_str(), sn.info.fwMajor, sn.info.fwMinor, sn.info.protoVersion);
    ImGui::TextUnformatted(info);
    ImGui::SameLine();
    if (ImGui::Button(tr("Disconnect"))) { S.radio.disconnect(); S.listening = false; S.armed = false; ImGui::End(); return; }
    // radio and the simple controls (receive first)
    struct K { const char* name; int bit; };
    static const K ks[] = {{"nRF24", w::kBitNrf24}, {"WiFi", w::kBitWifi}, {"Bluetooth LE", w::kBitBle}, {"802.15.4", w::kBitIeee802154}};
    for (int i = 0; i < 4; i++) {
        if (!(sn.info.radios & ks[i].bit)) continue;
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(ks[i].name, S.kind == i) && S.kind != i) { if (S.listening) stopListening(); S.kind = i; S.view = 0; S.selAp = -1; }
    }
    if (!(sn.info.radios & w::kBitNrf24)) ImGui::TextDisabled("%s", tr("No nRF24 module found on the SPI bus (check the wiring and the 3.3 V supply)."));
    if (S.kind == 0) {
        static const int rates[3] = {250, 1000, 2000};
        int ri = S.nrf.rateKbps == 250 ? 0 : S.nrf.rateKbps == 2000 ? 2 : 1;
        ImGui::SetNextItemWidth(110 * gUi); ImGui::InputInt(tr("Channel"), &S.nrf.channel); S.nrf.channel = std::max(0, std::min(125, S.nrf.channel));
        ImGui::SameLine(); ImGui::TextDisabled("%d MHz", 2400 + S.nrf.channel);
        ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi); if (ImGui::Combo("##e32rate", &ri, "250 kbit/s\0" "1 Mbit/s\0" "2 Mbit/s\0")) S.nrf.rateKbps = rates[ri];
    } else if (S.kind == 1) {
        ImGui::SetNextItemWidth(160 * gUi); ImGui::Combo("##e32wm", &S.wifiMode, tr("Listen (all channels)\0Scan (active)\0"));
        if (S.wifiMode == 0 && S.wifi.channel == 0) { /* hop */ }
    }
    ImGui::SameLine();
    if (!S.listening) { if (ImGui::Button(tr("Listen"))) applyAndStart(); }
    else if (ImGui::Button(tr("Stop"))) stopListening();
    ImGui::SameLine(); if (ImGui::Button(tr("Clear"))) S.radio.clearResults();
    ImGui::SameLine(); ImGui::TextDisabled("%s", said().c_str());
    ImGui::Separator();
    // results get what is left after the Advanced header (collapsed: one line)
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##e32res", ImVec2(0, S.advOpen ? avail * 0.45f : avail - ImGui::GetFrameHeightWithSpacing() * 1.4f));
    if (S.kind == 0) nrfTables(sn); else if (S.kind == 1) wifiTables(sn); else if (S.kind == 2) bleTable(sn); else ieeeTable(sn);
    ImGui::EndChild();
    advanced(sn, connected);
    ImGui::End();
}
} // namespace

void esp32Window(App& a) {
    if (a.rxId != 1) {   // one radio window for the program: RX 1's
        if (App* f = receiverById(1)) { f->esp32Open = a.esp32Open; esp32Window(*f); a.esp32Open = f->esp32Open; }
        return;
    }
    if (a.esp32Open) windowPanel(a);
    else if (S.busy == 0 && S.th.joinable()) S.th.join();
}
