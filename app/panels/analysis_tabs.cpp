// receiver analysis tabs: sync, history, frame map, signalling, constellations, channel, impulse, SNR, FEC, transport stream, log
#include "../app.h"
#include <cmath>
#include <ctime>

void syncTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "DVB-T synchronisation"); ImGui::PopTextWrapPos(); }
        if (rx.state == 0) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("looking for a cyclic prefix: the FFT size (2K/8K) and guard interval are found from the correlation of each symbol's guard with its end"); ImGui::PopTextWrapPos(); } return; }
        if (ImGui::BeginTable("synct", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 220 * gUi);
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
    captionFit(ImGui::GetContentRegionAvail().x, "P1 detector - C-A-B correlation (peak > 0.30 triggers an S1/S2 decode)");
    // never shorter than 5 lines: in a short tab the view scrolls instead (the trace was 22 px tall at 1024 x 600)
    if (plt::BeginPlot("##p1", ImVec2(-1, std::max(h * 0.33f, 5.f * ImGui::GetTextLineHeight())), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
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
    captionFit(ImGui::GetContentRegionAvail().x, "guard-interval score (mean cyclic-prefix correlation over 3 symbols)");
    // the rest of the tab, never less than 7 lines: in a short tab the view scrolls (the bars were 37 px tall at 1024 x 600)
    const float rowH = std::max(ImGui::GetContentRegionAvail().y, 7.f * ImGui::GetTextLineHeight());
    if (plt::BeginPlot("##gi", ImVec2(w * 0.38f, rowH), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, "score", plt::AxisFlags_NoGridLines, 0);
        plt::SetupAxisLimits(plt::Y1, 0, 1.05, plt::Cond_Always);
        plt::SetupAxisLimits(plt::X1, -0.6, kNumGi - 0.4, plt::Cond_Always);
        double pos[kNumGi]; const char* lab[kNumGi];
        for (int i = 0; i < kNumGi; i++) { pos[i] = i; lab[i] = guardName(i); }
        plt::SetupAxisTicks(plt::X1, pos, kNumGi, lab);
        float sc[kNumGi], x[kNumGi];
        for (int i = 0; i < kNumGi; i++) { sc[i] = rx.giScore[i]; x[i] = (float)i; }
        plt::Spec bs; bs.FillColor = pal::dev() ? pal::accent(0.85f) : ImVec4(0.45f, 0.65f, 1.0f, 0.85f);
        plt::PlotBars("gi", x, sc, kNumGi, 0.7, bs);
        plt::EndPlot();
    }
    ImGui::SameLine();
    ImGui::BeginChild("hist", ImVec2(0, rowH));
    float hh = std::max(ImGui::GetContentRegionAvail().y / 3 - 4, 5.f * ImGui::GetTextLineHeight());   // a short tab: the three scroll instead of shrinking
    historyPlot("##hc", "CFO (Hz)", a.hCfo, ImVec2(-1, hh));
    historyPlot("##hs", "CP-SNR (dB)", a.hSnr, ImVec2(-1, hh));
    historyPlot("##ht", "timing (samples)", a.hTiming, ImVec2(-1, hh));
    ImGui::EndChild();
}

void historyTab(App& a) {
    if (a.hist.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("history fills while the receiver runs (4 samples per second, last 15 minutes)"); ImGui::PopTextWrapPos(); } return; }
    const double nowT = glfwGetTime();
    {
        static const int wins[] = {30, 60, 300, 900};
        static const char* names[] = {"30 s", "1 min", "5 min", "15 min"};
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("window");
        for (int i = 0; i < 4; i++) { ImGui::SameLine(0, 6 * gUi); if (pillButton(names[i], a.histWindow == wins[i])) a.histWindow = wins[i]; }
        const char* hint = a.fmMode ? "gaps in a line mean there was no station on the frequency" : a.dabMode ? "gaps in a line mean the receiver was not locked"
                                    : "gaps in a line mean the receiver was not locked; CFO and SRO are on Receiver > Sync";
        sameLineIf(ImGui::CalcTextSize(hint).x, 14 * gUi);   // on a line of its own (wrapped) when the tab is narrow
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", hint); ImGui::PopTextWrapPos();
    }
    std::vector<float> xs(a.hist.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.hist[i].t - nowT);
    auto series = [&](const char* id, const char* ylab, std::initializer_list<std::pair<const char*, float App::HistSample::*>> ser, double ymin, double ymax, bool fixed, bool last) {
        if (!plt::BeginPlot(id, ImVec2(0, 0), plt::Flags_NoTitle | plt::Flags_NoLegend)) return;
        plt::SetupAxes(last ? "seconds ago" : nullptr, nullptr, last ? 0 : plt::AxisFlags_NoTickLabels, fixed ? 0 : plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, -a.histWindow, 0, plt::Cond_Always);
        if (fixed) plt::SetupAxisLimits(plt::Y1, ymin, ymax, plt::Cond_Once);
        int k = 0;
        const ImVec4 cols[3] = {pal::dev() ? pal::accent() : ImVec4(0.45f, 0.75f, 1, 1), ImVec4(0.95f, 0.7f, 0.2f, 1), ImVec4(0.4f, 0.85f, 0.5f, 1)};
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
    // never squashed below about four lines each: in a short tab the plots scroll instead
    const float plotsH = std::max(ImGui::GetContentRegionAvail().y, 4 * 4.5f * ImGui::GetTextLineHeight() + 2.5f * ImGui::GetTextLineHeight());
    if (plt::BeginSubplots("##hist", 4, 1, ImVec2(-1, plotsH), plt::Subplot_LinkAllX | plt::Subplot_NoTitle | plt::Subplot_NoLegend | plt::Subplot_NoMenus)) {
        series("quality (%)##h1", "quality %", {{"quality", &App::HistSample::quality}}, 0, 100, true, false);
        if (a.fmMode) {
            series("audio SNR (dB)##h2", "audio SNR dB", {{"audio SNR", &App::HistSample::snr}}, 0, 0, false, false);
            series("RDS blocks ok (%)##h3", "RDS blocks ok %", {{"RDS blocks ok", &App::HistSample::aux}}, 0, 100, true, false);
        } else {
            series("SNR / MER (dB)##h2", "SNR / MER dB", {{"data SNR", &App::HistSample::snr}, {"MER", &App::HistSample::mer}}, 0, 0, false, false);
            series("FEC block loss (%)##h3", "lost blocks %", {{"lost blocks", &App::HistSample::loss}}, 0, 100, true, false);
        }
        series("ADC level (dBFS)##h4", "ADC dBFS", {{"rms", &App::HistSample::level}}, -60, 0, true, true);
        plt::EndSubplots();
    }
}

void frameMapTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("A DVB-T frame has 68 OFDM symbols; four frames form a superframe. Forward error correction here is a continuous stream (Viterbi + Reed-Solomon), so there is no block map as in DVB-T2."); ImGui::PopTextWrapPos(); }
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
    if (!rx.l1preOk) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("the frame structure appears once L1-pre has been decoded"); ImGui::PopTextWrapPos(); } return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const L1Pre& p = rx.l1pre;
    static const int nP2tab[6] = {8, 2, 4, 16, 1, 1};
    const int fftCode = p.s2 >> 1;
    const int nP2 = fftCode >= 0 && fftCode < 6 ? nP2tab[fftCode] : 1;
    const int nData = p.numDataSyms;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("T2 frame: P1 + %d P2 symbol%s + %d data symbols (%s, GI %s)  -  %.1f ms", nP2, nP2 == 1 ? "" : "s", nData, fftModeFromSize(rx.fftN) ? fftModeFromSize(rx.fftN)->name : "?", guardName(rx.giIdx), rx.frameMs); ImGui::PopTextWrapPos(); }
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
    if (rx.plpValid) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("PLP %d: %d FEC blocks per frame (%s, rate index %d%s), time interleaver %d", rx.plpId, rx.plpBlocks, rx.plpFec.mod == 0 ? "QPSK" : rx.plpFec.mod == 1 ? "16-QAM" : rx.plpFec.mod == 2 ? "64-QAM" : "256-QAM", rx.plpFec.rate, rx.plpFec.rotation ? ", rotated" : "", p.numDataSyms > 0 ? 0 : 0); ImGui::PopTextWrapPos(); }
    ImGui::Separator();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("FEC block map: one row per decoded frame (newest at the bottom), one cell per FEC block. Green = decoded, red = failed."); ImGui::PopTextWrapPos(); }
    const auto& bm = rx.blockMap;
    if (bm.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no decoded frames yet"); ImGui::PopTextWrapPos(); } return; }
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
        if (!rx.dvbt.tpsOk) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("TPS (transmission parameter signalling) has not been decoded yet"); ImGui::PopTextWrapPos(); } return; }
        static const char* modes[] = {"2K", "8K"}; static const char* hier[] = {"non-hierarchical", "hierarchical, alpha = 1", "hierarchical, alpha = 2", "hierarchical, alpha = 4"};
        dvbt::Params q; q.mode = rx.dvbt.mode; q.guard = rx.dvbt.guard; q.mod = rx.dvbt.mod; q.hier = rx.dvbt.hier; q.crHp = rx.dvbt.crHp; q.crLp = rx.dvbt.crLp;
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "DVB-T transmission parameters (TPS, EN 300 744)"); ImGui::PopTextWrapPos(); }
        if (ImGui::BeginTable("tps", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 200 * gUi);
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
        if (rx.dvbt.hier) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.2f, 1), "! hierarchical modulation is announced: only the high-priority stream is decoded correctly by this receiver"); ImGui::PopTextWrapPos(); }
        return;
    }
    const RxTelemetry& rx = a.rx;
    ImGui::PushFont(a.mono, 0);
    auto row = [](const char* k, const char* fmt, ...) {
        char b[160];
        va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
        ImGui::TextDisabled("%-22s", k); ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos();   // a long value wraps under itself
    };
    ImGui::TextColored(pal::heading(), "P1 preamble");
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
    { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "OFDM / frame (blind, before L1-pre)"); ImGui::PopTextWrapPos(); }
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
    { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "L1-pre   (decoded %llu, failed %llu, LDPC iterations %d)", (unsigned long long)rx.l1preGood, (unsigned long long)rx.l1preBad, rx.l1Iters); ImGui::PopTextWrapPos(); }
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
    { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "L1-post   (decoded %llu, failed %llu)", (unsigned long long)rx.l1postGood, (unsigned long long)rx.l1postBad); ImGui::PopTextWrapPos(); }
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
    } else { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", rx.l1preGood ? "not decoded (L1-pre must be valid first)" : "not decoded yet"); ImGui::PopTextWrapPos(); }
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
    captionFit(side, "Equalised levels (%zu symbols)", at.levels.size());
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
    captionFit(side, "Equaliser response (%zu taps)", at.eqTaps.size());
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
    captionFit(side, "Symbol levels in sequence");
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
    captionFit(side, "ATSC receiver");
    ImGui::BeginChild("atxt", sz);
    ImGui::PushFont(a.mono, 0);
    ImGui::PushTextWrapPos(0);   // the lines wrap in a narrow row
    ImGui::Text("pilot       %s", at.pilot ? "locked" : "searching");
    { ImGui::PushTextWrapPos(0); ImGui::Text("segment     %s  sync %.2f", at.segSync ? "locked" : "searching", at.syncQuality); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("field       %s  (%d)", at.fieldSync ? "locked" : "searching", at.fieldParity); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("CFO %+.0f Hz  SRO %+.1f ppm", at.cfoHz, at.sroPpm); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("SNR %.1f dB  data %.1f dB", at.snrDb, at.dataSnrDb); ImGui::PopTextWrapPos(); }
    ImGui::Text("fields %llu", (unsigned long long)at.fields);
    { ImGui::PushTextWrapPos(0); ImGui::Text("RS ok %llu fixed %llu", (unsigned long long)at.rsClean, (unsigned long long)at.rsCorrected); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("RS failed %llu (%.1f%%)", (unsigned long long)at.rsFailed, at.segErrorRate * 100); ImGui::PopTextWrapPos(); }
    ImGui::Text("lock losses %llu", (unsigned long long)at.lockLosses);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::EndGroup();
}

// A compact read-out next to the constellations (new interface): the numbers that say how the receiver is doing
void receiverGlance(App& a, float w, float h) {
    const RxTelemetry& rx = a.rx;
    if (const ModeUi* mu = modeUi(a.family)) if (mu->receiver) { mu->receiver(a); return; }
    ImGui::TextDisabled("Receiver");
    ImGui::BeginChild("##glance", ImVec2(w, std::max(40.f, h - ImGui::GetTextLineHeightWithSpacing())));   // scrolls when the row is short
    auto kv = [&](const char* k, const char* fmt, auto... v) {
        ImGui::TextDisabled("%s", k); kvColumn(112 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    const bool run = a.engine.running();
    kv("state", "%s", !run ? "stopped" : rx.state == 2 ? "locked" : "searching");
    if (run) {
        if (rx.standard == 0) {
            kv("SNR", "%.1f dB", rx.dataValid ? rx.dataSnrDb : rx.cpSnrDb);
            kv("carrier offset", "%+.0f Hz", rx.cfoHz);
            kv("clock offset", "%+.2f ppm", rx.sroPpm);
            kv("FFT / guard", "%d / %s", rx.fftN, rx.giIdx >= 0 ? guardName(rx.giIdx) : "-");
            kv("L1 pre / post", "%llu / %llu ok", (unsigned long long)rx.l1preGood, (unsigned long long)rx.l1postGood);
            kv("FEC blocks", "%llu ok, %llu bad", (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad);
            if (rx.plpMerDb > 0) kv("MER", "%.1f dB", rx.plpMerDb);
        } else if (rx.standard == 1) {
            kv("SNR", "%.1f dB", rx.dataSnrDb);
            kv("carrier offset", "%+.0f Hz", rx.cfoHz);
            kv("FFT / guard", "%d / %s", rx.fftN, rx.giIdx >= 0 ? dvbt::guardName(rx.giIdx) : "-");
            kv("TPS", "%s", rx.dvbt.tpsOk ? "ok" : "searching");
            if (rx.dvbt.tpsOk) kv("modulation", "%s %s", dvbt::modName(rx.dvbt.mod), dvbt::rateName(rx.dvbt.crHp));
            kv("packets", "%llu", (unsigned long long)rx.dvbt.packets);
            kv("Reed-Solomon", "%llu fixed, %llu failed", (unsigned long long)rx.dvbt.rsCorrected, (unsigned long long)rx.dvbt.rsFailed);
        } else {
            kv("SNR", "%.1f dB", rx.dataSnrDb);
            kv("carrier offset", "%+.0f Hz", rx.cfoHz);
        }
        {
            const SampleLoss l = a.engine.sampleLoss();
            ImGui::TextDisabled("dropped"); kvColumn(112 * gUi);
            ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextColored(lossColour(l), "%s", lossText(l).c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
            lossTooltip(l);
            if (l.live) {
                ImGui::TextDisabled("arriving"); kvColumn(112 * gUi);
                ImGui::PushFont(a.mono, 0);
                if (l.deliveredPct < 0) ImGui::TextUnformatted("-");
                else if (l.rateLow) ImGui::TextColored(pal::badRed(), "%.1f%%", l.deliveredPct);
                else ImGui::Text("%.1f%%", l.deliveredPct);
                ImGui::PopFont();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Samples the radio handed over, as a percent of the rate it was set to. Warns below 97%. Not the receiver load.");
            }
            kv("receiver load", "%.0f%%", l.loadPct);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Processing time of a block versus its duration. High load is not the same as lost samples.");
        }
    }
    ImGui::EndChild();
}

void constellationsTab(App& a) {
    if (const ModeUi* mu = modeUi(a.family)) { if (mu->panels) mu->panels(a); return; }
    if (a.dabMode) { dabPanels(a); return; }
    if (a.fmMode) { fmPanels(a); return; }
    if (a.rx.standard == 2) { atscPanels(a); return; }
    const RxTelemetry& rx = a.rx;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    // the new interface packs the plots to the left and uses the rest for numbers, when there is room for them (else they are on the Receiver tab)
    const float glanceW = pal::dev() && availW * 0.36f >= 230 * gUi ? std::min(260.f * gUi, availW * 0.36f) : 0.f;
    const bool glance = glanceW > 0;
    const float roomH = availH - ImGui::GetTextLineHeightWithSpacing() - ImGui::GetFrameHeightWithSpacing();   // a caption above, the view buttons below
    // the plots share the width equally; on a small window the less useful ones go first (raw OFDM cells, then the TPS / P1 carriers, then the pilots)
    const float gap0 = 8 * gUi, minPw = 84 * gUi;
    const float usableW = availW - (glance ? glanceW + 2 * gap0 : 0.f);
    bool show[4] = {true, true, true, true};
    int nPlots = 4;
    static const int dropOrder[3] = {3, 0, 1};
    for (int k = 0; k < 3 && nPlots > 1 && (usableW - (nPlots + 1) * gap0) / nPlots < minPw; k++) { show[dropOrder[k]] = false; nPlots--; }
    const float pw0 = std::max(48.f * gUi, (usableW - (nPlots + 1) * gap0) / nPlots);
    const float side = std::min(pw0, std::max(48.f * gUi, roomH) * 1.6f);   // the plot width: as wide as the share, but not a thin strip next to a short row
    const float gap = nPlots > 0 ? std::max(gap0, (usableW - nPlots * side) / (nPlots + 1)) : gap0;
    const ImVec2 sz(side, std::max(48.f * gUi, std::min(roomH, side * 1.25f)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    int drawn = 0;
    char cap[96];
    auto caption = [&](const char* fmt, auto... args) {   // as wide as the plot below it at most: the whole caption on hover
        snprintf(cap, sizeof cap, fmt, args...);
        const std::string c = fitCaption(cap, side);
        ImGui::TextDisabled("%s", c.c_str());
        if (c != cap && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cap);
    };
    if (show[0]) {
    drawn++;
    ImGui::BeginGroup();
    if (rx.standard == 5) caption("TMCC carriers, DBPSK (%zu cells)", rx.p1Const.size()); else if (rx.standard == 1) caption("TPS carriers, DBPSK (%zu cells)", rx.p1Const.size()); else caption("P1 carriers (%zu cells)", rx.p1Const.size());
    scatter("##c1", rx.p1Const, sz, 2.5, pal::accent(0.9f));
    ImGui::EndGroup();
    }
    if (show[1]) {
    if (drawn++) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (rx.standard == 1 || rx.standard == 5) caption("Pilots, equalised (%zu cells)", rx.eqCells.size()); else caption("P2 cells, equalised (%zu cells)", rx.eqCells.size());
    scatter("##ceq", rx.eqCells, sz, 2.0, pal::accent(0.8f));
    ImGui::EndGroup();
    }
    {
    if (drawn++) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    if (!rx.plpConst.empty()) caption("PLP %d, decoded cells  MER %.1f dB", rx.plpId, rx.plpMerDb);
    else if (rx.standard == 1 || rx.standard == 5) caption("Data cells, equalised (%zu cells)", rx.eqData.size());
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
        if (ImGui::CalcTextSize("cellsdensityclusters").x + 6 * 8 + 8 * gUi <= side) {
            if (pillButton(tr("cells"), a.constView == 0, 8)) a.constView = 0;
            ImGui::SameLine(0, 4 * gUi);
            if (pillButton(tr("density"), a.constView == 1, 8)) a.constView = 1;
            ImGui::SameLine(0, 4 * gUi);
            if (pillButton(tr("clusters"), a.constView == 2, 8)) a.constView = 2;
        } else {   // narrower than the three buttons: the same choice as a drop-down under the plot
            ImGui::SetNextItemWidth(side);
            comboTr("##cview", &a.constView, {N_("cells"), N_("density"), N_("clusters")});
        }
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
    }
    if (show[3]) {
    if (drawn++) ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    caption("OFDM cells, raw (%zu cells)", rx.rawCells.size());
    scatter("##c3", rx.rawCells, sz, 3.0, pal::accent(0.7f));
    ImGui::EndGroup();
    }
    if (glance) {
        ImGui::SameLine(0, 2 * gap);
        ImGui::BeginGroup();
        receiverGlance(a, ImGui::GetContentRegionAvail().x - gap, side + ImGui::GetTextLineHeightWithSpacing());
        ImGui::EndGroup();
    }
}

void channelTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (!rx.chValid) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no channel estimate yet (needs a decoded P1 and a P2 symbol)"); ImGui::PopTextWrapPos(); } return; }
    {
        const MultipathReport& mr = a.mpd.report();
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(mr.level == MultipathLevel::None ? ImVec4(0.4f, 0.85f, 0.5f, 1) : mr.level == MultipathLevel::Mild ? ImVec4(0.95f, 0.8f, 0.3f, 1) : ImVec4(0.95f, 0.5f, 0.25f, 1),
                           "Multipath analysis: %s", mr.headline.c_str()); ImGui::PopTextWrapPos(); }
        for (auto& r : mr.reasons) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("- %s", r.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::Separator();
    }
    const float h = ImGui::GetContentRegionAvail().y - 56;   // room for the two captions
    double fnMhz = rx.nativeRate / 1e6, binMhz = fnMhz / rx.fftN;
    std::vector<float> xs(rx.chMagDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.chDecim) - (rx.chCarriers - 1) / 2.0) * binMhz);
    captionFit(ImGui::GetContentRegionAvail().x, "|H(f)| from P2 pilots, dB (%d carriers%s)", rx.chCarriers, rx.extCarriers ? ", extended" : "");
    const float minH = 6.5f * ImGui::GetTextLineHeight();   // a short tab: the plots keep this much and the pane scrolls
    if (plt::BeginPlot("##chm", ImVec2(-1, std::max(minH, h * 0.5f)), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "dB", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = pal::accent();
        plt::PlotLine("mag", xs.data(), rx.chMagDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
    captionFit(ImGui::GetContentRegionAvail().x, "phase of H(f), rad (slope = timing, curvature = echoes)");
    if (plt::BeginPlot("##chp", ImVec2(-1, std::max(minH, ImGui::GetContentRegionAvail().y)), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
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
    if (!rx.chValid || rx.irDb.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no channel estimate yet"); ImGui::PopTextWrapPos(); } return; }
    std::vector<float> xs(rx.irDb.size());
    double usPerSample = 1e6 / rx.nativeRate;
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)((rx.irTauMin + (double)i) * usPerSample);
    captionFit(ImGui::GetContentRegionAvail().x, "power-delay profile (dB rel. strongest path). Shaded: guard interval (%.1f us)", rx.guard * usPerSample);
    if (plt::BeginPlot("##ir", ImVec2(-1, std::max(6.5f * ImGui::GetTextLineHeight(), ImGui::GetContentRegionAvail().y)), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("delay (us)", "dB");
        plt::SetupAxisLimits(plt::Y1, -80, 5, plt::Cond_Once);
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Once);
        double gx[2] = {0, rx.guard * usPerSample}, gy[2] = {5, 5};
        plt::Spec gs; gs.FillColor = ImVec4(0.15f, 0.55f, 0.20f, 0.18f); gs.LineColor = ImVec4(0, 0, 0, 0);
        plt::PlotShaded("gi", gx, gy, 2, -80.0, gs);
        plt::Spec sp; sp.LineColor = pal::dev() ? pal::accent() : ImVec4(0.45f, 0.75f, 1, 1);
        plt::PlotLine("pdp", xs.data(), rx.irDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
}

void snrTab(App& a) {
    const RxTelemetry& rx = a.rx;
    if (rx.snrDb.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("per-carrier SNR appears once a full frame has been received"); ImGui::PopTextWrapPos(); } if (rx.chValid) { ImGui::PushTextWrapPos(0); ImGui::Text("SNR estimate: %.1f dB", rx.p2SnrDb); ImGui::PopTextWrapPos(); } return; }
    double binMhz = rx.nativeRate / 1e6 / rx.fftN;
    std::vector<float> xs(rx.snrDb.size());
    for (size_t i = 0; i < xs.size(); i++) xs[i] = (float)(a.freqMhz + ((double)(i * rx.snrStep) - (rx.chCarriers - 1) / 2.0) * binMhz);
    captionFit(ImGui::GetContentRegionAvail().x, "pilot-derived SNR across the channel (scattered pilots, smoothed over %d points); mean %.1f dB", 21, rx.p2SnrDb);
    if (plt::BeginPlot("##snrc", ImVec2(-1, std::max(6.5f * ImGui::GetTextLineHeight(), ImGui::GetContentRegionAvail().y)), plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("frequency (MHz)", "SNR (dB)", 0, plt::AxisFlags_AutoFit);
        plt::SetupAxisFormat(plt::X1, "%.2f");
        plt::SetupAxisLimits(plt::X1, xs.front(), xs.back(), plt::Cond_Always);
        plt::Spec sp; sp.LineColor = ImVec4(0.35f, 0.85f, 0.45f, 1); sp.FillFade = 0.9f;
        plt::PlotLine("snr", xs.data(), rx.snrDb.data(), (int)xs.size(), sp);
        plt::EndPlot();
    }
}

void fecTab(App& a) {
    if (a.rx.standard == 1) {
        const RxTelemetry& rx = a.rx;
        { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "DVB-T channel decoder: de-interleaving, Viterbi (inner code), outer de-interleaver, Reed-Solomon (204,188)"); ImGui::PopTextWrapPos(); }
        if (!rx.dvbt.tpsOk) { ImGui::TextDisabled("waiting for TPS"); return; }
        const double tot = (double)(rx.dvbt.rsClean + rx.dvbt.rsCorrected + rx.dvbt.rsFailed);
        if (ImGui::BeginTable("fect", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230 * gUi);
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
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Uncorrectable packets are passed on with the transport_error_indicator set, so the player skips them."); ImGui::PopTextWrapPos(); }
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
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("waiting for L1-post (PLP configuration)..."); ImGui::PopTextWrapPos(); }
        if (rx.plpSkipped) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "%d frame(s) skipped: PLP uses inter-frame time interleaving or lies outside the received cells", rx.plpSkipped); ImGui::PopTextWrapPos(); }
        ImGui::PopFont();
        return;
    }
    static const char* modN[] = {"QPSK", "16-QAM", "64-QAM", "256-QAM"};
    ImGui::TextColored(pal::heading(), "PLP %d", rx.plpId);
    row("FEC frame / code rate", "%s, %s", rx.plpFec.shortFrame ? "short (16200)" : "normal (64800)", rateName(rx.plpFec.rate));
    row("constellation", "%s%s", modN[rx.plpFec.mod & 3], rx.plpFec.rotation ? ", rotated + cyclic Q delay" : "");
    row("FEC blocks per frame", "%d", rx.plpBlocks);
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::heading(), "decoder (all frames since start)"); ImGui::PopTextWrapPos(); }
    double tot = (double)(rx.blocksOk + rx.blocksBad);
    row("frames decoded", "%llu  (dropped, decoder busy: %llu)", (unsigned long long)rx.plpFrames, (unsigned long long)rx.plpFramesDropped);
    row("FEC blocks OK / failed", "%llu / %llu  (%.2f%% good)", (unsigned long long)rx.blocksOk, (unsigned long long)rx.blocksBad, tot ? 100.0 * rx.blocksOk / tot : 0.0);
    row("BB headers with valid CRC", "%llu", (unsigned long long)rx.headersOk);
    row("BCH bit errors corrected", "%llu", (unsigned long long)rx.plpBchCorrected);
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "last frame");
    row("MER", "%.1f dB", rx.plpMerDb);
    row("BER before LDPC", "%.2e", rx.plpPreBer);
    row("LDPC iterations (average)", "%.1f", rx.plpIters);
    row("decode time", "%.0f ms for %.0f ms of signal (%s)", rx.plpDecodeMs, rx.frameMs, rx.plpOnGpu ? "GPU" : "CPU");
    if (rx.plpHeaderUpl) row("BB header", "UPL %d bits, DFL %d bits, SYNCD %d", rx.plpHeaderUpl, rx.plpHeaderDfl, rx.plpHeaderSyncd);
    ImGui::PopFont();
}

static void tsPidsView(App& a) {
    const TsSnapshot& ts = a.ts;
    ImGui::PushFont(a.mono, 0);
    char b[64], b2[64];
    { ImGui::PushTextWrapPos(0); ImGui::Text("network \"%s\"  onid 0x%04X  tsid 0x%04X   %s", ts.networkName.c_str(), ts.onid, ts.tsid, ts.utc.c_str()); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("multiplex %s (null packets %s)   packets %llu   continuity errors %llu   TEI %llu",
                fmtKbps(b, sizeof b, ts.muxKbps), fmtKbps(b2, sizeof b2, ts.nullKbps), (unsigned long long)ts.totalPackets, (unsigned long long)ts.ccErrors, (unsigned long long)ts.teiPackets); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::Text("BB frames %llu (lost %llu)  mode %s  ISSYI %d NPD %d  SIS/MIS %d ISI %d  resyncs %llu  CRC-8 errors %llu  skipped %llu", (unsigned long long)a.bb.frames, (unsigned long long)a.bb.framesLost,
                a.bb.hem ? "high-efficiency" : "normal", a.bb.issyi, a.bb.npd, a.bb.sisMis, a.bb.isi, (unsigned long long)a.bb.resyncs, (unsigned long long)a.bb.crcErrors, (unsigned long long)a.bb.unsupported); ImGui::PopTextWrapPos(); }
    ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginTable("pids", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("content");
        ImGui::TableSetupColumn("packets", ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn("rate", ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
        ImGui::TableSetupColumn("CC err", ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn("scr", ImGuiTableColumnFlags_WidthFixed, 40 * gUi);
        ImGui::TableHeadersRow();
        for (auto& p : ts.pids) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%04X", p.pid);
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(p.label.c_str()); ImGui::PopTextWrapPos();   // wraps in a narrow tab
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)p.packets);
            ImGui::TableNextColumn(); { char r[32]; ImGui::TextUnformatted(fmtKbps(r, sizeof r, p.kbps)); }
            ImGui::TableNextColumn(); if (p.ccErrors) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1), "%llu", (unsigned long long)p.ccErrors); else ImGui::TextDisabled("0");
            ImGui::TableNextColumn(); if (p.scrambled) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "yes"); else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

// ---- service information beyond the multiplex: transponder lists, bouquets, other transport streams, time offsets, CAT, carousels, application signalling
static std::string fmtUtc(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    std::tm r{};
#ifdef _WIN32
    gmtime_s(&r, &tt);
#else
    gmtime_r(&tt, &r);
#endif
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d UTC", r.tm_year + 1900, r.tm_mon + 1, r.tm_mday, r.tm_hour, r.tm_min);
    return b;
}

void carouselView(App& a);   // carousel_view.cpp: carousels, files, preview, MHEG-5 scene drawing
static void tsTablesView(App& a, int view) {
    const TsSnapshot& ts = a.ts;
    const SiSnapshot* si = ts.si.get();
    ImGui::PushFont(a.mono, 0);
    auto none = [&](const char* what) { ImGui::TextDisabled("%s", what); };
    if (view == 1) {   // networks and transponders
        if (!si || si->networks.empty()) none("no network information table received yet");
        else for (const auto& n : si->networks) {
            ImGui::TextColored(pal::accent(), "network 0x%04X \"%s\"%s  %zu transport streams", n.id, n.name.c_str(), n.actual ? "  (this network)" : "", n.transponders.size());
            if (ImGui::BeginTable(("##nit" + std::to_string(n.id)).c_str(), 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX)) {
                ImGui::TableSetupColumn("onid"); ImGui::TableSetupColumn("tsid"); ImGui::TableSetupColumn("delivery"); ImGui::TableSetupColumn("frequency"); ImGui::TableSetupColumn("pol / bw"); ImGui::TableSetupColumn("symbol rate"); ImGui::TableSetupColumn("modulation, FEC"); ImGui::TableSetupColumn("position, services");
                ImGui::TableHeadersRow();
                for (const auto& t : n.transponders) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("0x%04X", t.onid);
                    ImGui::TableNextColumn(); ImGui::Text("0x%04X", t.tsid);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(t.kind == 1 ? (t.s2 ? "satellite DVB-S2" : "satellite DVB-S") : t.kind == 2 ? "cable" : t.kind == 3 ? "terrestrial" : t.kind == 4 ? "terrestrial T2" : t.kind == 5 ? "satellite S2X" : "?");
                    ImGui::TableNextColumn(); ImGui::Text("%.3f MHz", t.freqMhz);
                    ImGui::TableNextColumn(); if (t.kind == 1) ImGui::TextUnformatted(siPolName(t.pol)); else if (t.bandwidthMhz > 0) ImGui::Text("%.1f MHz", t.bandwidthMhz); else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn(); if (t.symbolRateK > 0) ImGui::Text("%.1f ksym/s", t.symbolRateK); else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn(); if (t.kind == 1 || t.kind == 2) ImGui::Text("%s, %s", siModName(t.kind, t.modulation), siFecName(t.fecInner)); else ImGui::TextUnformatted("-");
                    ImGui::TableNextColumn(); if (t.kind == 1) ImGui::Text("%.1f %c, %zu services", std::fabs(t.orbitDeg), t.orbitDeg >= 0 ? 'E' : 'W', t.services.size()); else ImGui::Text("%zu services", t.services.size());
                }
                ImGui::EndTable();
            }
        }
    } else if (view == 2) {   // services of other transport streams
        if (!si || si->otherServices.empty()) none("no services of other transport streams received yet (SDT other)");
        else if (ImGui::BeginTable("##sdto", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("onid"); ImGui::TableSetupColumn("tsid"); ImGui::TableSetupColumn("service"); ImGui::TableSetupColumn("name"); ImGui::TableSetupColumn("provider"); ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();
            for (const auto& v : si->otherServices) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("0x%04X", v.onid);
                ImGui::TableNextColumn(); ImGui::Text("0x%04X", v.tsid);
                ImGui::TableNextColumn(); ImGui::Text("%d", v.sid);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v.name.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v.provider.c_str());
                ImGui::TableNextColumn(); if (v.freeCa) ImGui::TextColored(pal::warnAmber(), "CA flag set");
            }
            ImGui::EndTable();
        }
        if (si && !si->eitOther.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(pal::accent(), "guide of other transport streams (EIT other)");
            for (const auto& e : si->eitOther) ImGui::Text("0x%04X/%d: %u events, %s to %s%s%s", e.tsid, e.sid, e.events, fmtUtc(e.first).c_str(), fmtUtc(e.last).c_str(), e.nowTitle.empty() ? "" : ", now: ", e.nowTitle.c_str());
        }
    } else if (view == 3) {   // bouquets
        if (!si || si->bouquets.empty()) none("no bouquet association table received yet");
        else for (const auto& b : si->bouquets) {
            ImGui::TextColored(pal::accent(), "bouquet 0x%04X \"%s\"  %zu services", b.id, b.name.c_str(), b.services.size());
            std::string l;
            for (size_t i = 0; i < b.services.size() && i < 60; i++) { char x[48]; snprintf(x, sizeof x, "%s%d/0x%04X", i ? ", " : "", b.services[i].sid, b.services[i].tsid); l += x; }
            ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(l.c_str()); ImGui::PopTextWrapPos();
        }
    } else if (view == 4) {   // time
        ImGui::Text("TDT: %s", ts.utc.empty() ? "-" : ts.utc.c_str());
        if (si && si->totUtc) ImGui::Text("TOT: %s", fmtUtc(si->totUtc).c_str());
        if (!si || si->timeOffsets.empty()) none("no local time offsets received yet (TOT)");
        else for (const auto& o : si->timeOffsets) ImGui::Text("%s region %d: UTC%+d:%02d, next %+d:%02d at %s", o.country.c_str(), o.region, o.offsetMin / 60, std::abs(o.offsetMin) % 60, o.nextOffsetMin / 60, std::abs(o.nextOffsetMin) % 60, fmtUtc(o.changeAt).c_str());
    } else if (view == 5) {   // CA
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Conditional access is only listed: the CA systems named in the CAT and which streams are scrambled. Nothing is descrambled.");
        ImGui::PopTextWrapPos();
        if (!si || si->ca.empty()) none("no conditional access table received yet");
        else for (const auto& c : si->ca) ImGui::Text("CA system 0x%04X %s, EMM PID 0x%04X", c.caSystemId, c.name.empty() ? "" : ("(" + c.name + ")").c_str(), c.emmPid);
        int scr = 0;
        for (const auto& sv : ts.services) if (sv.scrambled) scr++;
        ImGui::Text("%d of %zu services arrive scrambled", scr, ts.services.size());
    } else if (view == 6) {   // carousels
        carouselView(a);
    } else if (view == 7) {   // application signalling and other payloads
        if (!ts.data) none("no private data PIDs found");
        else {
            for (const auto& t : ts.data->ait) {
                ImGui::TextColored(pal::accent(), "AIT on PID 0x%04X: %s (type 0x%04X), version %d", t.pid, t.typeName.c_str(), t.appType, t.version);
                for (const auto& ap : t.apps) {
                    ImGui::Text("  %s  org 0x%X app 0x%X  %s", ap.name.empty() ? "(no name)" : ap.name.c_str(), ap.orgId, ap.appId, ap.controlName.c_str());
                    for (const auto& tr : ap.transports) ImGui::TextDisabled("      %s%s%s", tr.c_str(), ap.initialPath.empty() ? "" : "  start ", ap.initialPath.c_str());
                }
            }
            for (const auto& c : ts.data->scte) ImGui::Text("SCTE-35 PID 0x%04X: %s x%llu%s%s", c.pid, c.commandName.c_str(), (unsigned long long)c.count, c.outOfNetwork ? ", out of network" : "", c.durationSec > 0 ? (", break " + std::to_string((int)c.durationSec) + " s").c_str() : "");
            ImGui::Spacing();
            ImGui::TextColored(pal::accent(), "private PIDs");
            for (const auto& p : ts.data->pids) ImGui::Text("  0x%04X  %s  (%llu packets)", p.pid, p.kind.c_str(), (unsigned long long)p.packets);
        }
    } else if (view == 8) {   // DVB-RCS / RCS2 signalling
        if (!ts.rcs) none("no DVB-RCS signalling found (an interactive network's outbound declares it with RCS content descriptors in its PMT)");
        else {
            const RcsView& r = *ts.rcs;
            ImGui::TextColored(pal::accent(), "INTERACTIVE NETWORK  %s  (id %d)", r.networkName.empty() ? "(no name)" : r.networkName.c_str(), r.networkId);
            for (const auto& f : r.forward) {
                static const char* stds[] = {"DVB-S", "DVB-S2 CCM", "DVB-S2 ACM", "DVB-S2X"};
                static const char* pols[] = {"H", "V", "left", "right"};
                ImGui::Text("forward link: satellite %d beam %d NCC %d  %.5f GHz %s  %.1f deg  %s  %.3f Msym/s%s", f.satelliteId, f.beamId, f.nccId, f.frequencyHz / 1e9, pols[f.polarization & 3], f.orbitalDeg, stds[f.standard & 3], f.symbolRate / 1e6, f.isi >= 0 ? (" ISI " + std::to_string(f.isi)).c_str() : "");
            }
            for (const auto& l : r.returns) ImGui::Text("return link: satellite %d beam %d gateway %d  superframe %d  offset %+.1f kHz", l.satelliteId, l.beamId, l.gatewayId, l.superframeId, l.txOffsetHz / 1e3);
            if (r.haveNcr) ImGui::Text("NCR on PID 0x%04X: %llu packets%s%s", r.ncr.pid, (unsigned long long)r.ncr.packets, r.ncr.intervalS > 0 ? (", every " + std::to_string((int)std::round(r.ncr.intervalS * 1000)) + " ms").c_str() : "", r.ncr.havePpm ? (", clock " + std::to_string(r.ncr.ppm).substr(0, 6) + " ppm").c_str() : "");
            for (const auto& s : r.superframes) ImGui::Text("superframe %d: centre %.4f GHz, %.2f ms, %zu frames", s.id, s.centreHz / 1e9, s.durationS * 1e3, s.frames.size());
            for (const auto& f : r.frameTypes) { ImGui::Text("frame type %d: %.2f ms, %d timeslots, %zu groups", f.id, f.durationS * 1e3, f.timeslots, f.groups.size()); }
            for (const auto& t : r.timeslots) ImGui::Text("timeslot type %d: %.0f Bd, %s %s%s, %s", t.id, t.symbolRate, rcsModulationName(t.modulation), rcsRateName(t.codeRate), t.turbo ? " turbo" : "", rcsPayloadName(t.payload));
            ImGui::Text("TBTP: %llu assignments to %zu terminals;  CMT: %llu entries for %zu;  TIM: %llu unicast to %zu terminals", (unsigned long long)r.tbtpAssignments, r.tbtpLogons, (unsigned long long)r.cmtEntries, r.cmtLogons, (unsigned long long)r.timUnicast, r.timMacs);
            if (r.timStatus >= 0) { std::string fl; for (const auto& x : rcsTimFlags(r.timStatus)) fl += (fl.empty() ? "" : ", ") + x; ImGui::Text("TIM status 0x%02X%s%s", r.timStatus, fl.empty() ? "" : ": ", fl.c_str()); }
            ImGui::Spacing();
            ImGui::TextColored(pal::accent(), "SIGNALLING TABLES");
            for (const auto& kv : r.tables) {
                const RcsTableTally& t = kv.second;
                ImGui::Text("  0x%02X %-28s PID 0x%04X  %llu sections, %llu bytes%s%s", kv.first, rcsTableName(kv.first), t.pid, (unsigned long long)t.sections, (unsigned long long)t.bytes,
                            t.sections >= 3 ? (", every " + std::to_string((int)std::round((t.lastS - t.firstS) / (double)(t.sections - 1) * 1000)) + " ms").c_str() : "", t.crcBad ? ", CRC errors" : "");
            }
            for (const auto& m : r.declaredMissing) ImGui::TextDisabled("  declared on PID 0x%04X, not sent: %s", m.first, rcsTableName(m.second));
        }
    }
    ImGui::PopFont();
}

static int g_tsView = 0;
void tsTab(App& a) {
    subNav("tsv", g_tsView, {"PIDs", "Networks", "Other services", "Bouquets", "Time", "Conditional access", "Carousels", "Data", "Interactive"});
    if (g_tsView == 0) tsPidsView(a);
    else tsTablesView(a, g_tsView);
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
    ImGui::BeginChild("hplots", ImVec2(0, std::max(h * 0.70f, h - 110 * gUi)));   // a short tab: the log keeps a few lines, the plots scroll
    historyTab(a);
    ImGui::EndChild();
    ImGui::BeginChild("hlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    logPanel(a);
    ImGui::EndChild();
}

