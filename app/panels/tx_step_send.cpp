// The preview block of the transmit panel. A live spectrum and waterfall of the IQ (the planned signal while idle, the IQ really going to the radio
// while it runs), an IQ scatter, peak / RMS meters in dBFS with a clipping lamp, the occupied bandwidth and the sample rate, the Start / Stop and Carrier-tune
// buttons, the emergency "STOP ALL" and the log.
#include "tx_state.h"
#include "../ui/typography.h"
#include "../ui/icons.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace txui;

namespace {

// a level bar in dBFS (-60 .. 0) with the value; `hot` paints it red
void meter(const char* label, float lin, bool hot) {
    const float db = lin > 1e-6f ? 20.f * std::log10(lin) : -120.f;
    const float frac = std::max(0.f, std::min(1.f, (db + 60.f) / 60.f));
    const float lw = 64 * gUi;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(lw);
    const ImVec4 col = hot ? kRed : db > -6 ? pal::warnAmber() : pal::okGreen();
    char txt[32];
    snprintf(txt, sizeof txt, "%.1f dBFS", (double)std::max(-99.f, db));
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar(frac, ImVec2(-1, ImGui::GetFrameHeight() * 0.8f), txt);
    ImGui::PopStyleColor();
}

// an IQ scatter of up to 600 samples in a square box
void scatter(const std::vector<cf32>& iq, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##iqbox", ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(ImGuiCol_FrameBg), 4 * gUi);
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f);
    dl->AddLine(ImVec2(p.x + size / 2, p.y), ImVec2(p.x + size / 2, p.y + size), grid);
    dl->AddLine(ImVec2(p.x, p.y + size / 2), ImVec2(p.x + size, p.y + size / 2), grid);
    dl->AddCircle(ImVec2(p.x + size / 2, p.y + size / 2), size / 2 - 2, grid, 40);   // full scale
    if (iq.empty()) return;
    const size_t n = std::min<size_t>(600, iq.size()), step = std::max<size_t>(1, iq.size() / n);
    const ImU32 c = ImGui::GetColorU32(pal::accent(0.7f));
    for (size_t i = 0; i < iq.size(); i += step)
        dl->AddRectFilled(ImVec2(p.x + size / 2 + iq[i].real() * (size / 2 - 2) - 1, p.y + size / 2 - iq[i].imag() * (size / 2 - 2) - 1),
                          ImVec2(p.x + size / 2 + iq[i].real() * (size / 2 - 2) + 1, p.y + size / 2 - iq[i].imag() * (size / 2 - 2) + 1), c);
}

// spectrum line and waterfall of `iq` centred on fc MHz; the waterfall keeps its rows only while a signal is shown
void spectrumPlots(TxUi& s, const std::vector<cf32>& iq, double rate, double fc, float height) {
    if (!s.wfInit && gGfx) { s.wf.init(gGfx); s.wfInit = true; }
    constexpr int kFft = 2048;
    std::vector<float> db;
    if (iq.size() >= 64) txSpectrumDb(iq.data() + (iq.size() > (size_t)kFft ? iq.size() - kFft : 0), std::min(iq.size(), (size_t)kFft), kFft, db);
    const double bw = rate > 0 ? rate / 1e6 : 2;
    s.specX.resize(db.empty() ? 2 : db.size());
    s.specY.resize(s.specX.size());
    if (db.empty()) {
        s.specX[0] = fc - bw / 2; s.specX[1] = fc + bw / 2;
        s.specY[0] = s.specY[1] = -120.f;
    } else {
        const int n = (int)db.size();
        for (int i = 0; i < n; i++) {
            const int k = (i + n / 2) % n;   // DC in the middle of the plot
            s.specX[(size_t)i] = fc + ((i - n / 2) / (double)n) * (rate / 1e6);
            s.specY[(size_t)i] = db[(size_t)k];
        }
        if (s.wfInit) {
            std::vector<float> row(s.specY.size());
            for (size_t i = 0; i < s.specY.size(); i++) row[i] = (float)s.specY[i];
            s.wf.offer(row, ImGui::GetTime());
        }
    }
    const float hs = std::max(90.f * gUi, height * 0.55f);
    if (plt::BeginPlot("##txspec", ImVec2(-1, hs), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_OpenBottom)) {
        plt::SetupAxes("MHz", "dB");
        plt::SetupAxisLimits(plt::X1, fc - bw / 2, fc + bw / 2, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -100, 5, plt::Cond_Once);
        plt::Spec ln; ln.LineColor = pal::accent(0.95f);
        if (!db.empty()) plt::PlotLine("tx", s.specX.data(), s.specY.data(), (int)s.specX.size(), ln);
        plt::EndPlot();
    }
    const float hf = std::max(60.f * gUi, height - hs - 4 * gUi);
    if (s.wfInit && s.wf.img && plt::BeginPlot("##txwf", ImVec2(-1, hf), plt::Flags_NoLegend | plt::Flags_NoTitle | plt::Flags_OpenTop)) {
        plt::SetupAxes("MHz", nullptr);
        plt::SetupAxisLimits(plt::X1, fc - bw / 2, fc + bw / 2, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -(double)Waterfall::H, 0, plt::Cond_Always);
        if (!db.empty() && s.wf.filled > 0) {
            const double x0 = fc - bw / 2, x1 = fc + bw / 2;
            ImTextureID tex = s.wf.img->texture();
            const int w = s.wf.writeRow, H = Waterfall::H;
            plt::PlotImage("a", tex, plt::Point(x0, -(H - w)), plt::Point(x1, 0), ImVec2(0, (float)w / H), ImVec2(1, 1));
            if (w > 0) plt::PlotImage("b", tex, plt::Point(x0, -(double)H), plt::Point(x1, -(H - w)), ImVec2(0, 0), ImVec2(1, (float)w / H));
        }
        plt::EndPlot();
    }
}

// "0.25 Msps" / "12.5 kHz" for a rate or a width in Hz
std::string hzText(double hz) {
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6);
    else snprintf(b, sizeof b, "%.1f kHz", hz / 1e3);
    return b;
}

} // namespace

namespace txui {

// The preview: spectrum and waterfall of what is sent, the levels, carrier tune, stop all and the log
void stepSend(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode, bool running) {
    if (!mode) { ImGui::TextDisabled("%s", tr("Choose a signal in the list.")); return; }

    const PreviewData& pv = previewFor(s, *mode, radio);
    const double fc = (running && s.freqSent > 0 ? s.freqSent : s.freqMhz * 1e6) / 1e6;
    const float avail = ImGui::GetContentRegionAvail().y;
    ImGui::TextDisabled("%s", running ? (s.sessionDry ? tr("Test: the IQ the radio would get") : tr("The IQ going to the radio")) : tr("What will be sent (preview)"));
    if (pv.busy && !running) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("making the preview...")); }
    if (!pv.error.empty() && !running) banner(Note::Warn, pv.error.c_str());
    const float plotH = std::max(180.f * gUi, std::min(avail - 230 * gUi, 420.f * gUi));
    spectrumPlots(s, pv.iq, pv.rateHz, fc, plotH);

    // levels: from the running session (its limiter counters) or measured on the preview
    TxLevel lv;
    bool hot = false;
    double limitedPct = 0;
    if (running) {
        const TxSession::Stats st = s.session.stats();
        lv.peak = st.peak; lv.rms = st.rms; limitedPct = st.limitedPct;
        hot = st.limitedPct > 0.01 || st.peak >= 0.98f;
    } else if (!pv.iq.empty()) {
        lv = txMeasure(pv.iq.data(), pv.iq.size());
        hot = lv.peak >= 0.98f;
    }
    s.meterPeak = std::max(lv.peak, s.meterPeak * 0.95f);   // the bar falls slowly, as a meter does
    s.meterRms = s.meterRms * 0.8f + lv.rms * 0.2f;
    ImGui::Spacing();
    const float leftW = std::max(200.f * gUi, ImGui::GetContentRegionAvail().x - 130 * gUi - 8 * gUi);
    ImGui::BeginGroup();
    ImGui::PushItemWidth(leftW);
    ImGui::BeginChild("##txmeters", ImVec2(leftW, 152 * gUi), false, ImGuiWindowFlags_NoScrollbar);
    meter(tr("Peak"), s.meterPeak, hot);
    meter(tr("RMS"), s.meterRms, false);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Clipping"));
    ImGui::SameLine(64 * gUi);
    const ImVec2 lp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(lp.x + 8 * gUi, lp.y + ImGui::GetFrameHeight() * 0.5f), 6 * gUi, ImGui::GetColorU32(hot ? kRed : pal::okGreen()));
    ImGui::Dummy(ImVec2(18 * gUi, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    if (hot) ImGui::TextColored(kRed, running ? tr("the limiter is working: %.2f%% of samples (lower the level)") : tr("at full scale: the limiter will round the peaks"), limitedPct);
    else ImGui::TextDisabled("%s", tr("none: the signal fits"));
    const double rate = pv.rateHz > 0 ? pv.rateHz : txRequestRate(*mode, makeReq(s, *mode, false), radio ? TxRateLimits{radio->txMinRateHz, radio->txMaxRateHz} : TxRateLimits{});
    std::vector<float> dbs;
    double obw = 0;
    if (pv.iq.size() >= 2048) { txSpectrumDb(pv.iq.data() + pv.iq.size() - 2048, 2048, 2048, dbs); obw = txOccupiedBandwidthHz(dbs, rate); }
    ImGui::TextDisabled("%s", tr("Sample rate"));
    ImGui::SameLine(140 * gUi);
    ImGui::Text("%s", hzText(rate).c_str());
    ImGui::TextDisabled("%s", tr("Occupied bandwidth"));
    ImGui::SameLine(140 * gUi);
    if (obw > 0) ImGui::Text("~%s", hzText(obw).c_str()); else ImGui::TextDisabled("-");
    // the output power: the radio's measured table (a full-scale carrier at this level) moved by the signal's own RMS and peak
    if (const double fs = radio && s.destKind != 1 ? txCalPowerDbm(calibrationFor(s.radioKey), s.gainDb, s.ampOn) : NAN; std::isfinite(fs) && s.meterRms > 1e-6f) {
        ImGui::TextDisabled("%s", tr("Output power"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Predicted from this radio's power table (Advanced > Calibration of this radio): average and peak envelope power of what is being sent. A prediction, not a measurement."));
        ImGui::SameLine(140 * gUi);
        ImGui::Text(tr("~%.1f dBm average, %.1f dBm peak"), fs + 20 * std::log10(s.meterRms), fs + 20 * std::log10(std::max(s.meterPeak, 1e-6f)));
    }
    ImGui::EndChild();
    ImGui::PopItemWidth();
    ImGui::EndGroup();
    ImGui::SameLine();
    scatter(pv.iq, 130 * gUi);
    if (running) {
        const TxSession::Stats st = s.session.stats();
        if (st.underruns) ImGui::TextColored(pal::warnAmber(), tr("The generator fell behind %llu time(s): silence was sent for those gaps."), (unsigned long long)st.underruns);
        if (!st.fileNote.empty()) ImGui::TextDisabled("%s", st.fileNote.c_str());
    }

    ImGui::Spacing();
    // carrier tune (hold) and the emergency stop, next to the header's Start / Stop
    const bool can = canStart(a, s, radio, conflict, mode);
    ImGui::BeginDisabled((!can || (txNeedsArm(s) && !s.armed)) && !running);
    ImGui::Button(tr("Carrier tune (hold)"));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Unmodulated carrier at this frequency and level, for adjusting the transmission (drive, filter, SWR, dummy-load level), not a message. Hold the button. Stop still applies. On a radio, ARM first."));
    const bool hold = ImGui::IsItemActive();
    if (hold && !s.carrierHeld) {
        s.carrierHeld = true;
        if (running) s.session.setCarrierTune(true);
        else s.pendingCarrier = true;
    } else if (!hold && s.carrierHeld) {
        s.carrierHeld = false;
        if (s.carrierOnly) stopTx(a, s, "(carrier tune released)");
        else if (s.session.running()) s.session.setCarrierTune(false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.08f, 0.08f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.12f, 0.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(tr("STOP ALL"))) { stopTx(a, s, "(stop all)"); s.error.clear(); }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stops the transmission or test, the carrier and any pending start, and gives the radio back."));

    typo::separatorText(tr("Log"));
    const std::vector<std::string> log = s.session.log();
    if (ImGui::BeginChild("##txlog", ImVec2(0, 100 * gUi), ImGuiChildFlags_Borders)) {
        for (const auto& l : log) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
    // what was sent since the program started: one line a transmission, newest first
    typo::separatorText(tr("Sent"));
    if (s.history.empty()) ImGui::TextDisabled("%s", tr("Nothing sent yet."));
    else if (ImGui::BeginTable("##txhist", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, 120 * gUi))) {
        ImGui::TableSetupColumn(tr("UTC")); ImGui::TableSetupColumn(tr("Signal")); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn(tr("Length"));
        ImGui::TableSetupColumn(tr("What"), ImGuiTableColumnFlags_WidthStretch);
        for (const auto& h : s.history) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.utc.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s%s", h.mode.c_str(), h.rf ? "" : tr(" (no RF)"));
            ImGui::TableNextColumn(); ImGui::Text("%.6f", h.mhz);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", h.seconds);
            ImGui::TableNextColumn(); ImGui::Text("%s%s%s", h.what.c_str(), h.what.empty() ? "" : ", ", h.ended.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace txui
