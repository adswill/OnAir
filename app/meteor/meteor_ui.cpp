// Meteor-M LRPT screens: the picture of the pass growing line by line (a colour composite, 123 by day or 125 with the infrared channel
// at night, chosen from the channels sent, or one channel in grey), Save PNG and New picture; the frequency (137.900 / 137.100) and
// the modulation; the receiver's state, the channels heard and the symbol constellation.
#include "app.h"
#include "panels/gallery.h"
#include "gfx/gfx.h"
#include "map/wx_overlay.h"
#include "platform/platform.h"
#include "dect2/meteor/meteor_rx.h"
#include "dect2/meteor/meteor_tel.h"
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

using dect2::MeteorImage;
using dect2::MeteorStrip;
using dect2::MeteorTelemetry;
using dect2::kMeteorWidth;

// views: 0 automatic composite, 1 composite 123, 2 composite 125, 3 .. 8 channel 1 .. 6
const char* const kViews[] = {"Colour (automatic)", "Colour 123", "Colour 125", "Channel 1", "Channel 2", "Channel 3", "Channel 4", "Channel 5", "Channel 6"};
constexpr int kNumViews = 9;
const char* const kMods[] = {"Automatic", "OQPSK 72k (N2-3, N2-4)", "QPSK 72k (N2)", "OQPSK 80k interleaved"};

struct State {
    bool loaded = false;
    int view = 0, mod = 0, pushedMod = -1;
    gfx::Image* tex = nullptr;
    int texRows = 0;                      // rows the texture holds
    uint64_t imgId = 0;
    int shownView = -1;
    std::array<int, 3> shownCh{};         // the channels the texture was drawn from
    std::vector<std::array<const MeteorStrip*, 3>> drawn;
    bool follow = true;
    std::string msg;
    int enh = 0;                          // 0 as received, 1 contrast stretch, 2 stretch + thermal colours (one channel), 3 stretch + brighten
    bool autoSave = false;
    uint8_t lut[6][256];
    int lutRows = -1; int lutEnh = -1;
    int lastRows = 0; double lastGrow = 0; uint64_t savedId = 0;
};
State S;

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    S.view = std::clamp((int)plat::prefs().getI("meteorView", 0), 0, kNumViews - 1);
    S.mod = std::clamp((int)plat::prefs().getI("meteorMod", 0), 0, 3);
    S.enh = std::clamp((int)plat::prefs().getI("meteorEnh", 0), 0, 3);
    S.autoSave = plat::prefs().getB("meteorAutoSave", false);
    for (auto& l : S.lut) for (int i = 0; i < 256; i++) l[i] = (uint8_t)i;
}

bool live(const App& a) { return a.rx.standard == 60; }   // the engine reports its standard code minus one

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

bool hasCh(const MeteorImage& im, int c) { return im.packets[(size_t)c] > 0; }

// the channels (0 .. 5) behind red, green and blue; -1 none. One channel: the same in all three (grey).
std::array<int, 3> channelsFor(const MeteorImage* im, int view) {
    if (view >= 3) { const int c = view - 3; return {c, c, c}; }
    if (view == 1) return {0, 1, 2};
    if (view == 2) return {0, 1, 4};
    if (!im) return {0, 1, 2};
    if (hasCh(*im, 0) && hasCh(*im, 1) && hasCh(*im, 2)) return {0, 1, 2};   // day
    if (hasCh(*im, 0) && hasCh(*im, 1) && hasCh(*im, 4)) return {0, 1, 4};   // night: the infrared channel instead of 3
    for (int c = 0; c < 6; c++) if (hasCh(*im, c)) return {c, c, c};
    return {0, 1, 2};
}

const MeteorStrip* stripOf(const MeteorImage& im, int c, int row) {
    const auto& v = im.ch[(size_t)c];
    return row < (int)v.size() ? v[(size_t)row].get() : nullptr;
}

// Enhancement: per channel a stretch of the 2 % .. 98 % range of its own grey levels to the full range (the satellite's channels
// sit in a narrow band), optionally brightened; recomputed as the picture grows
void computeLut(const MeteorImage& im) {
    if (S.lutRows == im.rows && S.lutEnh == S.enh) return;
    if (S.lutRows >= 0 && S.lutEnh == S.enh && im.rows - S.lutRows < 24 && im.rows >= S.lutRows) return;
    S.lutRows = im.rows; S.lutEnh = S.enh;
    for (int c = 0; c < 6; c++) {
        uint8_t* l = S.lut[c];
        if (S.enh == 0) { for (int i = 0; i < 256; i++) l[i] = (uint8_t)i; continue; }
        uint64_t hist[256] = {}, n = 0;
        for (const auto& st : im.ch[(size_t)c]) if (st) for (uint8_t v : st->px) if (v) { hist[v]++; n++; }
        if (n < 2000) { for (int i = 0; i < 256; i++) l[i] = (uint8_t)i; continue; }
        int lo = 1, hi = 255;
        uint64_t acc = 0;
        for (int i = 1; i < 256; i++) { acc += hist[i]; if (acc >= n / 50) { lo = i; break; } }
        acc = 0;
        for (int i = 255; i >= 1; i--) { acc += hist[i]; if (acc >= n / 50) { hi = i; break; } }
        if (hi - lo < 8) { lo = std::max(1, lo - 4); hi = std::min(255, hi + 4); }
        for (int i = 0; i < 256; i++) {
            double v = i == 0 ? 0 : std::clamp((i - lo) / (double)(hi - lo), 0.0, 1.0);
            if (S.enh == 3) v = std::pow(v, 0.7);
            l[i] = (uint8_t)std::lround(v * 255);
        }
    }
}

// grey level -> colour: cold (bright in the infrared) white, then red, yellow, green, cyan, blue for the warm ground
uint32_t thermal(uint8_t g) {
    const double t = 1.0 - g / 255.0;   // 0 cold .. 1 warm (inverted grey: the infrared channel shows cold clouds bright)
    static const double st[6][3] = {{255, 255, 255}, {200, 40, 40}, {240, 220, 40}, {60, 200, 80}, {40, 200, 220}, {20, 40, 140}};
    const double x = t * 5; const int i = std::min(4, (int)x); const double f = x - i;
    const uint32_t r = (uint32_t)(st[i][0] + (st[i + 1][0] - st[i][0]) * f), gg = (uint32_t)(st[i][1] + (st[i + 1][1] - st[i][1]) * f), b = (uint32_t)(st[i][2] + (st[i + 1][2] - st[i][2]) * f);
    return 0xFF000000u | b << 16 | gg << 8 | r;
}

void composeRow(const MeteorImage& im, const std::array<int, 3>& ch, int row, uint32_t* px) {
    const MeteorStrip* s[3] = {stripOf(im, ch[0], row), stripOf(im, ch[1], row), stripOf(im, ch[2], row)};
    for (int i = 0; i < 8 * kMeteorWidth; i++) {
        const uint32_t r = s[0] ? S.lut[ch[0]][s[0]->px[(size_t)i]] : 0, g = s[1] ? S.lut[ch[1]][s[1]->px[(size_t)i]] : 0, b = s[2] ? S.lut[ch[2]][s[2]->px[(size_t)i]] : 0;
        px[i] = (S.enh == 2 && ch[0] == ch[1] && ch[1] == ch[2] && s[0] && s[0]->px[(size_t)i]) ? thermal((uint8_t)r) : (0xFF000000u | b << 16 | g << 8 | r);
    }
}

// brings the texture up to the picture: new rows and the strips that changed since they were drawn
void update(const MeteorTelemetry& t) {
    const MeteorImage* im = t.image.get();
    if (!im || !gGfx) return;
    const std::array<int, 3> ch = channelsFor(im, S.view);
    const int lr = S.lutRows, le = S.lutEnh;
    computeLut(*im);
    const bool fresh = im->id != S.imgId || S.shownView != S.view || ch != S.shownCh || lr != S.lutRows || le != S.lutEnh;
    if (fresh) { S.imgId = im->id; S.shownView = S.view; S.shownCh = ch; S.drawn.clear(); }
    if (im->rows > S.texRows || !S.tex) {
        const int cap = std::max(64, (im->rows + 127) / 128 * 128);
        delete S.tex;
        S.tex = gGfx->createImage(kMeteorWidth, cap * 8, 0xFF000000u);
        S.texRows = cap;
        S.drawn.clear();
    }
    if (!S.tex) return;
    S.drawn.resize((size_t)im->rows, {nullptr, nullptr, nullptr});
    std::vector<uint32_t> px((size_t)8 * kMeteorWidth);
    for (int r = 0; r < im->rows; r++) {
        const std::array<const MeteorStrip*, 3> cur = {stripOf(*im, ch[0], r), stripOf(*im, ch[1], r), stripOf(*im, ch[2], r)};
        if (cur == S.drawn[(size_t)r]) continue;
        composeRow(*im, ch, r, px.data());
        S.tex->update(0, r * 8, kMeteorWidth, 8, px.data());
        S.drawn[(size_t)r] = cur;
    }
}

void savePng(const MeteorTelemetry& t) {
    const MeteorImage* im = t.image.get();
    if (!im || im->rows <= 0) { S.msg = "nothing to save yet"; return; }
    const std::array<int, 3> ch = channelsFor(im, S.view);
    S.lutRows = -1; computeLut(*im);
    const int h = im->rows * 8;
    std::vector<uint32_t> px((size_t)h * kMeteorWidth);
    for (int r = 0; r < im->rows; r++) composeRow(*im, ch, r, &px[(size_t)r * 8 * kMeteorWidth]);
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
    if (ch[0] == ch[1] && ch[1] == ch[2]) snprintf(name, sizeof name, "%s-meteor-ch%d.png", ts, ch[0] + 1);
    else snprintf(name, sizeof name, "%s-meteor-%d%d%d.png", ts, ch[0] + 1, ch[1] + 1, ch[2] + 1);
    const std::string dir = plat::dataDir() + "/meteor";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + name;
    S.msg = gfx::writePng(path.c_str(), reinterpret_cast<const uint8_t*>(px.data()), kMeteorWidth, h) ? "saved " + path : "could not write " + path;
}

// ---------------------------------------------------------------- the picture tab

void tab(App& a) {
    loadState();
    const MeteorTelemetry& t = a.rx.meteor;
    const bool on = live(a);
    if (on) update(t);
    if (on && t.image && t.image->rows >= 2 && wxoverlay::wanted(a)) {   // the picture of the pass on the map (View > Map)
        const MeteorImage& im = *t.image;
        const std::array<int, 3> ch = channelsFor(&im, S.view);
        const int h = im.rows * 8;
        std::vector<uint32_t> px((size_t)h * kMeteorWidth);
        for (int r = 0; r < im.rows; r++) {
            composeRow(im, ch, r, &px[(size_t)r * 8 * kMeteorWidth]);
            if (!stripOf(im, ch[0], r) && !stripOf(im, ch[1], r) && !stripOf(im, ch[2], r)) std::fill_n(&px[(size_t)r * 8 * kMeteorWidth], 8 * kMeteorWidth, 0u);   // a lost strip stays clear
        }
        wxoverlay::setSwath(im.id, "Meteor-M LRPT", kMeteorWidth, h, px, 6.0);
    }
    if (on && t.image) {   // auto-save: the picture stopped growing
        const double now = ImGui::GetTime();
        if (t.image->rows != S.lastRows) { S.lastRows = t.image->rows; S.lastGrow = now; }
        if (S.autoSave && t.image->rows >= 16 && t.image->id != S.savedId && now - S.lastGrow > 90) { savePng(t); S.savedId = t.image->id; }
    }
    ImGui::SetNextItemWidth(std::min(170 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##metview", &S.view, kViews, kNumViews)) { plat::prefs().setI("meteorView", S.view); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Colour: channels 1, 2 and 3 as red, green and blue by day, 1, 2 and 5 (infrared) at night;\n"
                                                  "automatic picks from the channels the satellite sends. A channel alone is shown in grey.");
    flowNext(6 * gUi);
    ImGui::SetNextItemWidth(130 * gUi);
    if (comboTr("##metenh", &S.enh, {N_("As received"), N_("Contrast stretch"), N_("Thermal colours"), N_("Stretch + brighten")})) { plat::prefs().setI("meteorEnh", S.enh); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Enhancement of the picture (saved PNGs too). Contrast stretch: each channel's own 2-98 %% range to the full range.\nThermal colours colour a single (infrared) channel by temperature; pick a Channel view for it.");
    flowNext(8 * gUi);
    if (ImGui::Button("Save PNG")) savePng(t);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The picture as shown, at full size, in <data folder>/meteor/");
    flowNext(5 * gUi);
    if (runningButton(a, tr("New picture"))) { a.engine.meteor().clearImage(); S.msg.clear(); }
    flowNext(8 * gUi);
    if (ImGui::Checkbox("Auto-save", &S.autoSave)) { plat::prefs().setB("meteorAutoSave", S.autoSave); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save the picture as a PNG when no new lines have come for 90 seconds (the pass is over)");
    flowNext(8 * gUi);
    ImGui::Checkbox("Follow", &S.follow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keep the newest lines in view while the picture grows");
    flowEnd();
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    const MeteorImage* im = on ? t.image.get() : nullptr;
    if (!im || im->rows == 0 || !S.tex) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", !a.engine.running() ? "Start the receiver during a pass (View > Passes can start it at AOS and follow the Doppler)."
                                                      : on ? dect2::meteorSummary(t).c_str() : "starting");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("##metpic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float w = std::max(50.f, ImGui::GetContentRegionAvail().x), z = w / kMeteorWidth;
    const float h = im->rows * 8 * z;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(S.tex->texture(), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, (float)im->rows / (float)S.texRows));
    ImGui::Dummy(ImVec2(w, h));
    if (S.follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const MeteorTelemetry& t = a.rx.meteor;
    if (!ImGui::BeginTable("##metl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Packets");
    for (int c = 0; c < 6; c++) {
        const uint64_t n = t.apidPackets[c];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (n) ImGui::Text("Channel %d (APID %d)", c + 1, 64 + c); else ImGui::TextDisabled("Channel %d (APID %d)", c + 1, 64 + c);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)n);
    }
    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Telemetry (APID 70)"); ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)t.apidPackets[6]);
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const MeteorTelemetry& t = a.rx.meteor;
    kvRow(a, "state", "%s", dect2::meteorSummary(t).c_str());
    kvRow(a, "modulation", "%s at %.0f ksym/s (setting: %s)", dect2::meteorModName(t.modulation), t.symbolRate / 1e3, dect2::meteorModName(t.modSetting));
    kvRow(a, "carrier", "offset %+.2f kHz, lock %.2f, symbol SNR %.1f dB", t.cfoHz / 1e3, t.carrierLock, t.snrDb);
    kvRow(a, "symbol clock", "%+.0f ppm", t.clockPpm);
    if (t.rotation >= 0) kvRow(a, "phase ambiguity", "%d%s", t.rotation & 7, t.rotation >= 8 ? ", half a symbol late" : "");
    kvRow(a, "frames", "%llu good, %llu failed RS, %llu bytes corrected, %llu sync words%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
          (unsigned long long)t.rsCorrected, (unsigned long long)t.asmHits, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "spacecraft %d, virtual channel %d, counter %u", t.scid, t.vcid, t.vcduCounter);
    kvRow(a, "packets", "%llu, %llu lost", (unsigned long long)t.packets, (unsigned long long)t.packetsLost);
    if (t.lastQuality) {
        const uint32_t s = t.onboardMs / 1000;
        kvRow(a, "on board", "%02u:%02u:%02u (satellite time), quality %d", s / 3600 % 24, s / 60 % 60, s % 60, t.lastQuality);
    }
    if (t.image) kvRow(a, "picture", "%d lines of %d pixels", t.image->rows * 8, dect2::kMeteorWidth);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void panels(App& a) {
    const MeteorTelemetry& t = a.rx.meteor;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    if (live(a)) for (const auto& p : t.constellation) pts.emplace_back(p.first, p.second);
    scatter("##metc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const MeteorTelemetry& t = a.rx.meteor;
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::meteorSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Meteor-M LRPT";
    if (live(a)) l2 = dect2::meteorSummary(a.rx.meteor);
}

// ---------------------------------------------------------------- the gallery: a picture is handed over when it has stopped growing (or a new one starts)

struct GalleryHold {
    std::shared_ptr<const MeteorImage> pend;
    double start = 0, changedAt = 0;
    int rows = 0, pubRows = -1;
};
GalleryHold gHold;

void galleryFlush() {
    GalleryHold& g = gHold;
    if (!g.pend || g.pend->rows <= 0 || g.pubRows == g.pend->rows) return;
    const MeteorImage& im = *g.pend;
    g.pubRows = im.rows;
    const int h = im.rows * 8;
    gallery::Meta m;
    m.planes.resize(6);
    std::string chans;
    for (int c = 0; c < 6; c++) {
        if (!hasCh(im, c)) continue;
        chans += (chans.empty() ? "" : " ") + std::to_string(c + 1);
        std::vector<uint8_t>& pl = m.planes[(size_t)c];
        pl.assign((size_t)h * kMeteorWidth, 0);
        for (int r = 0; r < im.rows; r++)
            if (const MeteorStrip* s = stripOf(im, c, r)) memcpy(&pl[(size_t)r * 8 * kMeteorWidth], s->px.data(), (size_t)8 * kMeteorWidth);
    }
    const std::array<int, 3> ch = channelsFor(&im, 0);
    std::vector<uint32_t> px((size_t)h * kMeteorWidth);
    for (int r = 0; r < im.rows; r++) composeRow(im, ch, r, &px[(size_t)r * 8 * kMeteorWidth]);
    m.detail = "channels " + chans + ", " + std::to_string(h) + " lines";
    std::string name;
    const double dt = 8.0 / 6.0;   // a strip of 8 lines at 6 lines a second
    if (const dect2::Sgp4* sat = passesFindSat("METEOR-M", g.start + im.rows * dt / 2, &name)) {
        m.satellite = name;
        for (int r = 0; r < im.rows; r++) {
            double lat, lon, alt;
            if (!dect2::subPoint(*sat, g.start + r * dt, lat, lon, alt)) { m.track.clear(); break; }
            m.track.push_back({lat, lon});
        }
        m.linesPerTrack = 8; m.swathKm = 2800;
    }
    gallery::publish("meteor-" + std::to_string((long long)g.start), "Meteor-M LRPT", g.start, kMeteorWidth, h, reinterpret_cast<const uint8_t*>(px.data()), m);
}

void galleryTick(const MeteorTelemetry& t, bool live) {
    GalleryHold& g = gHold;
    const double now = (double)time(nullptr);
    if (t.image && t.image->rows > 0) {
        if (!g.pend || g.pend->id != t.image->id) { galleryFlush(); g = GalleryHold(); g.pend = t.image; g.rows = t.image->rows; g.start = now - g.rows * 8.0 / 6.0; g.changedAt = now; }
        else if (t.image->rows != g.rows) { g.pend = t.image; g.rows = t.image->rows; g.changedAt = now; }
    }
    if (g.pend && g.pubRows != g.pend->rows && (!live || now - g.changedAt > 45)) galleryFlush();
}

void tick(App& a) {
    loadState();
    galleryTick(a.rx.meteor, a.engine.running());
    if (!a.engine.running()) { S.pushedMod = -1; return; }
    if (S.pushedMod != S.mod) { a.engine.meteor().setModulation(S.mod); S.pushedMod = S.mod; }
}

void tuner(App& a, bool& retune) {
    loadState();
    ImGui::TextDisabled("Frequency");
    sameLineIf(90 * gUi, 5 * gUi);
    int n = 0;
    const dect2::MeteorPreset* p = dect2::meteorPresets(n);
    char cur[32]; snprintf(cur, sizeof cur, "%.3f MHz", a.freqMhz);
    ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##metf", cur)) {
        for (int i = 0; i < n; i++) if (ImGui::Selectable(p[i].label, std::fabs(a.freqMhz - p[i].mhz) < 0.0005)) { tuneFreq(a, p[i].mhz); retune = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Meteor-M N2-3 and N2-4 send LRPT on 137.900 or 137.100 MHz (which one changes; check a current list).");
    flowNext(8 * gUi);
    ImGui::TextDisabled("Modulation");
    sameLineIf(150 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
    if (ImGui::Combo("##metm", &S.mod, kMods, 4)) { plat::prefs().setI("meteorMod", S.mod); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic tries OQPSK 72k (N2-3, N2-4), then QPSK 72k, then the interleaved 80k mode, until frames decode.");
    flowEnd();
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    ImGui::TextDisabled("modulation");
    flowNext(); ImGui::SetNextItemWidth(170 * gUi);
    static const char* const kM[] = {"OQPSK 72k", "QPSK 72k", "OQPSK 80k interleaved"};
    int m = sc.modeOpt[0] == 2 ? 1 : sc.modeOpt[0] == 3 ? 2 : 0;
    if (ImGui::Combo("##metsm", &m, kM, 3)) { sc.modeOpt[0] = m == 1 ? 2 : m == 2 ? 3 : 1; changed = true; }
    flowNext();
    bool night = sc.modeOpt[1] == 1;
    if (ImGui::Checkbox("Night channels", &night)) { sc.modeOpt[1] = night ? 1 : 0; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Channels 1, 2 and 5 instead of 1, 2 and 3");
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const MeteorTelemetry& t = a.rx.meteor;
    out.push_back({"Es/N0  dB", "%.0f", t.snrDb, 0, 25, t.snrDb >= 10 ? 1 : t.snrDb >= 6 ? 2 : 3});
    out.push_back({"LINES", "%.0f", t.image ? t.image->rows * 8.0 : 0.0, 0, 4000, t.state >= 3 ? 1 : 0});
}

} // namespace

extern const ModeUi kMeteorUi;
const ModeUi kMeteorUi = {
    .sideTitle = "CHANNELS",
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
};
