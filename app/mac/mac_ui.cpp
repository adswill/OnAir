// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MAC television screens: the picture (the analog TV video view), with "scrambled (EuroCrypt)" over it when the service is under conditional
// access, the line and data-eye scopes, the status lamps, the service identification (channel and network name, aspect, access, sound) and
// the options of the test signal. The receiver (MacReceiver) makes the pictures; the telemetry carries the waveforms and the numbers.
#include "app.h"
#include "dect2/mac/mac_rx.h"
#include "dect2/mac/mac_tel.h"
#include <cmath>
#include <cstring>
#include <memory>

namespace {

struct State {
    bool wasRunning = false;
    gfx::Image* img = nullptr;
    int imgW = 0, imgH = 0;
    uint64_t frameSeq = 0;
    std::shared_ptr<const AtvFrame> frame;
    float pushedVol = -1.f;
    bool pushedMute = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 48; }   // the engine reports its standard code minus one

void kv(const App& a, const char* k, const std::string& v, float col = 130) {
    ImGui::TextDisabled("%s", k); kvColumn(col * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}
std::string fmt(const char* f, double v) { char b[64]; snprintf(b, sizeof b, f, v); return b; }

std::string access(const dect2::MacTelemetry& t) {
    if (!t.scrambled) return t.scrambling.empty() ? "clear" : t.scrambling;
    std::string s = "scrambled";
    if (!t.caSystem.empty()) s += " (" + t.caSystem + ")";
    return s;
}

void tick(App& a) {
    const bool run = a.engine.running();
    if (run && (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute)) {   // the sound controls are shared with the other modes
        a.engine.mac().setVolume(a.volume);
        a.engine.mac().setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = run;
}

// ---------------------------------------------------------------- scopes
void scope(const char* id, ImVec2 size, const std::vector<float>& w, float lo, float hi, bool dots) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    const ImVec2 a0(p0.x + 6 * gUi, p0.y + 4 * gUi), a1(p1.x - 6 * gUi, p1.y - 4 * gUi);
    if (a1.x - a0.x < 20 || a1.y - a0.y < 20) return;
    auto Y = [&](float v) { return a1.y - (std::max(lo, std::min(hi, v)) - lo) / (hi - lo) * (a1.y - a0.y); };
    const ImU32 grid = IM_COL32(48, 52, 54, 255);
    for (float g : {0.f, 0.5f, 1.f}) dl->AddLine(ImVec2(a0.x, Y(dots ? (g - 0.5f) * 2.f : g)), ImVec2(a1.x, Y(dots ? (g - 0.5f) * 2.f : g)), grid);
    if (w.size() < 2) return;
    const ImU32 col = IM_COL32(150, 200, 240, 255);
    dl->PushClipRect(a0, a1, true);
    if (dots) {
        for (size_t i = 0; i < w.size(); i++) dl->AddCircleFilled(ImVec2(a0.x + (float)i / (w.size() - 1) * (a1.x - a0.x), Y(w[i])), 1.6f * gUi, col);
    } else {
        std::vector<ImVec2> pts(w.size());
        for (size_t i = 0; i < w.size(); i++) pts[i] = ImVec2(a0.x + (float)i / (w.size() - 1) * (a1.x - a0.x), Y(w[i]));
        dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.3f);
    }
    dl->PopClipRect();
}

// ---------------------------------------------------------------- the picture
void uploadFrame(App& a) {
    if (!live(a) || !gGfx) return;
    auto f = a.engine.mac().frame(S.frameSeq);
    if (!f || f->width <= 0 || f->height <= 0 || f->rgba.size() < (size_t)f->width * f->height * 4) return;
    if (!S.img || S.imgW != f->width || S.imgH != f->height) {
        delete S.img;
        S.img = gGfx->createImage(f->width, f->height, 0xFF000000u);
        S.imgW = f->width; S.imgH = f->height;
    }
    std::vector<uint32_t> px((size_t)f->width * f->height);
    memcpy(px.data(), f->rgba.data(), px.size() * 4);
    S.img->update(0, 0, f->width, f->height, px.data());
    S.frame = f;
}

void picture(App& a, ImVec2 box) {
    const bool on = live(a);
    const dect2::MacTelemetry& t = a.rx.mac;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##macpic", box);
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(0, 0, 0, 255));
    const float ar = (on && t.aspectKnown) ? (t.wide ? 16.f / 9.f : 4.f / 3.f) : (S.img && S.imgH > 0 ? (float)S.imgW / S.imgH : 4.f / 3.f);
    ImVec2 sz = box;
    if (box.x / box.y > ar) sz.x = box.y * ar; else sz.y = box.x / ar;
    const ImVec2 q0(p0.x + (box.x - sz.x) * 0.5f, p0.y + (box.y - sz.y) * 0.5f), q1(q0.x + sz.x, q0.y + sz.y);
    if (S.img && S.frame) {
        dl->AddImage(S.img->texture(), q0, q1);
        if (!on || t.state != 2 || t.scrambled) dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, t.scrambled ? 110 : 150));
    }
    std::string msg;
    if (!a.engine.running()) msg = "stopped";
    else if (!on) msg = "starting";
    else if (t.state == 0) msg = "no MAC signal";
    else if (t.state == 1) msg = "line sync found, waiting for line 625";
    else if (t.scrambled) msg = "scrambled" + (t.caSystem.empty() ? std::string() : " (" + t.caSystem + ")");
    else if (!S.frame) msg = "waiting for a picture";
    if (!msg.empty()) {
        const ImVec2 ts = ImGui::CalcTextSize(msg.c_str());
        const ImVec2 at(p0.x + (box.x - ts.x) * 0.5f, p0.y + (box.y - ts.y) * 0.5f);
        if (t.scrambled && on) dl->AddRectFilled(ImVec2(at.x - 8 * gUi, at.y - 4 * gUi), ImVec2(at.x + ts.x + 8 * gUi, at.y + ts.y + 4 * gUi), IM_COL32(0, 0, 0, 200), 4 * gUi);
        dl->AddText(at, t.scrambled && on ? IM_COL32(240, 190, 90, 255) : IM_COL32(170, 174, 178, 255), msg.c_str());
    }
}

std::string caption(const App& a) {
    const dect2::MacTelemetry& t = a.rx.mac;
    if (!live(a)) return a.engine.running() ? "starting" : "stopped";
    if (t.state == 0) return "searching for a MAC signal";
    char b[240];
    snprintf(b, sizeof b, "%s   %s   %s   %s", t.variantName.c_str(), t.channelName.empty() ? "(name not yet)" : t.channelName.c_str(),
             t.aspectKnown ? (t.wide ? "16:9" : "4:3") : "-", access(t).c_str());
    return b;
}

void tab(App& a) {
    uploadFrame(a);
    const bool on = live(a);
    const dect2::MacTelemetry& t = a.rx.mac;
    const float W = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    {
        ImGui::PushFont(a.mono, 0);
        const std::string cap = caption(a), shown = ellipsize(cap, std::max(40 * gUi, W - 12 * gUi));
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopFont();
        if (shown != cap && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cap.c_str());
    }
    const float rest = ImGui::GetContentRegionAvail().y;
    const bool side = W - rest * 16.f / 9.f > 260 * gUi;
    if (side) {
        const float sw = std::min(W * 0.36f, std::max(260.f * gUi, W - rest * 16.f / 9.f - 8 * gUi));
        picture(a, ImVec2(W - sw - 8 * gUi, rest));
        ImGui::SameLine(0, 8 * gUi);
        ImGui::BeginGroup();
        ImGui::TextDisabled("Line (baseband)");
        static const std::vector<float> none;
        scope("##macline", ImVec2(sw, std::min(150.f * gUi, rest * 0.3f)), on ? t.lineWave : none, -0.1f, 1.1f, false);
        ImGui::TextDisabled("Data burst eye");
        scope("##maceye", ImVec2(sw, std::min(110.f * gUi, rest * 0.22f)), on ? t.eye : none, -1.5f, 1.5f, true);
        ImGui::Spacing();
        kv(a, "data SNR", on && t.state ? fmt("%.1f dB", t.snrDb) : "-", 96);
        kv(a, "video SNR", on && t.state == 2 ? fmt("%.1f dB", t.videoSnrDb) : "-", 96);
        kv(a, "sound", on && t.soundPresent ? fmt("%.0f dBFS", t.soundLevelDb) : on && t.soundScrambled ? "scrambled" : "-", 96);
        ImGui::EndGroup();
    } else {
        const float scopeH = std::min(120.f * gUi, rest * 0.25f);
        picture(a, ImVec2(W, rest - scopeH - 4 * gUi));
        static const std::vector<float> none;
        scope("##macline", ImVec2(W, scopeH), on ? t.lineWave : none, -0.1f, 1.1f, false);
    }
}

// ---------------------------------------------------------------- the other hooks
void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::MacTelemetry& t = a.rx.mac;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Line sync", !on ? 0 : t.state >= 1 ? (t.syncQuality > 0.9f ? 1 : 2) : 0); flowNext(12 * gUi);
    lamp("Frame sync", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("SI", !on ? 0 : t.siSeen ? 1 : t.state == 2 ? 2 : 0); flowNext(12 * gUi);
    lamp("Sound", !on ? 0 : t.soundPresent ? 1 : t.soundScrambled ? 3 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("System", t.state ? t.variantName : "searching");
    if (!t.channelName.empty()) ro("Channel", t.channelName);
    ro("Access", access(t));
    ro("Data SNR", fmt("%.1f dB", t.snrDb));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const dect2::MacTelemetry& t = a.rx.mac;
    l1 = "MAC";
    if (!live(a)) return;
    if (t.state == 0) { l2 = "searching"; return; }
    l1 = t.channelName.empty() ? t.variantName : t.channelName;
    l2 = t.variantName + "  " + (t.aspectKnown ? (t.wide ? "16:9" : "4:3") : "") + "  " + access(t);
}

void panels(App& a) {
    const bool on = live(a);
    const dect2::MacTelemetry& t = a.rx.mac;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 4);
    const float colW = std::max(120.f, (W - 3 * gap) / 3.f);
    static const std::vector<float> none;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup(); captionFit(colW, "Line (baseband)"); scope("##pline", ImVec2(colW, plotH), on ? t.lineWave : none, -0.1f, 1.1f, false); ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup(); captionFit(colW, "Data burst eye"); scope("##peye", ImVec2(colW, plotH), on ? t.eye : none, -1.5f, 1.5f, true); ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Levels");
    kv(a, "black / white", on && t.state == 2 ? fmt("%.2f", t.blackLevel) + " / " + fmt("%.2f V", t.whiteLevel) : "-", 120);
    kv(a, "data level", on && t.state ? fmt("%.2f V", t.dataAmp) : "-", 120);
    kv(a, "deviation", on && t.state ? fmt("%.1f MHz/V", t.deviationMhzPerV) : "-", 120);
    kv(a, "offset", on && t.state ? fmt("%+.0f kHz", t.cfoHz / 1e3) : "-", 120);
    kv(a, "bit errors", on && t.state == 2 ? fmt("%.1e", t.bitErrorRate) : "-", 120);
    ImGui::EndGroup();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::MacTelemetry& t = a.rx.mac;
    char b[200];
    ImGui::BeginChild("##mac_kv", ImVec2(0, 0));
    if (ImGui::BeginTable("##mackv", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Signal");
        kv(a, "state", t.state == 2 ? "frame sync, decoding" : t.state == 1 ? "line sync" : "searching");
        kv(a, "system", t.variantName.empty() ? "-" : t.variantName);
        kv(a, "data SNR", fmt("%.1f dB", t.snrDb));
        kv(a, "video SNR", t.state == 2 ? fmt("%.1f dB", t.videoSnrDb) : "-");
        kv(a, "offset", fmt("%+.1f kHz", t.cfoHz / 1e3));
        kv(a, "deviation", fmt("%.1f MHz/V", t.deviationMhzPerV));
        kv(a, "spectrum", t.inverted ? "inverted (corrected)" : "normal");
        snprintf(b, sizeof b, "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb); kv(a, "input", b);
        ImGui::Spacing();
        ImGui::TextDisabled("Sync");
        snprintf(b, sizeof b, "%.2f Hz (%+.1f ppm)", t.lineHz, t.lineErrPpm); kv(a, "line rate", t.lineHz > 0 ? b : "-");
        kv(a, "line sync", fmt("%.0f %%", t.syncQuality * 100));
        snprintf(b, sizeof b, "%llu found, %llu frames", (unsigned long long)t.fswFound, (unsigned long long)t.frames); kv(a, "frame sync", b);
        kv(a, "frame count", t.fcnt >= 0 ? std::to_string(t.fcnt) : "-");
        kv(a, "bit errors", t.state == 2 ? fmt("%.2e (frame sync word)", t.bitErrorRate) : "-");

        ImGui::TableNextColumn();
        ImGui::TextDisabled("Service");
        kv(a, "channel", t.channelName.empty() ? "-" : t.channelName);
        kv(a, "network", t.networkName.empty() ? "-" : t.networkName);
        kv(a, "country", t.country.empty() ? "-" : t.country);
        snprintf(b, sizeof b, "%04X", t.chid); kv(a, "channel id", t.state == 2 ? b : "-");
        kv(a, "aspect", t.aspectKnown ? (t.wide ? "16:9" : "4:3") : "-");
        kv(a, "access", access(t));
        if (t.scrambled) kv(a, "ECM packets", std::to_string(t.ecmPackets));
        ImGui::Spacing();
        ImGui::TextDisabled("Packets");
        snprintf(b, sizeof b, "%llu (%llu corrected, %llu dummy)", (unsigned long long)t.packets, (unsigned long long)t.packetsCorrected, (unsigned long long)t.dummyPackets); kv(a, "headers", b);
        snprintf(b, sizeof b, "%llu good, %llu bad", (unsigned long long)t.siGroupsOk, (unsigned long long)t.siGroupsBad); kv(a, "SI data groups", b);
        ImGui::Spacing();
        ImGui::TextDisabled("Sound");
        kv(a, "address", t.soundAddress >= 0 ? std::to_string(t.soundAddress) : "-");
        kv(a, "coding", t.soundMode.empty() ? "-" : t.soundMode);
        snprintf(b, sizeof b, "%llu packets, %llu errors", (unsigned long long)t.soundPackets, (unsigned long long)t.soundErrors); kv(a, "packets", b);
        kv(a, "level", t.soundPresent ? fmt("%.1f dBFS", t.soundLevelDb) : "-");
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void list(App& a) {
    const dect2::MacTelemetry& t = a.rx.mac;
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see the channel"); ImGui::PopTextWrapPos(); return; }
    if (t.state == 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("searching for D2-MAC / D-MAC line sync"); ImGui::PopTextWrapPos(); return; }
    kv(a, "channel", t.channelName.empty() ? "-" : t.channelName, 90);
    kv(a, "system", t.variantName, 90);
    kv(a, "aspect", t.aspectKnown ? (t.wide ? "16:9" : "4:3") : "-", 90);
    kv(a, "access", access(t), 90);
    kv(a, "sound", t.soundPresent ? "digital, 32 kHz" : t.soundScrambled ? "scrambled" : "-", 90);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Tune the centre of the FM channel (satellite IF behind an LNB). EuroCrypt and other conditional access are detected, not descrambled. B-MAC and C-MAC are not decoded.");
    ImGui::PopTextWrapPos();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    auto combo = [&](const char* id, int& v, std::initializer_list<const char*> names, float w) {
        ImGui::SetNextItemWidth(w * gUi);
        int i = 0; const char* cur = "";
        for (const char* n : names) { if (i == v) cur = n; i++; }
        if (ImGui::BeginCombo(id, cur)) {
            i = 0;
            for (const char* n : names) { if (ImGui::Selectable(n, v == i)) { v = i; changed = true; } i++; }
            ImGui::EndCombo();
        }
    };
    ImGui::TextDisabled("system"); ImGui::SameLine(0, 5 * gUi);
    combo("##macsys", sc.modeOpt[0], {"D2-MAC", "D-MAC"}, 90);
    sameLineIf(160 * gUi, 8 * gUi);
    bool scr = sc.modeOpt[1] != 0;
    if (ImGui::Checkbox("EuroCrypt", &scr)) { sc.modeOpt[1] = scr; changed = true; }
    sameLineIf(80 * gUi, 8 * gUi);
    bool wide = sc.modeOpt[2] != 0;
    if (ImGui::Checkbox("16:9", &wide)) { sc.modeOpt[2] = wide; changed = true; }
    sameLineIf(200 * gUi, 8 * gUi);
    ImGui::TextDisabled("C/N"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cn = (float)std::min(sc.snrDb, 40.0);
    if (ImGui::SliderFloat("##maccn", &cn, 6, 40, cn >= 40 ? "no noise" : "%.0f dB")) { sc.snrDb = cn >= 40 ? 100 : cn; changed = true; }
    sameLineIf(200 * gUi, 8 * gUi);
    ImGui::TextDisabled("offset"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##maccfo", &cfo, -500, 500, "%.0f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    sameLineIf(200 * gUi, 8 * gUi);
    ImGui::TextDisabled("picture"); ImGui::SameLine(0, 5 * gUi);
    combo("##macpat", sc.modeOpt[3], {"test card", "colour bars", "grey ramp"}, 110);
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const dect2::MacTelemetry& t = a.rx.mac;
    out.push_back({"DATA SNR  dB", "%.1f", on ? t.snrDb : 0, 0, 30, !on ? 0 : t.snrDb >= 15 ? 1 : t.snrDb >= 9 ? 2 : 3});
    out.push_back({"VIDEO SNR  dB", "%.1f", on ? t.videoSnrDb : 0, 0, 50, !on ? 0 : t.videoSnrDb >= 35 ? 1 : t.videoSnrDb >= 25 ? 2 : 3});
    out.push_back({"LINE SYNC  %", "%.0f", on ? t.syncQuality * 100 : 0, 0, 100, !on ? 0 : t.syncQuality > 0.95f ? 1 : t.syncQuality > 0.6f ? 2 : 3});
    out.push_back({"SOUND  dBFS", "%.1f", on ? t.soundLevelDb : -100, -80, 0, !on ? 0 : t.soundPresent ? 1 : 2});
}

} // namespace

extern const ModeUi kMacUi;
const ModeUi kMacUi = {
    .sideTitle = "CHANNEL",
    .tabName = "Video",
    .tabIcon = Ic::Tv,
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
