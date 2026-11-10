// HD Radio screens: the Radio tab (Station: call sign, names, slogan, location, message, the programs and what is playing with its album
// art; Data: the large objects and the other data services; Signal: the receiver's figures), the program list on the right, the status
// lamps, the analysis row (constellation, MER, P1 frames), the Receiver tab and the options of the test signal. HD Radio sound is HDC, a
// patented codec that OnAir does not ship: with the user's own patched FAAD2 the program selected on the right is played (hdr_hdc.h).
#include "dect2/hdr/hdr_maps.h"
#include "dect2/common/png.h"
#include <map>
#include "app.h"
#include "codec/speech_codec_ui.h"
#include "dect2/hdr/hdr_l2.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/hdr/hdr_tel.h"
#include "dect2/common/png.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

bool freqStepArrow(const char* id, ImGuiDir dir);

namespace {

constexpr int kStd = 22;   // RxTelemetry::standard of HD Radio (the tuning table says 23, the engine reports 22)

struct Pic { gfx::Image* img = nullptr; int w = 0, h = 0; uint64_t version = 0; bool failed = false; };

struct State {
    int view = 0;                     // Station, Data, Signal
    int sel = 0;                      // the program whose data is shown (0 = HD1)
    uint64_t lastSeq = 0;
    std::map<std::pair<int, int>, Pic> pics;   // decoded LOT pictures by (port, LOT id)
    std::deque<float> mer, p1;          // a point per report
    uint64_t prevP1Ok = 0, prevP1Bad = 0, prevPidsOk = 0, prevPidsBad = 0;
    double p1OkAt = -1e9, p1BadAt = -1e9, pidsOkAt = -1e9, pidsBadAt = -1e9;
    bool bandInit = false;
    bool raster9 = false;             // AM channels 9 kHz apart (Europe, Africa, Asia, Oceania) instead of the Americas' 10 kHz
    double lastFm = 98.5, lastAm = 1.03;   // MHz: where the FM and AM buttons go back to
};
State S;

const ImVec4 kGood(0.40f, 0.85f, 0.50f, 1), kWarn(0.95f, 0.60f, 0.25f, 1), kBad(0.95f, 0.40f, 0.35f, 1);

bool live(const App& a) { return a.rx.standard == kStd; }   // the engine reports its standard code minus one
bool locked(const App& a) { return live(a) && a.rx.hdr.state >= 2; }

int blockLamp(double okAt, double badAt, double window, double now) {
    const bool ok = now - okAt < window, bad = now - badAt < window;
    return ok && bad ? 2 : ok ? 1 : bad ? 3 : 0;
}

std::string progLabel(const HdrProgram& p) { return "HD" + std::to_string(p.number + 1); }
std::string progName(const HdrProgram& p) { return p.name.empty() ? progLabel(p) : p.name; }

const HdrProgram* selected(const HdrTelemetry& t) {
    for (const auto& p : t.programs) if (p.number == S.sel) return &p;
    return t.programs.empty() ? nullptr : &t.programs[0];
}

const char* stateText(int s) { return s >= 3 ? "decoding" : s == 2 ? "locked, waiting for a frame" : s == 1 ? "signal found, synchronising" : "searching"; }

std::string codecText(int m) {
    if (m < 0) return "-";
    char b[48];
    snprintf(b, sizeof b, "HDC, codec mode %d", m);
    return b;
}

const char* transportText(int t) { return t == 0 ? "stream" : t == 1 ? "packets" : t == 3 ? "files (LOT)" : "-"; }

std::string mimeText(uint32_t m) {
    const char* n = dect2::hdr::mimeName(m);
    char b[48];
    if (*n) return n;
    snprintf(b, sizeof b, "MIME %08X", m);
    return b;
}

std::string sizeText(uint32_t n) {
    char b[32];
    if (n >= 10240) snprintf(b, sizeof b, "%.0f kB", n / 1024.0);
    else if (n >= 1024) snprintf(b, sizeof b, "%.1f kB", n / 1024.0);
    else snprintf(b, sizeof b, "%u bytes", n);
    return b;
}

std::string placeText(const HdrTelemetry& t) {
    if (!t.haveLocation) return "-";
    char b[96];
    snprintf(b, sizeof b, "%.4f %c, %.4f %c, %d m", std::fabs(t.latitude), t.latitude >= 0 ? 'N' : 'S', std::fabs(t.longitude), t.longitude >= 0 ? 'E' : 'W', t.altitudeM);
    return b;
}

void kvRow(const App& a, const char* k, const std::string& v, float x = 120.f) {
    ImGui::TextDisabled("%s", k); kvColumn(x * gUi);   // a narrow pane: the column moves left and the value wraps
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(v.c_str()); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// ---------------------------------------------------------------- pictures (LOT objects)

bool isPicture(const HdrLotInfo& l) {
    if (l.mime == dect2::hdr::kMimePng || l.mime == dect2::hdr::kMimeJpeg || l.mime == dect2::hdr::kMimePrimaryImage || l.mime == dect2::hdr::kMimeStationLogo) return true;
    std::string n = l.name;
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    auto ends = [&](const char* e) { const size_t k = strlen(e); return n.size() >= k && n.compare(n.size() - k, k, e) == 0; };
    return ends(".png") || ends(".jpg") || ends(".jpeg");
}

// The texture of a complete LOT picture: PNG with OnAir's own decoder, anything else (JPEG) through the system's picture decoder
const Pic* picture(App& a, const HdrLotInfo& l) {
    if (!l.complete || !gGfx || !isPicture(l)) return nullptr;
    const auto key = std::make_pair(l.port, l.lot);
    Pic& p = S.pics[key];
    if (p.version == l.version && (p.img || p.failed)) return p.img ? &p : nullptr;
    p.version = l.version;
    p.failed = true;
    std::vector<uint8_t> bytes;
    if (!a.engine.hdr().lotBytes(l.port, l.lot, bytes) || bytes.empty()) { p.failed = false; p.version = 0; return nullptr; }   // not published yet
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    std::vector<uint8_t> rgba;
    if (bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && dect2::decodePng(bytes.data(), bytes.size(), w, h, rgba) && w > 0 && h > 0 && w <= 2048 && h <= 2048) {
        px.resize((size_t)w * h);
        memcpy(px.data(), rgba.data(), px.size() * 4);
    } else {
        char name[64];
        snprintf(name, sizeof name, "/hdradio_%04X_%d.%s", l.port, l.lot, bytes[0] == 0xFF ? "jpg" : "img");
        const std::string path = plat::cacheDir() + name;
        { std::ofstream f(path, std::ios::binary); f.write((const char*)bytes.data(), (std::streamsize)bytes.size()); }
        if (!plat::decodeImage(path, w, h, px) || w <= 0 || h <= 0 || w > 2048 || h > 2048) return nullptr;
    }
    if (!p.img || p.w != w || p.h != h) { delete p.img; p.img = gGfx->createImage(w, h, 0xFF000000u); }
    p.img->update(0, 0, w, h, px.data());
    p.w = w; p.h = h;
    p.failed = false;
    return &p;
}

// draws the picture into a box of side `box`, keeping its shape; an outline with a note when there is none
void drawPicture(const Pic* p, float box, const char* none) {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (p && p->img && p->w > 0 && p->h > 0) {
        const float z = std::min(box / (float)p->w, box / (float)p->h);
        const float w = p->w * z, h = p->h * z;
        const ImVec2 q0(o.x + (box - w) * 0.5f, o.y + (box - h) * 0.5f);
        dl->AddImage(p->img->texture(), q0, ImVec2(q0.x + w, q0.y + h));
    } else {
        dl->AddRect(o, ImVec2(o.x + box, o.y + box), IM_COL32(60, 64, 70, 255), 4.f);
        icons::draw(Ic::Radio, ImVec2(o.x + box * 0.5f, o.y + box * 0.42f), box * 0.32f, IM_COL32(70, 80, 92, 255), dl);
        if (none && box > 60 * gUi) {
            const ImVec2 ts = ImGui::CalcTextSize(none);
            if (ts.x < box - 6) dl->AddText(ImVec2(o.x + (box - ts.x) * 0.5f, o.y + box * 0.72f), ImGui::GetColorU32(ImGuiCol_TextDisabled), none);
        }
    }
    ImGui::Dummy(ImVec2(box, box));
}

const HdrLotInfo* findLot(const HdrTelemetry& t, int port, int lot) {
    for (const auto& l : t.lots) if (l.lot == lot && (port < 0 || l.port == port) && l.complete) return &l;
    return nullptr;
}

// the album art of a program: the LOT its program data names, on its primary image port when the guide gives one
const HdrLotInfo* albumArt(const HdrTelemetry& t, const HdrProgram& p) {
    if (p.xhdrLot < 0) return nullptr;
    const HdrLotInfo* l = p.artPort >= 0 ? findLot(t, p.artPort, p.xhdrLot) : nullptr;
    return l ? l : findLot(t, -1, p.xhdrLot);
}

// the station logo of a program: the logo service its own SIG entry lists, else the station-wide logo service of the guide, else any
// complete file whose LOT header says station logo
const HdrLotInfo* newestOn(const HdrTelemetry& t, int port) {
    for (const auto& l : t.lots) if (l.port == port && l.complete) return &l;   // lots are sorted newest first
    return nullptr;
}
const HdrLotInfo* stationLogo(const HdrTelemetry& t, const HdrProgram* p) {
    if (p && p->logoPort >= 0) if (const HdrLotInfo* l = newestOn(t, p->logoPort)) return l;
    for (const auto& d : t.dataServices)
        if (d.fromSig && d.mime == dect2::hdr::kMimeStationLogo && d.program < 0) if (const HdrLotInfo* l = newestOn(t, d.port)) return l;
    for (const auto& l : t.lots) if (l.complete && l.mime == dect2::hdr::kMimeStationLogo) return &l;
    return nullptr;
}

// ---------------------------------------------------------------- tick

void tick(App& a) {
    {   // the sound follows the volume, the mute and the program selected on the right (HD1 .. HD4 ...); cheap atomics, so every frame
        dect2::HdrReceiver& r = a.engine.hdr();
        const HdrProgram* sp = selected(a.rx.hdr);
        r.setVolume(a.volume); r.setMuted(a.muted); r.setProgram(sp ? sp->number : S.sel);
    }
    if (!a.engine.running()) {   // forget the pictures of the last session
        for (auto& kv : S.pics) delete kv.second.img;
        S.pics.clear();
    }
    if (!live(a) || a.rx.seq == S.lastSeq) return;
    S.lastSeq = a.rx.seq;
    const HdrTelemetry& t = a.rx.hdr;
    const double now = ImGui::GetTime();
    auto count = [&](uint64_t cur, uint64_t& prev, double& at) { if (cur < prev) prev = 0; if (cur > prev) at = now; prev = cur; };
    const uint64_t okBefore = S.prevP1Ok, badBefore = S.prevP1Bad;
    count(t.blocksOk, S.prevP1Ok, S.p1OkAt); count(t.blocksBad, S.prevP1Bad, S.p1BadAt);
    const uint64_t dOk = S.prevP1Ok >= okBefore ? S.prevP1Ok - okBefore : 0, dBad = S.prevP1Bad >= badBefore ? S.prevP1Bad - badBefore : 0;
    if (dOk + dBad > 0) { S.p1.push_back(100.f * (float)dOk / (float)(dOk + dBad)); if (S.p1.size() > 600) S.p1.pop_front(); }
    count(t.pidsOk, S.prevPidsOk, S.pidsOkAt); count(t.pidsBad, S.prevPidsBad, S.pidsBadAt);
    if (t.state >= 2) { S.mer.push_back(t.snrDb); if (S.mer.size() > 600) S.mer.pop_front(); }
}

// ---------------------------------------------------------------- program rows (the Station tab and the list on the right)

void programCards(App& a, const char* id) {
    const HdrTelemetry& t = a.rx.hdr;
    const HdrProgram* cur = selected(t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cardH = 54 * gUi;
    ImGui::PushID(id);
    for (const auto& p : t.programs) {
        const bool sel = cur && cur->number == p.number;
        ImGui::PushID(p.number);
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##card", ImVec2(std::max(1.f, w), cardH))) S.sel = p.number;
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s: show its program data", progLabel(p).c_str());
        dl->AddRectFilled(o, ImVec2(o.x + w, o.y + cardH - 4), sel ? IM_COL32(30, 36, 40, 255) : hov ? IM_COL32(26, 27, 29, 255) : IM_COL32(16, 17, 19, 255), 4.f);
        dl->AddRect(o, ImVec2(o.x + w, o.y + cardH - 4), sel ? pal::remap(IM_COL32(115, 184, 209, 255)) : IM_COL32(48, 50, 52, 255), 4.f, 0, sel ? 1.6f : 1.f);
        dl->AddText(a.ui, 15.f * gUi, ImVec2(o.x + 8 * gUi, o.y + 6 * gUi), sel ? pal::remap(IM_COL32(120, 200, 255, 255)) : IM_COL32(150, 170, 190, 255), progLabel(p).c_str());
        const std::string name = ellipsize(progName(p), w - 52 * gUi, 15.f * gUi);
        dl->AddText(a.ui, 15.f * gUi, ImVec2(o.x + 46 * gUi, o.y + 6 * gUi), IM_COL32(245, 247, 250, 255), name.c_str());
        float x = o.x + 8 * gUi;
        const float ty = o.y + 29 * gUi;
        const float right = o.x + w - 6 * gUi;
        auto tag = [&](const char* s, ImU32 bg, ImU32 fg) {
            if (x + ImGui::CalcTextSize(s).x + 10 * gUi > right) return;   // a narrow list: the tags that do not fit are left out
            x += tagAt(dl, ImVec2(x, ty), s, bg, fg) + 4 * gUi;
        };
        tag("HDC", IM_COL32(34, 74, 108, 255), IM_COL32(225, 232, 240, 255));
        const char* type = dect2::hdrProgramTypeName(p.type);
        if (*type) tag(type, IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255));
        if (p.kbps > 0.05) { char k[24]; snprintf(k, sizeof k, "%.0f kbit/s", p.kbps); tag(k, IM_COL32(38, 46, 58, 255), IM_COL32(190, 200, 214, 255)); }
        if (p.access) tag("restricted", IM_COL32(112, 52, 50, 255), IM_COL32(225, 232, 240, 255));
        ImGui::PopID();
    }
    ImGui::PopID();
}

// The sound line: whether the user's HDC library is there, and what is being played. The ? button opens the install guide (codec_help.cpp).
void hdcNote(const App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    ImGui::PushTextWrapPos(0);
    codecHelpButton(CodecHelp::Hdc);
    ImGui::SameLine();
    if (!live(a)) ImGui::TextDisabled("%s", tr("HD Radio sound needs the HDC codec, which you install yourself (click ?)"));
    else if (!t.hdcProblem.empty()) ImGui::TextColored(kWarn, tr("HDC sound codec cannot be used: %s"), t.hdcProblem.c_str());
    else if (!t.hdcFound) { ImGui::TextColored(kWarn, "%s", tr("HDC sound codec not installed (no sound; click ? for the steps)")); ImGui::SameLine(); appcodec::discordCodecButton(true); }
    else if (t.hdcPlaying) ImGui::TextColored(kGood, tr("sound: HD%d (HDC)"), t.soundProgram + 1);
    else ImGui::TextDisabled(tr("sound: HD%d, waiting for audio"), t.soundProgram + 1);
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- Station

void nowPlaying(App& a, const HdrProgram& p) {
    const HdrTelemetry& t = a.rx.hdr;
    sectionHeader(Ic::Play, ("Now playing on " + progLabel(p)).c_str());
    const float W = ImGui::GetContentRegionAvail().x;
    const float box = std::min(160.f * gUi, std::max(72.f * gUi, W * 0.3f));
    const HdrLotInfo* art = albumArt(t, p);
    const Pic* pic = art ? picture(a, *art) : nullptr;
    const bool side = W > box + 220 * gUi;     // the album art beside the text, or above it in a narrow pane
    if (side) ImGui::BeginGroup();
    if (p.psdCount == 0) {
        ImGui::PushTextWrapPos(side ? ImGui::GetCursorPosX() + W - box - 16 * gUi : 0);
        ImGui::TextDisabled("%s", live(a) ? "waiting for the program service data" : "-");
        ImGui::PopTextWrapPos();
    } else {
        const float wrapX = side ? ImGui::GetCursorPosX() + W - box - 16 * gUi : 0;
        ImGui::PushTextWrapPos(wrapX);
        ImGui::PushFont(a.ui, 22.f);
        ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", p.title.empty() ? "-" : p.title.c_str());
        ImGui::PopFont();
        ImGui::PushFont(a.ui, 17.f);
        ImGui::TextUnformatted(p.artist.empty() ? "-" : p.artist.c_str());
        ImGui::PopFont();
        if (!p.album.empty()) ImGui::TextDisabled("%s", p.album.c_str());
        ImGui::Spacing();
        ImGui::PopTextWrapPos();
        const float kx = 70.f;
        if (!p.genre.empty()) kvRow(a, "genre", p.genre, kx);
        if (!p.comment.empty()) kvRow(a, "comment", p.comment, kx);
        if (!p.comSeller.empty() || !p.comPrice.empty() || !p.comDesc.empty()) {
            std::string c = p.comDesc;
            if (!p.comSeller.empty()) c += (c.empty() ? "" : ", ") + p.comSeller;
            if (!p.comPrice.empty()) c += (c.empty() ? "" : ", ") + p.comPrice;
            if (!p.comValid.empty()) c += " (until " + p.comValid + ")";
            if (!p.comUrl.empty()) c += "  " + p.comUrl;
            kvRow(a, "advert", c, kx);
        }
        char b[64];
        snprintf(b, sizeof b, "%llu messages", (unsigned long long)p.psdCount);
        kvRow(a, "data", b, kx);
    }
    if (side) { ImGui::EndGroup(); ImGui::SameLine(std::max(0.f, ImGui::GetWindowContentRegionMax().x - box)); }
    drawPicture(pic, box, p.xhdrLot >= 0 ? (art ? "loading" : "waiting") : "no art");
    if (ImGui::IsItemHovered()) {
        if (art) ImGui::SetTooltip("album art: %s, %s, LOT %d on port %04X", art->name.c_str(), sizeText(art->size).c_str(), art->lot, art->port);
        else if (p.xhdrLot >= 0) ImGui::SetTooltip("the program data names LOT %d as the album art; it has not come in yet", p.xhdrLot);
        else ImGui::SetTooltip("this program names no album art");
    }
}

// ---------------------------------------------------------------- band, channel raster, stepping

// the channel nearest to mhz on the band's raster: FM 87.9 .. 107.9 MHz on odd tenths (200 kHz), AM 530 .. 1700 kHz in 10 kHz steps or
// 531 .. 1710 kHz in 9 kHz steps
double snapChannel(double mhz, bool am, bool r9) {
    if (!am) { const double n = std::round((mhz - 87.9) / 0.2); return 87.9 + 0.2 * std::max(0.0, std::min(100.0, n)); }
    const double k0 = r9 ? 531 : 530, st = r9 ? 9 : 10, kmax = r9 ? 1710 : 1700;
    const double n = std::round((mhz * 1000 - k0) / st);
    return (k0 + st * std::max(0.0, std::min((kmax - k0) / st, n))) / 1000.0;
}

void bandBar(App& a) {
    if (!S.bandInit) {
        S.bandInit = true;
        S.raster9 = plat::prefs().getB("hdrAm9k", false);
        S.lastFm = plat::prefs().getD("hdrLastFm", 98.5);
        S.lastAm = plat::prefs().getD("hdrLastAm", 1.03);
    }
    const bool am = dect2::hdrIsAmBand(a.freqMhz);
    if (am) S.lastAm = a.freqMhz; else S.lastFm = a.freqMhz;
    auto go = [&](double mhz) {
        tuneFreq(a, mhz);
        plat::prefs().setD(dect2::hdrIsAmBand(mhz) ? "hdrLastAm" : "hdrLastFm", mhz);
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Band"); flowNext(6 * gUi);
    if (ImGui::RadioButton("FM##hdrb", !am) && am) go(snapChannel(S.lastFm, false, S.raster9));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("FM hybrid stations, 88 to 108 MHz (200 kHz raster)");
    flowNext(6 * gUi);
    if (ImGui::RadioButton("AM##hdrb", am) && !am) go(snapChannel(S.lastAm, true, S.raster9));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("AM hybrid (MA1) and all-digital (MA3) stations, 530 to 1710 kHz. Needs a radio that reaches medium wave (an HF radio, an RTL-SDR in direct sampling, an upconverter); 1 Msps is plenty.");
    flowNext(10 * gUi);
    const double step = am ? (S.raster9 ? 0.009 : 0.010) : 0.2;
    if (freqStepArrow("##hdrdn", ImGuiDir_Left)) go(snapChannel(snapChannel(a.freqMhz, am, S.raster9) - step, am, S.raster9));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("the channel below. Hold for a continuous change.");
    flowNext(4 * gUi);
    ImGui::AlignTextToFramePadding();
    if (am) ImGui::Text("%.0f kHz", a.freqMhz * 1000); else ImGui::Text("%.1f MHz", a.freqMhz);
    flowNext(4 * gUi);
    if (freqStepArrow("##hdrup", ImGuiDir_Right)) go(snapChannel(snapChannel(a.freqMhz, am, S.raster9) + step, am, S.raster9));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("the channel above. Hold for a continuous change.");
    if (am) {
        flowNext(10 * gUi);
        static const char* const items[] = {"10 kHz raster (the Americas)", "9 kHz raster (elsewhere)"};
        int cur = S.raster9 ? 1 : 0;
        float tw = 0;
        for (const char* it : items) tw = std::max(tw, ImGui::CalcTextSize(it).x);
        ImGui::SetNextItemWidth(tw + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x);
        if (ImGui::Combo("##hdrr", &cur, items, 2)) { S.raster9 = cur == 1; plat::prefs().setB("hdrAm9k", S.raster9); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("AM channels are 10 kHz apart in North and South America (where HD Radio AM is on the air), 9 kHz elsewhere");
    }
    flowEnd();
}

void stationTab(App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    bandBar(a);
    ImGui::Spacing();
    if (!locked(a)) {
        ImGui::PushFont(a.ui, 22.f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(!a.engine.running() ? "stopped" : !live(a) ? "starting" : stateText(t.state));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.engine.running() ? "Tune to an HD Radio station: FM 88 to 108 MHz or AM 530 to 1710 kHz (hybrid or all-digital); the waveform is found by itself."
                                                   : "Start the receiver. Test signal: pick the synthetic source.");
        ImGui::PopTextWrapPos();
        hdcNote(a);
        return;
    }
    // the station
    const HdrLotInfo* logo = stationLogo(t, selected(t));
    const Pic* lp = logo ? picture(a, *logo) : nullptr;
    if (lp) { drawPicture(lp, 56 * gUi, nullptr); ImGui::SameLine(0, 10 * gUi); }
    ImGui::BeginGroup();
    ImGui::PushFont(a.ui, 30.f);
    ImGui::TextColored(pal::dev() ? pal::accent() : ImVec4(0.55f, 0.80f, 1.f, 1), "%s", t.callSign.empty() ? "----" : t.callSign.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    {   // the long station name, else the universal short name; the other one below it when it differs
        const std::string& big = !t.longName.empty() ? t.longName : t.stationName;
        const std::string& small = !t.longName.empty() ? t.stationName : std::string();
        if (!big.empty()) {
            ImGui::PushFont(a.ui, 18.f);
            ImGui::TextUnformatted(big.c_str());
            ImGui::PopFont();
        }
        if (!small.empty() && small != big) ImGui::TextDisabled("%s", small.c_str());
    }
    if (!t.slogan.empty()) ImGui::TextUnformatted(t.slogan.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::Spacing();
    char b[96];
    std::string where = placeText(t);
    if (!t.countryCode.empty()) { snprintf(b, sizeof b, "%s%s, facility %d", where == "-" ? "" : "   ", t.countryCode.c_str(), t.facilityId); where = (where == "-" ? std::string() : where) + b; }
    kvRow(a, "location", where.empty() ? "-" : where, 80);
    kvRow(a, "message", t.message.empty() ? "-" : t.message, 80);
    if (!t.alert.empty()) {
        ImGui::TextDisabled("alert"); kvColumn(80 * gUi);
        ImGui::PushTextWrapPos(0); ImGui::TextColored(kBad, "%s", t.alert.c_str()); ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    // the programs
    sectionHeader(Ic::Radio, "Programs");
    if (t.programs.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("reading the program list"); ImGui::PopTextWrapPos(); }
    programCards(a, "st");
    hdcNote(a);
    ImGui::Spacing();
    if (const HdrProgram* p = selected(t)) nowPlaying(a, *p);
}

// ---------------------------------------------------------------- Data

void dataTab(App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    if (!locked(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "no HD Radio signal yet" : "start the receiver"); ImGui::PopTextWrapPos(); return; }
    sectionHeader(Ic::File, "Files received (LOT)");
    if (t.lots.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("none yet: pictures and other files come in the station's data services"); ImGui::PopTextWrapPos(); }
    else if (ImGui::BeginTable("hdrlot", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        const float th = 44 * gUi;
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, th);
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Port");
        ImGui::TableSetupColumn("State");
        ImGui::TableHeadersRow();
        for (const auto& l : t.lots) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, th + 4 * gUi);
            ImGui::TableNextColumn();
            const Pic* p = picture(a, l);
            if (isPicture(l)) {
                drawPicture(p, th, nullptr);
                if (p && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    drawPicture(p, 200 * gUi, nullptr);
                    ImGui::Text("%d x %d", p->w, p->h);
                    ImGui::EndTooltip();
                }
            } else ImGui::Dummy(ImVec2(th, th));
            ImGui::PushTextWrapPos(0);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(l.name.empty() ? "-" : l.name.c_str());
            ImGui::TextDisabled("LOT %d", l.lot);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(l.size ? sizeText(l.size).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", l.size ? mimeText(l.mime).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::Text("%04X", l.port); ImGui::PopFont();
            ImGui::TableNextColumn();
            if (l.complete) ImGui::TextColored(kGood, "complete");
            else ImGui::TextDisabled("%s of %s", sizeText(l.have).c_str(), l.size ? sizeText(l.size).c_str() : "?");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    sectionHeader(Ic::Layers, "Data services");
    bool any = false;
    if (ImGui::BeginTable("hdrdsv", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Service");
        ImGui::TableSetupColumn("Belongs to");
        ImGui::TableSetupColumn("Port");
        ImGui::TableSetupColumn("Transport");
        ImGui::TableSetupColumn("Content");
        ImGui::TableSetupColumn("Received");
        ImGui::TableHeadersRow();
        for (const auto& d : t.dataServices) {
            any = true;
            ImGui::TableNextRow();
            ImGui::PushTextWrapPos(0);
            ImGui::TableNextColumn();
            if (d.fromSig) ImGui::TextUnformatted(d.name.empty() ? ("service " + std::to_string(d.service)).c_str() : d.name.c_str());
            else ImGui::TextDisabled("listed in the station information");
            ImGui::TableNextColumn();
            if (d.program >= 0) ImGui::Text("HD%d", d.program + 1); else ImGui::TextDisabled("%s", d.fromSig ? "the station" : "-");
            ImGui::TableNextColumn();
            if (d.port >= 0) { ImGui::PushFont(a.mono, 0); ImGui::Text("%04X", d.port); ImGui::PopFont(); } else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", transportText(d.aasType));
            ImGui::TableNextColumn();
            if (d.fromSig) {
                const char* type = dect2::hdrDataTypeName(d.sdType);
                ImGui::TextUnformatted(mimeText(d.mime).c_str());
                if (d.sdType > 0 && *type) ImGui::TextDisabled("%s", type);
            }
            else {
                const char* type = dect2::hdrDataTypeName(d.sdType);
                ImGui::Text("type %d%s%s%s", d.sdType, *type ? " (" : "", type, *type ? ")" : "");
            }
            ImGui::TableNextColumn();
            if (d.fromSig) ImGui::TextDisabled("%llu packets, %s", (unsigned long long)d.packets, sizeText((uint32_t)std::min<uint64_t>(d.bytes, 0xFFFFFFFFu)).c_str());
            else ImGui::TextDisabled("-");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    if (!any) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("the station information guide has not come yet"); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Pictures (album art, station logos) are shown; traffic, weather and other services are listed with the amount received, their content is not decoded.");
    ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------- Signal (and the Receiver tab)

void signalRows(App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    char b[160];
    auto kf = [&](const char* k, const char* fmt, auto... v) { snprintf(b, sizeof b, fmt, v...); kvRow(a, k, b, 150); };
    const bool on = live(a);
    sectionHeader(Ic::Chip, "Receiver");
    kvRow(a, "state", on ? stateText(t.state) : a.engine.running() ? "starting" : "stopped", 150);
    kvRow(a, "waveform", !on || t.band == 0 ? "-" : t.band == 1 ? "FM hybrid (analog FM with digital sidebands)"
                         : t.serviceMode == 2 ? "AM all-digital (the carrier and digital sidebands, no analog audio)" : "AM hybrid (analog AM with digital sidebands)", 150);
    kvRow(a, "service mode", on && !t.modeName.empty() ? t.modeName : "-", 150);
    kf("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kf("signal time", "%.1f s", t.timeSec);
    kf("receiver load", "%.1f %% of real time", t.loadPct);
    sectionHeader(Ic::Gauge, "Measurements");
    if (on && t.state >= 2) {
        if (t.band == 1) {
            kf("MER", "%.1f dB lower sideband, %.1f dB upper", t.merLower, t.merUpper);
            if (std::fabs(t.merLower - t.merUpper) >= 6.0f)
                ImGui::TextDisabled("%s", t.merLower < t.merUpper ? tr("The lower sideband is much worse: probably a station on the channel below. The decoder relies on the cleaner side.")
                                                                  : tr("The upper sideband is much worse: probably a station on the channel above. The decoder relies on the cleaner side."));
        }
        else kf("MER", "%.1f dB (primary sidebands)", t.merLower);
        kf("channel bit errors", "%.2f %% (P1, before the Viterbi decoder)", t.ber * 100);
        kf("carrier offset", "%+.1f Hz", t.cfoHz);
        kf("block count", "%d", t.blockCount);
    } else kvRow(a, "MER", "-", 150);
    kf("block syncs", "%llu", (unsigned long long)t.syncCount);
    sectionHeader(Ic::Layers, "Counters");
    kf("P1 frames", "%llu ok, %llu bad header", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.band == 2 || t.p3Ok + t.p3Bad) kf("P3 frames", "%llu ok, %llu bad header", (unsigned long long)t.p3Ok, (unsigned long long)t.p3Bad);
    kf("PIDS frames", "%llu ok, %llu bad CRC", (unsigned long long)t.pidsOk, (unsigned long long)t.pidsBad);
    kf("audio PDUs", "%llu", (unsigned long long)t.pduCount);
    kf("AAS packets", "%llu ok, %llu bad", (unsigned long long)t.aasPackets, (unsigned long long)t.aasBad);
    if (t.hdcFound) kf("HDC sound", "HD%d: %llu decoded, %llu errors, %llu lost (silence), %llu decoder restarts", t.soundProgram + 1, (unsigned long long)t.hdcDecoded, (unsigned long long)t.hdcErrors, (unsigned long long)t.hdcLost, (unsigned long long)t.hdcResets);
    sectionHeader(Ic::Speaker, "Audio");
    for (const auto& p : t.programs) {
        snprintf(b, sizeof b, "%s, %.1f kbit/s, %llu packets ok, %llu bad", codecText(p.codecMode).c_str(), p.kbps, (unsigned long long)p.packetsOk, (unsigned long long)p.packetsBad);
        kvRow(a, (progLabel(p) + " " + progName(p)).c_str(), b, 150);
    }
    hdcNote(a);
}

void signalTab(App& a) {
    ImGui::BeginChild("##hdr_sig", ImVec2(0, 0));
    signalRows(a);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the analysis row

void panels(App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    const bool on = locked(a);
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float gap = 12 * gUi;
    const float plotH = std::max(70.f, H - ImGui::GetTextLineHeightWithSpacing() - 8);
    const float side = std::min(plotH, std::max(80.f, W * 0.2f));
    const float colW = std::max(90.f, (W - 4 * gap - side) / 2.f);
    const std::vector<cf32> none;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + gap * 0.5f);
    ImGui::BeginGroup();
    captionFit(side, "%s (%zu)", t.band == 2 ? "Primary 64-QAM" : "Data QPSK", on ? t.constel.size() : (size_t)0);
    scatter("##hdr_const", on ? t.constel : none, ImVec2(side, side), t.band == 2 ? 4.2 : 1.6, pal::accent(0.7f));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "MER (dB)");
    historyPlot("##hdr_mer", "dB", S.mer, ImVec2(colW, plotH));
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    captionFit(colW, "P1 frames ok (%%)");
    historyPlot("##hdr_p1", "%", S.p1, ImVec2(colW, plotH));
    ImGui::EndGroup();
}


// Traffic and radar maps put together from the LOT files of the data services (TMT_ tiles, DWRO_ overlay, DWRI_ position)
void mapsView(App& a) {
    static dect2::hdr::TrafficMap traffic;
    static std::map<std::string, uint64_t> seen;
    static Pic trafficPic, radarPic;
    static dect2::hdr::RadarInfo radar;
    static std::string radarStamp;
    const HdrTelemetry& t = a.rx.hdr;
    for (const auto& l : t.lots) {
        if (!l.complete) continue;
        const std::string key = std::to_string(l.port) + ":" + l.name;
        if (seen[key] == l.version + 1) continue;
        int x, y; std::string st, id;
        const bool isT = dect2::hdr::parseTrafficName(l.name, x, y, st), isR = dect2::hdr::parseRadarName(l.name, id, st), isI = l.name.rfind("DWRI_", 0) == 0;
        if (!isT && !isR && !isI) continue;
        std::vector<uint8_t> bytes;
        if (!a.engine.hdr().lotBytes(l.port, l.lot, bytes) || bytes.empty()) continue;
        seen[key] = l.version + 1;
        if (isI) { double c[4]; if (dect2::hdr::parseRadarInfo(std::string(bytes.begin(), bytes.end()), id, c)) { radar.valid = true; radar.id = id; memcpy(radar.corners, c, sizeof c); } continue; }
        int w = 0, h = 0; std::vector<uint8_t> rgba;
        if (!dect2::decodePng(bytes.data(), bytes.size(), w, h, rgba)) continue;
        auto upload = [](Pic& p, const dect2::hdr::MapImage& m) { if (!p.img || p.w != m.w || p.h != m.h) { delete p.img; p.img = gGfx->createImage(m.w, m.h, 0xFF000000u); } std::vector<uint32_t> px((size_t)m.w * m.h); memcpy(px.data(), m.rgba.data(), px.size() * 4); p.img->update(0, 0, m.w, m.h, px.data()); p.w = m.w; p.h = m.h; p.failed = false; };
        if (isT && traffic.addTile(x, y, st, rgba.data(), w, h)) upload(trafficPic, traffic.image());
        if (isR && st >= radarStamp) { radarStamp = st; dect2::hdr::MapImage im; dect2::hdr::radarComposite(rgba.data(), w, h, im); upload(radarPic, im); }
    }
    if (!trafficPic.img && !radarPic.img) return;
    if (trafficPic.img) { ImGui::TextDisabled("%s %s", tr("Traffic map"), traffic.image().stamp.c_str()); drawPicture(&trafficPic, 300 * gUi, ""); ImGui::SameLine(); }
    if (radarPic.img) { ImGui::BeginGroup(); ImGui::TextDisabled("%s %s", tr("Radar"), radarStamp.c_str()); if (radar.valid) ImGui::TextDisabled("%.2f, %.2f - %.2f, %.2f", radar.corners[0], radar.corners[1], radar.corners[2], radar.corners[3]); drawPicture(&radarPic, 300 * gUi, ""); ImGui::EndGroup(); }
}

void tab(App& a) {
    mapsView(a);
    subNav("hdrv", S.view, {"Station", "Data", "Signal"});
    if (S.view == 2) { signalTab(a); return; }
    ImGui::BeginChild("##hdr_tab", ImVec2(0, 0));
    if (S.view == 0) stationTab(a);
    else dataTab(a);
    ImGui::EndChild();
}

void receiver(App& a) {
    ImGui::BeginChild("##hdr_rcv", ImVec2(0, 0));
    bandBar(a);
    ImGui::Spacing();
    signalRows(a);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the list on the right

void list(App& a) {
    const HdrTelemetry& t = a.rx.hdr;
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see the programs"); ImGui::PopTextWrapPos(); return; }
    if (t.programs.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.state >= 2 ? "reading the program list" : "searching for an HD Radio signal"); ImGui::PopTextWrapPos(); return; }
    if (!t.callSign.empty()) { ImGui::PushFont(a.ui, 18.f); ImGui::TextUnformatted(t.callSign.c_str()); ImGui::PopFont(); }
    programCards(a, "side");
    hdcNote(a);
}

// ---------------------------------------------------------------- status bar, summary, meters

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const HdrTelemetry& t = a.rx.hdr;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const double now = ImGui::GetTime();
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Signal", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : t.state >= 2 ? 1 : t.state == 1 ? 2 : 0); flowNext(12 * gUi);
    lamp("P1", !on ? 0 : blockLamp(S.p1OkAt, S.p1BadAt, 4.0, now)); flowNext(12 * gUi);
    lamp("PIDS", !on ? 0 : blockLamp(S.pidsOkAt, S.pidsBadAt, 2.0, now)); flowNext(12 * gUi);
    lamp("SIS", !on ? 0 : !t.callSign.empty() ? 1 : t.pidsOk ? 2 : 0); flowNext(12 * gUi);
    lamp("PSD", !on ? 0 : t.psdSeen ? 1 : 0); flowNext(12 * gUi);
    lamp("Data", !on ? 0 : t.dataSeen ? 1 : t.aasBad && !t.aasPackets ? 3 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[80];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", stateText(t.state));
    if (t.state < 2) { snprintf(b, sizeof b, "%.0f dBFS", t.levelDb); ro("Level", b); return; }
    snprintf(b, sizeof b, "%s %s", t.band == 2 ? "AM" : "FM", t.modeName.c_str()); ro("Mode", b);
    snprintf(b, sizeof b, "%.1f dB", t.snrDb); ro("MER", b);
    snprintf(b, sizeof b, "%+.1f Hz", t.cfoHz); ro("CFO", b);
    if (!t.callSign.empty()) ro("Station", t.callSign);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HD Radio";
    if (live(a)) l2 = dect2::hdrSummary(a.rx.hdr);
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const HdrTelemetry& t = a.rx.hdr;
    const uint64_t n = t.blocksOk + t.blocksBad;
    const double okPct = n ? 100.0 * (double)t.blocksOk / (double)n : 0;
    out.push_back({"MER  dB", "%.1f", t.snrDb, 0, 30, t.state < 2 ? 0 : t.snrDb >= 10 ? 1 : t.snrDb >= 5 ? 2 : 3});
    out.push_back({"P1 OK  %", "%.0f", okPct, 0, 100, n == 0 ? 0 : okPct > 95 ? 1 : okPct > 70 ? 2 : 3});
    out.push_back({"LEVEL  dBFS", "%.0f", t.levelDb, -100, 0, t.levelDb > -6 ? 3 : 0});
}

// ---------------------------------------------------------------- the test signal

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    static const char* items[] = {"FM hybrid (MP1)", "AM hybrid (MA1)", "FM extended hybrid (MP3)", "AM all-digital (MA3)"};
    ImGui::TextDisabled("test signal");
    flowNext();
    const int cur = sc.modeOpt[0] >= 0 && sc.modeOpt[0] <= 3 ? sc.modeOpt[0] : 0;
    float tw = 0;
    for (const char* s : items) tw = std::max(tw, ImGui::CalcTextSize(s).x);
    ImGui::SetNextItemWidth(tw + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().FramePadding.x);
    if (ImGui::BeginCombo("##hdrw", items[cur])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(items[i], cur == i)) { sc.modeOpt[0] = i; changed = true; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Station WONR with two programs (three in MP3), program data and a picture. The audio packets are filler, so there is nothing to hear.");
}

} // namespace

extern const ModeUi kHdrUi;
const ModeUi kHdrUi = {
    .sideTitle = "PROGRAMS",
    .tabName = "Radio",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
