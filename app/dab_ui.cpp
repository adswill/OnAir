// DAB / DAB+ digital radio panels.
#include "app.h"

// DAB / DAB+ screens (included by main.cpp): channel plan, status lines, constellation panels, station list and player,
// the Radio and Ensemble tabs, and the Band III scanner.

// ------------------------------------------------------------------ Band III channel plan
struct DabChan { const char* name; double mhz; };
static const DabChan kDabCh[] = {
    {"5A", 174.928}, {"5B", 176.640}, {"5C", 178.352}, {"5D", 180.064}, {"6A", 181.936}, {"6B", 183.648}, {"6C", 185.360}, {"6D", 187.072},
    {"7A", 188.928}, {"7B", 190.640}, {"7C", 192.352}, {"7D", 194.064}, {"8A", 195.936}, {"8B", 197.648}, {"8C", 199.360}, {"8D", 201.072},
    {"9A", 202.928}, {"9B", 204.640}, {"9C", 206.352}, {"9D", 208.064}, {"10A", 209.936}, {"10B", 211.648}, {"10C", 213.360}, {"10D", 215.072},
    {"11A", 216.928}, {"11B", 218.640}, {"11C", 220.352}, {"11D", 222.064}, {"12A", 223.936}, {"12B", 225.648}, {"12C", 227.360}, {"12D", 229.072},
    {"13A", 230.784}, {"13B", 232.496}, {"13C", 234.208}, {"13D", 235.776}, {"13E", 237.488}, {"13F", 239.200}};
static constexpr int kNumDabCh = (int)(sizeof kDabCh / sizeof *kDabCh);

const char* dabChannelName(double mhz) {
    for (const auto& c : kDabCh) if (std::fabs(c.mhz - mhz) < 0.05) return c.name;
    return nullptr;
}

// A selector of DAB channels, used instead of the bandwidth box in DAB mode. Returns true when the frequency changed.
bool dabChannelCombo(App& a) {
    const char* cur = dabChannelName(a.freqMhz);
    char lbl[48];
    snprintf(lbl, sizeof lbl, cur ? "%s  %.3f MHz" : "Channel", cur ? cur : "", a.freqMhz);
    bool changed = false;
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::BeginCombo("##dabch", lbl)) {
        for (const auto& c : kDabCh) {
            char l[48];
            snprintf(l, sizeof l, "%-4s %.3f MHz", c.name, c.mhz);
            if (ImGui::Selectable(l, cur && !strcmp(cur, c.name))) { a.freqMhz = c.mhz; changed = true; }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("DAB Band III channels 5A to 13F. Every DAB multiplex (ensemble) sits on one of these.");
    return changed;
}

// ------------------------------------------------------------------ status lines
void dabStatus(App& a) {
    const bool run = a.engine.running();
    const SignalStats& st = a.spec.stats;
    const DabTelemetry& d = a.rx.dab;
    const bool live = run && a.rx.standard == 3;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !live ? 0 : d.state == 2 ? 1 : 0); flowNext(12 * gUi);
    lamp("PRS", !live ? 0 : d.cirPeak > 40 ? 1 : d.cirPeak > 15 ? 2 : 0); flowNext(12 * gUi);
    lamp("FIC", !live ? 0 : d.ficRecentOk >= 11 ? 1 : d.ficRecentOk > 0 ? 2 : d.state == 2 ? 3 : 0); flowNext(12 * gUi);
    lamp("Ensemble", !live ? 0 : d.ensemble ? 1 : d.fibOk ? 2 : 0); flowNext(12 * gUi);
    lamp("MSC", !live || d.audio.sub < 0 ? 0 : d.audio.superframesOk > 0 && d.audio.superframesBad * 4 <= d.audio.superframesOk ? 1 : d.audio.frames > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Audio", !live || d.audio.sub < 0 ? 0 : d.audio.decoding ? 1 : 2, (int)Ic::Speaker); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val, ImVec4 col = ImVec4(0.93f, 0.95f, 0.97f, 1)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextColored(col, "%s", val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!run) ro("State", "stopped", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (!live) ro("State", "starting", ImVec4(0.6f, 0.64f, 0.68f, 1));
    else if (d.state == 2) ro("State", "Locked", ImVec4(0.35f, 0.90f, 0.45f, 1));
    else ro("State", "searching", ImVec4(0.6f, 0.64f, 0.68f, 1));
    if (live && d.state == 2) {
        ro("Mode", "DAB mode I");
        if (d.ensemble) {
            ro("Ensemble", d.ensembleLabel.empty() ? "-" : d.ensembleLabel);
            snprintf(b, sizeof b, "%d", d.services); ro("Stations", b);
        } else ro("Ensemble", "reading...", ImVec4(0.6f, 0.64f, 0.68f, 1));
    }
    flowBreak();
    if (live && d.state == 2) {
        snprintf(b, sizeof b, "%+.1f Hz", d.cfoHz); ro("CFO", b);
        snprintf(b, sizeof b, "%.1f dB", d.snrDb); ro("SNR", b);
        const double tot = (double)(d.fibOk + d.fibBad);
        snprintf(b, sizeof b, "%.1f%%", tot > 0 ? 100.0 * (double)d.fibOk / tot : 0.0); ro("FIC ok", b);
    }
    if (run) { snprintf(b, sizeof b, "%.3f Msps", a.engine.sampleRate() / 1e6); ro("fs", b); }
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Gauge, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Level"); ImGui::SameLine(0, 5 * gUi);
    {
        const ImU32 col = adc == AdcStatus::Overload ? IM_COL32(176, 66, 58, 255) : adc == AdcStatus::Good ? pal::remap(IM_COL32(40, 112, 150, 255)) : IM_COL32(176, 130, 48, 255);
        snprintf(b, sizeof b, run ? "%.1f dBFS" : "-", st.rmsDbfs);
        gaugePill(130, run ? (st.rmsDbfs + 60.f) / 60.f : 0.f, col, b);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("ADC level (rms). %s\npeak %.2f   clip %.3f%%   DC %+.3f / %+.3f", adcAdvice(adc).c_str(), st.peak, st.clipFraction * 100, st.dcI, st.dcQ);
    }
    flowNext(15 * gUi);
    ImGui::AlignTextToFramePadding();
    iconInline(Ic::Signal, iconDim(), 0.9f); ImGui::SameLine(0, 4 * gUi); ImGui::TextDisabled("Signal"); ImGui::SameLine(0, 5 * gUi);
    {
        const float t = live && d.state == 2 ? std::min(1.f, std::max(0.f, (float)((d.snrDb - 4.0) / 18.0))) : 0.f;
        const ImU32 col = t < 0.25f ? IM_COL32(176, 66, 58, 255) : t < 0.5f ? IM_COL32(176, 130, 48, 255) : pal::remap(IM_COL32(40, 112, 150, 255));
        snprintf(b, sizeof b, live && d.state == 2 ? "%.0f%%  %s" : "-", t * 100, t > 0.8f ? "excellent" : t > 0.55f ? "good" : t > 0.3f ? "fair" : "poor");
        gaugePill(130, t, col, b);
        if (ImGui::IsItemHovered() && live) ImGui::SetTooltip("DAB needs roughly 10 dB SNR for error-free audio.\nSNR %.1f dB, FIC blocks ok %d of 12 in the last frame.", d.snrDb, d.ficRecentOk);
    }
    flowNext(15 * gUi);
    if (run) { snprintf(b, sizeof b, "%llu", (unsigned long long)a.engine.droppedSamples()); ro("dropped", b, a.engine.droppedSamples() ? ImVec4(0.95f, 0.45f, 0.3f, 1) : ImVec4(0.93f, 0.95f, 0.97f, 1)); }
    if (run && a.devices[a.devIdx].kind != DeviceInfo::File) {
        ImGui::AlignTextToFramePadding();
        if (adc == AdcStatus::Overload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1), "ADC OVERLOAD - %s", a.agcOn ? "AGC is lowering the gain" : "reduce the gain (or enable AGC)"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::Low) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC level low - raise the gain"); ImGui::PopTextWrapPos(); }
        else if (adc == AdcStatus::NoSignal) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.15f, 1), "ADC sees almost nothing - antenna / gain?"); ImGui::PopTextWrapPos(); }
        else flowBreak();
    } else flowBreak();
}

// ------------------------------------------------------------------ constellation row
void dabHistory(App& a) {
    if (a.dabStation >= 0 && a.dabMode && a.engine.running()) { a.engine.dabSelect(a.dabStation); a.engine.dabAudio().setVolume(a.volume); a.dabStation = -1; }
    static uint64_t lastSeq = 0;
    if (a.rx.standard != 3 || a.rx.seq == lastSeq) return;
    lastSeq = a.rx.seq;
    auto push = [](std::deque<float>& dq, float v) { dq.push_back(v); if (dq.size() > 600) dq.pop_front(); };
    if (a.rx.dab.state == 2) { push(a.dabSnrH, (float)a.rx.dab.snrDb); push(a.dabFicH, (float)a.rx.dab.ficRecentOk); }
}

void dabPanels(App& a) {
    const DabTelemetry& d = a.rx.dab;
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(90.f, availH - 26.f - ImGui::GetFrameHeight());
    const float sqW = std::min(plotH, std::max(120.f, availW * 0.25f));                    // the square plot keeps its own width,
    const float colW = std::max(120.f, (availW - 5 * gap - sqW) / 3.f);                    // the other three share the rest
    const ImVec2 sz(colW, plotH);
    const ImVec2 sq(sqW, sqW);        // the cell plot stays square
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap);
    ImGui::BeginGroup();
    captionFit(sqW, "DQPSK cells (%zu)", d.constellation.size());
    scatter("##d1", d.constellation, sq, 1.8, pal::accent(0.40f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Impulse response (echoes)");
    if (plt::BeginPlot("##dcir", sz, plt::Flags_NoLegend | plt::Flags_NoTitle)) {
        plt::SetupAxes("us", nullptr, 0, plt::AxisFlags_NoTickLabels);
        if (!d.cir.empty()) {
            const int n = (int)d.cir.size();
            int pk = 0;
            for (int i = 1; i < n; i++) if (d.cir[(size_t)i] > d.cir[(size_t)pk]) pk = i;
            std::vector<double> x(n), y(n);
            for (int i = 0; i < n; i++) {
                int rel = i - pk;
                if (rel > n / 2) rel -= n; else if (rel < -n / 2) rel += n;
                x[(size_t)i] = rel * (1e6 / 2.048e6);
                y[(size_t)i] = d.cir[(size_t)i] / std::max(1e-6f, d.cir[(size_t)pk]);
            }
            // plot in delay order
            std::vector<int> order(n);
            for (int i = 0; i < n; i++) order[(size_t)i] = i;
            std::sort(order.begin(), order.end(), [&](int p, int q) { return x[(size_t)p] < x[(size_t)q]; });
            std::vector<double> xs2(n), ys2(n);
            for (int i = 0; i < n; i++) { xs2[(size_t)i] = x[(size_t)order[(size_t)i]]; ys2[(size_t)i] = y[(size_t)order[(size_t)i]]; }
            plt::SetupAxisLimits(plt::X1, -60, 120, plt::Cond_Always);
            plt::SetupAxisLimits(plt::Y1, 0, 1.05, plt::Cond_Always);
            plt::Spec sp; sp.LineColor = pal::accent(); sp.LineWeight = 1.2f;
            plt::PlotLine("cir", xs2.data(), ys2.data(), n, sp);
        }
        plt::EndPlot();
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "SNR (dB)");
    historyPlot("##dsnr", "dB", a.dabSnrH, sz);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "FIC blocks ok per frame (12 = all)");
    historyPlot("##dfic", "blocks", a.dabFicH, sz);
    ImGui::EndGroup();
}

// ------------------------------------------------------------------ stations (right panel)
std::string dabSubText(const DabEnsemble& e, int sub) {
    auto it = e.subs.find(sub);
    if (it == e.subs.end()) return "";
    const DabSubchannel& s = it->second;
    char b[96];
    if (!s.eep) snprintf(b, sizeof b, "sub %d, UEP %d", sub, s.uepIndex);
    else snprintf(b, sizeof b, "sub %d, %d kbit/s, EEP %d-%c", sub, s.bitrate, s.level + 1, s.option ? 'B' : 'A');
    return b;
}

void dabSelectStation(App& a, int sub, bool play) {
    if (play) {
        a.engine.dabSelect(sub);
        a.engine.dabAudio().setVolume(a.volume);
        a.engine.dabAudio().setMuted(a.muted);
    } else a.engine.dabSelect(-1);
}

void dabStations(App& a) {
    const bool run = a.engine.running();
    const DabEnsemble ens = a.engine.dabEnsemble();
    const int cur = a.engine.dabSelected();
    sectionHeader(Ic::Radio, "Stations");
    const float cardH = 54;
    ImGui::BeginChild("dabsvcs", ImVec2(0, std::max(140.f * gUi, ImGui::GetContentRegionAvail().y * 0.46f)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ens.services.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(run ? "reading the ensemble information..." : "start the receiver to see the stations"); ImGui::PopTextWrapPos(); }
    std::vector<const DabService*> list;
    for (const auto& kv : ens.services) list.push_back(&kv.second);
    for (const DabService* sv : list) {
        const DabComponent* ac = sv->audio();
        const bool sel = ac && ac->subId == cur;
        ImGui::PushID((int)sv->sid);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, cardH)) && ac;
        const bool hov = ImGui::IsItemHovered();
        if (clicked) dabSelectStation(a, ac->subId, !sel);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? IM_COL32(30, 36, 40, 255) : hov ? IM_COL32(26, 27, 29, 255) : IM_COL32(16, 17, 19, 255), 4.f);
        dl->AddRect(p, ImVec2(p.x + w, p.y + cardH - 4), sel ? pal::remap(IM_COL32(115, 184, 209, 255)) : IM_COL32(48, 50, 52, 255), 4.f, 0, sel ? 1.6f : 1.f);
        icons::draw(sel ? Ic::Speaker : Ic::Radio, ImVec2(p.x + 20, p.y + 15), 17.f, sel ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(120, 136, 156, 255), dl);
        const std::string name = sv->label.empty() ? "service " + std::to_string(sv->sid) : sv->label;
        dl->AddText(a.ui, 16.f, ImVec2(p.x + 34, p.y + 6), IM_COL32(245, 247, 250, 255), name.c_str());
        float x = p.x + 10;
        const float ty = p.y + 30;
        if (ac) {
            x += tagAt(dl, ImVec2(x, ty), sv->dabPlus() ? "DAB+" : "DAB", IM_COL32(34, 74, 108, 255)) + 4;
            auto it = ens.subs.find(ac->subId);
            if (it != ens.subs.end() && it->second.bitrate > 0) { char k[24]; snprintf(k, sizeof k, "%d kbit/s", it->second.bitrate); x += tagAt(dl, ImVec2(x, ty), k, IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)) + 4; }
            if (it != ens.subs.end() && !it->second.eep) tagAt(dl, ImVec2(x, ty), "UEP: not supported yet", IM_COL32(112, 52, 50, 255));
        } else tagAt(dl, ImVec2(x, ty), "data", IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255));
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::Spacing();
    const DabAudioStats au = a.rx.dab.audio;
    sectionHeader(Ic::Play, "Player");
    if (cur < 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("click a station to play it"); ImGui::PopTextWrapPos(); }
    else if (ImGui::BeginTable("dkv", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 88 * gUi);
        auto row = [&](Ic ic, const char* k, const char* fmt, auto... args) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(ic, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("%s", k); ImGui::TableNextColumn();
            char b[120]; snprintf(b, sizeof b, fmt, args...); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos();   // wraps in a narrow column
        };
        row(Ic::Speaker, "Audio", "%s  %dk  %.0f kHz %s", au.codec.empty() ? "-" : au.codec.c_str(), au.bitrate, au.sampleRate / 1000.0, au.channels == 1 ? "mono" : "stereo");
        row(Ic::Clock, "Buffer", "%d ms", au.bufferedMs);
        row(Ic::Layers, "Frames", "%llu superframes ok / %llu bad", (unsigned long long)au.superframesOk, (unsigned long long)au.superframesBad);
        row(Ic::Warning, "Errors", "%llu bytes fixed, %llu audio frames bad", (unsigned long long)au.rsCorrected, (unsigned long long)au.auBad);
        ImGui::TableNextRow(); ImGui::TableNextColumn(); iconInline(a.muted ? Ic::Mute : Ic::Speaker, iconDim(), 0.9f); ImGui::SameLine(0, 5 * gUi); ImGui::TextDisabled("Volume"); ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        float vol = a.volume * 100.f;
        if (ImGui::SliderFloat("##dvol", &vol, 0, 100, "%.0f %%")) { a.volume = vol / 100.f; a.engine.dabAudio().setVolume(a.volume); }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    {   // a "now playing" box where the video preview sits in the other modes
        const float vh = std::max(60.f, ImGui::GetContentRegionAvail().y - 30.f);
        ImGui::BeginChild("dabnow", ImVec2(0, vh), ImGuiChildFlags_Borders);
        std::string name = "no station";
        std::string dls;
        for (const auto& kv : ens.services) { const DabComponent* ac = kv.second.audio(); if (ac && ac->subId == cur) { name = kv.second.label; dls = kv.second.dls; } }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        if (!pal::dev()) icons::draw(Ic::Radio, ImVec2(p.x + cw * 0.5f, p.y + 28), 36.f, cur >= 0 ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(70, 80, 92, 255));
        ImGui::Dummy(ImVec2(1 * gUi, (pal::dev() ? 6 : 52) * gUi));
        const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
        ImGui::SetCursorPosX(std::max(0.f, (cw - ts.x) * 0.5f));
        ImGui::TextUnformatted(name.c_str());
        if (!dls.empty()) { ImGui::SetCursorPosX(8); ImGui::PushTextWrapPos(cw - 8); ImGui::TextDisabled("%s", dls.c_str()); ImGui::PopTextWrapPos(); }
        ImGui::EndChild();
    }
}

// ------------------------------------------------------------------ tabs
void dabRadioTab(App& a) {
    const DabEnsemble ens = a.engine.dabEnsemble();
    const int cur = a.engine.dabSelected();
    if (ens.services.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "waiting for the ensemble information (a second or two)..." : "start the receiver on a DAB channel (5A to 13F)"); ImGui::PopTextWrapPos(); } return; }
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(a.ui, 20.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", ens.label.empty() ? "DAB ensemble" : ens.label.c_str());
    ImGui::PopFont();
    {   // what does not fit goes on the next line (a narrow tab)
        char info[64]; snprintf(info, sizeof info, "%zu stations   ensemble id %04X", ens.services.size(), ens.eid);
        sameLineIf(ImGui::CalcTextSize(info).x, 12 * gUi);
        ImGui::TextDisabled("%s", info);
    }
    if (ens.utc) {
        time_t t = (time_t)ens.utc; struct tm m; dect2::gmTime(t, &m); char tb[64]; strftime(tb, sizeof tb, "broadcast time %a %d %b %Y %H:%M UTC", &m);
        sameLineIf(ImGui::CalcTextSize(tb).x, 12 * gUi); ImGui::TextDisabled("%s", tb);
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("dabtbl", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
        ImGui::TableSetupColumn("Station");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Stream");
        ImGui::TableSetupColumn("Id");
        ImGui::TableSetupColumn("Now");
        ImGui::TableHeadersRow();
        for (const auto& kv : ens.services) {
            const DabService& sv = kv.second;
            const DabComponent* ac = sv.audio();
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26 * gUi);
            ImGui::TableNextColumn();
            ImGui::PushID((int)sv.sid);
            if (ac) {
                const bool sel = ac->subId == cur;
                if (iconFlat(sel ? Ic::Stop : Ic::Play, sel ? "Stop" : "Play this station", sel)) dabSelectStation(a, ac->subId, !sel);
            }
            ImGui::PopID();
            ImGui::PushTextWrapPos(0);   // the cells wrap in a narrow tab
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(sv.label.empty() ? "-" : sv.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ac ? (sv.dabPlus() ? "DAB+ (HE-AAC)" : "DAB (MP2)") : "data");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ac ? dabSubText(ens, ac->subId).c_str() : "");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextDisabled("%04X", sv.sid); ImGui::PopFont();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", sv.dls.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
}

void dabEnsembleTab(App& a) {
    const DabTelemetry& d = a.rx.dab;
    const DabEnsemble ens = a.engine.dabEnsemble();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("DAB receiver, transmission mode I: null symbol and phase reference sync, DQPSK, fast information channel, EEP sub-channels, DAB+ superframes"); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    auto row = [&](const char* k, const char* fmt, auto... v) {   // a long value wraps under itself in a narrow tab
        ImGui::TextDisabled("%s", k); ImGui::SameLine(std::min(190 * gUi, ImGui::GetContentRegionAvail().x * 0.45f));
        ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont();
    };
    row("frame sync", "%s", d.state == 2 ? "locked" : "searching");
    row("phase reference", "correlation peak %.0f times the noise floor", d.cirPeak);
    row("carrier offset", "%+.1f Hz", d.cfoHz);
    row("signal quality", "%.1f dB (DQPSK cluster scatter)", d.snrDb);
    row("frames", "%llu (96 ms each)", (unsigned long long)d.frames);
    row("fast information", "%llu blocks ok, %llu bad, %d of 12 ok in the last frame", (unsigned long long)d.fibOk, (unsigned long long)d.fibBad, d.ficRecentOk);
    row("ensemble", "%s  (id %04X)", ens.label.c_str(), ens.eid);
    ImGui::Spacing();
    sectionHeader(Ic::Layers, "Sub-channels");
    if (ImGui::BeginTable("dsub", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Id"); ImGui::TableSetupColumn("Start (CU)"); ImGui::TableSetupColumn("Size (CU)"); ImGui::TableSetupColumn("Protection"); ImGui::TableSetupColumn("kbit/s"); ImGui::TableSetupColumn("Carries");
        ImGui::TableHeadersRow();
        for (const auto& kv : ens.subs) {
            const DabSubchannel& s = kv.second;
            std::string who;
            for (const auto& sv : ens.services) for (const auto& c : sv.second.comps) if (c.subId == s.id) who += (who.empty() ? "" : ", ") + sv.second.label;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", s.id);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.start);
            ImGui::TableNextColumn(); ImGui::Text("%d", s.size);
            ImGui::TableNextColumn(); ImGui::Text(s.eep ? "EEP %d-%c" : "UEP %d", s.eep ? s.level + 1 : s.uepIndex, s.option ? 'B' : 'A');
            ImGui::TableNextColumn(); ImGui::Text("%d", s.bitrate);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(who.c_str());
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ scanner (Band III, one ensemble per channel)
void dabScanTab(App& a) {
    App::DabScan& s = a.dabScan;
    const bool run = a.engine.running();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Tunes through the 38 Band III channels (5A to 13F) and lists the ensembles with their stations. About a minute."); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    if (!s.running) {
        ImGui::BeginDisabled(!run);
        if (iconButton(Ic::Scan, "Scan Band III", pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) {
            s.running = true; s.idx = -1; s.t0 = 0; s.results.clear(); s.savedFreq = a.freqMhz;
            a.engine.dabSelect(-1);
        }
        ImGui::EndDisabled();
        if (!run) { ImGui::SameLine(); ImGui::TextDisabled("start the receiver first"); }
    } else {
        if (iconButton(Ic::Stop, "Stop scan", IM_COL32(112, 48, 48, 255), IM_COL32(146, 62, 62, 255))) { s.running = false; a.freqMhz = s.savedFreq; }
        ImGui::SameLine(0, 12 * gUi);
        ImGui::TextDisabled("channel %d of %d", std::max(0, s.idx) + 1, kNumDabCh);
        ImGui::ProgressBar((float)(std::max(0, s.idx)) / kNumDabCh, ImVec2(220, ImGui::GetFrameHeight() - 6));
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("dscan", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("MHz", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Ensemble"); ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthFixed, 70); ImGui::TableSetupColumn("Stations");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.results.size(); i++) {
            const auto& r = s.results[i];
            if (!r.found) continue;
            ImGui::TableNextRow(ImGuiTableRowFlags_None, 26 * gUi);
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(r.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                a.freqMhz = r.mhz; a.tune.centerHz = r.mhz * 1e6;
                if (a.engine.running()) { a.engine.retuneReset(a.tune); a.peak.clear(); }
                s.running = false;
            }
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", r.mhz);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f dB", r.snr);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", r.stations.c_str());
        }
        ImGui::EndTable();
    }
    if (!s.running && s.results.empty()) ImGui::TextDisabled("no scan yet");
}

// The scan state machine, called every frame
void dabScanStep(App& a) {
    App::DabScan& s = a.dabScan;
    if (!s.running) return;
    if (!a.engine.running()) { s.running = false; return; }
    const double now = ImGui::GetTime();
    auto tuneTo = [&](int idx) {
        a.freqMhz = kDabCh[idx].mhz;
        a.tune.centerHz = a.freqMhz * 1e6;
        a.engine.retuneReset(a.tune);
        a.peak.clear();
        s.idx = idx; s.t0 = now; s.lockT = 0;
    };
    if (s.idx < 0) { tuneTo(0); return; }
    const DabTelemetry& d = a.rx.dab;
    const bool locked = a.rx.standard == 3 && d.state == 2;
    if (locked && s.lockT == 0) s.lockT = now;
    const DabEnsemble ens = a.engine.dabEnsemble();
    bool done = false;
    // finished when the ensemble is known with all labels, or a few seconds after sync, or no sync after the search time
    bool labelled = ens.valid && !ens.label.empty();
    for (const auto& kv : ens.services) if (kv.second.label.empty()) labelled = false;
    if (locked && labelled && now - s.lockT > 0.6) done = true;
    else if (locked && now - s.lockT > 5.0) done = true;
    else if (!locked && now - s.t0 > 2.2) done = true;
    if (!done) return;
    App::DabScan::Res r;
    r.name = kDabCh[s.idx].name; r.mhz = kDabCh[s.idx].mhz; r.found = locked && ens.valid;
    if (r.found) {
        r.label = ens.label; r.snr = (float)d.snrDb;
        for (const auto& kv : ens.services) { if (!r.stations.empty()) r.stations += ", "; r.stations += kv.second.label.empty() ? "?" : kv.second.label; }
        a.engine.log("DAB scan: " + r.name + " " + ens.label + " (" + std::to_string(ens.services.size()) + " stations)");
    }
    s.results.push_back(r);
    if (s.idx + 1 >= kNumDabCh) { s.running = false; a.freqMhz = s.savedFreq; a.tune.centerHz = a.freqMhz * 1e6; a.engine.retuneReset(a.tune); a.engine.log("DAB scan finished"); }
    else tuneTo(s.idx + 1);
}

