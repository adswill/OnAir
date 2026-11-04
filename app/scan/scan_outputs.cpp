// the Scan tab and the Outputs tab (network tuner, files, UDP)
#include "../app.h"
#include "../state/results_export.h"
#include <random>

void scanTabCommon(App& a);
void scanDbHost(App& a, void (*inner)(App&));   // scan_db_ui.cpp: "Shared data" view and the share dialog around the scan tab

static void scanTabMode(App& a);
void scanTab(App& a) { if (!a.newUi) scannerButton(a); scanDbHost(a, scanTabMode); }

static void scanTabMode(App& a) {
    if (const ModeUi* mu = modeUi(a.family)) { if (mu->scan) mu->scan(a); else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Scanning is not available for this mode yet. Tune with the frequency field."); ImGui::PopTextWrapPos(); } return; }
    if (a.dabMode) { dabScanTab(a); return; }
    if (a.fmMode) { fmScanTab(a); return; }
    scanTabCommon(a);
}

// The scan tab of the TV modes: DVB, ATSC, ATSC 3.0, ISDB-T and (through its ModeUi) DTMB share it. The scanner picks the standard from the flags.
void scanTabCommon(App& a) {
    const bool dtmb = a.family == 7;   // the DTMB family, see engineStd()
    ScanProgress pr = a.scanner.progress();
    auto res = a.scanner.results();
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) a.devIdx = 0;   // a rescan that found fewer radios must not leave the choice dangling
    int hw = -1;
    // the radio chosen in the toolbar, else the first one in the list (a radio's first entry is its default input; the later ones are
    // its other antenna sockets)
    if (a.devices[a.devIdx].isRadio()) hw = a.devIdx;
    else for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) { hw = i; break; }
    if (hw < 0) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Scanning needs a radio (HackRF, Airspy, SDRplay, ...)."); ImGui::PopTextWrapPos(); } return; }
    static std::string scanErr;
    ImGui::BeginDisabled(pr.running);
    // The ranges come from the band plan (Scan tab > Band plan: a country, or the user's own file); "None" keeps the lists below.
    const bool planChanged = bandPlanPicker(a);
    const std::vector<TvRange> planned = tvBandRanges(a);
    const char* presetsDvb[] = {"UHF TV 474-858 MHz (8 MHz)", "VHF TV 174-230 MHz (7 MHz)", "Custom"};
    const char* presetsAtsc[] = {"US UHF TV ch 14-36 (470-608 MHz)", "US VHF TV ch 7-13 (174-216 MHz)", "Custom"};
    const char* presetsIsdbt[] = {"UHF TV ch 13-62 (473-767 MHz)", "VHF TV ch 7-13 (177-213 MHz)", "Custom"};
    const char* presetsDtmb[] = {"China UHF TV 474-858 MHz (8 MHz)", "China VHF TV ch 6-12 (171-219 MHz)", "Cuba UHF TV ch 14-51 (473-695 MHz, 6 MHz)", "Custom"};   // Hong Kong and Macau use channels of the same 8 MHz raster
    auto setRange = [&](double lo, double hi, double step, double bw) { a.scanCfg.startMHz = lo; a.scanCfg.stopMHz = hi; a.scanCfg.stepMHz = step; a.scanCfg.bwMhz = bw; };
    ImGui::SetNextItemWidth(std::min(260 * gUi, ImGui::GetContentRegionAvail().x));
    if (!planned.empty()) {
        std::vector<const char*> names;
        for (const TvRange& r : planned) names.push_back(r.label.c_str());
        names.push_back("Custom");
        auto apply = [&](int i) {
            const TvRange& r = planned[(size_t)i];
            setRange(r.startMhz, r.stopMhz, r.stepMhz, r.bwMhz);
            if (dtmb) { a.dtmbBwMhz = r.bwMhz < 7 ? 6 : 8; applyBandwidth(a); savePrefs(a); }   // DTMB is scanned with the channel width set in its tuner
        };
        if (a.scanPreset < 0 || a.scanPreset > (int)planned.size()) a.scanPreset = 0;
        if (planChanged) { a.scanPreset = 0; apply(0); }
        if (ImGui::Combo("##range", &a.scanPreset, names.data(), (int)names.size()) && a.scanPreset < (int)planned.size()) apply(a.scanPreset);
    } else
    if (ImGui::Combo("##range", &a.scanPreset, dtmb ? presetsDtmb : a.isdbtMode ? presetsIsdbt : a.atscMode ? presetsAtsc : presetsDvb, dtmb ? 4 : 3)) {
        if (dtmb) {   // the centres of the 8 MHz channels
            if (a.scanPreset == 0) setRange(474, 858, 8, 8);
            if (a.scanPreset == 1) setRange(171, 219, 8, 8);
            // Cuba: DTMB in the American 6 MHz raster (the DTMB channel width follows: the receiver is set to 6 MHz)
            if (a.scanPreset == 2) setRange(473, 695, 6, 6);
            if (a.scanPreset <= 2) { a.dtmbBwMhz = a.scanPreset == 2 ? 6 : 8; applyBandwidth(a); savePrefs(a); }
        } else if (a.isdbtMode) {   // the centres of the 6 MHz channels are 1/7 MHz above a whole number
            if (a.scanPreset == 0) setRange(473.143, 767.143, 6, 6);
            if (a.scanPreset == 1) setRange(177.143, 213.143, 6, 6);
        } else if (a.atscMode) {
            if (a.scanPreset == 0) setRange(473, 605, 6, 6);
            if (a.scanPreset == 1) setRange(177, 213, 6, 6);
        } else {
            if (a.scanPreset == 0) setRange(474, 858, 8, 8);
            if (a.scanPreset == 1) setRange(177.5, 226.5, 7, 7);
        }
    }
    // the row wraps in a narrow window, each label with its field
    auto field = [&](const char* label, const char* id, double* v, float w, const char* fmt) {
        flowNext();
        ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(w * gUi); ImGui::InputDouble(id, v, 0, 0, fmt);
        ImGui::EndGroup();
    };
    field("from", "##from", &a.scanCfg.startMHz, 80, "%.1f");
    field("to", "##to", &a.scanCfg.stopMHz, 80, "%.1f");
    field("step", "##step", &a.scanCfg.stepMHz, 60, "%.1f");
    ImGui::BeginDisabled(a.scanCfg.autoBandwidth || a.atsc3Mode || dtmb);   // ATSC 3.0 and DTMB are scanned with their one channel width
    field("bw", "##bw", &a.scanCfg.bwMhz, 60, "%.0f");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(a.atscMode || dtmb);   // the 6 MHz modes and DTMB have a fixed width
    flowNext(); ImGui::Checkbox("detect bandwidth", &a.scanCfg.autoBandwidth);
    flowEnd();
    ImGui::EndDisabled();
    ImGui::Checkbox("read service names (slower, ~9 s per mux)", &a.scanCfg.identifyServices);
    ImGui::Checkbox("optimise the gain on every signal found (up to ~12 s each)", &a.scanCfg.autoGain);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Auto gain's \"Optimise once\" on each occupied channel before it is decoded: the gain with the best MER that does not overload.\n"
                                                        "The next channel starts from it. The amp and the antenna power stay as you set them.");
    ImGui::EndDisabled();
    if (a.scanCfg.stepMHz < 1) a.scanCfg.stepMHz = 1;
    if (!pr.running) {
        if (ImGui::Button("  Start scan  ")) {
            a.scanCfg.tune = a.tune;
            a.scanCfg.atsc = a.atscMode && !a.isdbtMode && !a.atsc3Mode;
            a.scanCfg.atsc3 = a.atsc3Mode;
            a.scanCfg.isdbt = a.isdbtMode;
            a.scanCfg.dtmb = dtmb;
            if (dtmb) a.scanCfg.bwMhz = a.dtmbBwMhz;   // DTMB is scanned with the channel width set in its tuner (8 or 6 MHz)
            std::string err;
            if (!Scanner::check(a.devices[hw], a.scanCfg, err)) { a.engine.log("scan: " + err); scanErr = err; }   // before the receiver is stopped
            else {
                a.scanWasRunning = a.engine.running();
                // The scanner opens the radio itself, so the receiver lets go of it first (stop() returns when the radio is closed and
                // every thread of this engine is joined). The scanner works on a copy of the device entry: a rescan cannot change it under it.
                if (a.scanWasRunning) a.engine.stop();
                const DeviceInfo radio = a.devices[hw];
                if (!a.scanner.start(radio, a.scanCfg, err)) { a.engine.log("scan: " + err); scanErr = err; }
                else scanErr.clear();
            }
        }
        ImGui::SameLine();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s  (uses the gains from the toolbar; stops the receiver while scanning)", pr.phase.c_str()); ImGui::PopTextWrapPos(); }
        if (!scanErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.3f, 1), "Cannot scan: %s", scanErr.c_str());
    } else {
        if (ImGui::Button("  Stop scan  ")) a.scanner.stop();
        ImGui::SameLine();
        ImGui::ProgressBar(pr.total ? (float)pr.index / pr.total : 0, ImVec2(260 * gUi, 0));
        ImGui::SameLine();
        ImGui::Text("%.1f MHz - %s", pr.currentMHz, pr.phase.c_str());
    }
    {
        static std::string note;
        resexp::Table t; t.cols = {"time", "frequency_hz", "mode", "bandwidth_mhz", "level_dbfs", "result", "detail", "snr_db", "plp", "services"};
        const std::string now = resexp::stamp();
        for (const auto& r : res) {
            if (!r.seen) continue;
            std::string sv; for (auto& x : r.services) { if (!sv.empty()) sv += "; "; sv += x; }
            if (sv.empty() && !r.t2) sv = r.note;
            t.add({now, resexp::num(r.freqMHz * 1e6, 0), r.standard.empty() ? "scan" : r.standard, resexp::num(r.bwMhz, 1), resexp::num(r.levelDbfs, 1), r.t2 ? r.standard : "too weak", r.mode,
                   r.t2 && (r.snrDb != 0 || r.standard != "ATSC 3.0") ? resexp::num(r.snrDb, 1) : "", r.plpInfo, sv});
        }
        resexp::saveButton(t, "tv", note, "svtv");
        if (!note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", note.c_str()); }
    }
    ImGui::Separator();
    // the scan list keeps every transponder scanned (one row each, headed by its frequency); the live service list shows only the tuned one
    ImGui::BeginDisabled(pr.running || res.empty());
    if (ImGui::SmallButton(tr("Clear"))) a.scanner.setResults({});
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Forgets the scan results. The live service list is not touched."));
    ImGui::SameLine();
    ImGui::Checkbox(tr("Show services from all scanned transponders"), &a.scanShowAll);
    if (ImGui::BeginTable("scan", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Transponder MHz"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn("level", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn("result", ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed, 240 * gUi);
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 60 * gUi);
        ImGui::TableSetupColumn("PLP", ImGuiTableColumnFlags_WidthFixed, 180 * gUi);
        ImGui::TableSetupColumn("services");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < res.size(); i++) {
            auto& r = res[i];
            if (!r.seen) continue;   // only the channels where the scanned standard was found: not the empty ones, nor signals of another kind
            if (!a.scanShowAll && std::fabs(r.freqMHz - a.freqMhz) > std::max(0.5, r.bwMhz * 0.5)) continue;   // only the transponder being received
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[32]; snprintf(lbl, sizeof lbl, "%.1f##%zu", r.freqMHz, i);
            if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns) && !pr.running) {
                a.freqMhz = r.freqMHz; a.bwIdx = r.bwMhz >= 7.5 ? 0 : 1;
                for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == r.bwMhz) a.bwIdx = k;
                a.tune.centerHz = a.freqMhz * 1e6; applyBandwidth(a);
                applyGpuPolicy(a); a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
                a.engine.start(a.devices[hw], a.tune, a.file);
                a.devIdx = hw; a.smooth.clear(); a.peak.clear(); a.lastSeq = 0; savePrefs(a);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to tune the receiver to this channel");
            ImGui::TableNextColumn(); ImGui::Text("%.1f dBFS", r.levelDbfs);
            ImGui::TableNextColumn();
            if (r.t2) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1), "%s", r.standard.c_str()); else ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "too weak");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableNextColumn(); if (r.t2) { if (r.snrDb != 0 || r.standard != "ATSC 3.0") ImGui::Text("%.1f dB", r.snrDb); else ImGui::TextDisabled("-"); }   // the ATSC 3.0 receiver reports no SNR
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
        // Two programmes often share a name and a type (two "W8RZ-DT (TV)" rows). The visible text can match; the id after ## cannot, or ImGui reports a conflicting ID.
        auto face = [](const TsService& sv) {
            return (sv.name.empty() ? "service " + std::to_string(sv.id) : sv.name) + " (" + sv.typeName() + ")";
        };
        auto labelOf = [&](const TsService& sv) {
            const std::string n = face(sv);
            int same = 0;
            for (const auto& o : a.ts.services) if (face(o) == n) same++;
            return same > 1 ? n + "  #" + std::to_string(sv.id) : n;
        };
        std::string cur = "Whole multiplex";
        for (auto& sv : a.ts.services) if (sv.id == a.selService) cur = labelOf(sv);
        ImGui::SetNextItemWidth(std::min(320 * gUi, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("service").x - ImGui::GetStyle().ItemInnerSpacing.x));
        if (ImGui::BeginCombo("service", cur.c_str())) {
            if (ImGui::Selectable("Whole multiplex", a.selService < 0)) { a.selService = -1; ch = true; }
            for (auto& sv : a.ts.services) {
                const std::string n = labelOf(sv) + "##svc" + std::to_string(sv.id);
                if (ImGui::Selectable(n.c_str(), sv.id == a.selService)) { a.selService = sv.id; ch = true; }
            }
            ImGui::EndCombo();
        }
    }
    // the rows of this tab wrap in a narrow window (flowNext() between their parts)
    if (ImGui::Checkbox("remove null packets", &a.out.dropNull)) ch = true;
    flowNext(); ImGui::TextDisabled("(a single service is always rewritten with its own PAT)"); flowEnd();
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "File (.ts)");
    ImGui::SetNextItemWidth(std::min(520 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputText("##fp", a.filePath, sizeof a.filePath);
    flowNext();
    if (ImGui::Button("Choose...")) { auto p = saveFileDialog("recording.ts"); if (!p.empty()) snprintf(a.filePath, sizeof a.filePath, "%s", p.c_str()); }
    flowNext();
    if (ImGui::Checkbox("record", &a.out.file)) ch = true;
    flowEnd();
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "UDP");
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::InputText("address", a.udpHost, sizeof a.udpHost)) {}
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("port", &a.out.port, 0, 0);
    flowNext(); ImGui::SetNextItemWidth(70 * gUi);
    ImGui::InputInt("TTL", &a.out.ttl, 0, 0);
    flowNext();
    ImGui::Checkbox("RTP", &a.out.rtp);
    flowNext();
    if (ImGui::Checkbox("stream", &a.out.udp)) ch = true;
    flowEnd();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Unicast or multicast (e.g. 239.1.1.1). Datagrams carry 7 packets and are paced evenly. Play with: ffplay udp://@:%d", a.out.port); ImGui::PopTextWrapPos(); }
    if (ch || ImGui::IsItemDeactivatedAfterEdit()) applyOutputs(a);
    if (ImGui::Button("Apply address / port / TTL")) applyOutputs(a);
    ImGui::Spacing();
    OutputStats os = a.engine.outputStats();
    ImGui::PushFont(a.mono, 0);
    if (os.fileOpen) { ImGui::PushTextWrapPos(0); ImGui::Text("recording: %llu packets, %.1f MB", (unsigned long long)os.filePackets, os.fileBytes / 1e6); ImGui::PopTextWrapPos(); }
    else ImGui::TextDisabled("recording: off");
    if (os.udpOpen) { ImGui::PushTextWrapPos(0); ImGui::Text("streaming: %llu datagrams, queue %.0f ms, dropped %llu, send errors %llu, catch-ups %llu", (unsigned long long)os.udpDatagrams, os.udpQueueMs, (unsigned long long)os.udpDropped, (unsigned long long)os.udpSendErrors, (unsigned long long)os.udpCatchUps); ImGui::PopTextWrapPos(); }
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
        flowNext(); ImGui::Checkbox("share on the network", &a.netLan);
        flowNext(); ImGui::SetNextItemWidth(130 * gUi); ImGui::InputText("key (optional)", a.netKey, sizeof a.netKey);
        ImGui::EndDisabled();
        flowNext();
        bool on = ns.running;
        if (ImGui::Checkbox("serve", &on)) apply = true;
        flowEnd();
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
            for (const auto& ad : a.net.addresses()) { ImGui::PushTextWrapPos(0); ImGui::Text("http://%s:%d/lineup.m3u%s", ad.c_str(), ns2.port, k.c_str()); ImGui::PopTextWrapPos(); }
            { ImGui::PushTextWrapPos(0); ImGui::Text("viewers: %d   sent: %.1f MB", ns2.clients, ns2.bytesSent / 1e6); ImGui::PopTextWrapPos(); }
            ImGui::PopFont();
        }
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Open the playlist in VLC or any IPTV app; /guide.xml has the programme guide. Plex and Jellyfin: add an HDHomeRun at that address."); ImGui::PopTextWrapPos(); }
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Without a key, anyone on your network can watch while \"share on the network\" is on."); ImGui::PopTextWrapPos(); }
    }
    if (airplay::available()) {
        ImGui::Spacing();
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "Cast to a TV (AirPlay)"); ImGui::PopTextWrapPos(); }
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
        else if (st == airplay::State::Choosing) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("choose a device in the list..."); ImGui::PopTextWrapPos(); }
        else if (st == airplay::State::Failed) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.3f, 1), "%s", airplay::message().c_str());
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Starting takes several seconds. The picture is H.264 or HEVC as broadcast; the sound is converted to AAC. OnAir plays on as usual: mute it here if you do not want it twice."); ImGui::PopTextWrapPos(); }
    }
}

