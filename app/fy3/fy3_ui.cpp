// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FY-3 AHRPT and FY-3 MPT screens (families 235 and 236): the instrument pictures of the pass growing row by row (VIRR, MERSI: a colour composite or one
// channel in grey), Save PNG and New picture; the satellite preset with its frequency and symbol rate, the symbol rate and roll-off for anything else;
// the receiver's state with a "why it does not decode yet" line, the margin over the Es/N0 the decoder needs, the next pass of the satellite when a TLE
// file is in the data folder (the Satellites tab downloads them), and the instruments heard.
// Both modes use the same code; the template argument is the product (0 AHRPT, 1 MPT).
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/fy3/fy3_proto.h"
#include "dect2/fy3/fy3_rx.h"
#include "dect2/fy3/fy3_tel.h"
#include "dect2/geo/passes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using dect2::Fy3Image;
using dect2::Fy3Row;
using dect2::Fy3Telemetry;
namespace fs = std::filesystem;

constexpr float kNeedDb = 2.2f;   // Es/N0 at which this receiver decodes 99% of the frames on the test signal (docs/fy3.md)

struct State {
    bool loaded = false;
    int view = 0, preset = 0, pushedPreset = -1, instSel = 0;
    double rsMsym = 0;                 // 0: automatic
    double pushedRs = -1;
    gfx::Image* tex = nullptr;
    int texRows = 0, texW = 0;
    uint64_t imgId = 0;
    int shownView = -1, shownInst = -1;
    std::vector<const Fy3Row*> drawn;
    bool follow = true;
    std::string msg;
    // contrast of the microwave instruments (their 16-bit counts do not fill the eight bits): the 2nd and 98th percentile of each channel
    std::vector<uint8_t> lo, hi;
    int stretchRows = 0, stretchInst = -1;
    // the next pass
    double passAt = -1000;
    std::string passText;
};
State S[2];

fs::path fsPath(const std::string& p) { return fs::path(reinterpret_cast<const char8_t*>(p.c_str())); }

const char* keyPre(int p) { return p ? "fy3mPre" : "fy3aPre"; }
const char* keyView(int p) { return p ? "fy3mView" : "fy3aView"; }
const char* keyRs(int p) { return p ? "fy3mRs" : "fy3aRs"; }

void loadState(int p) {
    State& s = S[p];
    if (s.loaded) return;
    s.loaded = true;
    int n; dect2::fy3Presets(p, n);
    s.preset = std::clamp((int)plat::prefs().getI(keyPre(p), 0), 0, n);
    s.view = std::max(0, (int)plat::prefs().getI(keyView(p), 0));
    s.rsMsym = std::clamp(plat::prefs().getD(keyRs(p), 0.0), 0.0, 500.0);
}

bool live(const App& a, int p) { return a.engine.running() && a.rx.standard == (p ? 237 : 236); }   // the engine reports its standard code minus one
const Fy3Telemetry& tel(const App& a, int p) { return p ? a.rx.fy3m : a.rx.fy3a; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// --- colour composites and channels ---------------------------------------------------------------------------------------------------------
struct Comp { int r, g, b; bool invB; const char* label; };
// the composite of an instrument, -1 when it has none
Comp compositeFor(int inst, int nch) {
    using namespace dect2::fy3;
    if (inst == Virr && nch >= 10) return {0, 8, 6, false, "True colour (1, 9, 7)"};
    if ((inst == Mersi1 || inst == Mersi2 || inst == Mersi3) && nch >= 4) return {2, 1, 0, false, "True colour (3, 2, 1)"};
    return {-1, -1, -1, false, ""};
}
bool thermal(int inst, int ch) {
    using namespace dect2::fy3;
    if (inst == Virr) return ch == 2 || ch == 3 || ch == 4;
    if (inst == Mersi1) return ch == 4;
    if (inst == Mersi2 || inst == Mersi3) return ch == 4 || ch == 5;
    return false;
}

struct Views { std::vector<std::string> names; bool hasColour = false; Comp comp{}; };
Views viewsFor(const Fy3Image& im) {
    Views v;
    v.comp = compositeFor(im.inst, im.channels);
    v.hasColour = v.comp.r >= 0;
    if (v.hasColour) v.names.push_back(v.comp.label);
    if (im.inst == dect2::fy3::Virr && im.channels >= 5) { v.names.push_back("False colour (2, 1, 4 inverted)"); }
    for (int c = 0; c < im.channels; c++) v.names.push_back(std::string("Channel ") + dect2::fy3::instChannelName(im.inst, c));
    return v;
}

bool microwave(int inst) { return inst == dect2::fy3::Mwri || inst == dect2::fy3::Mwri2; }

void computeStretch(const Fy3Image& im, State& s) {
    s.stretchInst = im.inst; s.stretchRows = im.rows;
    s.lo.assign((size_t)im.channels, 0); s.hi.assign((size_t)im.channels, 255);
    if (!microwave(im.inst)) return;
    for (int c = 0; c < im.channels; c++) {
        uint32_t h[256] = {};
        uint32_t n = 0;
        for (int r = 0; r < im.rows; r += 3) {
            const Fy3Row* l = (size_t)r < im.row.size() ? im.row[(size_t)r].get() : nullptr;
            if (!l) continue;
            for (int x = 0; x < im.width; x += 2) { h[l->px[(size_t)c * (size_t)im.width + (size_t)x]]++; n++; }
        }
        if (n < 50) continue;
        uint32_t acc = 0; int lo = 0, hi = 255;
        for (int v = 0; v < 256; v++) { acc += h[v]; if (acc >= n * 2 / 100) { lo = v; break; } }
        acc = 0;
        for (int v = 255; v >= 0; v--) { acc += h[v]; if (acc >= n * 2 / 100) { hi = v; break; } }
        if (hi - lo < 4) hi = std::min(255, lo + 4);
        s.lo[(size_t)c] = (uint8_t)lo; s.hi[(size_t)c] = (uint8_t)hi;
    }
}

void composeRow(const Fy3Image& im, const Fy3Row* l, int view, uint32_t* px, const State* st = nullptr) {
    const int w = im.width;
    if (!l) { for (int i = 0; i < w; i++) px[i] = 0xFF000000u; return; }
    const uint8_t* p = l->px.data();
    const Views v = viewsFor(im);
    int idx = view;
    int mode = 0;   // 0 grey channel, 1 composite, 2 false colour
    if (v.hasColour) { if (idx == 0) mode = 1; else idx--; }
    if (mode == 0 && im.inst == dect2::fy3::Virr && im.channels >= 5) { if (idx == 0) mode = 2; else idx--; }
    for (int i = 0; i < w; i++) {
        uint32_t r, g, b;
        if (mode == 1) { r = p[(size_t)v.comp.r * (size_t)w + (size_t)i]; g = p[(size_t)v.comp.g * (size_t)w + (size_t)i]; b = p[(size_t)v.comp.b * (size_t)w + (size_t)i]; }
        else if (mode == 2) { r = p[(size_t)w + (size_t)i]; g = p[(size_t)i]; b = (uint32_t)(255 - p[3 * (size_t)w + (size_t)i]); }
        else {
            const int c = std::clamp(idx, 0, im.channels - 1);
            uint32_t g8 = p[(size_t)c * (size_t)w + (size_t)i];
            if (st && microwave(im.inst) && (size_t)c < st->lo.size()) { const int lo = st->lo[(size_t)c], hi = st->hi[(size_t)c]; g8 = (uint32_t)std::clamp((int)(g8 - lo) * 255 / std::max(1, hi - lo), 0, 255); }
            if (thermal(im.inst, c)) g8 = 255 - g8;
            r = g = b = g8;
        }
        px[i] = 0xFF000000u | b << 16 | g << 8 | r;
    }
}

const Fy3Image* pickImage(const Fy3Telemetry& t, State& s) {
    if (t.images.empty()) return nullptr;
    s.instSel = std::clamp(s.instSel, 0, (int)t.images.size() - 1);
    return t.images[(size_t)s.instSel].get();
}

void update(const Fy3Telemetry& t, State& s) {
    const Fy3Image* im = pickImage(t, s);
    if (!im || !gGfx || im->rows <= 0) return;
    const int w = im->width;
    const bool fresh = im->id != s.imgId || s.shownView != s.view || s.shownInst != im->inst || s.texW != w;
    if (fresh) { s.imgId = im->id; s.shownView = s.view; s.shownInst = im->inst; s.drawn.clear(); }
    if (im->rows > s.texRows || !s.tex || s.texW != w) {
        const int cap = std::max(128, (im->rows + 127) / 128 * 128);
        delete s.tex;
        s.tex = gGfx->createImage(w, cap, 0xFF000000u);
        s.texRows = cap; s.texW = w;
        s.drawn.clear();
    }
    if (!s.tex) return;
    if (microwave(im->inst) && (s.stretchInst != im->inst || im->rows >= 2 * std::max(8, s.stretchRows))) { computeStretch(*im, s); s.drawn.clear(); }
    s.drawn.resize((size_t)im->rows, nullptr);
    std::vector<uint32_t> px((size_t)w);
    for (int r = 0; r < im->rows; r++) {
        const Fy3Row* cur = (size_t)r < im->row.size() ? im->row[(size_t)r].get() : nullptr;
        if (cur == s.drawn[(size_t)r]) continue;
        composeRow(*im, cur, s.view, px.data(), &s);
        s.tex->update(0, r, w, 1, px.data());
        s.drawn[(size_t)r] = cur;
    }
}

void savePng(const Fy3Telemetry& t, State& s, int p) {
    const Fy3Image* im = pickImage(t, s);
    if (!im || im->rows <= 0) { s.msg = "nothing to save yet"; return; }
    const int w = im->width;
    std::vector<uint32_t> px((size_t)im->rows * (size_t)w);
    for (int r = 0; r < im->rows; r++) composeRow(*im, (size_t)r < im->row.size() ? im->row[(size_t)r].get() : nullptr, s.view, &px[(size_t)r * (size_t)w], &s);
    char ts[32];
    const time_t tt = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    char name[96];
    snprintf(name, sizeof name, "%s-fy3-%s-%s-v%d.png", ts, p ? "mpt" : "ahrpt", dect2::fy3::instName(im->inst), s.view);
    const std::string dir = plat::dataDir() + "/fy3";
    std::error_code ec;
    fs::create_directories(fsPath(dir), ec);
    const std::string path = dir + "/" + name;
    s.msg = gfx::writePng(path.c_str(), reinterpret_cast<const uint8_t*>(px.data()), w, im->rows) ? "saved " + path : "could not write " + path;
}

// --- the next pass of the satellite (TLE files of the Satellites tab) ----------------------------------------------------------------------------
std::string readAll(const std::string& p) {
    std::ifstream in(fsPath(p), std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Which satellites the preset stands for: the TLE names to look for ("FENGYUN 3C" / "FY-3C")
std::vector<std::string> presetSats(int p, int preset) {
    int n; const dect2::Fy3Preset* pr = dect2::fy3Presets(p, n);
    std::vector<std::string> out;
    std::string s = preset > 0 && preset <= n ? pr[preset - 1].sats : (p ? "FY-3A, FY-3B, FY-3C" : "FY-3C, FY-3A, FY-3B");
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) { while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1); if (!tok.empty()) out.push_back(tok); }
    return out;
}

bool tleMatches(const std::string& name, const std::string& sat /* "FY-3C" */) {
    std::string n;
    for (char c : name) n += (char)std::toupper((unsigned char)c);
    const char letter = sat.back();
    return n.find(std::string("FENGYUN 3") + letter) != std::string::npos || n.find(std::string("FENGYUN-3") + letter) != std::string::npos || n.find(std::string("FY-3") + letter) != std::string::npos ||
           n.find(std::string("FY 3") + letter) != std::string::npos;
}

std::string clockOf(double t) {
    char b[32];
    const std::time_t tt = (std::time_t)t;
    std::tm lt = *std::localtime(&tt);
    std::strftime(b, sizeof b, "%H:%M", &lt);
    return b;
}

const std::string& passText(State& s, int p) {
    const double now = (double)time(nullptr);
    if (now - s.passAt < 600) return s.passText;
    s.passAt = now;
    s.passText.clear();
    double lat, lon;
    receiverLocation(lat, lon);
    dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
    const auto sats = presetSats(p, s.preset);
    std::vector<dect2::Tle> tles;
    for (const char* f : {"weather.txt", "stations.txt", "keplerpass.txt", "pasted.txt", "amateur.txt"}) {
        const std::string path = plat::dataDir() + "/tle/" + f;
        std::error_code ec;
        if (!fs::exists(fsPath(path), ec)) continue;
        for (const dect2::Tle& t : dect2::parseTleText(readAll(path))) tles.push_back(t);
    }
    if (tles.empty()) { s.passText = "No satellite elements yet: download the weather satellites in the Satellites tab to see the next pass here."; return s.passText; }
    double best = 1e18; std::string txt;
    for (const std::string& sat : sats)
        for (const dect2::Tle& t : tles) {
            if (!tleMatches(t.name, sat)) continue;
            dect2::Sgp4 sg;
            if (!sg.init(t)) continue;
            const auto ps = dect2::findPasses(sg, o, now, now + 24 * 3600, 5);
            if (ps.empty()) continue;
            if (ps.front().aos < best) {
                best = ps.front().aos;
                char b[200];
                if (ps.front().aos <= now) snprintf(b, sizeof b, "%s is overhead now until %s (highest %.0f deg).", sat.c_str(), clockOf(ps.front().los).c_str(), ps.front().maxElDeg);
                else snprintf(b, sizeof b, "Next pass: %s %s - %s, highest %.0f deg (aim at azimuth %.0f deg at the start).", sat.c_str(), clockOf(ps.front().aos).c_str(), clockOf(ps.front().los).c_str(), ps.front().maxElDeg, ps.front().aosAzDeg);
                txt = b;
            }
        }
    s.passText = txt.empty() ? std::string("No pass of ") + (sats.empty() ? "FY-3" : sats[0]) + " in the next 24 hours above 5 degrees (or its elements are not in the weather list)." : txt;
    return s.passText;
}

// --- screens --------------------------------------------------------------------------------------------------------------------------
template <int P> void tab(App& a) {
    loadState(P);
    State& s = S[P];
    const Fy3Telemetry& t = tel(a, P);
    const bool on = live(a, P);
    if (on) update(t, s);
    const Fy3Image* im = on ? pickImage(t, s) : nullptr;
    if (im && t.images.size() > 1) {
        std::vector<std::string> nm;
        for (const auto& i : t.images) nm.push_back(dect2::fy3::instName(i->inst));
        std::string joined;
        for (auto& n : nm) { joined += n; joined.push_back('\0'); }
        ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
        ImGui::Combo("##fy3inst", &s.instSel, joined.c_str());
        flowNext(8 * gUi);
    }
    if (im) {
        const Views v = viewsFor(*im);
        std::string joined;
        for (auto& n : v.names) { joined += n; joined.push_back('\0'); }
        s.view = std::clamp(s.view, 0, (int)v.names.size() - 1);
        ImGui::SetNextItemWidth(std::min(250 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::Combo("##fy3view", &s.view, joined.c_str())) { plat::prefs().setI(keyView(P), s.view); plat::prefs().flush(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A colour composite, or one channel in grey (thermal channels are shown with cold clouds white).\nMERSI's 250 m channels are averaged to the 1 km grid of the picture.");
        flowNext(8 * gUi);
    }
    if (ImGui::Button("Save PNG")) savePng(t, s, P);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The picture as shown, at full size, in <data folder>/fy3/");
    flowNext(5 * gUi);
    if (runningButton(a, tr("New picture"))) { (P ? a.engine.fy3Mpt() : a.engine.fy3Ahrpt()).clearImage(); s.msg.clear(); }
    flowNext(8 * gUi);
    ImGui::Checkbox("Follow", &s.follow);
    flowEnd();
    if (!s.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (!im || im->rows == 0 || !s.tex) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        if (!a.engine.running()) {
            ImGui::TextDisabled("%s", P ? "Start the receiver while FY-3A, FY-3B or FY-3C is overhead. FY-3 MPT is an X-band downlink (7.75 - 7.78 GHz, 18.7 Msym/s): it needs a dish with an X-band feed, "
                                           "a down-converter to L-band and a radio that samples 28 Msps or more (20 Msps gives a reduced result)."
                                         : "Start the receiver while FY-3C (1701.4 MHz, 2.6 Msym/s) or FY-3A / FY-3B (1704.5 MHz, 2.8 Msym/s) is overhead. It needs a dish (about 1 m or more, following the satellite) "
                                           "with an LNA at the feed, and a fast radio: 4 Msps or more (HackRF, Airspy, SDRplay, Pluto; an RTL-SDR is too slow). FY-3D and later send AHRPT in X-band.");
            ImGui::TextDisabled("%s", passText(s, P).c_str());
        } else if (on) {
            ImGui::TextDisabled("%s", dect2::fy3Summary(t).c_str());
            if (!t.hint.empty()) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1), "%s", t.hint.c_str());
            ImGui::TextDisabled("%s", passText(s, P).c_str());
        } else ImGui::TextDisabled("starting");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild(P ? "##fy3mpic" : "##fy3apic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float w = std::max(50.f, ImGui::GetContentRegionAvail().x), z = w / (float)im->width;
    const float h = (float)im->rows * z;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(s.tex->texture(), p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, (float)im->rows / (float)s.texRows));
    ImGui::Dummy(ImVec2(w, h));
    if (s.follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

template <int P> void list(App& a) {
    if (!live(a, P)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const Fy3Telemetry& t = tel(a, P);
    if (!ImGui::BeginTable(P ? "##fy3ml" : "##fy3al", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("Instrument", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("Rows / packets");
    ImGui::TableHeadersRow();
    for (const auto& st : t.inst) {
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        ImGui::TextUnformatted(dect2::fy3::instName(st.inst));
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)st.frames);
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)st.units);
    }
    for (int v = 0; v < 64; v++) {
        if (!t.vcidFrames[v]) continue;
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("virtual channel %d%s", v, v == 63 ? " (fill)" : "");
        ImGui::TableNextColumn(); ImGui::TextDisabled("%llu", (unsigned long long)t.vcidFrames[v]);
        ImGui::TableNextColumn();
    }
    ImGui::EndTable();
}

template <int P> void receiver(App& a) {
    if (!live(a, P)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const Fy3Telemetry& t = tel(a, P);
    static const char* const kSat[8] = {"FY-3A", "FY-3B", "FY-3C", "FY-3D", "FY-3E", "FY-3F", "FY-3G", "FY-3H"};
    kvRow(a, "state", "%s", dect2::fy3Summary(t).c_str());
    if (!t.hint.empty()) kvRow(a, "why not yet", "%s", t.hint.c_str());
    kvRow(a, "symbol rate", "%.4f Msym/s%s", t.symbolRate / 1e6, t.rateMeasured > 0 ? " (measured from the signal)" : t.satSetting ? " (the preset's)" : " (the first that fits)");
    kvRow(a, "carrier", "offset %+.2f kHz, lock %.2f", t.cfoHz / 1e3, t.carrierLock);
    const float margin = t.snrDb - kNeedDb;
    kvRow(a, "Es/N0", "%.1f dB (the decoder needs %.1f dB: %s%.1f dB %s)", t.snrDb, kNeedDb, margin >= 0 ? "+" : "", margin, margin >= 3 ? "to spare" : margin >= 0 ? "margin, a fade may cost frames" : "short, the dish or the LNA is too small");
    kvRow(a, "symbol clock", "%+.0f ppm", t.clockPpm);
    kvRow(a, "convolutional codes", "%s (metrics %.2f / %.2f%s)", t.railLock ? "locked" : "searching", t.railMetric[0], t.railMetric[1], t.diffMode >= 0 ? ", arrangement of the rails found" : "");
    kvRow(a, "frames", "%llu good, %llu failed, %llu bytes corrected, %llu sync words%s", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsCorrected,
          (unsigned long long)t.asmHits, t.frameLock ? ", in sync" : "");
    if (t.scid >= 0) kvRow(a, "VCDU", "%s (spacecraft %d), virtual channel %d, counter %u", t.sat >= 0 && t.sat < 8 ? kSat[t.sat] : "unknown satellite", t.scid, t.vcid, t.vcduCounter);
    if (t.lines) {
        const uint32_t s = t.onboardMs / 1000;
        kvRow(a, "on board", "%02u:%02u:%02u (satellite time), %llu picture rows", s / 3600 % 24, s / 60 % 60, s % 60, (unsigned long long)t.lines);
    }
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

template <int P> void panels(App& a) {
    const Fy3Telemetry& t = tel(a, P);
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    if (live(a, P)) for (const auto& p : t.constellation) pts.emplace_back(p.first, p.second);
    scatter(P ? "##fy3mc" : "##fy3ac", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

template <int P> void status(App& a) {
    const bool run = a.engine.running(), on = live(a, P);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const Fy3Telemetry& t = tel(a, P);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Code", !on ? 0 : t.railLock ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::fy3Summary(t).c_str()); ImGui::PopFont();
}

template <int P> void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = P ? "FY-3 MPT" : "FY-3 AHRPT";
    if (live(a, P)) l2 = dect2::fy3Summary(tel(a, P));
}

template <int P> void tick(App& a) {
    loadState(P);
    State& s = S[P];
    if (!a.engine.running()) { s.pushedPreset = -1; s.pushedRs = -1; return; }
    dect2::Fy3Receiver& rx = P ? a.engine.fy3Mpt() : a.engine.fy3Ahrpt();
    if (s.pushedPreset != s.preset) { rx.setPreset(s.preset); s.pushedPreset = s.preset; }
    if (s.pushedRs != s.rsMsym) { rx.setSymbolRate(s.rsMsym * 1e6); s.pushedRs = s.rsMsym; }
}

template <int P> void tuner(App& a, bool& retune) {
    loadState(P);
    State& s = S[P];
    ImGui::TextDisabled("Satellite");
    sameLineIf(90 * gUi, 5 * gUi);
    int n = 0;
    const dect2::Fy3Preset* pr = dect2::fy3Presets(P, n);
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    const char* cur = s.preset > 0 ? pr[s.preset - 1].label : "Automatic (the symbol rate is measured)";
    if (ImGui::BeginCombo(P ? "##fy3mp" : "##fy3ap", cur)) {
        if (ImGui::Selectable("Automatic (the symbol rate is measured)", s.preset == 0)) { s.preset = 0; plat::prefs().setI(keyPre(P), 0); plat::prefs().flush(); }
        for (int i = 0; i < n; i++)
            if (ImGui::Selectable(pr[i].label, s.preset == i + 1)) {
                s.preset = i + 1;
                tuneFreq(a, pr[i].mhz); retune = true;
                s.rsMsym = 0;
                plat::prefs().setI(keyPre(P), s.preset); plat::prefs().setD(keyRs(P), 0); plat::prefs().flush();
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(P ? "FY-3A / B / C medium rate picture transmission: X-band, 18.7 Msym/s, MERSI pictures.\nIt needs an X-band down-converter, and a radio that samples 28 Msps or more for the whole signal."
                                                   : "L-band AHRPT: FY-3C 1701.4 MHz at 2.6 Msym/s, FY-3A / B 1704.5 MHz at 2.8 Msym/s (VIRR pictures).\nFY-3D to FY-3H send AHRPT in X-band (7.8 GHz, 30 - 38.4 Msym/s; FY-3G 6): the same decoder, a down-converter and a very fast radio.\nAutomatic measures the symbol rate; expect Doppler of up to +-40 kHz, the receiver follows it.");
    flowNext(8 * gUi);
    ImGui::TextDisabled("Symbol rate");
    sameLineIf(150 * gUi, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(110 * gUi, ImGui::GetContentRegionAvail().x));
    float rs = (float)s.rsMsym;
    if (ImGui::InputFloat(P ? "##fy3mrs" : "##fy3ars", &rs, 0, 0, "%.4f Msym/s")) { s.rsMsym = std::clamp((double)rs, 0.0, 500.0); plat::prefs().setD(keyRs(P), s.rsMsym); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("0: the preset's rate, or measured from the signal when the satellite is Automatic. Any other value is used as it is (no limits).");
    flowEnd();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", passText(s, P).c_str());
    const bool xband = P || (s.preset >= 3);
    ImGui::TextDisabled("%s", xband ? "Dish: 1.2 m or larger with an X-band feed and a low-noise down-converter; the decoder needs an Es/N0 of about 2.2 dB."
                                    : "Dish: 1 m or more, tracking the satellite, with an LNA (NF under 1 dB) at the feed and a short cable; the decoder needs an Es/N0 of about 2.2 dB (a 1 m dish with a good LNA gives 6 - 9 dB at 30 degrees elevation).");
    ImGui::PopTextWrapPos();
}

template <int P> void synth(App& a, bool& changed) {
    dect2::SynthConfig& sc = a.tune.synth;
    static const char* const kM[] = {"FY-3A", "FY-3B", "FY-3C", "FY-3D (MERSI-II)", "FY-3E (MERSI-LL)", "FY-3F (MERSI-3)", "FY-3G (MERSI-RM)", "FY-3H (MERSI-3)"};
    ImGui::TextDisabled("satellite");
    flowNext(); ImGui::SetNextItemWidth(190 * gUi);
    int m = std::clamp(sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 2, 0, 7);
    if (ImGui::Combo(P ? "##fy3msm" : "##fy3asm", &m, kM, 8)) { sc.modeOpt[0] = m; changed = true; }
    flowNext();
    bool conj = (sc.modeOpt[1] & 1) != 0, swp = (sc.modeOpt[1] & 2) != 0;
    if (ImGui::Checkbox("Q negated", &conj)) { sc.modeOpt[1] = (sc.modeOpt[1] & ~1) | (conj ? 1 : 0); changed = true; }
    flowNext();
    if (ImGui::Checkbox("I / Q exchanged", &swp)) { sc.modeOpt[1] = (sc.modeOpt[1] & ~2) | (swp ? 2 : 0); changed = true; }
}

template <int P> void meters(const App& a, std::vector<ModeMeter>& out) {
    const Fy3Telemetry& t = tel(a, P);
    out.push_back({"Es/N0  dB", "%.0f", t.snrDb, 0, 15, t.snrDb >= kNeedDb + 3 ? 1 : t.snrDb >= kNeedDb ? 2 : 3});
    out.push_back({"ROWS", "%.0f", (double)t.lines, 0, 4000, t.state >= 3 ? 1 : 0});
}

// Simple view: the picture with its controls, and the signal and the instruments beside it
template <int P> void command(App& a) {
    modepanel::panes({{"##fy3cpic", nullptr, [&a] { tab<P>(a); }, 2.2f, 300.f},
                      {"##fy3csig", tr("Signal"), [&a] { receiver<P>(a); ImGui::Spacing(); list<P>(a); }, 1.f, 240.f}}, 340.f);
}

} // namespace

extern const ModeUi kFy3AhrptUi;
extern const ModeUi kFy3MptUi;
const ModeUi kFy3AhrptUi = {
    .sideTitle = "INSTRUMENTS",
    .tabName = "Picture",
    .tabIcon = Ic::Camera,
    .tab = tab<0>,
    .receiver = receiver<0>,
    .list = list<0>,
    .panels = panels<0>,
    .status = status<0>,
    .summary = summary<0>,
    .tuner = tuner<0>,
    .synth = synth<0>,
    .tick = tick<0>,
    .meters = meters<0>,
    .command = command<0>,
};
const ModeUi kFy3MptUi = {
    .sideTitle = "INSTRUMENTS",
    .tabName = "Picture",
    .tabIcon = Ic::Camera,
    .tab = tab<1>,
    .receiver = receiver<1>,
    .list = list<1>,
    .panels = panels<1>,
    .status = status<1>,
    .summary = summary<1>,
    .tuner = tuner<1>,
    .synth = synth<1>,
    .tick = tick<1>,
    .meters = meters<1>,
    .command = command<1>,
};
