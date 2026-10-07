// the Scan tab and the Outputs tab (network tuner, files, UDP)
#include "app.h"
#include <random>

void scanTab(App& a) {
    if (const ModeUi* mu = modeUi(a.family)) { if (mu->scan) mu->scan(a); else ImGui::TextDisabled("Scanning is not available for this mode yet. Tune with the frequency field."); return; }
    if (a.dabMode) { dabScanTab(a); return; }
    if (a.fmMode) { fmScanTab(a); return; }
    if (a.atsc3Mode) { ImGui::TextDisabled("Channel scanning does not know ATSC 3.0 yet.\nTune to a channel with the frequency field in the toolbar; the receiver finds the bootstrap by itself."); return; }
    ScanProgress pr = a.scanner.progress();
    auto res = a.scanner.results();
    int hw = -1;
    if (a.devices[a.devIdx].isRadio()) hw = a.devIdx;   // the radio chosen in the toolbar, else the last one in the list
    else for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) hw = i;
    if (hw < 0) { ImGui::TextDisabled("Scanning needs a radio (HackRF, Airspy, SDRplay, ...)."); return; }
    static std::string scanErr;
    ImGui::BeginDisabled(pr.running);
    const char* presetsDvb[] = {"UHF 474-858 MHz (8 MHz)", "VHF III 174-230 MHz (7 MHz)", "Custom"};
    const char* presetsAtsc[] = {"US UHF ch 14-36 (470-608 MHz)", "US VHF high ch 7-13 (174-216 MHz)", "Custom"};
    const char* presetsIsdbt[] = {"UHF ch 13-62 (473-767 MHz)", "VHF high ch 7-13 (177-213 MHz)", "Custom"};
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::Combo("##range", &a.scanPreset, a.isdbtMode ? presetsIsdbt : a.atscMode ? presetsAtsc : presetsDvb, 3)) {
        if (a.isdbtMode) {   // the centres of the 6 MHz channels are 1/7 MHz above a whole number
            if (a.scanPreset == 0) { a.scanCfg.startMHz = 473.143; a.scanCfg.stopMHz = 767.143; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
            if (a.scanPreset == 1) { a.scanCfg.startMHz = 177.143; a.scanCfg.stopMHz = 213.143; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
        } else if (a.atscMode) {
            if (a.scanPreset == 0) { a.scanCfg.startMHz = 473; a.scanCfg.stopMHz = 605; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
            if (a.scanPreset == 1) { a.scanCfg.startMHz = 177; a.scanCfg.stopMHz = 213; a.scanCfg.stepMHz = 6; a.scanCfg.bwMhz = 6; }
        } else {
            if (a.scanPreset == 0) { a.scanCfg.startMHz = 474; a.scanCfg.stopMHz = 858; a.scanCfg.stepMHz = 8; a.scanCfg.bwMhz = 8; }
            if (a.scanPreset == 1) { a.scanCfg.startMHz = 177.5; a.scanCfg.stopMHz = 226.5; a.scanCfg.stepMHz = 7; a.scanCfg.bwMhz = 7; }
        }
    }
    ImGui::SameLine(); ImGui::TextUnformatted("from"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("##from", &a.scanCfg.startMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("to"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputDouble("##to", &a.scanCfg.stopMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("step"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputDouble("##step", &a.scanCfg.stepMHz, 0, 0, "%.1f");
    ImGui::SameLine(); ImGui::TextUnformatted("bw"); ImGui::SameLine(0, 4 * gUi); ImGui::SetNextItemWidth(60 * gUi);
    ImGui::BeginDisabled(a.scanCfg.autoBandwidth);
    ImGui::InputDouble("##bw", &a.scanCfg.bwMhz, 0, 0, "%.0f");
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::Checkbox("detect bandwidth", &a.scanCfg.autoBandwidth);
    ImGui::Checkbox("read service names (slower, ~9 s per mux)", &a.scanCfg.identifyServices);
    ImGui::EndDisabled();
    if (a.scanCfg.stepMHz < 1) a.scanCfg.stepMHz = 1;
    if (!pr.running) {
        if (ImGui::Button("  Start scan  ")) {
            a.scanCfg.tune = a.tune;
            a.scanCfg.atsc = a.atscMode && !a.isdbtMode;
            a.scanCfg.isdbt = a.isdbtMode;
            std::string err;
            if (!Scanner::check(a.devices[hw], a.scanCfg, err)) { a.engine.log("scan: " + err); scanErr = err; }   // before the receiver is stopped
            else {
                a.scanWasRunning = a.engine.running();
                if (a.scanWasRunning) a.engine.stop();
                if (!a.scanner.start(a.devices[hw], a.scanCfg, err)) { a.engine.log("scan: " + err); scanErr = err; }
                else scanErr.clear();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s  (uses the gains from the toolbar; stops the receiver while scanning)", pr.phase.c_str());
        if (!scanErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.3f, 1), "Cannot scan: %s", scanErr.c_str());
    } else {
        if (ImGui::Button("  Stop scan  ")) a.scanner.stop();
        ImGui::SameLine();
        ImGui::ProgressBar(pr.total ? (float)pr.index / pr.total : 0, ImVec2(260 * gUi, 0));
        ImGui::SameLine();
        ImGui::Text("%.1f MHz - %s", pr.currentMHz, pr.phase.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginTable("scan", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("result", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, 240);
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("PLP", ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn("services");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < res.size(); i++) {
            auto& r = res[i];
            if (!r.occupied && !r.t2) continue; // hide empty channels
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[32]; snprintf(lbl, sizeof lbl, "%.1f##%zu", r.freqMHz, i);
            if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns) && !pr.running) {
                a.freqMhz = r.freqMHz; a.bwIdx = r.bwMhz >= 7.5 ? 0 : 1;
                for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == r.bwMhz) a.bwIdx = k;
                a.tune.centerHz = a.freqMhz * 1e6; applyBandwidth(a);
                a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
                a.engine.start(a.devices[hw], a.tune, a.file);
                a.devIdx = hw; a.smooth.clear(); a.peak.clear(); a.lastSeq = 0; savePrefs(a);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to tune the receiver to this channel");
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", r.levelDbfs);
            ImGui::TableNextColumn();
            if (r.t2) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "%s", r.standard.c_str()); else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "other");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); if (r.t2) ImGui::Text("%.1f dB", r.snrDb);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.plpInfo.c_str());
            ImGui::TableNextColumn();
            std::string sv;
            for (auto& x : r.services) { if (!sv.empty()) sv += ",  "; sv += x; }
            if (sv.empty() && !r.t2) sv = r.note;
            if (!r.unsupported.empty()) sv = (sv.empty() ? "" : sv + "  ") + "! " + r.unsupported[0];
            ImGui::TextWrapped("%s", sv.c_str());
        }
        ImGui::EndTable();
    }
}

void outputsTab(App& a) {
    bool ch = false;
    ImGui::TextColored(pal::heading(), "What to send");
    {
        std::string cur = "Whole multiplex";
        for (auto& sv : a.ts.services) if (sv.id == a.selService) cur = sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name;
        ImGui::SetNextItemWidth(320 * gUi);
        if (ImGui::BeginCombo("service", cur.c_str())) {
            if (ImGui::Selectable("Whole multiplex", a.selService < 0)) { a.selService = -1; ch = true; }
            for (auto& sv : a.ts.services) {
                std::string n = (sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + "  [" + sv.typeName() + "]";
                if (ImGui::Selectable(n.c_str(), sv.id == a.selService)) { a.selService = sv.id; ch = true; }
            }
            ImGui::EndCombo();
        }
    }
    if (ImGui::Checkbox("remove null packets", &a.out.dropNull)) ch = true;
    ImGui::SameLine(); ImGui::TextDisabled("(a single service is always rewritten with its own PAT)");
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "File (.ts)");
    ImGui::SetNextItemWidth(520 * gUi);
    ImGui::InputText("##fp", a.filePath, sizeof a.filePath);
    ImGui::SameLine();
    if (ImGui::Button("Choose...")) { auto p = saveFileDialog("recording.ts"); if (!p.empty()) snprintf(a.filePath, sizeof a.filePath, "%s", p.c_str()); }
    ImGui::SameLine();
    if (ImGui::Checkbox("record", &a.out.file)) ch = true;
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "UDP");
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::InputText("address", a.udpHost, sizeof a.udpHost)) {}
    ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("port", &a.out.port, 0, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt("TTL", &a.out.ttl, 0, 0);
    ImGui::SameLine();
    ImGui::Checkbox("RTP", &a.out.rtp);
    ImGui::SameLine();
    if (ImGui::Checkbox("stream", &a.out.udp)) ch = true;
    ImGui::TextDisabled("Unicast or multicast (e.g. 239.1.1.1). Datagrams carry 7 packets and are paced evenly. Play with: ffplay udp://@:%d", a.out.port);
    if (ch || ImGui::IsItemDeactivatedAfterEdit()) applyOutputs(a);
    if (ImGui::Button("Apply address / port / TTL")) applyOutputs(a);
    ImGui::Spacing();
    OutputStats os = a.engine.outputStats();
    ImGui::PushFont(a.mono, 0);
    if (os.fileOpen) ImGui::Text("recording: %llu packets, %.1f MB", (unsigned long long)os.filePackets, os.fileBytes / 1e6);
    else ImGui::TextDisabled("recording: off");
    if (os.udpOpen) ImGui::Text("streaming: %llu datagrams, queue %.0f ms, dropped %llu", (unsigned long long)os.udpDatagrams, os.udpQueueMs, (unsigned long long)os.udpDropped);
    else ImGui::TextDisabled("streaming: off");
    ImGui::PopFont();
    if (!os.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", os.error.c_str());
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "Network tuner");
    {
        const NetTunerStats ns = a.net.stats();
        bool apply = false;
        ImGui::BeginDisabled(ns.running);
        ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt("port##net", &a.netPort, 0, 0);
        ImGui::SameLine(); ImGui::Checkbox("share on the network", &a.netLan);
        ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi); ImGui::InputText("key (optional)", a.netKey, sizeof a.netKey);
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool on = ns.running;
        if (ImGui::Checkbox("serve", &on)) apply = true;
        if (apply) {
            if (on) {
                NetTunerConfig nc; nc.port = std::max(1024, std::min(65535, a.netPort)); nc.localOnly = !a.netLan; nc.key = a.netKey;
                a.netOn = a.net.start(nc);
                a.engine.log(a.netOn ? "network tuner on port " + std::to_string(nc.port) : "network tuner: " + a.net.stats().error);
            } else { a.net.stop(); a.netOn = false; a.engine.log("network tuner stopped"); }
        }
        const NetTunerStats ns2 = a.net.stats();
        if (!ns2.error.empty() && !ns2.running) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", ns2.error.c_str());
        if (ns2.running) {
            ImGui::PushFont(a.mono, 0);
            const std::string k = a.netKey[0] ? std::string("?key=") + a.netKey : "";
            for (const auto& ad : a.net.addresses()) ImGui::Text("http://%s:%d/lineup.m3u%s", ad.c_str(), ns2.port, k.c_str());
            ImGui::Text("viewers: %d   sent: %.1f MB", ns2.clients, ns2.bytesSent / 1e6);
            ImGui::PopFont();
        }
        ImGui::TextDisabled("Open the playlist in VLC or any IPTV app; /guide.xml has the programme guide. Plex and Jellyfin: add an HDHomeRun at that address.");
        ImGui::TextDisabled("Without a key, anyone on your network can watch while \"share on the network\" is on.");
    }
    if (airplay::available()) {
        ImGui::Spacing();
        ImGui::TextColored(pal::heading(), "Cast to a TV (AirPlay)");
        const int sid = a.engine.player().selected();
        const airplay::State st = airplay::state();
        ImGui::BeginDisabled(sid < 0 || st == airplay::State::Choosing);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::Button("Cast the playing service...");
        const ImVec2 sz = ImGui::GetItemRectSize();
        ImGui::EndDisabled();
        if (sid < 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Play a service first (click it in the list).");
        if (pressed) {
            // an Apple TV fetches the stream from this computer: the network tuner must be reachable from the network, with a key
            NetTunerStats ns = a.net.stats();
            if (!ns.running || !a.netLan || !a.netKey[0]) {
                if (!a.netKey[0]) snprintf(a.netKey, sizeof a.netKey, "%08x", (unsigned)std::random_device{}());
                a.net.stop();
                a.netLan = true;
                NetTunerConfig nc; nc.port = std::max(1024, std::min(65535, a.netPort)); nc.localOnly = false; nc.key = a.netKey;
                a.netOn = a.net.start(nc);
                a.engine.log(a.netOn ? "network tuner on port " + std::to_string(nc.port) + " (for casting)" : "network tuner: " + a.net.stats().error);
                ns = a.net.stats();
            }
            const auto addrs = a.net.addresses();
            if (ns.running && !addrs.empty()) {
                const std::string url = "http://" + addrs[0] + ":" + std::to_string(ns.port) + "/hls/" + std::to_string(sid) + "/index.m3u8?key=" + a.netKey;
                airplay::choose(gWindow, at.x, at.y, sz.x, sz.y, url);
                a.engine.log("casting " + url);
            } else a.engine.log("casting: the network tuner is not reachable from the network");
        }
        ImGui::SameLine();
        if (st == airplay::State::Casting) { ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "casting"); ImGui::SameLine(); if (ImGui::SmallButton("stop casting")) airplay::stop(); }
        else if (st == airplay::State::Choosing) ImGui::TextDisabled("choose a device in the list...");
        else if (st == airplay::State::Failed) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", airplay::message().c_str());
        ImGui::TextDisabled("Starting takes several seconds. The picture is H.264 or HEVC as broadcast; the sound is converted to AAC. OnAir plays on as usual: mute it here if you do not want it twice.");
    }
}

