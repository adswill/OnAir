// Analog TV screens: the picture with a line scope, the status lamps, the vertical-interval and level panels, the receiver readouts and the options
// of the test signal. The receiver (AtvReceiver) makes the pictures; the telemetry carries the waveforms and the numbers.
#include "app.h"
#include "dect2/atv_std.h"
#include <cmath>
#include <memory>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    gfx::Image* img = nullptr;                  // the picture as a texture
    int imgW = 0, imgH = 0;
    uint64_t frameSeq = 0;
    std::shared_ptr<const AtvFrame> frame;      // the last picture (for the caption)
    int deint = 0;                              // 0 weave, 1 bob
    int sysHint = -1, colHint = -1;             // -1 automatic
    bool colourOn = true;
    float saturation = 1.f, hue = 0.f;
    int detector = 0;                           // 0 automatic, 1 envelope only
    int pushedDeint = -1, pushedSys = -2, pushedCol = -2, pushedDet = -1, pushedMod = -1;
    bool pushedColourOn = true;
    float pushedSat = -1.f, pushedHue = -999.f;
    float pushedVol = -1.f;
    bool pushedMute = false;
};
State S;

// engine standard 9 is analog TV
bool live(const App& a) { return a.engine.running() && a.rx.standard == 9; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.deint = (int)plat::prefs().getI("atvDeint", 0) == 1 ? 1 : 0;
}

const ImVec4 kDim(0.62f, 0.65f, 0.68f, 1);

// name and colour system hints, in the order of AtvSys and AtvColourKind
const char* const kSysNames[] = {"B (7 MHz)", "G (8 MHz)", "I", "D/K", "M", "N"};
const char* const kColNames[] = {"mono", "PAL", "NTSC", "SECAM"};

void pushControls(App& a) {
    AtvReceiver& rx = a.engine.atv();
    if (S.pushedDeint != S.deint) { rx.setDeinterlace(S.deint); S.pushedDeint = S.deint; }
    if (S.pushedSys != S.sysHint || S.pushedCol != S.colHint) { rx.setStandard(S.sysHint, S.colHint); S.pushedSys = S.sysHint; S.pushedCol = S.colHint; }
    if (S.pushedColourOn != S.colourOn) { rx.setColour(S.colourOn); S.pushedColourOn = S.colourOn; }
    if (S.pushedSat != S.saturation) { rx.setSaturation(S.saturation); S.pushedSat = S.saturation; }
    if (S.pushedHue != S.hue) { rx.setHue(S.hue); S.pushedHue = S.hue; }
    if (S.pushedDet != S.detector) { rx.setDetector(S.detector); S.pushedDet = S.detector; }
    const int mod = a.atvFm ? 1 : 0;
    if (S.pushedMod != mod) { rx.setModulation(mod); S.pushedMod = mod; }
}

// the sound controls are shared with the other modes: push them to the receiver when they change or it starts
void tick(App& a) {
    loadState();
    const bool run = a.engine.running();
    if (run) {
        if (!S.wasRunning) {   // a new run: send everything again
            S.pushedDeint = -1; S.pushedSys = -2; S.pushedCol = -2; S.pushedDet = -1; S.pushedMod = -1; S.pushedSat = -1.f; S.pushedHue = -999.f; S.pushedVol = -1.f;
            S.pushedColourOn = !S.colourOn;
        }
        pushControls(a);
        if (a.volume != S.pushedVol || a.muted != S.pushedMute) {
            a.engine.atv().setVolume(a.volume);
            a.engine.atv().setMuted(a.muted);
            S.pushedVol = a.volume; S.pushedMute = a.muted;
        }
    }
    S.wasRunning = run;
}

// ---------------------------------------------------------------- drawing helpers

std::string num(bool has, const char* fmt, double v) { if (!has) return "-"; char b[40]; snprintf(b, sizeof b, fmt, v); return b; }

void kv(const App& a, const char* k, const std::string& v, float col = 130) {
    ImGui::TextDisabled("%s", k); kvColumn(col * gUi);   // a narrow pane: the column moves left and the value wraps
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

struct Grid { float v; const char* label; };

// A trace on a dark plate with level lines. xTicks: labels along the bottom at the given fractions of the width.
void scope(const char* id, ImVec2 size, const std::vector<float>& w, float lo, float hi, std::initializer_list<Grid> hLines,
           std::initializer_list<Grid> xTicks, ImU32 col) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    float labW = 34 * gUi;   // room for the widest level label
    for (const Grid& g : hLines) labW = std::max(labW, ImGui::CalcTextSize(g.label).x + 6 * gUi);
    const float labH = ImGui::GetTextLineHeight() + 2;
    const ImVec2 a0(p0.x + labW, p0.y + 4 * gUi), a1(p1.x - 6 * gUi, p1.y - labH);   // the area of the trace
    if (a1.x - a0.x < 20 || a1.y - a0.y < 20) return;
    auto Y = [&](float v) { return a1.y - (v - lo) / (hi - lo) * (a1.y - a0.y); };
    const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
    float lastY = -FLT_MAX;   // a small scope: a label that would touch the one before it is left out (the grid line stays)
    for (const Grid& g : hLines) {
        const float y = Y(g.v);
        dl->AddLine(ImVec2(a0.x, y), ImVec2(a1.x, y), grid);
        if (std::fabs(y - lastY) < ImGui::GetTextLineHeight()) continue;
        dl->AddText(ImVec2(p0.x + 3 * gUi, y - ImGui::GetTextLineHeight() * 0.5f), txt, g.label);
        lastY = y;
    }
    float lastX = -FLT_MAX;
    for (const Grid& g : xTicks) {
        const float x = a0.x + g.v * (a1.x - a0.x);
        dl->AddLine(ImVec2(x, a0.y), ImVec2(x, a1.y), grid);
        const float tw = ImGui::CalcTextSize(g.label).x, lx = std::min(x - tw * 0.5f, a1.x - tw);
        if (lx < lastX + 4 * gUi) continue;
        dl->AddText(ImVec2(lx, a1.y + 1), txt, g.label);
        lastX = lx + tw;
    }
    if (w.size() < 2) return;
    dl->PushClipRect(a0, a1, true);
    std::vector<ImVec2> pts(w.size());
    for (size_t i = 0; i < w.size(); i++) pts[i] = ImVec2(a0.x + (float)i / (float)(w.size() - 1) * (a1.x - a0.x), Y(std::max(lo, std::min(hi, w[i]))));
    dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, 1.4f);
    dl->PopClipRect();
}

const ImU32 kTrace = IM_COL32(140, 230, 160, 255);

void lineScope(const App& a, ImVec2 size, bool on) {
    const AtvTelemetry& t = a.rx.atv;
    const double us = on && t.lineHz > 1000 ? 1e6 / t.lineHz : 64.0;
    char b1[16], b2[16], b3[16], b4[16];
    snprintf(b1, sizeof b1, "%.0f us", us * 0.25); snprintf(b2, sizeof b2, "%.0f us", us * 0.5); snprintf(b3, sizeof b3, "%.0f us", us * 0.75); snprintf(b4, sizeof b4, "%.0f us", us);
    static const std::vector<float> none;
    scope("##line", size, on ? t.lineWave : none, -0.55f, 1.2f, {{1.f, "white"}, {0.f, "black"}, {-0.43f, "sync"}}, {{0.25f, b1}, {0.5f, b2}, {0.75f, b3}, {1.f, b4}}, kTrace);
}

void vbiScope(const App& a, ImVec2 size, bool on) {
    static const std::vector<float> none;
    scope("##vbi", size, on ? a.rx.atv.vbiWave : none, -0.55f, 1.2f, {{1.f, "white"}, {0.f, "black"}, {-0.43f, "sync"}},
          {{0.f, "0"}, {1.f / 3, "5 lines"}, {2.f / 3, "10 lines"}, {1.f, "15"}}, kTrace);
}

// the spectrum around the vision carrier, dB relative to the strongest part
void spectrumScope(const App& a, ImVec2 size, bool on) {
    const AtvTelemetry& t = a.rx.atv;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##aspec", size);
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 9, 10, 255));
    const float labW = 34 * gUi, labH = ImGui::GetTextLineHeight() + 2;
    const ImVec2 a0(p0.x + labW, p0.y + 4 * gUi), a1(p1.x - 6 * gUi, p1.y - labH);
    if (a1.x - a0.x < 20 || a1.y - a0.y < 20) return;
    const float f0 = t.specLoMhz, f1 = t.specHiMhz;
    const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
    float top = -10, bot = -80;
    if (on && !t.specDb.empty()) { float mx = -200; for (float v : t.specDb) mx = std::max(mx, v); top = std::ceil(mx / 10.f) * 10.f; bot = top - 70.f; }
    auto X = [&](double mhz) { return a0.x + (float)((mhz - f0) / (f1 - f0)) * (a1.x - a0.x); };
    auto Y = [&](float v) { return a1.y - (v - bot) / (top - bot) * (a1.y - a0.y); };
    float lastY = FLT_MAX, lastX = -FLT_MAX;   // a small plot: labels that would touch the one before are left out (the grid lines stay)
    for (float d = bot; d <= top + 0.1f; d += 10.f) {
        dl->AddLine(ImVec2(a0.x, Y(d)), ImVec2(a1.x, Y(d)), grid);
        if (lastY - Y(d) < ImGui::GetTextLineHeight()) continue;
        char b[16]; snprintf(b, sizeof b, "%.0f", d);
        dl->AddText(ImVec2(p0.x + 3 * gUi, Y(d) - ImGui::GetTextLineHeight() * 0.5f), txt, b);
        lastY = Y(d);
    }
    for (int m = (int)std::ceil(f0); m <= (int)std::floor(f1); m++) {
        const float x = X(m);
        dl->AddLine(ImVec2(x, a0.y), ImVec2(x, a1.y), grid);
        char b[16]; snprintf(b, sizeof b, "%+d", m);
        const float tw = ImGui::CalcTextSize(b).x;
        if (x - tw * 0.5f < lastX + 4 * gUi) continue;
        dl->AddText(ImVec2(x - tw * 0.5f, a1.y + 1), txt, b);
        lastX = x + tw * 0.5f;
    }
    if (!on || t.specDb.size() < 2) return;
    dl->PushClipRect(a0, a1, true);
    if (t.state > 0) {
        dl->AddLine(ImVec2(X(0), a0.y), ImVec2(X(0), a1.y), IM_COL32(200, 160, 70, 140));   // the vision carrier
        if (t.soundHz != 0 && t.soundSpacingMhz > 0) dl->AddLine(ImVec2(X(t.soundSpacingMhz), a0.y), ImVec2(X(t.soundSpacingMhz), a1.y), IM_COL32(110, 170, 220, 140));
    }
    std::vector<ImVec2> pts(t.specDb.size());
    for (size_t i = 0; i < pts.size(); i++) pts[i] = ImVec2(a0.x + (float)i / (float)(pts.size() - 1) * (a1.x - a0.x), Y(std::max(bot, std::min(top, t.specDb[i]))));
    dl->AddPolyline(pts.data(), (int)pts.size(), kTrace, 0, 1.4f);
    dl->PopClipRect();
}

// ---------------------------------------------------------------- the picture

void uploadFrame(App& a) {
    if (!live(a) || !gGfx) return;
    auto f = a.engine.atv().frame(S.frameSeq);
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
    const AtvTelemetry& t = a.rx.atv;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##atvpic", box);
    dl->AddRectFilled(p0, ImVec2(p0.x + box.x, p0.y + box.y), IM_COL32(0, 0, 0, 255));
    const float ar = S.img && S.imgH > 0 ? (float)S.imgW / S.imgH : 4.f / 3.f;
    ImVec2 sz = box;
    if (box.x / box.y > ar) sz.x = box.y * ar; else sz.y = box.x / ar;
    const ImVec2 q0(p0.x + (box.x - sz.x) * 0.5f, p0.y + (box.y - sz.y) * 0.5f), q1(q0.x + sz.x, q0.y + sz.y);
    if (S.img && S.frame) {
        dl->AddImage(S.img->texture(), q0, q1);
        if (!on || t.state != 2) dl->AddRectFilled(q0, q1, IM_COL32(0, 0, 0, 150));   // an old picture
    }
    const char* msg = nullptr;
    if (!a.engine.running()) msg = "stopped";
    else if (!on) msg = "starting";
    else if (t.state == 0) msg = "no signal";
    else if (t.state == 1) msg = "carrier found, waiting for sync";
    else if (!S.frame) msg = "waiting for a picture";
    if (msg) {
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(p0.x + (box.x - ts.x) * 0.5f, p0.y + (box.y - ts.y) * 0.5f), IM_COL32(170, 174, 178, 255), msg);
    }
}

std::string caption(const App& a) {
    const AtvTelemetry& t = a.rx.atv;
    if (!live(a) || t.system.empty()) return a.engine.running() ? "no standard found yet" : "stopped";
    char b[200];
    const double fr = S.frame ? S.frame->fieldRate : t.fieldHz;
    snprintf(b, sizeof b, "%s   %s   %.2f fields/s   %s", t.system.c_str(), t.colour ? (t.colourSystem.empty() ? "colour" : t.colourSystem.c_str()) : t.colourKiller ? "colour killed" : "monochrome", fr,
             S.deint == 1 ? "bob" : "weave");
    return b;
}

void tab(App& a) {
    loadState();
    uploadFrame(a);
    const bool on = live(a);
    const AtvTelemetry& t = a.rx.atv;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    // caption row: what it is, and the field mode
    ImGui::AlignTextToFramePadding();
    {   // the caption gives way to the two buttons on the right: cut short with "..." in a narrow tab (whole on hover)
        const float pillsX = W - 150 * gUi, x0 = ImGui::GetCursorPosX();
        ImGui::PushFont(a.mono, 0);
        const std::string cap = caption(a), shown = ellipsize(cap, std::max(40 * gUi, pillsX - x0 - 12 * gUi));
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopFont();
        if (shown != cap && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cap.c_str());
    }
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20 * gUi, W - 150 * gUi));
    if (pillButton("Weave", S.deint == 0)) { S.deint = 0; plat::prefs().setI("atvDeint", 0); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Both fields woven into one picture: sharp, but moving edges show combing.");
    ImGui::SameLine(0, 4 * gUi);
    if (pillButton("Bob", S.deint == 1)) { S.deint = 1; plat::prefs().setI("atvDeint", 1); savePrefs(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("One field at a time, lines doubled: no combing, half the vertical detail.");
    const float rest = ImGui::GetContentRegionAvail().y;
    const bool side = W - rest * 4.f / 3.f > 300 * gUi;   // wide window: the scope goes beside the picture
    if (side) {
        const float sw = std::min(W * 0.4f, std::max(300.f * gUi, W - rest * 4.f / 3.f - 8 * gUi));
        picture(a, ImVec2(W - sw - 8 * gUi, rest));
        ImGui::SameLine(0, 8 * gUi);
        ImGui::BeginGroup();
        ImGui::TextDisabled("Line (composite)");
        lineScope(a, ImVec2(sw, std::min(190.f * gUi, rest * 0.4f)), on);
        ImGui::Spacing();
        kv(a, "video SNR", on ? num(true, "%.1f dB", t.snrDb) : "-", 96);
        kv(a, "sync", on ? num(true, "%.0f %%", t.syncQuality * 100) : "-", 96);
        kv(a, "burst", on && t.colour ? num(true, "%.0f %%", t.burstLevel * 100) : "-", 96);
        kv(a, "sound", on && t.soundPresent ? num(true, "%.0f dBFS", t.soundLevelDb) : "-", 96);
        kv(a, "detector", on ? (t.syncDetector ? "synchronous" : "envelope") : "-", 96);
        ImGui::EndGroup();
    } else {
        const float scopeH = std::min(150.f * gUi, rest * 0.3f);
        picture(a, ImVec2(W, rest - scopeH - 4 * gUi));
        lineScope(a, ImVec2(W, scopeH), on);
    }
}

// ---------------------------------------------------------------- the other hooks

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const AtvTelemetry& t = a.rx.atv;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Vision carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("Line sync", !on ? 0 : (t.state >= 1 && t.syncQuality > 0.8f) ? 1 : (t.state >= 1 && t.syncQuality > 0.2f) ? 2 : 0); flowNext(12 * gUi);
    lamp("Field sync", !on ? 0 : t.state == 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("Colour", !on ? 0 : t.colour ? 1 : t.colourKiller ? 2 : 0); flowNext(12 * gUi);
    lamp("Sound", !on ? 0 : t.soundPresent ? 1 : t.state >= 1 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[96];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("System", t.system.empty() ? (t.state == 0 ? "searching" : "carrier found") : t.system);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("Video SNR", b);
    snprintf(b, sizeof b, "%.0f %%", t.syncQuality * 100); ro("Sync", b);
    if (t.colour) { snprintf(b, sizeof b, "%.0f %%", t.burstLevel * 100); ro("Burst", b); }
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Fields ok / bad", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    const AtvTelemetry& t = a.rx.atv;
    l1 = "Analog TV";
    if (!live(a)) return;
    char b[120];
    if (t.system.empty()) { l2 = t.state == 0 ? "searching" : "carrier found"; return; }
    l1 = t.system;
    snprintf(b, sizeof b, "%s  video SNR %.0f dB  %s", t.state == 2 ? "locked" : "not locked", t.snrDb, t.colour ? t.colourSystem.c_str() : "no colour");
    l2 = b;
}

void panels(App& a) {
    const bool on = live(a);
    const AtvTelemetry& t = a.rx.atv;
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi, plotH = std::max(60.f, H - ImGui::GetTextLineHeightWithSpacing() - 4);
    const float colW = std::max(120.f, (W - 3 * gap) / 3.f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(colW, "Line (composite)");
    lineScope(a, ImVec2(colW, plotH), on);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Vertical interval (field sync)");
    vbiScope(a, ImVec2(colW, plotH), on);
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "Levels");
    kv(a, "sync depth", on ? num(true, "%.0f %%", t.syncDepthPct) : "-", 130);
    kv(a, "compression", on ? num(true, "%.0f %%", t.syncCompressionPct) : "-", 130);
    kv(a, "white peak", on ? num(true, "%.2f", t.whitePeak) : "-", 130);
    kv(a, "burst", on && t.colour ? num(true, "%.0f %%", t.burstLevel * 100) : "-", 130);
    kv(a, "burst phase", on && t.colour ? num(true, "%+.1f deg", t.chromaPhaseErrDeg) : "-", 130);
    kv(a, "carrier", on ? num(true, "%.1f dBFS", t.carrierDbfs) : "-", 130);
    kv(a, "C/N", on ? num(true, "%.1f dB", t.carrierToNoiseDb) : "-", 130);
    ImGui::EndGroup();
}

void receiver(App& a) {
    const bool on = live(a);
    if (!on) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const AtvTelemetry& t = a.rx.atv;
    const float W = ImGui::GetContentRegionAvail().x;
    const bool fmv = t.modulation == 1;   // FM video: no vision carrier; the whole input is the signal
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(fmv ? "Spectrum of the input (dB, MHz from the centre)" : "Spectrum around the vision carrier (dB, MHz from the carrier)"); ImGui::PopTextWrapPos(); }
    spectrumScope(a, ImVec2(W, std::min(170.f * gUi, ImGui::GetContentRegionAvail().y * 0.35f)), on);
    ImGui::Spacing();
    char b[160];
    ImGui::BeginChild("##atv_kv", ImVec2(0, 0));
    if (ImGui::BeginTable("##atvkv", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Standard");
        kv(a, "state", t.state == 2 ? "locked, decoding" : t.state == 1 ? (fmv ? "line sync" : "carrier and line sync") : (fmv ? "looking for lines" : "searching"));
        kv(a, "system", t.system.empty() ? "-" : t.system);
        kv(a, "colour system", t.colourSystem.empty() ? "-" : t.colourSystem + (t.colour ? "" : t.colourKiller ? " (burst too weak)" : ""));
        snprintf(b, sizeof b, "%d lines, %.3f fields/s", t.lines, t.fieldHz); kv(a, "raster", t.lines ? b : "-");
        if (fmv) {
            kv(a, "detector", "FM discriminator");
            ImGui::Spacing();
            ImGui::TextDisabled("FM video");
            kv(a, "sync tip", t.state == 0 ? "trying" : t.fmSyncLow ? "lowest frequency" : "highest frequency");
            snprintf(b, sizeof b, "%.1f dB", t.snrDb); kv(a, "video SNR", b);
            ImGui::Spacing();
            ImGui::TextDisabled("Sound");
            kv(a, "audio", "not decoded");
        } else {
        kv(a, "detector", t.syncDetector ? "synchronous" : "envelope");
        ImGui::Spacing();
        ImGui::TextDisabled("Carriers");
        snprintf(b, sizeof b, "%+.3f MHz", t.visionHz / 1e6); kv(a, "vision", b);
        if (t.soundHz != 0) snprintf(b, sizeof b, "%+.3f MHz (spacing %.2f MHz)", t.soundHz / 1e6, t.soundSpacingMhz); else snprintf(b, sizeof b, "not found");
        kv(a, "sound", b);
        snprintf(b, sizeof b, "%+.1f kHz", t.cfoHz / 1e3); kv(a, "offset", b);
        snprintf(b, sizeof b, "%.1f dBFS", t.carrierDbfs); kv(a, "carrier level", b);
        snprintf(b, sizeof b, "%.1f dB", t.carrierToNoiseDb); kv(a, "carrier / noise", b);
        snprintf(b, sizeof b, "%.1f dB", t.snrDb); kv(a, "video SNR", b);
        ImGui::Spacing();
        ImGui::TextDisabled("Sound");
        kv(a, "carrier", t.soundPresent ? "present" : "none");
        snprintf(b, sizeof b, "%.1f kHz", t.soundDevKhz); kv(a, "deviation", b);
        snprintf(b, sizeof b, "%.1f dBFS", t.soundLevelDb); kv(a, "audio level", b);
        }

        ImGui::TableNextColumn();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Line and field sync"); ImGui::PopTextWrapPos(); }
        snprintf(b, sizeof b, "%.2f Hz  (%+.1f ppm)", t.lineHz, t.lineErrPpm); kv(a, "line rate", t.lineHz > 0 ? b : "-");
        snprintf(b, sizeof b, "%.0f %%", t.syncQuality * 100); kv(a, "sync found", b);
        snprintf(b, sizeof b, "%llu", (unsigned long long)t.lineCount); kv(a, "lines", b);
        snprintf(b, sizeof b, "%llu", (unsigned long long)t.fieldCount); kv(a, "fields", b);
        snprintf(b, sizeof b, "%llu", (unsigned long long)t.frameCount); kv(a, "pictures", b);
        snprintf(b, sizeof b, "%llu ok, %llu damaged", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); kv(a, "fields decoded", b);
        snprintf(b, sizeof b, "%d", t.fieldNo + 1); kv(a, "last field", b);
        ImGui::Spacing();
        ImGui::TextDisabled("Levels");
        snprintf(b, sizeof b, "%.1f %%", t.syncDepthPct); kv(a, "sync depth", b);
        snprintf(b, sizeof b, "%.1f %%", t.syncCompressionPct); kv(a, "sync compression", b);
        snprintf(b, sizeof b, "%.2f", t.whitePeak); kv(a, "white peak", b);
        snprintf(b, sizeof b, "%.0f %%", t.burstLevel * 100); kv(a, "burst", t.colour || t.colourKiller ? b : "-");
        snprintf(b, sizeof b, "%+.1f deg", t.chromaPhaseErrDeg); kv(a, "burst phase error", t.colour ? b : "-");
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// Put the next control on the same line if it fits (the toolbar), else on the next one (the sidebar).
void nextIf(float w) { sameLineIf(w * gUi, 8 * gUi); }   // (it tested the room on the next line, so it never wrapped)
// the width of a control that follows a label: wanted, but never wider than what is left
float fit(float want) { return std::max(40.f * gUi, std::min(want * gUi, ImGui::GetContentRegionAvail().x - 6 * gUi)); }

void tuner(App& a, bool&) {
    loadState();
    ImGui::TextDisabled("Modulation"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(130));
    if (ImGui::BeginCombo("##atvmod", a.atvFm ? "FM video (FPV)" : "AM (broadcast)")) {
        bool chg = false;
        if (ImGui::Selectable("AM (broadcast)", !a.atvFm) && a.atvFm) { a.atvFm = false; chg = true; }
        if (ImGui::Selectable("FM video (FPV)", a.atvFm) && !a.atvFm) { a.atvFm = true; chg = true; }
        ImGui::EndCombo();
        if (chg) {   // the radio runs at another rate: open it again
            savePrefs(a);
            if (a.engine.running()) startReceiver(a); else applyBandwidth(a);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("AM: broadcast TV, a vision carrier with a sound carrier (PAL, SECAM, NTSC).\nFM video: analog FPV and video links (5.8 GHz, 1.2 GHz): the picture is the frequency of the carrier.\nNeeds 20 Msps, 17 MHz of the radio's band; the polarity is found by itself. The sound is not decoded yet.");
    nextIf(240);
    ImGui::TextDisabled("System"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(84));
    if (ImGui::BeginCombo("##atvsys", S.sysHint < 0 ? "automatic" : kSysNames[S.sysHint])) {
        if (ImGui::Selectable("automatic", S.sysHint < 0)) S.sysHint = -1;
        for (int i = 0; i < kAtvSysCount; i++) if (ImGui::Selectable(kSysNames[i], S.sysHint == i)) S.sysHint = i;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The television system: it sets the line count, the sound spacing and the channel layout.\nAutomatic finds it from the line rate and the sound carrier.");
    nextIf(240);
    ImGui::TextDisabled("Colour"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(fit(84));
    if (ImGui::BeginCombo("##atvcol", S.colHint < 0 ? "automatic" : kColNames[S.colHint])) {
        if (ImGui::Selectable("automatic", S.colHint < 0)) S.colHint = -1;
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(kColNames[i], S.colHint == i)) S.colHint = i;
        ImGui::EndCombo();
    }
}

void decoder(App&, bool&) {
    loadState();
    ImGui::Checkbox("Colour", &S.colourOn);
    nextIf(120);
    bool env = S.detector == 1;
    if (ImGui::Checkbox("Envelope detector", &env)) S.detector = env ? 1 : 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Always use the envelope detector. The default locks to the carrier (synchronous detection), which\nsurvives ghosts and overmodulation better, and falls back to the envelope detector by itself.");
    ImGui::TextDisabled("Saturation"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(fit(90));
    ImGui::SliderFloat("##atvsat", &S.saturation, 0.f, 2.f, "%.2f");
    nextIf(240);
    ImGui::TextDisabled("Tint"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(fit(90));
    ImGui::SliderFloat("##atvhue", &S.hue, -30.f, 30.f, "%.0f deg");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hue adjustment, used by NTSC only (PAL and SECAM correct themselves).");
}

// The test signal: see atv_gen.h for what each option means. The first row has what is changed most; the rest is in a pop-up.
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
    combo("##tsys", sc.modeOpt[0], {"B/G", "B (7 MHz)", "I", "D/K", "M", "N"}, 90);
    nextIf(180);
    ImGui::TextDisabled("colour"); ImGui::SameLine(0, 5 * gUi);
    combo("##tcol", sc.modeOpt[1], {"by system", "PAL", "NTSC", "SECAM", "none"}, 90);
    nextIf(190);
    ImGui::TextDisabled("C/N"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cn = (float)std::min(sc.snrDb, 60.0);
    if (ImGui::SliderFloat("##tcn", &cn, 8, 60, cn >= 60 ? "no noise" : "%.0f dB")) { sc.snrDb = cn >= 60 ? 100 : cn; changed = true; }
    nextIf(200);
    ImGui::TextDisabled("CFO"); ImGui::SameLine(0, 5 * gUi); ImGui::SetNextItemWidth(100 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##tcfo", &cfo, -100, 100, "%.0f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
    nextIf(90);
    if (ImGui::Button("More...")) ImGui::OpenPopup("##atvmore");
    if (ImGui::BeginPopup("##atvmore")) {
        ImGui::TextDisabled("picture"); ImGui::SameLine(110 * gUi);
        combo("##tpic", sc.modeOpt[2], {"test card", "colour bars", "grey ramp"}, 120);
        ImGui::TextDisabled("sound"); ImGui::SameLine(110 * gUi);
        combo("##tsnd", sc.modeOpt[3], {"tone, with gaps", "melody", "carrier only", "no carrier", "steady tone"}, 120);
        ImGui::TextDisabled("hum"); ImGui::SameLine(110 * gUi);
        combo("##thum", sc.modeOpt[4], {"off", "50 Hz", "60 Hz"}, 120);
        bool ghost = sc.echoDb != 0;
        if (ImGui::Checkbox("ghost", &ghost)) { sc.echoDb = ghost ? 18 : 0; changed = true; }
        if (ghost) {
            ImGui::TextDisabled("strength"); ImGui::SameLine(110 * gUi); ImGui::SetNextItemWidth(120 * gUi);
            float e = (float)sc.echoDb;
            if (ImGui::SliderFloat("##techo", &e, 6, 40, "-%.0f dB")) { sc.echoDb = e; changed = true; }
            ImGui::TextDisabled("delay"); ImGui::SameLine(110 * gUi); ImGui::SetNextItemWidth(120 * gUi);
            float d = sc.modeVal[2] > 0 ? (float)sc.modeVal[2] : 1.5f;
            if (ImGui::SliderFloat("##tedl", &d, 0.2f, 8.f, "%.1f us")) { sc.modeVal[2] = d; changed = true; }
        }
        ImGui::TextDisabled("sync compression"); ImGui::SameLine(110 * gUi); ImGui::SetNextItemWidth(120 * gUi);
        float sc1 = (float)sc.modeVal[1];
        if (ImGui::SliderFloat("##tcomp", &sc1, 0, 80, "%.0f %%")) { sc.modeVal[1] = sc1; changed = true; }
        ImGui::EndPopup();
    }
}

// the right-hand list: there is one channel
void list(App& a) {
    const AtvTelemetry& t = a.rx.atv;
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see the channel"); ImGui::PopTextWrapPos(); } return; }
    if (t.system.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.state == 0 ? "searching for a vision carrier" : "carrier found, identifying the system"); ImGui::PopTextWrapPos(); } return; }
    kv(a, "system", t.system, 90);
    kv(a, "colour", t.colour ? t.colourSystem : t.colourKiller ? "burst too weak" : "none", 90);
    char b[96];
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); kv(a, "video SNR", b, 90);
    if (t.soundHz != 0) { snprintf(b, sizeof b, "FM, %.1f kHz dev.", t.soundDevKhz); kv(a, "sound", t.soundPresent ? b : "carrier only", 90); }
    else kv(a, "sound", "none found", 90);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Tune the centre of the channel; the vision carrier is found by itself. NICAM and two-carrier sound are not decoded.");
    ImGui::PopTextWrapPos();
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const bool on = live(a);
    const AtvTelemetry& t = a.rx.atv;
    out.push_back({"VIDEO SNR  dB", "%.1f", on ? t.snrDb : 0, 0, 50, !on ? 0 : t.snrDb >= 35 ? 1 : t.snrDb >= 25 ? 2 : 3});
    out.push_back({"CARRIER  dBFS", "%.1f", on ? t.carrierDbfs : -100, -80, 0, !on ? 0 : t.carrierDbfs > -3 ? 3 : t.carrierDbfs > -50 ? 1 : 2});
    out.push_back({"SYNC FOUND  %", "%.0f", on ? t.syncQuality * 100 : 0, 0, 100, !on ? 0 : t.syncQuality > 0.9f ? 1 : t.syncQuality > 0.5f ? 2 : 3});
    out.push_back({"BURST  %", "%.0f", on && t.colour ? t.burstLevel * 100 : 0, 0, 150, !on ? 0 : t.colour ? (t.burstLevel > 0.5f ? 1 : 2) : 0});
    out.push_back({"SYNC DEPTH  %", "%.0f", on ? t.syncDepthPct : 0, 0, 40, !on ? 0 : t.syncDepthPct > 20 ? 1 : 2});
    out.push_back({"SOUND  dBFS", "%.1f", on ? t.soundLevelDb : -100, -80, 0, !on ? 0 : t.soundPresent ? 1 : 2});
}

} // namespace

extern const ModeUi kAtvUi;
const ModeUi kAtvUi = {
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
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
