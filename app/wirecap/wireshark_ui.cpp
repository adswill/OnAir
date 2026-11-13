// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > Wireshark: one switch per mode. The work is in core (core/src/wirecap, docs/WIRESHARK-3.8.md); every switch is off until the user turns it on.
#include "../app.h"
#include "../ui/typography.h"
#include "dect2/wirecap/wire.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace wire = dect2::wire;
namespace fs = std::filesystem;

namespace {
wire::Config gCfg;
std::string gInstall;
bool gInstallOk = false;

std::string defaultDir() { return plat::dataDir() + "/wireshark"; }

std::string keyOf(int c) { return std::string("wire_") + wire::kChans[c].id; }

std::string extcapProgram() {
    std::string exe;
#if defined(_WIN32)
    char p[MAX_PATH * 2] = {0};
    if (GetModuleFileNameA(nullptr, p, (DWORD)sizeof p - 1)) exe = p;
    const char* name = "onair-extcap.exe";
#elif defined(__APPLE__)
    char p[4096]; uint32_t n = sizeof p;
    if (_NSGetExecutablePath(p, &n) == 0) exe = p;
    const char* name = "onair-extcap";
#else
    char p[4096]; const ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n > 0) exe.assign(p, (size_t)n);
    const char* name = "onair-extcap";
#endif
    const std::string dir = fs::path(exe).parent_path().string();
    for (const char* rel : {"/", "/../Resources/", "/../lib/onair/"}) {
        std::error_code ec;
        const fs::path h = fs::path(dir + rel + name).lexically_normal();
        if (fs::exists(h, ec)) return h.string();
    }
    return dir + "/" + name;
}

std::string wiresharkHome() {
#ifdef _WIN32
    const char* v = getenv("APPDATA");
#else
    const char* v = getenv("HOME");
#endif
    return v ? v : "";
}

bool isLocalHost(const std::string& h) { return h == "127.0.0.1" || h == "localhost" || h == "::1"; }
} // namespace

void loadWiresharkPrefs(App&) {
    plat::Prefs& d = plat::prefs();
    gCfg.dir = d.getS("wireDir", defaultDir());
    gCfg.files = d.getB("wireFiles", true);
    gCfg.live = d.getB("wireLive", true);
    gCfg.gsmtap = d.getB("wireGsmtap", true);
    gCfg.gsmtapHost = d.getS("wireGsmtapHost", "127.0.0.1");
    gCfg.gsmtapPort = (int)d.getI("wireGsmtapPort", wire::kGsmtapPort);
    gCfg.streamPort = (int)d.getI("wirePort", wire::kDefaultStreamPort);
    wire::Hub::get().configure(gCfg);
    for (int i = 0; i < wire::kChanCount; i++) wire::Hub::get().setEnabled((wire::Chan)i, d.getB(keyOf(i).c_str(), false));
}

void saveWiresharkPrefs(const App&) {
    plat::Prefs& d = plat::prefs();
    d.setS("wireDir", gCfg.dir); d.setB("wireFiles", gCfg.files); d.setB("wireLive", gCfg.live); d.setB("wireGsmtap", gCfg.gsmtap);
    d.setS("wireGsmtapHost", gCfg.gsmtapHost); d.setI("wireGsmtapPort", gCfg.gsmtapPort); d.setI("wirePort", gCfg.streamPort);
    for (int i = 0; i < wire::kChanCount; i++) d.setB(keyOf(i).c_str(), wire::Hub::get().enabled((wire::Chan)i));
}

void wiresharkWindow(App& a) {
    if (a.rxId != 1) {   // shared by all receivers: one window, RX 1
        if (App* f = receiverById(1)) { f->wiresharkOpen = a.wiresharkOpen; wiresharkWindow(*f); a.wiresharkOpen = f->wiresharkOpen; }
        return;
    }
    if (!a.wiresharkOpen) return;
    ImGui::SetNextWindowSize(ImVec2(620 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Wireshark"), &a.wiresharkOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    auto& hub = wire::Hub::get();
    bool changed = false;
    ImGui::TextWrapped("%s", tr("Turn a mode on to get its packets in Wireshark: as a capture interface (\"OnAir: ...\" in Wireshark's list) and as a pcapng file. Packets are passed on as received: what is encrypted on the air stays encrypted. Everything stays on this computer."));
    if (ImGui::Button(tr("Add OnAir to Wireshark"))) {
        const wire::InstallResult r = wire::installIntoWireshark(extcapProgram(), wiresharkHome());
        gInstallOk = r.ok;
        gInstall = r.ok ? std::string(tr("Done. Restart Wireshark, or press F5 in its interface list. Installed: ")) + r.extcapPath : r.error;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Copies the small extcap program and onair.lua into your personal Wireshark folders. Needed once."));
    if (!gInstall.empty()) ImGui::TextColored(gInstallOk ? ImVec4(0.5f, 0.85f, 0.5f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", gInstall.c_str());
    ImGui::Spacing();
    if (ImGui::BeginTable("wire", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Mode"));
        ImGui::TableSetupColumn(tr("On"));
        ImGui::TableSetupColumn(tr("Packets"));
        ImGui::TableSetupColumn(tr("Link type"));
        ImGui::TableHeadersRow();
        for (int i = 0; i < wire::kChanCount; i++) {
            const wire::ChanInfo& c = wire::kChans[i];
            const wire::Stats s = hub.stats((wire::Chan)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.display);
            ImGui::TableNextColumn();
            bool on = hub.enabled((wire::Chan)i);
            ImGui::PushID(i);
            if (ImGui::Checkbox("##on", &on)) { hub.setEnabled((wire::Chan)i, on); changed = true; }
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (s.dropped) ImGui::Text("%llu (%llu %s)", (unsigned long long)s.packets, (unsigned long long)s.dropped, tr("dropped"));
            else ImGui::Text("%llu", (unsigned long long)s.packets);
            if (s.clients > 0) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("live")); }
            if (!s.file.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.file.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", c.encap == wire::Encap::Gsmtap ? "GSMTAP" : c.linkName);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Read in Wireshark by"), c.dissector);
            if (!s.error.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.4f, 1), "%s", s.error.c_str()); }
        }
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        char b[512];
        bool cfg = false;
        cfg |= ImGui::Checkbox(tr("Write pcapng files"), &gCfg.files);
        snprintf(b, sizeof b, "%s", gCfg.dir.c_str());
        if (ImGui::InputText(tr("Folder"), b, sizeof b, ImGuiInputTextFlags_EnterReturnsTrue)) { gCfg.dir = b; cfg = true; }
        cfg |= ImGui::Checkbox(tr("Live capture for Wireshark (this computer only)"), &gCfg.live);
        int port = gCfg.streamPort;
        if (ImGui::InputInt(tr("Local port"), &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && port > 0 && port < 65536) { gCfg.streamPort = port; cfg = true; }
        cfg |= ImGui::Checkbox(tr("Send GSMTAP over UDP (GSM-R, TETRA, LTE)"), &gCfg.gsmtap);
        snprintf(b, sizeof b, "%s", gCfg.gsmtapHost.c_str());
        if (ImGui::InputText(tr("GSMTAP host"), b, sizeof b, ImGuiInputTextFlags_EnterReturnsTrue)) { gCfg.gsmtapHost = b; cfg = true; }
        port = gCfg.gsmtapPort;
        if (ImGui::InputInt(tr("GSMTAP port"), &port, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && port > 0 && port < 65536) { gCfg.gsmtapPort = port; cfg = true; }
        if (gCfg.gsmtap && !isLocalHost(gCfg.gsmtapHost)) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", tr("GSMTAP packets will leave this computer."));
        if (cfg) { hub.configure(gCfg); changed = true; }
    }
    if (changed) savePrefs(a);
    ImGui::End();
}
