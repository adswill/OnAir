// receiver analysis tabs: sync, history, frame map, signalling, constellations, channel, impulse, SNR, FEC, transport stream, log
#include "app.h"

void syncTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T synchronisation");
        if (rx.state == 0) { ImGui::TextDisabled("looking for a cyclic prefix: the FFT size (2K/8K) and guard interval are found from the correlation of each symbol's guard with its end"); return; }
        if (ImGui::BeginTable("synct", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 220);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("FFT size / guard interval", "%d / %s (%d samples)", rx.fftN, dvbt::guardName(rx.giIdx), rx.guard);
            row("Carrier frequency offset", "%+.1f Hz", rx.cfoHz);
            row("Symbols processed", "%llu", (unsigned long long)rx.symbols);
            row("Frame length", "%.2f ms (68 symbols)", rx.frameMs);
            row("TPS frame sync", "%s", rx.dvbt.tpsOk ? "locked" : "searching (sync word + BCH over 68 symbols)");
            row("Noise / SNR estimate", "%.1f dB", rx.dataSnrDb);
            ImGui::EndTable();
        }
        return;
    }
    const RxTelemetry& rx = a.rx;
    float h = ImGui::GetContentRegionAvail().y;
    ImGui::TextDisabled("P1 detector - C-A-B correlation (peak > 0.30 triggers an S1/S2 decode)");
    if (plt::BeginPlot("##p1", ImVec2(-1, h * 0.33f), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("time (ms)", "correlation");
        plt::SetupAxisLimits(plt::Y1, 0, 1.05, plt::Cond_Always);
        if (!rx.p1Trace.empty() && rx.nativeRate > 0) {
            std::vector<float> xv(rx.p1Trace.size());
            double dt = kTraceDecim / rx.nativeRate * 1e3;
            for (size_t i = 0; i < xv.size(); i++) xv[i] = (float)((i - (double)xv.size()) * dt);
            plt::SetupAxisLimits(plt::X1, xv.front(), 0, plt::Cond_Always);
            plt::PlotLine("m", xv.data(), rx.p1Trace.data(), (int)xv.size());
            float tx[2] = {xv.front(), 0}, ty[2] = {0.30f, 0.30f};
            plt::Spec ts; ts.LineColor = ImVec4(0.9f, 0.7f, 0.2f, 0.7f);
            plt::PlotLine("thr", tx, ty, 2, ts);
        }
        plt::EndPlot();
    }
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("guard-interval score (mean cyclic-prefix correlation over 3 symbols)");
    if (plt::BeginPlot("##gi", ImVec2(w * 0.38f, -1), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, "score", plt::AxisFlags_NoGridLines, 0);
        plt::SetupAxisLimits(plt::Y1, 0, 1.05, plt::Cond_Always);
        plt::SetupAxisLimits(plt::X1, -0.6, kNumGi - 0.4, plt::Cond_Always);
        double pos[kNumGi]; const char* lab[kNumGi];
        for (int i = 0; i < kNumGi; i++) { pos[i] = i; lab[i] = guardName(i); }
        plt::SetupAxisTicks(plt::X1, pos, kNumGi, lab);
        float sc[kNumGi], x[kNumGi];
        for (int i = 0; i < kNumGi; i++) { sc[i] = rx.giScore[i]; x[i] = (float)i; }
        plt::Spec bs; bs.FillColor = ImVec4(0.45f, 0.65f, 1.0f, 0.85f);
        plt::PlotBars("gi", x, sc, kNumGi, 0.7, bs);
        plt::EndPlot();
    }
    ImGui::SameLine();
    ImGui::BeginChild("hist", ImVec2(0, -1));
    float hh = ImGui::GetContentRegionAvail().y / 3 - 4;
    historyPlot("##hc", "CFO (Hz)", a.hCfo, ImVec2(-1, hh));
    historyPlot("##hs", "CP-SNR (dB)", a.hSnr, ImVec2(-1, hh));
    historyPlot("##ht", "timing (samples)", a.hTiming, ImVec2(-1, hh));
    ImGui::EndChild();
}

void historyTab(App& a) {
    if (a.hist.empty()) { ImGui::TextDisabled("history fills while the receiver runs (4 samples per second, last 15 minutes)"); return; }
    const double nowT = glfwGetTime();
    {
        static const int wins[] = {30, 60, 300, 900};
        static const char* names[] = {"30 s", "1 min", "5 min", "15 min"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("window");
        for (int i = 0; i < 4; i++) { ImGui::SameLine(0, 6 * gUi); if (pillButton(names[i], a.histWindow == wins[i])) a.histWindow = wins[i]; }
        ImGui::SameLine(0, 14 * gUi);
        ImGui::TextDisabled("gaps in a line mean the receiver was not locked; CFO and SRO are on Receiver > Sync");
    }
    std::vector<float> xs(a.hist.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.hist[i].t - nowT);
    auto series = [&](const char* id, const char* ylab, std::initializer_list<std::pair<const char*, float App::HistSample::*>> ser, double ymin, double ymax, bool fixed, bool last) {
        if (!plt::BeginPlot(id, ImVec2(0, 0), plt::Flags_NoTitle | plt::Flags_NoLegend)) return;
        plt::SetupAxes(last ? "seconds ago" : nullptr, nullptr, last ? 0 : plt::AxisFlags_NoTickLabels, fixed ? 0 : plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, -a.histWindow, 0, plt::Cond_Always);
        if (fixed) plt::SetupAxisLimits(plt::Y1, ymin, ymax, plt::Cond_Once);
        int k = 0;
        static const ImVec4 cols[3] = {ImVec4(0.45f, 0.75f, 1, 1), ImVec4(0.95f, 0.7f, 0.2f, 1), ImVec4(0.4f, 0.85f, 0.5f, 1)};
        for (auto& s : ser) {
            std::vector<float> ys(a.hist.size());
            for (size_t i = 0; i < ys.size(); i++) ys[i] = a.hist[i].*(s.second);
            plt::Spec sp; sp.LineColor = cols[k++ % 3]; sp.LineWeight = 1.6f;
            plt::PlotLine(s.first, xs.data(), ys.data(), (int)xs.size(), sp);
        }
        {   // caption inside the plot instead of a rotated axis label (the plots are short)
            const plt::Rect lim = plt::GetPlotLimits();
            const float tw = ImGui::CalcTextSize(ylab).x;
            plt::PlotText(ylab, lim.X.Min, lim.Y.Max, ImVec2(tw * 0.5f + 8, 10));
        }
        plt::EndPlot();
    };
    if (plt::BeginSubplots("##hist", 4, 1, ImVec2(-1, -1), plt::Subplot_LinkAllX | plt::Subplot_NoTitle | plt::Subplot_NoLegend | plt::Subplot_NoMenus)) {
        series("quality (%)##h1", "quality %", {{"quality", &App::HistSample::quality}}, 0, 100, true, false);
        series("SNR / MER (dB)##h2", "SNR / MER dB", {{"data SNR", &App::HistSample::snr}, {"MER", &App::HistSample::mer}}, 0, 0, false, false);
        series("FEC block loss (%)##h3", "lost blocks %", {{"lost blocks", &App::HistSample::loss}}, 0, 100, true, false);
        series("ADC level (dBFS)##h4", "ADC dBFS", {{"rms", &App::HistSample::level}}, -60, 0, true, true);
        plt::EndSubplots();
    }
}

void frameMapTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextDisabled("A DVB-T frame has 68 OFDM symbols; four frames form a superframe. Forward error correction here is a continuous stream (Viterbi + Reed-Solomon), so there is no block map as in DVB-T2.");
        if (rx.dvbt.tpsOk) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 o = ImGui::GetCursorScreenPos();
            const float W = ImGui::GetContentRegionAvail().x - 10, H = 22;
            for (int f = 0; f < 4; f++) {
                const float x0 = o.x + f * W / 4;
                dl->AddRectFilled(ImVec2(x0, o.y + f * 0), ImVec2(x0 + W / 4 - 3, o.y + H), f == rx.dvbt.frameIdx ? IM_COL32(40, 120, 70, 255) : IM_COL32(48, 50, 52, 255), 3.f);
                char t[24]; snprintf(t, sizeof t, "frame %d", f + 1);
                dl->AddText(ImVec2(x0 + 8, o.y + 4), IM_COL32(230, 235, 240, 255), t);
                if (f == rx.dvbt.frameIdx) dl->AddRectFilled(ImVec2(x0, o.y + H - 4), ImVec2(x0 + (W / 4 - 3) * (rx.dvbt.symbolIdx + 1) / 68.f, o.y + H), IM_COL32(110, 220, 140, 255), 3.f);
            }
            ImGui::Dummy(ImVec2(W, H + 8));
        }
        return;
    }
    const RxTelemetry& rx = a.rx;
    if (!rx.l1preOk) { ImGui::TextDisabled("the frame structure appears once L1-pre has been decoded"); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const L1Pre& p = rx.l1pre;
    static const int nP2tab[6] = {8, 2, 4, 16, 1, 1};
    const int fftCode = p.s2 >> 1;
    const int nP2 = fftCode >= 0 && fftCode < 6 ? nP2tab[fftCode] : 1;
    const int nData = p.numDataSyms;
    ImGui::TextDisabled("T2 frame: P1 + %d P2 symbol%s + %d data symbols (%s, GI %s)  -  %.1f ms", nP2, nP2 == 1 ? "" : "s", nData, fftModeFromSize(rx.fftN) ? fftModeFromSize(rx.fftN)->name : "?", guardName(rx.giIdx), rx.frameMs);
    ImVec2 o = ImGui::GetCursorScreenPos();
    const float W = ImGui::GetContentRegionAvail().x - 10, H = 34;
    const double symUnits = 1.0;             // widths in OFDM symbols; P1 is about half a long symbol
    const double p1u = 0.5, total = p1u + nP2 + nData;
    float x = o.x;
    auto seg = [&](double units, ImU32 col, const char* label) {
        const float w = (float)(units / total * W);
        dl->AddRectFilled(ImVec2(x, o.y), ImVec2(x + w - 1, o.y + H), col, 0.f);
        if (w > 28) { ImVec2 ts = ImGui::CalcTextSize(label); dl->AddText(ImVec2(x + (w - ts.x) / 2, o.y + (H - ts.y) / 2), IM_COL32(15, 18, 22, 255), label); }
        x += w;
    };
    (void)symUnits;
    seg(p1u, IM_COL32(240, 180, 60, 255), "P1");
    seg(nP2, IM_COL32(230, 120, 190, 255), "P2");
    seg(nData, IM_COL32(90, 200, 120, 255), "data symbols (PLP cells, pilots, edge pilots)");
    ImGui::Dummy(ImVec2(W, H + 6));
    if (rx.plpValid) ImGui::TextDisabled("PLP %d: %d FEC blocks per frame (%s, rate index %d%s), time interleaver %d", rx.plpId, rx.plpBlocks, rx.plpFec.mod == 0 ? "QPSK" : rx.plpFec.mod == 1 ? "16-QAM" : rx.plpFec.mod == 2 ? "64-QAM" : "256-QAM", rx.plpFec.rate, rx.plpFec.rotation ? ", rotated" : "", p.numDataSyms > 0 ? 0 : 0);
    ImGui::Separator();
    ImGui::TextDisabled("FEC block map: one row per decoded frame (newest at the bottom), one cell per FEC block. Green = decoded, red = failed.");
    const auto& bm = rx.blockMap;
    if (bm.empty()) { ImGui::TextDisabled("no decoded frames yet"); return; }
    size_t nb = 0;
    for (auto& f : bm) nb = std::max(nb, f.size());
    ImVec2 g = ImGui::GetCursorScreenPos();
    const float availW = ImGui::GetContentRegionAvail().x - 80, availH = ImGui::GetContentRegionAvail().y - 4;
    const float cw = nb ? std::max(1.f, availW / (float)nb) : 1.f;
    const float ch = std::max(2.f, std::min(10.f, availH / (float)bm.size()));
    for (size_t r = 0; r < bm.size(); r++) {
        const float y = g.y + r * ch;
        int ok = 0;
        for (size_t b = 0; b < bm[r].size(); b++) {
            ok += bm[r][b];
            dl->AddRectFilled(ImVec2(g.x + b * cw, y), ImVec2(g.x + (b + 1) * cw, y + ch - 1), bm[r][b] ? IM_COL32(60, 170, 90, 255) : IM_COL32(220, 60, 55, 255));
        }
        const float pct = bm[r].empty() ? 0.f : 100.f * ok / (float)bm[r].size();
        char t[16]; snprintf(t, sizeof t, "%.0f%%", pct);
        if (ch >= 8) dl->AddText(ImVec2(g.x + nb * cw + 6, y - 2), pct < 99.5f ? IM_COL32(240, 140, 120, 255) : IM_COL32(140, 150, 160, 255), t);
    }
    ImGui::Dummy(ImVec2(availW + 70, ch * bm.size()));
}

void signallingTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        if (!rx.dvbt.tpsOk) { ImGui::TextDisabled("TPS (transmission parameter signalling) has not been decoded yet"); return; }
        static const char* modes[] = {"2K", "8K"}; static const char* hier[] = {"non-hierarchical", "hierarchical, alpha = 1", "hierarchical, alpha = 2", "hierarchical, alpha = 4"};
        dvbt::Params q; q.mode = rx.dvbt.mode; q.guard = rx.dvbt.guard; q.mod = rx.dvbt.mod; q.hier = rx.dvbt.hier; q.crHp = rx.dvbt.crHp; q.crLp = rx.dvbt.crLp;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T transmission parameters (TPS, EN 300 744)");
        if (ImGui::BeginTable("tps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 200);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("FFT mode", "%s (%d carriers, %d data carriers per symbol)", modes[rx.dvbt.mode & 1], dvbt::carriersK(rx.dvbt.mode), dvbt::dataCarriers(rx.dvbt.mode));
            row("Guard interval", "%s of the useful symbol (%d samples)", dvbt::guardName(rx.dvbt.guard), dvbt::guardSamples(rx.dvbt.mode, rx.dvbt.guard));
            row("Constellation", "%s", dvbt::modName(rx.dvbt.mod));
            row("Hierarchy", "%s", hier[rx.dvbt.hier & 3]);
            row("Code rate (high priority)", "%s", dvbt::rateName(rx.dvbt.crHp));
            row("Code rate (low priority)", "%s", dvbt::rateName(rx.dvbt.crLp));
            row("Cell identifier", "%d", rx.dvbt.cellId);
            row("Frame in superframe", "%d of 4   (symbol %d of 68)", rx.dvbt.frameIdx + 1, rx.dvbt.symbolIdx + 1);
            row("Net bit rate", "%.2f Mbit/s", dvbt::netBitrate(q, rx.nativeRate) / 1e6);
            row("TPS block age", "%.2f s", rx.dvbt.secSinceTps);
            ImGui::EndTable();
        }
        if (rx.dvbt.hier) ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "! hierarchical modulation is announced: only the high-priority stream is decoded correctly by this receiver");
        return;
    }
    const RxTelemetry& rx = a.rx;
    ImGui::PushFont(a.mono, 0);
    auto row = [](const char* k, const char* fmt, ...) {
        char b[160];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::TextDisabled("%-22s", k); ImGui::SameLine(); ImGui::TextUnformatted(b);
    };
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "P1 preamble");
    if (rx.p1.valid) {
        const FftMode* fm = fftModeFromS2(rx.p1.s2field1);
        row("S1", "%d  %s", rx.p1.s1, s1Name(rx.p1.s1));
        row("S2 field 1", "%d  FFT %s", rx.p1.s2field1, fm ? fm->name : "?");
        row("S2 field 2", "%d  %s", rx.p1.mixed, rx.p1.mixed ? "mixed (T2 + FEF/other frames)" : "all frames same preamble type");
        row("sequence correlation", "%.2f", rx.p1.conf);
        row("C-A-B correlation", "%.2f", rx.p1.metric);
        row("P1 carrier CFO", "%+.1f Hz", rx.p1.cfoHz);
        row("P1 detections", "%llu", (unsigned long long)rx.p1Count);
        row("last P1", "%.2f s ago", rx.secSinceP1);
    } else ImGui::TextDisabled("no P1 decoded yet");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "OFDM / frame (blind, before L1-pre)");
    if (rx.state >= 1) {
        row("FFT size", "%d", rx.fftN);
        row("carriers (normal)", "%d", rx.carriers);
        if (rx.giIdx >= 0) {
            row("guard interval", "%s  (%d samples)  margin %.2fx", guardName(rx.giIdx), rx.guard, rx.giMargin);
            row("symbol duration", "%.1f us", (rx.fftN + rx.guard) / rx.nativeRate * 1e6);
        }
        if (rx.frameMs > 0) {
            row("frame length", "%.2f ms  =  P1 + %d symbols", rx.frameMs, rx.symbolsPerFrame);
            row("sample clock offset", "%+.1f ppm", rx.sroPpm);
        }
        row("CFO (tracked)", "%+.1f Hz", rx.cfoHz);
        row("CP correlation", "%.3f  (~%.1f dB SNR)", rx.cpCorr, rx.cpSnrDb);
        row("symbols processed", "%llu", (unsigned long long)rx.symbols);
    }
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "L1-pre   (decoded %llu, failed %llu, LDPC iterations %d)", (unsigned long long)rx.l1preGood, (unsigned long long)rx.l1preBad, rx.l1Iters);
    if (rx.l1preGood > 0) {
        const L1Pre& p = rx.l1pre;
        static const char* paprN[] = {"off", "ACE", "TR", "ACE + TR"};
        static const char* modN[] = {"BPSK", "QPSK", "16-QAM", "64-QAM"};
        static const char* verN[] = {"1.1.1", "1.2.1", "1.3.1"};
        row("TYPE", "%d  %s", p.type, p.type == 0 ? "TS" : p.type == 1 ? "GSE/GS" : "TS + GS");
        row("carrier mode", "%s", p.bwtExt ? "extended" : "normal");
        row("S1 / S2", "%d / %d (FFT %s%s)", p.s1, p.s2, fftModeFromS2(p.s2 >> 1) ? fftModeFromS2(p.s2 >> 1)->name : "?", (p.s2 & 1) ? ", FEF present" : "");
        row("guard interval", "%s", guardName(p.guardInterval));
        row("PAPR", "%s", p.papr < 4 ? paprN[p.papr] : "reserved");
        row("L1-post", "%s, rate 1/2, %s FEC, %d cells, %d info bits%s", p.l1Mod < 4 ? modN[p.l1Mod] : "?", p.l1Fec == 0 ? "16K" : "64K", p.postSize, p.postInfoSize, p.postScrambled ? ", scrambled" : "");
        row("pilot pattern", "PP%d", p.pilotPattern + 1);
        row("cell / network / system", "%d / 0x%04X / 0x%04X", p.cellId, p.networkId, p.systemId);
        row("T2 frames per super-frame", "%d", p.numFrames);
        row("data symbols per frame", "%d", p.numDataSyms);
        row("T2 version", "%s%s", p.version < 3 ? verN[p.version] : "?", p.lite ? "  (T2-Lite)" : "");
        row("RF channels", "%d (current %d)  regen %d  tx-id %d", p.numRf, p.curRf, p.regen, p.txIdAvail);
    } else ImGui::TextDisabled("not decoded yet");
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "L1-post   (decoded %llu, failed %llu)", (unsigned long long)rx.l1postGood, (unsigned long long)rx.l1postBad);
    if (rx.l1postGood > 0) {
        const L1Post& q = rx.l1post;
        static const char* cod[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "1/3", "2/5"};
        static const char* pmod[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
        static const char* ptype[] = {"common", "data type 1", "data type 2"};
        row("sub-slices / PLPs / aux", "%d / %d / %d", q.subSlices, q.numPlp, q.numAux);
        for (auto& rf : q.rf) row("RF frequency", "%.3f MHz  (index %d)", rf.freq / 1e6, rf.idx);
        row("frame index", "%d   change counter %d", q.frameIdx, q.changeCounter);
        for (size_t i = 0; i < q.plps.size(); i++) {
            const L1PlpConf& c = q.plps[i];
            char hdr[32]; snprintf(hdr, sizeof hdr, "PLP %d", c.id);
            row(hdr, "%s, %s, rate %s, %s, %s FEC, rotation %s", c.type < 3 ? ptype[c.type] : "?", c.mod < 4 ? pmod[c.mod] : "?", c.cod < 8 ? cod[c.cod] : "?",
                c.payloadType == 3 ? "TS" : c.payloadType == 0 ? "GFPS" : c.payloadType == 1 ? "GCS" : "GSE", c.fecType == 1 ? "64K" : "16K", c.rotation ? "on" : "off");
            row("", "max %d FEC blocks, TI %d (%s), group %d, interval %d, mode %d", c.numBlocksMax, c.timeIlLength, c.timeIlType ? "multi-frame" : "one frame", c.groupId, c.frameInterval, c.plpMode);
            if (i < q.dyn.size()) row("", "start %d, %d blocks in this frame", q.dyn[i].start, q.dyn[i].numBlocks);
        }
    } else ImGui::TextDisabled("%s", rx.l1preGood ? "not decoded (L1-pre must be valid first)" : "not decoded yet");
    ImGui::PopFont();
}

// ATSC: histogram of the equalised symbols (eight peaks), equaliser response, strip chart of the levels and a text summary
void atscPanels(App& a) {
    const AtscTelemetry& at = a.rx.atsc;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float side = std::max(90.f, std::min(availH - 26.f, availW / 4.f - 16.f));
    const float gap = std::max(6.f, (availW - 4 * side) / 5.f);
    const ImVec2 sz(side, side);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Equalised levels (%zu symbols)", at.levels.size());
    if (plt::BeginPlot("##ah", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoMouseText)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::X1, -9, 9, plt::Cond_Always);
        constexpr int NB = 72;
        float cnt[NB] = {}, xs[NB];
        for (int i = 0; i < NB; i++) xs[i] = -9.f + (i + 0.5f) * 18.f / NB;
        float mx = 1;
        for (float v : at.levels) { const int k = (int)((v + 9.f) / 18.f * NB); if (k >= 0 && k < NB) mx = std::max(mx, ++cnt[k]); }
        plt::SetupAxisLimits(plt::Y1, 0, mx * 1.15, plt::Cond_Always);
        plt::Spec sp; sp.FillColor = pal::accent(0.85f); sp.LineColor = pal::accent();
        plt::PlotBars("levels", xs, cnt, NB, 18.0 / NB * 0.9, sp);
        const double lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        plt::Spec gs; gs.LineColor = ImVec4(1, 1, 1, 0.35f);
        plt::PlotInfLines("ideal", lv, 8, gs);
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Equaliser response (%zu taps)", at.eqTaps.size());
    if (plt::BeginPlot("##ae", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoMouseText)) {
        plt::SetupAxes("symbols", nullptr, 0, plt::AxisFlags_AutoFit);
        if (!at.eqTaps.empty()) {
            std::vector<float> xs(at.eqTaps.size());
            for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(at.eqCursor - (int)i);   // symbols relative to the main tap: positive = echoes of later symbols
            plt::SetupAxisLimits(plt::X1, -(double)(at.eqTaps.size() - 1 - at.eqCursor) - 1, (double)at.eqCursor + 1, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent();
            plt::PlotLine("taps", xs.data(), at.eqTaps.data(), (int)xs.size(), sp);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("Symbol levels in sequence");
    if (plt::BeginPlot("##as", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_NoMouseText)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
        plt::SetupAxisLimits(plt::Y1, -9, 9, plt::Cond_Always);
        plt::SetupAxisLimits(plt::X1, 0, std::max<size_t>(2, at.levels.size()), plt::Cond_Always);
        if (!at.levels.empty()) {
            plt::Spec sp; sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.3f;
            sp.MarkerFillColor = sp.MarkerLineColor = sp.LineColor = pal::accent(0.6f);
            plt::PlotScatter("l", at.levels.data(), (int)at.levels.size(), 1.0, 0.0, sp);
        }
        const double lv[8] = {-7, -5, -3, -1, 1, 3, 5, 7};
        plt::Spec gs; gs.LineColor = ImVec4(1, 1, 1, 0.25f); gs.Flags = plt::InfLines_Horizontal;
        plt::PlotInfLines("ideal", lv, 8, gs);
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    ImGui::TextDisabled("ATSC receiver");
    ImGui::BeginChild("atxt", sz);
    ImGui::PushFont(a.mono, 0);
    ImGui::Text("pilot       %s", at.pilot ? "locked" : "searching");
    ImGui::Text("segment     %s  sync %.2f", at.segSync ? "locked" : "searching", at.syncQuality);
    ImGui::Text("field       %s  (%d)", at.fieldSync ? "locked" : "searching", at.fieldParity);
    ImGui::Text("CFO %+.0f Hz  SRO %+.1f ppm", at.cfoHz, at.sroPpm);
    ImGui::Text("SNR %.1f dB  data %.1f dB", at.snrDb, at.dataSnrDb);
    ImGui::Text("fields %llu", (unsigned long long)at.fields);
    ImGui::Text("RS ok %llu fixed %llu", (unsigned long long)at.rsClean, (unsigned long long)at.rsCorrected);
    ImGui::Text("RS failed %llu (%.1f%%)", (unsigned long long)at.rsFailed, at.segErrorRate * 100);
    ImGui::Text("lock losses %llu", (unsigned long long)at.lockLosses);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndGroup();
}

void constellationsTab(App& a) {
    if (a.dabMode) { dabPanels(a); return; }
    if (a.rx.standard == 2) { atscPanels(a); return; }
    const RxTelemetry& rx = a.rx;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float side = std::max(90.f, std::min(availH - 26.f - ImGui::GetFrameHeight(), availW / 4.f - 16.f));
    const float gap = std::max(6.f, (availW - 4 * side) / 5.f);
    const ImVec2 sz(side, side);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    char cap[96];
    auto caption = [&](const char* fmt, auto... args) { snprintf(cap, sizeof cap, fmt, args...); ImGui::TextDisabled("%s", cap); };
    ImGui::BeginGroup();
    if (rx.standard == 1) caption("TPS carriers, DBPSK (%zu cells)", rx.p1Const.size()); else caption("P1 carriers (%zu cells)", rx.p1Const.size());
    scatter("##c1", rx.p1Const, sz, 2.5, pal::accent(0.9f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (rx.standard == 1) caption("Pilots, equalised (%zu cells)", rx.eqCells.size()); else caption("P2 cells, equalised (%zu cells)", rx.eqCells.size());
    scatter("##ceq", rx.eqCells, sz, 2.0, pal::accent(0.8f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (!rx.plpConst.empty()) caption("PLP %d, decoded cells  MER %.1f dB", rx.plpId, rx.plpMerDb);
    else if (rx.standard == 1) caption("Data cells, equalised (%zu cells)", rx.eqData.size());
    else caption("Data cells, equalised (frame %llu)", (unsigned long long)rx.dataFrames);
    if (!rx.plpConst.empty()) {
        // cells of correctly decoded FEC blocks; points beyond the decision distance from the transmitted point are red
        std::vector<cf32> good, bad;
        for (size_t i = 0; i < rx.plpConst.size(); i++) (rx.plpConstErr.size() > i && rx.plpConstErr[i] ? bad : good).push_back(rx.plpConst[i]);
        const bool haveStats = a.cst.mod == rx.plpFec.mod && !a.cst.pts.empty();
        if (a.constView == 1 && haveStats) constDensityPlot(a, sz);
        else if (a.constView == 2 && haveStats) constClusterPlot(a, sz);
        else if (plt::BeginPlot("##c2", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal)) {
            plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
            plt::SetupAxisLimits(plt::X1, -1.4, 1.4, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, -1.4, 1.4, plt::Cond_Always);
            auto draw = [&](const char* id, const std::vector<cf32>& v, ImVec4 col) {
                if (v.empty()) return;
                plt::Spec sp; sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.3f; sp.Stride = sizeof(cf32);
                sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
                const float* d = reinterpret_cast<const float*>(v.data());
                plt::PlotScatter(id, d, d + 1, (int)v.size(), sp);
            };
            draw("ok", good, pal::accent(0.60f));
            draw("err", bad, ImVec4(0.88f, 0.52f, 0.40f, 0.75f));
            {   // the transmitted (unrotated) constellation points, drawn on top like the reference decoders do
                const int M = 2 * (rx.plpFec.mod + 1);
                std::vector<cf32> grid;
                for (unsigned l = 0; l < (1u << M); l++) grid.push_back(qamPoint(rx.plpFec.mod, false, l));
                plt::Spec gs; gs.Marker = plt::Marker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(cf32);
                gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.85f);
                const float* g = reinterpret_cast<const float*>(grid.data());
                plt::PlotScatter("ideal", g, g + 1, (int)grid.size(), gs);
            }
            plt::EndPlot();
        }
        ImGui::PushID("cview");
        if (pillButton("cells", a.constView == 0, 8)) a.constView = 0;
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton("density", a.constView == 1, 8)) a.constView = 1;
        ImGui::SameLine(0, 4 * gUi);
        if (pillButton("clusters", a.constView == 2, 8)) a.constView = 2;
        ImGui::PopID();
        if (a.constView == 2 && ImGui::IsItemHovered()) ImGui::SetTooltip("One ring per transmitted point: centre = average received position, radius = 1 sigma of the error.\nColour is relative to the average ring: green = tighter, red = looser.\nAt this MER the rings overlap their neighbours; the LDPC code corrects the resulting errors.");
    } else if (rx.standard == 1 && rx.dvbt.tpsOk) {
        if (plt::BeginPlot("##c2", sz, plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_Equal)) {
            plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, plt::AxisFlags_NoTickLabels);
            plt::SetupAxisLimits(plt::X1, -1.5, 1.5, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, -1.5, 1.5, plt::Cond_Always);
            if (!rx.eqData.empty()) {
                plt::Spec sp; sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.3f; sp.Stride = sizeof(cf32);
                const ImVec4 col(0.35f, 0.62f, 1.0f, 0.55f);
                sp.MarkerFillColor = col; sp.MarkerLineColor = col; sp.LineColor = col;
                const float* d = reinterpret_cast<const float*>(rx.eqData.data());
                plt::PlotScatter("data", d, d + 1, (int)rx.eqData.size(), sp);
            }
            std::vector<dvbt::cf32> pts;
            dvbt::constellation(rx.dvbt.mod, 0, pts);
            plt::Spec gs; gs.Marker = plt::Marker_Cross; gs.MarkerSize = 4.f; gs.Stride = sizeof(dvbt::cf32);
            gs.MarkerFillColor = gs.MarkerLineColor = gs.LineColor = ImVec4(1, 1, 1, 0.85f);
            const float* g = reinterpret_cast<const float*>(pts.data());
            plt::PlotScatter("ideal", g, g + 1, (int)pts.size(), gs);
            plt::EndPlot();
        }
    } else
        scatter("##c2", rx.eqData, sz, 1.8, pal::accent(0.6f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    caption("OFDM cells, raw (%zu cells)", rx.rawCells.size());
    scatter("##c3", rx.rawCells, sz, 3.0, pal::accent(0.7f));
    ImGui::EndGroup();
}

void channelTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (!rx.chValid) { ImGui::TextDisabled("no channel estimate yet (needs a decoded P1 and a P2 symbol)"); return; }
    {
        const MultipathReport& mr = a.mpd.report();
        ImGui::TextColored(mr.level == MultipathLevel::None ? ImVec4(0.4f, 0.85f, 0.5f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1) : ImVec4(0.95f, 0.5f, 0.25f, 1),
                           "Multipath analysis: %s", mr.headline.c_str());
        for (auto& r : mr.reasons) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("- %s", r.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::Separator();
    }
    const float h = ImGui::GetContentRegionAvail().y - 56;   // room for the two captions
    double fnMhz = rx.nativeRate / 1e6, binMhz = fnMhz / rx.fftN;
    std::vector<float> xs(rx.chMagDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.chDecim) - (rx.chCarriers - 1) / 2.0) * binMhz);
    ImGui::TextDisabled("|H(f)| from P2 pilots, dB (%d carriers%s)", rx.chCarriers, rx.extCarriers ? ", extended" : "");
    if (plt::BeginPlot("##chm", ImVec2(-1, std::max(60.f, h * 0.5f)), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "dB", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = pal::accent();
        plt::PlotLine("mag", xs.data(), rx.chMagDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
    ImGui::TextDisabled("phase of H(f), rad (slope = timing, curvature = echoes)");
    if (plt::BeginPlot("##chp", ImVec2(-1, -1), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "rad");
        plt::SetupAxisFormat(plt::X1, "%.2f");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -3.3, 3.3, plt::Cond_Always);
        plt::Spec sp; sp.LineColor = ImVec4(0.95f, 0.7f, 0.2f, 1); sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.2f;
        sp.LineWeight = 0.0f;
        plt::PlotScatter("ph", xs.data(), rx.chPhase.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
}

void impulseTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (!rx.chValid || rx.irDb.empty()) { ImGui::TextDisabled("no channel estimate yet"); return; }
    std::vector<float> xs(rx.irDb.size());
    double usPerSample = 1e6 / rx.nativeRate;
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)((rx.irTauMin + (double)i) * usPerSample);
    ImGui::TextDisabled("power-delay profile (dB rel. strongest path). Shaded: guard interval (%.1f us)", rx.guard * usPerSample);
    if (plt::BeginPlot("##ir", ImVec2(-1, -1), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("delay (us)", "dB");
        plt::SetupAxisLimits(plt::Y1, -80, 5, plt::Cond_Once);
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Once);
        double gx[2] = {0, rx.guard * usPerSample}, gy[2] = {5, 5};
        plt::Spec gs; gs.FillColor = ImVec4(0.15f, 0.55f, 0.20f, 0.18f); gs.LineColor = ImVec4(0, 0, 0, 0);
        plt::PlotShaded("gi", gx, gy, 2, -80.0, gs);
        plt::Spec sp; sp.LineColor = ImVec4(0.45f, 0.75f, 1, 1);
        plt::PlotLine("pdp", xs.data(), rx.irDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
}

void snrTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (rx.snrDb.empty()) { ImGui::TextDisabled("per-carrier SNR appears once a full frame has been received"); if (rx.chValid) ImGui::Text("SNR estimate: %.1f dB", rx.p2SnrDb); return; }
    double binMhz = rx.nativeRate / 1e6 / rx.fftN;
    std::vector<float> xs(rx.snrDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.snrStep) - (rx.chCarriers - 1) / 2.0) * binMhz);
    ImGui::Text("pilot-derived SNR across the channel (scattered pilots, smoothed over %d points); mean %.1f dB", 21, rx.p2SnrDb);
    if (plt::BeginPlot("##snrc", ImVec2(-1, -1), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "SNR (dB)", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = ImVec4(0.35f, 0.85f, 0.45f, 1);
        plt::PlotLine("snr", xs.data(), rx.snrDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
}

void fecTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "DVB-T channel decoder: de-interleaving, Viterbi (inner code), outer de-interleaver, Reed-Solomon (204,188)");
        if (!rx.dvbt.tpsOk) { ImGui::TextDisabled("waiting for TPS"); return; }
        const double tot = (double)(rx.dvbt.rsClean + rx.dvbt.rsCorrected + rx.dvbt.rsFailed);
        if (ImGui::BeginTable("fect", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230);
            auto row = [&](const char* k, const char* fmt, auto... args) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn(); char b[160]; snprintf(b, sizeof b, fmt, args...); ImGui::TextUnformatted(b); };
            row("Transport stream sync", "%s", rx.dvbt.fecSync ? "locked" : "searching (puncturing phase, byte phase)");
            row("Puncturing phase", "%d", rx.dvbt.punctPhase);
            row("Viterbi path-metric margin", "%.2f   (1.0 = noiseless)", rx.dvbt.viterbiMargin);
            row("Reed-Solomon blocks", "%llu", (unsigned long long)rx.dvbt.packets);
            row("   decoded without errors", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsClean, tot ? 100.0 * rx.dvbt.rsClean / tot : 0.0);
            row("   corrected", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsCorrected, tot ? 100.0 * rx.dvbt.rsCorrected / tot : 0.0);
            row("   uncorrectable", "%llu  (%.2f%%)", (unsigned long long)rx.dvbt.rsFailed, tot ? 100.0 * rx.dvbt.rsFailed / tot : 0.0);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Uncorrectable packets are passed on with the transport_error_indicator set, so the player skips them.");
        return;
    }
    const RxTelemetry& rx = a.rx;
    ImGui::PushFont(a.mono, 0);
    auto row = [](const char* k, const char* fmt, ...) {
        char b[200];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::TextDisabled("%-26s", k); ImGui::SameLine(); ImGui::TextUnformatted(b);
    };
    if (!rx.plpValid) {
        ImGui::TextDisabled("waiting for L1-post (PLP configuration)...");
        if (rx.plpSkipped) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "%d frame(s) skipped: PLP uses inter-frame time interleaving or lies outside the received cells", rx.plpSkipped);
        ImGui::PopFont();
        return;
    }
    static const char* modN[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "PLP %d", rx.plpId);
    row("FEC frame / code rate", "%s, %s", rx.plpFec.shortFrame ? "short (16200)" : "normal (64800)", rateName(rx.plpFec.rate));
    row("constellation", "%s%s", modN[rx.plpFec.mod & 3], rx.plpFec.rotation ? ", rotated + cyclic Q delay" : "");
    row("FEC blocks per frame", "%d", rx.plpBlocks);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "decoder (all frames since start)");
    double tot = (double)(rx.blocksOk + rx.blocksBad);
    row("frames decoded", "%llu  (dropped, decoder busy: %llu)", (unsigned long long)rx.plpFrames, (unsigned long long)rx.plpFramesDropped);
    row("FEC blocks OK / failed", "%llu / %llu  (%.2f%% good)", (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad, tot ? 100.0 * rx.blocksOk / tot : 0.0);
    row("BB headers with valid CRC", "%llu", (unsigned long long)rx.headersOk);
    row("BCH bit errors corrected", "%llu", (unsigned long long)rx.plpBchCorrected);
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.45f, 0.75f, 1, 1), "last frame");
    row("MER", "%.1f dB", rx.plpMerDb);
    row("BER before LDPC", "%.2e", rx.plpPreBer);
    row("LDPC iterations (average)", "%.1f", rx.plpIters);
    row("decode time", "%.0f ms for %.0f ms of signal (%s)", rx.plpDecodeMs, rx.frameMs, rx.plpOnGpu ? "GPU" : "CPU");
    if (rx.plpHeaderUpl) row("BB header", "UPL %d bits, DFL %d bits, SYNCD %d", rx.plpHeaderUpl, rx.plpHeaderDfl, rx.plpHeaderSyncd);
    ImGui::PopFont();
}

void tsTab(App& a) {
    const TsSnapshot& ts = a.ts;
    ImGui::PushFont(a.mono, 0);
    char b[64], b2[64];
    ImGui::Text("network \"%s\"  onid 0x%04X  tsid 0x%04X   %s", ts.networkName.c_str(), ts.onid, ts.tsid, ts.utc.c_str());
    ImGui::Text("multiplex %s (null packets %s)   packets %llu   continuity errors %llu   TEI %llu",
                fmtKbps(b, sizeof b, ts.muxKbps), fmtKbps(b2, sizeof b2, ts.nullKbps), (unsigned long long)ts.totalPackets, (unsigned long long)ts.ccErrors, (unsigned long long)ts.teiPackets);
    ImGui::Text("BB frames %llu (lost %llu)  mode %s  ISSYI %d NPD %d  resyncs %llu  CRC-8 errors %llu", (unsigned long long)a.bb.frames, (unsigned long long)a.bb.framesLost,
                a.bb.hem ? "high-efficiency" : "normal", a.bb.issyi, a.bb.npd, (unsigned long long)a.bb.resyncs, (unsigned long long)a.bb.crcErrors);
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginTable("pids", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("content");
        ImGui::TableSetupColumn("packets", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("rate", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("CC err", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("scr", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableHeadersRow();
        for (auto& p : ts.pids) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", p.pid);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
            ImGui::TableNextColumn(); { char r[32]; ImGui::TextUnformatted(fmtKbps(r, sizeof r, p.kbps)); }
            ImGui::TableNextColumn(); if (p.ccErrors) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1), "%llu", (unsigned long long)p.ccErrors); else ImGui::TextDisabled("0");
            ImGui::TableNextColumn(); if (p.scrambled) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "yes"); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

void logPanel(App& a) {
    ImGui::TextDisabled("Log");
    ImGui::SameLine();
    size_t total = 0;
    auto lines = a.engine.logSnapshot(total);
    ImGui::TextDisabled("%zu lines", total);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) {
        std::string all;
        for (auto& l : lines) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) a.engine.clearLog();
    ImGui::BeginChild("logtext", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(a.mono, 0);
    for (auto& l : lines) ImGui::TextUnformatted(l.c_str());
    ImGui::PopFont();
    if (a.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void historyLogTab(App& a) {
    const float h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("hplots", ImVec2(0, h * 0.70f));
    historyTab(a);
    ImGui::EndChild();
    ImGui::BeginChild("hlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    logPanel(a);
    ImGui::EndChild();
}

