// DMR screens: the two time slots and the call log, the symbol plots, the receiver counters and the options of the test signal.
// Voice is found, counted and logged but not decoded (AMBE+2 is proprietary), so there is no sound.
#include "app.h"
#include "dect2/dmr_proto.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace {

struct State {
    bool wasRunning = false;
    uint64_t lastSeq = 0;
    std::deque<float> snr, ber;      // a point per report (about four a second)
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 10; }

const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1);

void tick(App& a) {
    const bool run = a.engine.running();
    if (run) {   // the sound controls do nothing yet (no vocoder) but are kept in step, so that a codec can be plugged in
        a.engine.dmr().setVolume(a.volume);
        a.engine.dmr().setMuted(a.muted);
    }
    if (run && !S.wasRunning) { S.snr.clear(); S.ber.clear(); }
    S.wasRunning = run;
    if (live(a) && a.rx.seq != S.lastSeq) {
        S.lastSeq = a.rx.seq;
        const DmrTelemetry& t = a.rx.dmr;
        if (t.dataValid) {
            S.snr.push_back(t.snrDb); if (S.snr.size() > 240) S.snr.pop_front();
            S.ber.push_back(t.ber * 100.f); if (S.ber.size() > 240) S.ber.pop_front();
        }
    }
}

// ---------------------------------------------------------------- text helpers

std::string clockText(double sec) {
    const int s = (int)std::max(0.0, sec);
    char b[24]; snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

const char* kindName(int k) {
    static const char* n[5] = {"Group voice", "Private voice", "All call", "Data", "Control"};
    return k >= 0 && k < 5 ? n[k] : "?";
}

std::string idText(uint32_t id, bool known) {
    if (!known || id == 0) return "-";
    char b[16]; snprintf(b, sizeof b, "%u", id);
    return b;
}

// the target of a call: a talkgroup number for group calls
std::string targetText(const DmrCall& c) {
    if (!c.idsKnown || c.dst == 0) return "-";
    if (c.kind == 2 || c.dst == 0xFFFFFF) return "all";
    char b[24]; snprintf(b, sizeof b, c.kind == 0 ? "TG %u" : "%u", c.dst);
    return b;
}

// Voice frames come three to a 60 ms burst, so one is 20 ms: the length of a call that is still going on
double duration(const DmrCall& c) { return c.active ? c.voiceFrames * 0.02 : std::max(0.0, c.endSec - c.startSec); }

std::string flagsText(const DmrCall& c) {
    std::string f;
    auto add = [&](const char* s) { if (!f.empty()) f += ' '; f += s; };
    if (c.emergency) add("EMERG");
    if (c.privacy) add("ENC");
    if (c.lateEntry) add("LATE");
    if (!c.active && c.kind <= 2 && !c.terminated) add("NOEND");
    return f.empty() ? "-" : f;
}

// newest first, calls that are still going on at the top
std::vector<const DmrCall*> sortedCalls(const DmrTelemetry& t) {
    std::vector<const DmrCall*> v;
    for (const auto& c : t.callLog) v.push_back(&c);
    std::stable_sort(v.begin(), v.end(), [](const DmrCall* x, const DmrCall* y) {
        if (x->active != y->active) return x->active;
        return x->startSec > y->startSec;
    });
    return v;
}

void mono(const App& a, const std::string& s) { ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(s.c_str()); ImGui::PopFont(); }

// ---------------------------------------------------------------- the Calls tab

ImU32 stateColour(int st) {
    switch (st) {
    case 1: return IM_COL32(60, 140, 80, 255);     // voice
    case 2: return IM_COL32(60, 110, 170, 255);    // data
    case 3: return IM_COL32(70, 74, 78, 255);      // idle
    case 4: return IM_COL32(170, 125, 45, 255);    // control
    default: return IM_COL32(40, 42, 45, 255);
    }
}

void slotCard(const App& a, int idx, float w, float h) {
    const DmrTelemetry& t = a.rx.dmr;
    const bool on = live(a);
    const DmrSlot& s = t.slot[idx];
    static const char* sn[5] = {"no signal", "voice", "data", "idle", "control"};
    char id[24]; snprintf(id, sizeof id, "##slot%d", idx + 1);
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders);   // the parts of a line that do not fit go on the next one; the card scrolls when it is short
    const bool act = on && s.active;
    const int st = act ? s.state % 5 : 0;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Slot %d", idx + 1);
    ImGui::SameLine(0, 10 * gUi);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float tw = tagAt(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 2), on ? sn[st] : "stopped", stateColour(st));
        ImGui::Dummy(ImVec2(tw, ImGui::GetTextLineHeight() + 4));
    }
    if (act && !s.lastBurst.empty()) { sameLineIf(ImGui::CalcTextSize(s.lastBurst.c_str()).x, 10 * gUi); ImGui::TextDisabled("%s", s.lastBurst.c_str()); }
    if (act && s.inCall) {
        // the identities and the length come from the call being logged for this slot
        const DmrCall* cur = nullptr;
        for (const auto& c : t.callLog) if (c.active && c.slot == idx + 1) cur = &c;
        DmrCall c = cur ? *cur : DmrCall();
        if (!cur) { c.kind = s.callKind; c.src = s.src; c.dst = s.dst; c.idsKnown = s.src || s.dst; c.voiceFrames = s.voiceFrames; c.active = true; }
        ImGui::PushFont(a.mono, 0);
        ImGui::Text("%s", idText(c.src, c.idsKnown).c_str());
        ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("->"); ImGui::SameLine(0, 6 * gUi);
        ImGui::Text("%s", targetText(c).c_str());
        ImGui::PopFont();
        sameLineIf(ImGui::CalcTextSize(kindName(c.kind)).x, 10 * gUi);
        ImGui::TextColored(pal::okGreen(), "%s", kindName(c.kind));
        char b[96];
        if (c.kind <= 2) snprintf(b, sizeof b, "%.1f s   %d voice frames   %d FEC errors", duration(c), c.voiceFrames, c.fecErrors);
        else snprintf(b, sizeof b, "%.1f s   %d FEC errors", std::max(0.0, c.endSec - c.startSec), c.fecErrors);
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", b); ImGui::PopTextWrapPos();
        std::string extra = flagsText(c);
        if (extra == "-") extra.clear();
        if (!c.alias.empty()) extra += (extra.empty() ? "" : "   ") + std::string("alias ") + c.alias;
        if (!extra.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(c.emergency ? pal::badRed() : kDim, "%s", extra.c_str()); ImGui::PopTextWrapPos(); }
    } else {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(!on ? "start the receiver" : act ? "no call" : "nothing received on this slot"); ImGui::PopTextWrapPos(); }
    }
    if (act && s.bursts) {
        const ImVec4 col = s.rmsErr < 0.15f ? pal::okGreen() : s.rmsErr < 0.22f ? pal::warnAmber() : pal::badRed();
        ImGui::TextDisabled("symbol error"); ImGui::SameLine(0, 5 * gUi);
        ImGui::TextColored(col, "%.2f", s.rmsErr);
        flowNext(12 * gUi); ImGui::TextDisabled("BER"); ImGui::SameLine(0, 5 * gUi); ImGui::Text("%.2f %%", s.ber * 100.f);
        flowNext(12 * gUi); ImGui::TextDisabled("bursts"); ImGui::SameLine(0, 5 * gUi); ImGui::Text("%llu", (unsigned long long)s.bursts);
        flowEnd();
    }
    ImGui::EndChild();
}

void callTable(App& a, float h) {
    const DmrTelemetry& t = a.rx.dmr;
    const bool on = live(a);
    if (!ImGui::BeginTable("##dmrcalls", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 48 * gUi);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 96 * gUi);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("Dur s", ImGuiTableColumnFlags_WidthFixed, 48 * gUi);
    ImGui::TableSetupColumn("Voice fr", ImGuiTableColumnFlags_WidthFixed, 58 * gUi);
    ImGui::TableSetupColumn("FEC err", ImGuiTableColumnFlags_WidthFixed, 52 * gUi);
    ImGui::TableSetupColumn("Alias / note", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
    ImGui::TableHeadersRow();
    if (on) {
        for (const DmrCall* pc : sortedCalls(t)) {
            const DmrCall& c = *pc;
            ImGui::TableNextRow();
            if (c.active) ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
            else if (c.emergency) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(c.startSec).c_str());
            ImGui::TableNextColumn(); if (c.slot) ImGui::Text("%d", c.slot); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(kindName(c.kind));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idText(c.src, c.idsKnown).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(targetText(c).c_str());
            ImGui::TableNextColumn(); if (c.kind <= 2 || c.endSec > c.startSec) ImGui::Text("%.1f", duration(c)); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (c.kind <= 2) ImGui::Text("%d", c.voiceFrames); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::Text("%d", c.fecErrors);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(!c.alias.empty() ? c.alias : !c.note.empty() ? c.note : "-", ImGui::GetContentRegionAvail().x).c_str());   // a narrow tab: cut short, whole in the slot card
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(flagsText(c), ImGui::GetContentRegionAvail().x).c_str());
            if (c.active || c.emergency) ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

void messageTable(App& a, float h) {
    const DmrTelemetry& t = a.rx.dmr;
    const bool on = live(a);
    if (!ImGui::BeginTable("##dmrmsg", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 48 * gUi);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, 74 * gUi);
    ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 180 * gUi);
    ImGui::TableSetupColumn("CRC", ImGuiTableColumnFlags_WidthFixed, 36 * gUi);
    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableHeadersRow();
    if (on) {
        for (size_t i = t.messages.size(); i-- > 0;) {
            const DmrMessage& m = t.messages[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(m.sec).c_str());
            ImGui::TableNextColumn(); if (m.slot) ImGui::Text("%d", m.slot); else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(idText(m.src, true).c_str());
            ImGui::TableNextColumn(); { char b[24]; snprintf(b, sizeof b, m.group ? "TG %u" : "%u", m.dst); ImGui::TextUnformatted(b); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.format.c_str());
            ImGui::TableNextColumn(); if (m.crcOk) ImGui::TextColored(pal::okGreen(), "ok"); else ImGui::TextColored(pal::badRed(), "bad");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.text.c_str());
        }
    }
    ImGui::EndTable();
}

void tab(App& a) {
    const DmrTelemetry& t = a.rx.dmr;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x;
    const float gap = 8 * gUi;
    const float cardH = ImGui::GetTextLineHeightWithSpacing() * 4.f + ImGui::GetFrameHeight() + 14 * gUi;
    const float cw = (W - gap) * 0.5f;
    slotCard(a, 0, cw, cardH);
    ImGui::SameLine(0, gap);
    slotCard(a, 1, cw, cardH);
    // what the vocoder situation is, where a person looks for the sound
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Voice audio is not decoded: DMR voice uses the proprietary AMBE+2 vocoder, which OnAir does not include. Voice calls are found, counted and logged, there is no sound.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (on && !t.cachInfo.empty() && t.cachInfo != "null") ImGui::TextDisabled("CACH: %s", t.cachInfo.c_str());
    ImGui::Spacing();
    const float rest = ImGui::GetContentRegionAvail().y;
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const float msgH = std::max(70.f * gUi, std::min(rest * 0.34f, 150.f * gUi));
    ImGui::TextDisabled("Call log (%zu)", on ? t.callLog.size() : (size_t)0);
    callTable(a, std::max(60.f * gUi, rest - msgH - 2 * lh - 8 * gUi));
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Text messages (%zu)", on ? t.messages.size() : (size_t)0); ImGui::PopTextWrapPos(); }
    messageTable(a, std::max(40.f * gUi, ImGui::GetContentRegionAvail().y));
}

// ---------------------------------------------------------------- the list on the right

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see calls"); ImGui::PopTextWrapPos(); } return; }
    const DmrTelemetry& t = a.rx.dmr;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%llu calls and messages", (unsigned long long)t.calls); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##dmrlist", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("S", ImGuiTableColumnFlags_WidthFixed, 18 * gUi);
    ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthStretch, 1.f);
    ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 38 * gUi);
    ImGui::TableHeadersRow();
    for (const DmrCall* pc : sortedCalls(t)) {
        const DmrCall& c = *pc;
        ImGui::TableNextRow();
        if (c.active) ImGui::PushStyleColor(ImGuiCol_Text, pal::okGreen());
        else if (c.emergency) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        else if (c.kind >= 3) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TableNextColumn(); if (c.slot) ImGui::Text("%d", c.slot); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.kind == 4 ? "control" : idText(c.src, c.idsKnown).c_str());
        ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(c.kind == 4 ? (c.note.empty() ? "-" : c.note.c_str()) : targetText(c).c_str()); ImGui::PopTextWrapPos();   // a stretched column: wraps
        ImGui::TableNextColumn(); if (c.kind <= 2) ImGui::Text("%.1f", duration(c)); else ImGui::TextUnformatted("-");
        if (c.active || c.emergency || c.kind >= 3) ImGui::PopStyleColor();
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- the plots under the tabs

void levelLines(const char* name) {
    static const double lv[4] = {-3, -1, 1, 3}, th[3] = {-2, 0, 2};
    plt::Spec sp; sp.LineColor = ImVec4(0.5f, 0.55f, 0.6f, 0.55f); sp.LineWeight = 1.f;
    for (int i = 0; i < 4; i++) {
        const double xs[2] = {-1e6, 1e6}, ys[2] = {lv[i], lv[i]};
        plt::PlotLine(i == 0 ? name : "level", xs, ys, 2, sp);
    }
    (void)th;
}

void historyLine(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size, double yMin, double yMax) {
    if (plt::BeginPlot(id, size, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("reports (~4/s)", ylabel, 0, 0);
        plt::SetupAxisLimits(plt::X1, 0, 240, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, yMin, yMax, plt::Cond_Always);
        if (!h.empty()) {
            std::vector<float> v(h.begin(), h.end());
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("h", v.data(), (int)v.size(), sp);
        }
        plt::EndPlot();
    }
}

void panels(App& a) {
    const DmrTelemetry& t = a.rx.dmr;
    const bool on = live(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, colW = std::max(120.f, (W - 5 * gap) / 4.f);
    const float plotH = std::max(70.f, H - ImGui::GetTextLineHeightWithSpacing() - 8);
    const ImVec2 sz(colW, plotH);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);

    ImGui::BeginGroup();
    captionFit(colW, "4FSK symbols (last %zu)", on ? t.eye.size() : (size_t)0);   // the captions are never wider than their plots
    if (plt::BeginPlot("##dmrsym", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, plt::AxisFlags_NoTickLabels, 0);
        plt::SetupAxisLimits(plt::X1, 0, 800, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -4.5, 4.5, plt::Cond_Always);
        const double ticks[4] = {-3, -1, 1, 3};
        const char* labels[4] = {"-3", "-1", "+1", "+3"};
        plt::SetupAxisTicks(plt::Y1, ticks, 4, labels);
        levelLines("levels");
        if (on && !t.eye.empty()) {
            plt::Spec sp; sp.Marker = plt::Marker_Circle; sp.MarkerSize = 1.6f;
            const ImVec4 c = pal::accent(0.60f);
            sp.MarkerFillColor = c; sp.MarkerLineColor = c; sp.LineColor = c;
            plt::PlotScatter("symbols", t.eye.data(), (int)t.eye.size(), 1.0, 0.0, sp);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Level histogram");
    if (plt::BeginPlot("##dmrhist", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes(nullptr, nullptr, 0, plt::AxisFlags_NoTickLabels | plt::AxisFlags_AutoFit);
        plt::SetupAxisLimits(plt::X1, -5, 5, plt::Cond_Always);
        float mx = 1;
        std::vector<double> x(40), y(40);
        for (int i = 0; i < 40; i++) {
            x[(size_t)i] = -5.0 + 0.25 * i + 0.125;
            y[(size_t)i] = on ? t.levelHist[i] : 0;
            mx = std::max<float>(mx, (float)y[(size_t)i]);
        }
        plt::SetupAxisLimits(plt::Y1, 0, mx * 1.1, plt::Cond_Always);
        plt::Spec sp; sp.LineColor = pal::accent(0.85f); sp.FillColor = pal::accent(0.75f);
        plt::PlotBars("bins", x.data(), y.data(), 40, 0.22, sp);
        static const double lv[4] = {-3, -1, 1, 3};
        plt::Spec ms; ms.LineColor = ImVec4(0.9f, 0.9f, 0.9f, 0.40f); ms.Flags = plt::InfLines_Vertical;
        plt::PlotInfLines("levels", lv, 4, ms);
        plt::EndPlot();
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR of the symbols (dB)");
    historyLine("##dmrsnr", "dB", S.snr, sz, 0, 40);
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Bit error rate (%%)");
    double top = 1.0;
    for (float v : S.ber) top = std::max<double>(top, v);
    historyLine("##dmrber", "%", S.ber, sz, 0, top * 1.1);
    ImGui::EndGroup();
}

// ---------------------------------------------------------------- the Receiver tab

void spectrumPlotDmr(const DmrTelemetry& t, bool on, ImVec2 sz) {
    if (plt::BeginPlot("##dmrspec", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("kHz", "dB", 0, 0);
        plt::SetupAxisLimits(plt::X1, -12, 12, plt::Cond_Always);
        plt::SetupAxisLimits(plt::Y1, -70, 3, plt::Cond_Always);
        if (on && !t.spectrumDb.empty()) {
            const int n = (int)t.spectrumDb.size();
            std::vector<double> x((size_t)n), y((size_t)n);
            for (int i = 0; i < n; i++) { x[(size_t)i] = -12.0 + 24.0 * (i + 0.5) / n; y[(size_t)i] = t.spectrumDb[(size_t)i]; }
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("spectrum", x.data(), y.data(), n, sp);
        }
        plt::EndPlot();
    }
}

void receiver(App& a) {
    const DmrTelemetry& t = a.rx.dmr;
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 10 * gUi, cw = (W - 2 * gap) / 3.f;
    const bool stack = cw < 260 * gUi;   // a narrow tab: the three blocks one under the other (the tab scrolls)
    auto u = [](uint64_t v) { return (unsigned long long)v; };
    // one block of key/value lines in its own column
    auto block = [&](const char* id, auto&& rows, auto&& after) {
        ImGui::BeginChild(id, stack ? ImVec2(W, 0) : ImVec2(cw, H), stack ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None);
        if (ImGui::BeginTable("##kv", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.f);
            rows();
            ImGui::EndTable();
        }
        after();
        ImGui::EndChild();
    };
    auto none = [] {};
    auto kv = [&](const char* k, const char* fmt, auto... v) {
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", k);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text(fmt, v...); ImGui::PopFont();
    };
    auto head = [&](const char* s) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextColored(pal::heading(), "%s", s); ImGui::TableNextColumn(); };
    static const char* st[5] = {"-", "voice", "data", "idle", "control"};

    block("##dmr_a", [&] {
        head("Signal");
        kv("state", "%s", t.state == 2 ? "locked, decoding" : t.state == 1 ? "partly locked" : "searching");
        kv("link", "%s", t.link.empty() ? "-" : t.link.c_str());
        if (t.cc >= 0) kv("colour code", "%d", t.cc); else kv("colour code", "-");
        kv("slots followed", "%d", t.slotsLocked);
        kv("slot 1", "%s", t.slot[0].active ? st[t.slot[0].state % 5] : "-");
        kv("slot 2", "%s", t.slot[1].active ? st[t.slot[1].state % 5] : "-");
        kv("level", "%.1f dBFS", t.levelDbfs);
        if (t.noiseDbfs > -119) kv("noise floor", "%.1f dBFS", t.noiseDbfs); else kv("noise floor", "-");
        if (t.cnrDb > 0) kv("C/N 12.5 kHz", "%.1f dB", t.cnrDb); else kv("C/N 12.5 kHz", "-");
        kv("symbol SNR", "%.1f dB", t.snrDb);
        kv("carrier offset", "%+.0f Hz", t.cfoHz);
        kv("deviation", "%.0f Hz of 1944", t.devHz);
        kv("symbol clock", "%+.1f ppm", t.symbolPpm);
        kv("bit error rate", "%.3f %%", t.ber * 100.f);
        if (!t.cachInfo.empty() && t.cachInfo != "null") { head("CACH"); ImGui::TextDisabled("%s", t.cachInfo.c_str()); }
    }, none);
    if (!stack) ImGui::SameLine(0, gap);
    block("##dmr_b", [&] {
        head("Frame syncs");
        for (int i = 0; i < 9; i++) if (t.syncCount[i] || i < 4) kv(dmr::syncName(i), "%llu", u(t.syncCount[i]));
        head("Bursts");
        kv("voice", "%llu", u(t.voiceBursts));
        kv("  with embedded LC", "%llu", u(t.embeddedBursts));
        kv("reverse channel", "%llu", u(t.rcBursts));
        kv("idle", "%llu", u(t.idleOk));
        kv("unknown", "%llu", u(t.unknownBursts));
        for (int i = 0; i < 12; i++) if (t.burstCount[i]) kv(dmr::dataTypeName(i), "%llu", u(t.burstCount[i]));
        head("Symbol decisions");
        kv("level -3", "%llu", u(t.levelCount[0])); kv("level -1", "%llu", u(t.levelCount[1]));
        kv("level +1", "%llu", u(t.levelCount[2])); kv("level +3", "%llu", u(t.levelCount[3]));
    }, none);
    if (!stack) ImGui::SameLine(0, gap);
    block("##dmr_c", [&] {
        head("FEC");
        const uint64_t tot = t.blocksOk + t.blocksBad;
        kv("blocks ok / bad", "%llu / %llu", u(t.blocksOk), u(t.blocksBad));
        kv("block error rate", "%.2f %%", tot ? 100.0 * (double)t.blocksBad / (double)tot : 0.0);
        kv("BPTC clean", "%llu", u(t.bptcOk));
        kv("BPTC repaired", "%llu", u(t.bptcFixed));
        kv("BPTC lost", "%llu", u(t.bptcFail));
        kv("slot type repaired", "%llu", u(t.golayFixed));
        kv("slot type lost", "%llu", u(t.golayFail));
        kv("RS(12,9) clean", "%llu", u(t.rsOk));
        kv("RS(12,9) repaired", "%llu", u(t.rsFixed));
        kv("RS(12,9) lost", "%llu", u(t.rsFail));
        kv("CRC ok / bad", "%llu / %llu", u(t.crcOk), u(t.crcBad));
        kv("trellis ok / lost", "%llu / %llu", u(t.trellisOk), u(t.trellisFail));
        kv("EMB ok / lost", "%llu / %llu", u(t.embOk), u(t.embFail));
        kv("emb. LC ok / lost", "%llu / %llu", u(t.embLcOk), u(t.embLcFail));
        kv("calls, messages", "%llu", u(t.calls));
    }, [&] {
        ImGui::Spacing();
        ImGui::TextColored(pal::heading(), "Channel spectrum");
        spectrumPlotDmr(t, true, ImVec2(std::max(100.f, ImGui::GetContentRegionAvail().x), std::min(170.f * gUi, std::max(90.f * gUi, ImGui::GetContentRegionAvail().y))));
    });
}

// ---------------------------------------------------------------- status bar, top bar, meters

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const DmrTelemetry& t = a.rx.dmr;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Colour code", !on ? 0 : t.cc >= 0 ? 1 : 0); flowNext(12 * gUi);
    lamp("Slot 1", !on ? 0 : t.slot[0].active ? 1 : 0); flowNext(12 * gUi);
    lamp("Slot 2", !on ? 0 : t.slot[1].active ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("Link", t.link.empty() ? "-" : t.link);
    if (t.cc >= 0) { snprintf(b, sizeof b, "%d", t.cc); ro("CC", b); }
    snprintf(b, sizeof b, "%+.0f Hz", t.cfoHz); ro("CFO", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("SNR", b);
    snprintf(b, sizeof b, "%llu", (unsigned long long)t.calls); ro("Calls", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("FEC ok/bad", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const DmrTelemetry& t = a.rx.dmr;
    l1 = "DMR";
    if (!live(a)) return;
    static const char* st[5] = {"-", "voice", "data", "idle", "ctrl"};
    char b[128];
    if (t.state == 0) snprintf(b, sizeof b, "searching");
    else snprintf(b, sizeof b, "%s  CC %d  S1 %s  S2 %s  SNR %.0f dB", t.link.empty() ? "locked" : t.link.c_str(), t.cc,
                  t.slot[0].active ? st[t.slot[0].state % 5] : "-", t.slot[1].active ? st[t.slot[1].state % 5] : "-", t.snrDb);
    l2 = b;
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("colour code");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(60 * gUi);
    int cc = sc.modeOpt[0] == 0 ? 1 : sc.modeOpt[0] < 0 ? 0 : sc.modeOpt[0];
    if (ImGui::SliderInt("##dcc", &cc, 0, 15)) { sc.modeOpt[0] = cc == 1 ? 0 : cc == 0 ? -1 : cc; changed = true; }
    flowNext(10 * gUi); ImGui::TextDisabled("talkgroups");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(60 * gUi);
    int tg = sc.modeOpt[1] == 0 ? 3 : sc.modeOpt[1];
    if (ImGui::SliderInt("##dtg", &tg, 1, 8)) { sc.modeOpt[1] = tg == 3 ? 0 : tg; changed = true; }
    flowNext(10 * gUi); ImGui::TextDisabled("link");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(130 * gUi);
    static const char* links[3] = {"base station", "direct mode", "mobile"};
    const int lk = std::max(0, std::min(2, (int)sc.modeOpt[2]));
    if (ImGui::BeginCombo("##dlink", links[lk])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(links[i], lk == i)) { sc.modeOpt[2] = i; changed = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Base station: both time slots, continuous.\nDirect mode: one handset, bursts of single calls.\nMobile: a handset talking to a repeater (the uplink).");
    flowNext(10 * gUi); ImGui::TextDisabled("traffic");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(80 * gUi);
    static const char* traffic[3] = {"normal", "quiet", "busy"};
    const int tr = std::max(0, std::min(2, (int)sc.modeOpt[3]));
    if (ImGui::BeginCombo("##dtr", traffic[tr])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(traffic[i], tr == i)) { sc.modeOpt[3] = i; changed = true; }
        ImGui::EndCombo();
    }
    flowNext(10 * gUi); ImGui::TextDisabled("SNR");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(80 * gUi);
    float snr = (float)sc.snrDb;
    if (ImGui::SliderFloat("##dsnr", &snr, 5, 45, "%.0f dB")) { sc.snrDb = snr; changed = true; }
    flowNext(10 * gUi); ImGui::TextDisabled("CFO");
    ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##dcfo", &cfo, -5, 5, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const DmrTelemetry& t = a.rx.dmr;
    out.push_back({"SNR  dB", "%.1f", t.snrDb, 0, 40, t.state == 0 ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 8 ? 2 : 3});
    out.push_back({"LEVEL  dBFS", "%.1f", t.levelDbfs, -100, 0, 0});
    out.push_back({"DEVIATION  Hz", "%.0f", t.devHz, 0, 3000, t.state == 0 ? 0 : (t.devHz > 1500 && t.devHz < 2400) ? 1 : 2});
    out.push_back({"BER  %", "%.2f", t.ber * 100.0, 0, 10, t.state == 0 ? 0 : t.ber < 0.01f ? 1 : t.ber < 0.05f ? 2 : 3});
}

} // namespace

extern const ModeUi kDmrUi;
const ModeUi kDmrUi = {
    .sideTitle = "CALLS",
    .tabName = "Calls",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .stream = false,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
