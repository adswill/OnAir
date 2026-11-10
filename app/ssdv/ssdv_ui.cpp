// SSDV screens: the band presets, the picture being received (built up as packets arrive, grey where packets are missing), the other pictures
// of the session, the packets heard, and under Advanced a fixed speed. The receiver writes every picture to <Save pictures to>/SSDV as a JPEG when it is
// complete (or after 20 s without a new packet); "Save PNG" writes a PNG copy through the shared picture folder. See docs/ssdv.md.
#include "../panels/picsave_ui.h"
#include "app.h"
#include "gfx/gfx.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/common/time_fmt.h"
#include "dect2/ssdv/ssdv_proto.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/ssdv_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

struct State {
    bool loaded = false, wasRunning = false;
    int view = 0;                        // 0 pictures, 1 packets
    int sel = 0;                         // picture shown: 0 is the newest
    int baudChoice = 0;                  // index into kBauds, 0 = automatic
    int pushedBaud = -1;
    std::string pushedDir;
    gfx::Image* tex = nullptr;
    int texW = 0, texH = 0;
    std::string texKey;
    uint64_t texRev = ~0ull;
    dect2::ssdv::Rgb rgb;
};
State S;

constexpr int kBauds[] = {0, 300, 600, 1200, 2400, 4800, 9600, 19200, 115200, 96000};
constexpr int kNumBauds = (int)(sizeof kBauds / sizeof kBauds[0]);

std::string baudName(int b) {   // 115200 and 96000 are the two Wenet links
    if (b == 115200) return "Wenet 115.2 kbaud";
    if (b == 96000) return "Wenet 96 kbaud (I2S)";
    return std::to_string(b) + " baud";
}

bool live(const App& a) { return a.rx.standard == 207; }   // the engine reports its standard code minus one

void load() {
    if (S.loaded) return;
    S.loaded = true;
    S.baudChoice = (int)std::max(0L, std::min((long)kNumBauds - 1, plat::prefs().getI("ssdvBaud", 0)));
}

void presets(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Frequency"));
    flowNext(6 * gUi);
    int n = 0;
    const dect2::SsdvPreset* p = dect2::ssdvPresets(n);
    for (int i = 0; i < n; i++) {
        const bool here = std::fabs(a.freqMhz - p[i].mhz) < 0.002;
        if (here) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        char b[48]; snprintf(b, sizeof b, "%s##ssp%d", p[i].name, i);
        if (ImGui::Button(b)) tuneFreq(a, p[i].mhz);
        if (here) ImGui::PopStyleColor();
        flowNext(4 * gUi);
    }
    ImGui::TextDisabled("|"); flowNext(8 * gUi);
    char cl[40]; snprintf(cl, sizeof cl, "%s##ssdv", tr("Clear"));
    if (ImGui::Button(cl)) { a.engine.ssdv().clearResults(); S.sel = 0; S.texRev = ~0ull; }
    flowEnd();
}

void showPicture(App& a, const dect2::SsdvImage& im) {
    const std::string key = im.call + "/" + std::to_string(im.imageId);
    if (im.jpeg && (S.texKey != key || S.texRev != im.rev)) {
        dect2::ssdv::Rgb rgb;
        if (dect2::ssdv::decodeJpeg(im.jpeg->data(), im.jpeg->size(), rgb) && rgb.w > 0 && gGfx) {
            if (!im.gaps.empty()) dect2::ssdv::fillGaps(rgb, im.gaps);
            if (!S.tex || S.texW != rgb.w || S.texH != rgb.h) { delete S.tex; S.tex = gGfx->createImage(rgb.w, rgb.h, 0xFF000000u); S.texW = rgb.w; S.texH = rgb.h; }
            std::vector<uint32_t> px((size_t)rgb.w * rgb.h);
            for (size_t i = 0; i < px.size(); i++) px[i] = 0xFF000000u | ((uint32_t)rgb.rgb[i * 3 + 2] << 16) | ((uint32_t)rgb.rgb[i * 3 + 1] << 8) | rgb.rgb[i * 3];
            if (S.tex) S.tex->update(0, 0, rgb.w, rgb.h, px.data());
            S.rgb = std::move(rgb);
        }
        S.texKey = key; S.texRev = im.rev;
    }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(120 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("picture"), "%s #%d, %d x %d", im.call.c_str(), im.imageId, im.width, im.height);
    const int total = im.lastPacket >= 0 ? im.lastPacket + 1 : 0;
    if (total > 0) kv(tr("packets"), "%d of %d%s", im.packets, total, im.complete ? tr(" (complete)") : "");
    else kv(tr("packets"), "%d (%s)", im.packets, tr("end not seen yet"));
    if (total > 0) {
        const float frac = std::min(1.f, (float)im.packets / (float)total);
        ImGui::ProgressBar(frac, ImVec2(std::min(300.f * gUi, ImGui::GetContentRegionAvail().x), 8 * gUi), "");
    }
    if (!im.savedPath.empty()) { ImGui::TextDisabled("%s", im.savedPath.c_str()); }
    if (S.tex && S.texW > 0 && S.texKey == key) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float scale = std::max(0.1f, std::min(avail.x / S.texW, std::max(60.f, avail.y - 30 * gUi) / S.texH));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz(S.texW * scale, S.texH * scale);
        ImGui::GetWindowDrawList()->AddImage(S.tex->texture(), p, ImVec2(p.x + sz.x, p.y + sz.y));
        ImGui::Dummy(sz);
        if (ImGui::Button(tr("Save PNG##ssdv")) && !S.rgb.rgb.empty()) {
            std::vector<uint8_t> rgba((size_t)S.rgb.w * S.rgb.h * 4);
            for (size_t i = 0; i < (size_t)S.rgb.w * S.rgb.h; i++) { rgba[i * 4] = S.rgb.rgb[i * 3]; rgba[i * 4 + 1] = S.rgb.rgb[i * 3 + 1]; rgba[i * 4 + 2] = S.rgb.rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255; }
            picui::saveRgba("SSDV", time(nullptr), rgba.data(), S.rgb.w, S.rgb.h);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Write this picture as a PNG to the picture folder (Settings: Save pictures to). The JPEG is saved by itself."));
        picui::statusLine();
    }
}

void picturesView(App& a) {
    const auto& imgs = a.rx.ssdv.images;
    if (imgs.empty()) { ImGui::TextDisabled("%s", tr("no pictures yet")); return; }
    if (S.sel >= (int)imgs.size()) S.sel = 0;
    for (size_t i = 0; i < imgs.size(); i++) {
        char b[80]; snprintf(b, sizeof b, "%s #%d##ssdsel%zu", imgs[i].call.c_str(), imgs[i].imageId, i);
        if ((int)i == S.sel) ImGui::PushStyleColor(ImGuiCol_Button, pal::accent(0.45f));
        if (ImGui::Button(b)) S.sel = (int)i;
        if ((int)i == S.sel) ImGui::PopStyleColor();
        flowNext(4 * gUi);
    }
    flowEnd();
    showPicture(a, imgs[(size_t)S.sel]);
}

void packetsView(App& a) {
    const auto& pk = a.rx.ssdv.packets;
    if (!ImGui::BeginTable("##ssdp", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn(tr("Call")); ImGui::TableSetupColumn(tr("Picture")); ImGui::TableSetupColumn(tr("Packet"));
    ImGui::TableSetupColumn(tr("Baud")); ImGui::TableSetupColumn(tr("Fixed")); ImGui::TableSetupColumn(tr("Detail"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGui::PushFont(a.mono, 0);
    for (const auto& p : pk) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(p.utc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(p.call.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", p.imageId);
        ImGui::TableNextColumn(); ImGui::Text("%d%s", p.packetId, p.eoi ? " end" : "");
        ImGui::TableNextColumn(); ImGui::Text("%d%s", p.baud, p.wenet ? " Wenet" : p.async ? " 8N2" : "");
        ImGui::TableNextColumn(); if (p.rsErrors > 0) ImGui::Text("%d", p.rsErrors); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s%s%s", p.noFec ? "no FEC " : "", p.inverted ? "swapped " : "", p.snrDb > 0 ? "" : "");
    }
    ImGui::PopFont();
    ImGui::EndTable();
    if (pk.empty()) ImGui::TextDisabled("%s", tr("no packets yet"));
}

void tick(App& a) {
    load();
    if (!a.engine.running()) { S.wasRunning = false; S.pushedBaud = -1; S.pushedDir.clear(); return; }
    dect2::SsdvReceiver& r = a.engine.ssdv();
    if (kBauds[S.baudChoice] != S.pushedBaud) { r.setBaud(kBauds[S.baudChoice]); S.pushedBaud = kBauds[S.baudChoice]; }
    const std::string dir = picui::folder() + "/SSDV";
    if (dir != S.pushedDir) { r.setSaveDir(dir); S.pushedDir = dir; }
    S.wasRunning = true;
}

void tuner(App&, bool&) {
    load();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("From a sound recording: choose File as the source, press Open and pick the WAV (any sample rate, mono or stereo, 8 - 32 bit). It must hold the audio of an FM receiver with the FSK waveform (9600 baud or other speeds), not AFSK tones."));
    ImGui::PopTextWrapPos();
    if (ImGui::CollapsingHeader(trw("Advanced##ssdv"))) {
        ImGui::TextDisabled("%s", tr("Speed"));
        ImGui::SameLine();
        char cur[32];
        if (S.baudChoice == 0) snprintf(cur, sizeof cur, "%s", tr("Automatic")); else snprintf(cur, sizeof cur, "%s", baudName(kBauds[S.baudChoice]).c_str());
        ImGui::SetNextItemWidth(190 * gUi);
        if (ImGui::BeginCombo("##ssdvbaud", cur)) {
            for (int i = 0; i < kNumBauds; i++) {
                char b[32];
                if (i == 0) snprintf(b, sizeof b, "%s", tr("Automatic")); else snprintf(b, sizeof b, "%s", baudName(kBauds[i]).c_str());
                if (ImGui::Selectable(b, i == S.baudChoice)) { S.baudChoice = i; plat::prefs().setI("ssdvBaud", (long)i); }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Automatic tries 1200 - 19200 baud plain bits, 300 - 1200 baud RTTY-framed bytes and the two Wenet links (115.2 and 96 kbaud, LDPC) at the same time. Wenet needs a sample rate of 2 MHz or more. A fixed speed saves a little CPU."));
    }
}

void tab(App& a) {
    load();
    presets(a);
    ImGui::Spacing();
    subNav("ssdv", S.view, {"Pictures", "Packets"});
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    if (S.view == 0) picturesView(a); else packetsView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const auto& imgs = a.rx.ssdv.images;
    if (!ImGui::BeginTable("##ssdl", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn(tr("Picture")); ImGui::TableSetupColumn(tr("Packets")); ImGui::TableSetupColumn(tr("Last"), ImGuiTableColumnFlags_WidthStretch);
    for (size_t i = 0; i < imgs.size(); i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); if (ImGui::Selectable((imgs[i].call + " #" + std::to_string(imgs[i].imageId) + "##ssl" + std::to_string(i)).c_str(), (int)i == S.sel)) { S.sel = (int)i; S.view = 0; } ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::Text("%d%s", imgs[i].packets, imgs[i].complete ? " ok" : "");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%s", dect2::timefmt::clock(imgs[i].lastUtc).c_str());
    }
    ImGui::EndTable();
    if (imgs.empty()) ImGui::TextDisabled("%s", tr("no pictures yet"));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? tr("starting") : tr("stopped")); return; }
    const dect2::SsdvTelemetry& t = a.rx.ssdv;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv(tr("state"), "%s", dect2::ssdvSummary(t).c_str());
    if (t.wenetGps.valid) {
        const dect2::WenetGps& g = t.wenetGps;
        kv(tr("position"), "%.5f, %.5f  %.0f m  %d sats", g.lat, g.lon, g.alt, g.numSv);
        kv(tr("motion"), "%.1f m/s, heading %.0f, climb %+.1f m/s", g.speed, g.heading, g.ascent);
        if (g.haveSys) kv(tr("radio / CPU"), "%.1f C / %.1f C", g.radioTemp, g.cpuTemp);
        if (g.havePower) kv(tr("battery"), "%.2f V, %d mA", g.battV, g.battI);
    }
    if (t.wenetOrient.valid) kv(tr("attitude"), "heading %.1f, roll %.1f, pitch %.1f", t.wenetOrient.heading, t.wenetOrient.roll, t.wenetOrient.pitch);
    if (!t.wenetTexts.empty()) kv(tr("message"), "%s", t.wenetTexts.back().text.c_str());
    if (t.baud) kv(tr("speed"), "%d baud%s", t.baud, t.wenet ? tr(", Wenet (LDPC)") : t.async ? tr(", RTTY-framed bytes") : "");
    if (t.wenetOther || t.wenetIdle) kv(tr("Wenet other"), "%llu telemetry / text, %llu idle", (unsigned long long)t.wenetOther, (unsigned long long)t.wenetIdle);
    kv(tr("packets"), "%llu OK, %llu %s, %llu %s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, tr("failed"), (unsigned long long)t.corrected, tr("corrected"));
    kv(tr("pictures"), "%zu (%llu %s)", t.images.size(), (unsigned long long)t.imagesDone, tr("complete"));
    kv(tr("carrier offset"), "%+.0f Hz", t.cfoHz);
    kv(tr("input"), "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv(tr("signal time"), "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp(tr("Signal"), !on ? 0 : a.rx.ssdv.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp(tr("Packets"), !on ? 0 : a.rx.ssdv.dataValid ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("State")); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? tr("starting") : tr("stopped")) : dect2::ssdvSummary(a.rx.ssdv).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "SSDV";
    if (live(a)) l2 = dect2::ssdvSummary(a.rx.ssdv);
}

} // namespace

extern const ModeUi kSsdvUi;
const ModeUi kSsdvUi = {
    .sideTitle = "PICTURES",
    .tabName = "SSDV",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .tick = tick,
};
