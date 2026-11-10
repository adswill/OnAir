// HRPT / AHRPT screens: the AVHRR picture of the pass growing line by line (a false-colour composite or one of the five channels in
// grey), Save PNG and New picture; the frequency presets and the variant; the receiver's state, the packets heard and the constellation.
// The tab and the empty-state text say what the mode needs: a dish and a fast radio.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/hrpt/hrpt_rx.h"
#include "dect2/hrpt/hrpt_tel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using dect2::HrptImage;
using dect2::HrptLine;
using dect2::HrptTelemetry;
using dect2::kHrptWidth;

// views: 0 colour (channels 2, 1, 4), 1 .. 5 channel 1 .. 5 in grey
const char* const kViews[] = {"Colour (2, 1, 4)", "Channel 1 (visible)", "Channel 2 (near infrared)", "Channel 3 (3A / 3B)", "Channel 4 (thermal)", "Channel 5 (thermal)"};
constexpr int kNumViews = 6;
const char* const kVars[] = {"Automatic", "MetOp AHRPT (QPSK 2.33 Msym/s)", "FY-3 AHRPT (QPSK 2.8 Msym/s)", "NOAA HRPT (retired)"};

struct State {
    bool loaded = false;
    int view = 0, var = 0, pushedVar = -1;
    gfx::Image* tex = nullptr;
    int texRows = 0;
    uint64_t imgId = 0;
    int shownView = -1;
    std::vector<const HrptLine*> drawn;
    bool follow = true;
    std::string msg;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.view = std::clamp((int)plat::prefs().getI("hrptView", 0), 0, kNumViews - 1);
    S.var = std::clamp((int)plat::prefs().getI("hrptVar", 0), 0, 3);
}

bool live(const App& a) { return a.engine.running() && a.rx.standard == 65; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void composeLine(const HrptLine* l, int view, uint32_t* px) {
    if (!l) { for (int i = 0; i < kHrptWidth; i++) px[i] = 0xFF000000u; return; }
    const uint8_t* p = l->px.data();
    for (int i = 0; i < kHrptWidth; i++) {
        uint32_t r, g, b;
        if (view == 0) { r = p[kHrptWidth + i]; g = p[i]; b = (uint32_t)(255 - p[3 * kHrptWidth + i]); }
        else r = g = b = p[(size_t)(view - 1) * kHrptWidth + (size_t)i];
        px[i] = 0xFF000000u | b << 16 | g << 8 | r;
    }
}

void update(const HrptTelemetry& t) {
    const HrptImage* im = t.image.get();
    if (!im || !gGfx) return;
    const bool fresh = im->id != S.imgId || S.shownView != S.view;
    if (fresh) { S.imgId = im->id; S.shownView = S.view; S.drawn.clear(); }
    if (im->lines > S.texRows || !S.tex) {
        const int cap = std::max(128, (im->lines + 127) / 128 * 128);
        delete S.tex;
        S.tex = gGfx->createImage(kHrptWidth, cap, 0xFF000000u);
        S.texRows = cap;
        S.drawn.clear();
    }
    if (!S.tex) return;
    S.drawn.resize((size_t)im->lines, nullptr);
    std::vector<uint32_t> px((size_t)kHrptWidth);
    for (int r = 0; r < im->lines; r++) {
        const HrptLine* cur = (size_t)r < im->line.size() ? im->line[(size_t)r].get() : nullptr;
        if (cur == S.drawn[(size_t)r]) continue;
        composeLine(cur, S.view, px.data());
        S.tex->update(0, r, kHrptWidth, 1, px.data());
        S.drawn[(size_t)r] = cur;
    }
}

void savePng(const HrptTelemetry& t) {
    const HrptImage* im = t.image.get();
    if (!im || im->lines <= 0) { S.msg = "nothing to save yet"; return; }
    std::vector<uint32_t> px((size_t)im->lines * kHrptWidth);
    for (int r = 0; r < im->lines; r++) composeLine((size_t)r < im->line.size() ? im->line[(size_t)r].get() : nullptr, S.view, &px[(size_t)r * kHrptWidth]);
    char ts[32];
    const time_t tt = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    char name[64];
    if (S.view == 0) snprintf(name, sizeof name, "%s-hrpt-214.png", ts); else snprintf(name, sizeof name, "%s-hrpt-ch%d.png", ts, S.view);
    const std::string dir = plat::dataDir() + "/hrpt";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + name;
    S.msg = gfx::writePng(path.c_str(), reinterpret_cast<const uint8_t*>(px.data()), kHrptWidth, im->lines) ? "saved " + path : "could not write " + path;
}

void tab(App& a) {
    loadState();
    const HrptTelemetry& t = a.rx.hrpt;
    const bool on = live(a);
    if (on) update(t);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##hrview", &S.view, kViews, kNumViews)) { plat::prefs().setI("hrptView", S.view); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colour: near infrared (2) as red, visible (1) as green and the inverted thermal channel 4 as blue.\nA channel alone is shown in grey.");
    flowNext(8 * gUi);
    if (ImGui::Button("Save PNG")) savePng(t);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The picture as shown, at full size, in <data folder>/hrpt/");
    flowNext(5 * gUi);
    if (runningButton(a, tr("New picture"))) { a.engine.hrpt().clearImage(); S.msg.clear(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Follow", &S.follow);
    flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    const HrptImage* im = on ? t.image.get() : nullptr;
    if (!im || im->lines == 0 || !S.tex) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "Start the receiver while MetOp-B or MetOp-C is overhead. HRPT / AHRPT needs a dish (about 1 m or more, following the satellite) "
                                                        "with an LNA at the feed, and a fast radio: 4 Msps or more (HackRF, Airspy, SDRplay, Pluto; an RTL-SDR is too slow)."
                                      : on ? dect2::hrptSummary(t).c_str() : "starting");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("##hrpic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float w = std::max(50.f, ImGui::GetContentRegionAvail().x), z = w / kHrptWidth;
    const float h = im->lines * z;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(S.tex->texture(), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, (float)im->lines / (float)S.texRows));
    ImGui::Dummy(ImVec2(w, h));
    if (S.follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const HrptTelemetry& t = a.rx.hrpt;
    if (!ImGui::BeginTable("##hrl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Packets", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Count");
    static const char* const kNames[4] = {"AVHRR, channel 3A (APID 103)", "AVHRR, channel 3B (APID 104)", "NOAA minor frames", "other packets"};
    for (int c = 0; c < 4; c++) {
        const uint64_t n = t.apidPackets[c];
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        if (n) ImGui::TextUnformatted(kNames[c]); else ImGui::TextDisabled("%s", kNames[c]);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)n);
    }
    for (int v = 0; v < 64; v++) {
        if (!t.vcidFrames[v]) continue;
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("virtual channel %d (frames)", v);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)t.vcidFrames[v]);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const HrptTelemetry& t = a.rx.hrpt;
    kvRow(a, "state", "%s", dect2::hrptSummary(t).c_str());
    kvRow(a, "variant", "%s at %.0f ksym/s (setting: %s)", dect2::hrptVarName(t.variant), t.symbolRate / 1e3, dect2::hrptVarName(t.varSetting));
    kvRow(a, "carrier", "offset %+.2f kHz, lock %.2f, symbol SNR %.1f dB", t.cfoHz / 1e3, t.carrierLock, t.snrDb);
    kvRow(a, "symbol clock", "%+.0f ppm", t.clockPpm);
    if (t.rotation >= 0) kvRow(a, "ambiguity", "%d", t.rotation);
    kvRow(a, "frames", "%llu good, %llu failed, %llu bytes corrected, %llu sync words%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.rsCorrected, (unsigned long long)t.asmHits, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft %d, virtual channel %d, counter %u", t.scid, t.vcid, t.vcduCounter);
    kvRow(a, "packets", "%llu, %llu lost", (unsigned long long)t.packets, (unsigned long long)t.packetsLost);
    if (t.lines) {
        const uint32_t s = t.onboardMs / 1000;
        kvRow(a, "on board", "%02u:%02u:%02u (satellite time), %llu lines", s / 3600 % 24, s / 60 % 60, s % 60, (unsigned long long)t.lines);
    }
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    smart5WxPassCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

void panels(App& a) {
    const HrptTelemetry& t = a.rx.hrpt;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    if (live(a)) for (const auto& p : t.constellation) pts.emplace_back(p.first, p.second);
    scatter("##hrc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const HrptTelemetry& t = a.rx.hrpt;
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::hrptSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HRPT / AHRPT";
    if (live(a)) l2 = dect2::hrptSummary(a.rx.hrpt);
}

void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.pushedVar = -1; return; }
    if (S.pushedVar != S.var) { a.engine.hrpt().setVariant(S.var); S.pushedVar = S.var; }
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    int n = 0;
    const dect2::HrptPreset* p = dect2::hrptPresets(n);
    char cur[32]; snprintf(cur, sizeof cur, "%.3f MHz", a.freqMhz);
    ImGui::SetNextItemWidth(std::min(250 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##hrf", cur)) {
        for (int i = 0; i < n; i++)
            if (ImGui::Selectable(p[i].label, std::fabs(a.freqMhz - p[i].mhz) < 0.0005)) {
                tuneFreq(a, p[i].mhz); retune = true;
                S.var = p[i].variant; plat::prefs().setI("hrptVar", S.var); plat::prefs().flush();
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active now: MetOp-B and MetOp-C (AHRPT 1701.3 MHz) and the FY-3 satellites (check each one's frequency).\n"
                                                  "NOAA-15, -18 and -19 are retired: their HRPT is gone from the air, the NOAA entries are for old recordings.\n"
                                                  "Expect Doppler of up to +-40 kHz; the receiver follows it.");
    flowNext(8 * gUi);
    ImGui::TextDisabled("Variant");
    sameLineIf(150 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##hrv", &S.var, kVars, 4)) { plat::prefs().setI("hrptVar", S.var); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic tries MetOp, then FY-3, and the retired NOAA HRPT only now and then, until frames decode.");
    flowEnd();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("variant");
    flowNext(); ImGui::SetNextItemWidth(190 * gUi);
    static const char* const kM[] = {"MetOp AHRPT", "FY-3 AHRPT", "NOAA HRPT"};
    int m = sc.modeOpt[0] == 2 ? 1 : sc.modeOpt[0] == 3 ? 2 : 0;
    if (ImGui::Combo("##hrsm", &m, kM, 3)) { sc.modeOpt[0] = m == 1 ? 2 : m == 2 ? 3 : 1; changed = true; }
    flowNext();
    bool b = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("Channel 3B (APID 104)", &b)) { sc.modeOpt[1] = b ? 1 : 0; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const HrptTelemetry& t = a.rx.hrpt;
    out.push_back({"Es/N0  dB", "%.0f", t.snrDb, 0, 25, t.snrDb >= 8 ? 1 : t.snrDb >= 5 ? 2 : 3});
    out.push_back({"LINES", "%.0f", t.image ? (double)t.image->lines : 0.0, 0, 4000, t.state >= 3 ? 1 : 0});
}

} // namespace

// Simple view: the picture with its controls, and the signal and the products / channels beside it
static void command(App& a) {
    modepanel::panes({{"##hrptcpic", nullptr, [&a] { tab(a); }, 2.2f, 300.f},
                      {"##hrptcsig", tr("Signal"), [&a] { receiver(a); ImGui::Spacing(); list(a); }, 1.f, 240.f}}, 340.f);
}

extern const ModeUi kHrptUi;
const ModeUi kHrptUi = {
    .sideTitle = "PACKETS",
    .tabName = "Picture",
    .tabIcon = Ic::Camera,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .synth = synth,
    .tick = tick,
    .meters = meters,
    .command = command,
};
