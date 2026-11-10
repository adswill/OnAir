// Screens of the SatDump-list weather links (families 260 - 268, one receiver class): the picture of the pass growing line by line, the instrument and channel,
// Save PNG, the one-click "follow the next pass" button, and the receiver state. docs/SATDUMP-TIER1-3.7.md.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace dect2;

namespace {

struct State {
    bool loaded = false;
    int imageSel = 0, view = 0;
    gfx::Image* tex = nullptr;
    int texW = 0, texRows = 0, texCap = 0;
    uint64_t imgId = 0;
    int shownView = -1, shownImg = -1;
    std::vector<const WxLine*> drawn;
    bool follow = true, autoSave = false;
    bool dirSet = false;
    uint64_t cycles = 0, picId = 0;
    int pictureSel = 0, pid = 0;
    double symKs = 0;
    std::string msg, passMsg;
    int lastLines = 0;
    double lastGrow = 0;
    bool autoSaved = false;
};
State gS[kWxNumProfiles];

const WxImage* activeImage(const WxsatTelemetry& t, int sel) {
    if (sel == 1 && t.image2) return t.image2.get();
    return t.image.get();
}

bool live(const App& a, int P) { return a.engine.running() && a.rx.standard == 261 + P; }
constexpr bool isFiles(int P) { return P == kWxHimawariCast || P == kWxGeoNetCast; }
constexpr bool isGeo(int P) { return P == kWxMsgRaw || isFiles(P); }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void loadState(int P) {
    State& s = gS[P];
    if (s.loaded) return;
    s.loaded = true;
    s.view = std::max(0, (int)plat::prefs().getI((std::string("wxView") + wxProfile(P).id).c_str(), 0));
    s.autoSave = plat::prefs().getB((std::string("wxAutoSave") + wxProfile(P).id).c_str(), true);
}

// The views: 0 is the colour composite where there is one, then each channel in grey
int numViews(const WxImage* im) { return im ? im->channels + 1 : 1; }
void channelName(int P, const WxImage* im, int v, char* out, size_t n) {
    if (!im) { snprintf(out, n, "-"); return; }
    if (P == kWxApt) { snprintf(out, n, "%s", v == 1 ? "Channel A" : v == 2 ? "Channel B" : "Full line"); return; }
    if (v == 0) { snprintf(out, n, "Colour (2, 1, 4)"); return; }
    snprintf(out, n, "%s channel %d", im->name.c_str(), v);
}

void composeLine(int P, const WxImage* im, const WxLine* l, int view, uint32_t* px) {
    const int w = im->width;
    if (P == kWxApt) {
        const int from = view == 1 ? 86 : view == 2 ? 1126 : 0, wv = view == 0 ? 2080 : 909;
        for (int i = 0; i < wv; i++) { const uint32_t g = l ? l->px[(size_t)from + (size_t)i] : 0; px[i] = 0xFF000000u | g << 16 | g << 8 | g; }
        return;
    }
    if (!l) { for (int i = 0; i < w; i++) px[i] = 0xFF000000u; return; }
    const uint8_t* p = l->px.data();
    for (int i = 0; i < w; i++) {
        uint32_t r, g, b;
        if (view == 0) {
            const int c0 = std::min(1, im->channels - 1), c1 = 0, c2 = std::min(3, im->channels - 1);
            r = p[(size_t)c0 * w + i]; g = p[(size_t)c1 * w + i]; b = (uint32_t)(255 - p[(size_t)c2 * w + i]);
        } else r = g = b = p[(size_t)(view - 1) * w + i];
        px[i] = 0xFF000000u | b << 16 | g << 8 | r;
    }
}

int picWidth(int P, const WxImage* im, int view) { return P == kWxApt ? (view == 0 ? 2080 : 909) : im->width; }

void update(int P, const WxsatTelemetry& t) {
    State& s = gS[P];
    const WxImage* im = activeImage(t, s.imageSel);
    if (!im || !gGfx) return;
    const int w = picWidth(P, im, s.view);
    const bool fresh = im->id != s.imgId || s.shownView != s.view || s.shownImg != s.imageSel || s.texW != w;
    if (fresh) { s.imgId = im->id; s.shownView = s.view; s.shownImg = s.imageSel; s.drawn.clear(); }
    if (im->lines > s.texCap || !s.tex || s.texW != w) {
        const int cap = std::max(128, (im->lines + 127) / 128 * 128);
        delete s.tex;
        s.tex = gGfx->createImage(w, cap, 0xFF000000u);
        s.texCap = cap; s.texW = w;
        s.drawn.clear();
    }
    if (!s.tex) return;
    s.drawn.resize((size_t)im->lines, nullptr);
    std::vector<uint32_t> px((size_t)w);
    for (int r = 0; r < im->lines; r++) {
        const WxLine* cur = (size_t)r < im->line.size() ? im->line[(size_t)r].get() : nullptr;
        if (cur == s.drawn[(size_t)r]) continue;
        composeLine(P, im, cur, s.view, px.data());
        s.tex->update(0, r, w, 1, px.data());
        s.drawn[(size_t)r] = cur;
    }
    s.texRows = im->lines;
}

std::string saveImage(int P, const WxImage* im, int view) {
    if (!im || im->lines <= 0) return "nothing to save yet";
    const int w = picWidth(P, im, view);
    std::vector<uint32_t> px((size_t)im->lines * (size_t)w);
    for (int r = 0; r < im->lines; r++) composeLine(P, im, (size_t)r < im->line.size() ? im->line[(size_t)r].get() : nullptr, view, &px[(size_t)r * (size_t)w]);
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
    if (view == 0) snprintf(name, sizeof name, "%s-%s-%s.png", ts, wxProfile(P).id, P == kWxApt ? "full" : "colour");
    else snprintf(name, sizeof name, "%s-%s-ch%d.png", ts, wxProfile(P).id, view);
    const std::string dir = plat::dataDir() + "/wxsat/" + wxProfile(P).id;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + name;
    return gfx::writePng(path.c_str(), reinterpret_cast<const uint8_t*>(px.data()), w, im->lines) ? "saved " + path : "could not write " + path;
}

// What runs whatever panel is showing: where files go, and the finished MSG discs saved without being asked
void housekeeping(App& a, int P) {
    State& s = gS[P];
    loadState(P);
    if (isFiles(P) && !s.dirSet) { s.dirSet = true; a.engine.wxsat(P).setSaveDir(plat::dataDir() + "/wxsat/" + wxProfile(P).id); }
    if (P == kWxMsgRaw && live(a, P) && a.rx.wxsat.cyclesDone != s.cycles) {
        s.cycles = a.rx.wxsat.cyclesDone;
        if (s.autoSave && a.rx.wxsat.imageDone) {
            for (int v = 0; v < numViews(a.rx.wxsat.imageDone.get()); v++) s.msg = saveImage(P, a.rx.wxsat.imageDone.get(), v);
            if (a.rx.wxsat.image2Done) s.msg = saveImage(P, a.rx.wxsat.image2Done.get(), 1);
        }
    }
}

// HimawariCast, GeoNetCast: the pictures that have come together, and the files as they arrive
void filesTab(App& a, int P) {
    State& s = gS[P];
    const FilecastTelemetry& f = a.rx.wxsat.files;
    const bool on = live(a, P);
    if (!on && f.recent.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", P == kWxHimawariCast ? "Point a C-band dish (about 1.8 m or more) at JCSAT-2B with a 5150 MHz LNB and start: the receiver finds the carrier by itself. Full discs appear here, a channel at a time, every 10 to 30 minutes."
                                                       : "GeoNetCast needs a C-band dish and 52 Msps or more, so most radios can only play a recording. Use File > Open recording, or Test signal.");
        ImGui::PopTextWrapPos();
        return;
    }
    if (!f.pictures.empty()) {
        s.pictureSel = std::clamp(s.pictureSel, 0, (int)f.pictures.size() - 1);
        ImGui::SetNextItemWidth(std::min(240 * gUi, ImGui::GetContentRegionAvail().x));
        char cur[96]; const FilecastPicture& c = f.pictures[(size_t)s.pictureSel];
        snprintf(cur, sizeof cur, "%s  %s  (%d/%d)", c.channel.c_str(), c.stamp.c_str(), c.have, c.total);
        if (ImGui::BeginCombo("##fcpic", cur)) {
            for (int i = 0; i < (int)f.pictures.size(); i++) {
                char n[96]; snprintf(n, sizeof n, "%s  %s  (%d/%d)", f.pictures[(size_t)i].channel.c_str(), f.pictures[(size_t)i].stamp.c_str(), f.pictures[(size_t)i].have, f.pictures[(size_t)i].total);
                if (ImGui::Selectable(n, i == s.pictureSel)) s.pictureSel = i;
            }
            ImGui::EndCombo();
        }
        const FilecastPicture& p = f.pictures[(size_t)s.pictureSel];
        if (gGfx && p.px && (p.id != s.picId || !s.tex || s.texW != p.w || s.texCap != p.h)) {
            s.picId = p.id;
            delete s.tex;
            s.tex = gGfx->createImage(p.w, p.h, 0xFF000000u);
            s.texW = p.w; s.texCap = p.h;
            std::vector<uint32_t> row((size_t)p.w);
            if (s.tex) for (int y = 0; y < p.h; y++) {
                for (int x = 0; x < p.w; x++) { const uint32_t g = (*p.px)[(size_t)y * (size_t)p.w + (size_t)x]; row[(size_t)x] = 0xFF000000u | g << 16 | g << 8 | g; }
                s.tex->update(0, y, p.w, 1, row.data());
            }
        }
        const float H = ImGui::GetContentRegionAvail().y;
        ImGui::BeginChild("##fcimg", ImVec2(0, H * 0.72f), ImGuiChildFlags_None, ImGuiWindowFlags_None);
        if (s.tex && p.w > 0) {
            const ImVec2 av = ImGui::GetContentRegionAvail();
            const float z = std::max(0.05f, std::min(av.x / p.w, av.y / p.h));
            ImGui::Image(s.tex->texture(), ImVec2(p.w * z, p.h * z));
        }
        ImGui::EndChild();
        if (!p.path.empty()) ImGui::TextDisabled("saved: %s", p.path.c_str());
    }
    ImGui::TextDisabled("%llu files complete, %llu incomplete, %llu failed. Saved in <data folder>/wxsat/%s/", (unsigned long long)f.filesComplete, (unsigned long long)f.filesPartial, (unsigned long long)f.filesFailed, wxProfile(P).id);
    ImGui::BeginChild("##fclist");
    ImGui::PushFont(a.mono, 0);
    for (const auto& r : f.recent) {
        if (r.complete) ImGui::Text("%-60s %10llu", r.name.c_str(), (unsigned long long)r.size);
        else ImGui::TextDisabled("%-60s %10llu  %d/%d parts", r.name.c_str(), (unsigned long long)r.size, r.have, r.parts);
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

template<int P>
void tab(App& a) {
    housekeeping(a, P);
    if (isFiles(P)) { filesTab(a, P); return; }
    State& s = gS[P];
    const WxsatTelemetry& t = a.rx.wxsat;
    const bool on = live(a, P);
    if (on) update(P, t);
    const WxImage* im = on ? activeImage(t, s.imageSel) : nullptr;
    if (on && t.image && t.image2) {
        ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
        const char* names[2] = {t.image->name.c_str(), t.image2->name.c_str()};
        if (ImGui::Combo("##wxim", &s.imageSel, names, 2)) s.drawn.clear();
        flowNext(8 * gUi);
    }
    if (im) {
        ImGui::SetNextItemWidth(std::min(190 * gUi, ImGui::GetContentRegionAvail().x));
        int v = std::clamp(s.view, 0, numViews(im) - 1);
        char cur[64]; channelName(P, im, v, cur, sizeof cur);
        if (ImGui::BeginCombo("##wxview", cur)) {
            for (int i = 0; i < numViews(im); i++) {
                char n[64]; channelName(P, im, i, n, sizeof n);
                if (P == kWxApt && i >= 3) break;
                if (ImGui::Selectable(n, i == v)) { v = i; plat::prefs().setI((std::string("wxView") + wxProfile(P).id).c_str(), v); plat::prefs().flush(); }
            }
            ImGui::EndCombo();
        }
        s.view = v;
        flowNext(8 * gUi);
    }
    if (ImGui::Button("Save PNG")) s.msg = saveImage(P, im, s.view);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The picture as shown, full size, in <data folder>/wxsat/%s/", wxProfile(P).id);
    flowNext(5 * gUi);
    if (runningButton(a, tr("New picture"))) { a.engine.wxsat(P).clearImage(); s.msg.clear(); s.autoSaved = false; s.lastLines = 0; }
    flowNext(5 * gUi);
    if (!isGeo(P) && ImGui::Button(tr("Follow the next pass"))) s.passMsg = passesArmFamily(a, wxProfile(P).family);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Opens nothing else: tracks the next pass of this satellite, starts the receiver by itself at the rise, follows the Doppler and stops at the set.");
    flowNext(8 * gUi);
    ImGui::Checkbox(tr("Follow"), &s.follow);
    flowNext(8 * gUi);
    if (ImGui::Checkbox(tr("Save at the end of the pass"), &s.autoSave)) { plat::prefs().setB((std::string("wxAutoSave") + wxProfile(P).id).c_str(), s.autoSave); plat::prefs().flush(); }
    flowEnd();
    if (!s.msg.empty() || !s.passMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s %s", s.msg.c_str(), s.passMsg.c_str()); ImGui::PopTextWrapPos(); }
    // automatic save, 90 s after the last new line
    const double now = ImGui::GetTime();
    const WxImage* base = t.image.get();
    if (on && base) {
        if (base->lines != s.lastLines) { s.lastLines = base->lines; s.lastGrow = now; s.autoSaved = false; }
        if (s.autoSave && !s.autoSaved && base->lines > 8 && now - s.lastGrow > 90) { s.autoSaved = true; s.msg = saveImage(P, base, s.view); }
    }
    if (P == kWxArktikaGgak) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        const GgakTelemetry& g = t.ggak;
        ImGui::TextDisabled("%s", on ? wxsatSummary(t).c_str() : "Start the receiver while Arktika-M N1 or N2 is high in the sky (a dish or a good patch at 1703 MHz).");
        if (on && g.magOk) ImGui::Text("Magnetometer: Bx %.1f  By %.1f  Bz %.1f nT, |B| %.1f nT", g.bx, g.by, g.bz, g.bmag);
        ImGui::PopTextWrapPos();
        return;
    }
    if (!im || im->lines == 0 || !s.tex) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        const char* idle = P == kWxApt || P == kWxNoaaHrpt || P == kWxNoaaGac || P == kWxNoaaDsb
            ? "NOAA-15, -18 and -19 stopped transmitting in 2025: this mode opens recordings. Use File > Open recording with the right sample rate, or Test signal."
            : P == kWxMsgRaw ? "Point a dish (about 1.2 m or more) with an LNA at 1686.83 MHz at Meteosat-9, -10 or -11 and start: a disc takes 15 minutes and saves itself."
            : P == kWxElektroRdas || P == kWxArktikaRdas ? "Needs an X-band dish, a down-converter and 40 Msps or more (RDAS is 30.72 Msym/s); recordings work too."
            : "Press Follow the next pass, or start the receiver during a pass. Needs a dish on 1.7 GHz (about 1 m, following the satellite), an LNA at the feed and a fast radio.";
        ImGui::TextDisabled("%s", on ? wxsatSummary(t).c_str() : idle);
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("##wxpic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const float W = std::max(50.f, ImGui::GetContentRegionAvail().x), z = W / s.texW;
    const float h = im->lines * z;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(s.tex->texture(), p0, ImVec2(p0.x + W, p0.y + h), ImVec2(0, 0), ImVec2(1, (float)im->lines / (float)s.texCap));
    ImGui::Dummy(ImVec2(W, h));
    if (s.follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
    ImGui::EndChild();
}

template<int P>
void list(App& a) {
    if (!live(a, P)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const WxsatTelemetry& t = a.rx.wxsat;
    kvRow(a, "frames", "%llu good, %llu failed", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.lines) kvRow(a, "lines", "%llu", (unsigned long long)t.lines);
    if (t.packets) kvRow(a, "packets", "%llu", (unsigned long long)t.packets);
    if (t.tipFrames) kvRow(a, "TIP frames", "%llu", (unsigned long long)t.tipFrames);
    if (t.rsCorrected) kvRow(a, "RS corrected", "%llu bytes", (unsigned long long)t.rsCorrected);
    for (int v = 0; v < 64; v++) if (t.vcidFrames[v]) kvRow(a, "virtual channel", "%d: %llu", v, (unsigned long long)t.vcidFrames[v]);
    if (P == kWxArktikaGgak) for (const auto& l : t.ggak.log) { ImGui::TextDisabled("%s", l.c_str()); break; }
}

template<int P>
void receiver(App& a) {
    if (!live(a, P)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const WxsatTelemetry& t = a.rx.wxsat;
    kvRow(a, "state", "%s", wxsatSummary(t).c_str());
    kvRow(a, "carrier", "offset %+.2f kHz, lock %.2f, symbol SNR %.1f dB", t.cfoHz / 1e3, t.carrierLock, t.snrDb);
    if (!t.satName.empty()) kvRow(a, "satellite", "%s", t.satName.c_str());
    else if (t.scid >= 0) kvRow(a, "spacecraft id", "%d", t.scid);
    kvRow(a, "symbol rate", "%.1f ksym/s", t.symbolRate / 1e3);
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
    if (!t.hint.empty()) kvRow(a, "note", "%s", t.hint.c_str());
    if (wxProfile(P).retired) kvRow(a, "satellite", "retired 2025: recordings only");
    if (isFiles(P)) {
        const FilecastTelemetry& f = t.files;
        kvRow(a, "PID", "%d", f.pid);
        kvRow(a, "IP / Fazzt", "%llu datagrams, %llu packets (%llu bad)", (unsigned long long)f.datagrams, (unsigned long long)f.frames, (unsigned long long)f.badFrames);
        kvRow(a, "open files", "%llu", (unsigned long long)f.open);
        if (f.filesDropped) kvRow(a, "dropped", "%llu files (the disk or the machine could not keep up)", (unsigned long long)f.filesDropped);
        if (!f.lastError.empty()) kvRow(a, "last problem", "%s", f.lastError.c_str());
        ImGui::Spacing();
        if (ImGui::TreeNode(trw("Advanced"))) {
            State& s = gS[P];
            ImGui::SetNextItemWidth(130 * gUi);
            if (ImGui::InputDouble(tr("Symbol rate, ksym/s (0: the link's)"), &s.symKs, 0, 0, "%.3f")) a.engine.wxsat(P).setFilecastSymbolRate(s.symKs * 1e3);
            ImGui::SetNextItemWidth(130 * gUi);
            if (ImGui::InputInt(tr("PID (0: the link's)"), &s.pid)) a.engine.wxsat(P).setFilecastPid(s.pid);
            ImGui::TreePop();
        }
    }
    smart5WxPassCard(a);   // round-5 smart checks (app/smart5, docs/SMART5-UI-3.9.md)
}

template<int P>
void panels(App& a) {
    const WxsatTelemetry& t = a.rx.wxsat;
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    if (live(a, P)) for (const auto& p : t.constellation) pts.emplace_back(p.first, p.second);
    scatter("##wxc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

template<int P>
void status(App& a) {
    housekeeping(a, P);
    const bool run = a.engine.running(), on = live(a, P);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    const WxsatTelemetry& t = a.rx.wxsat;
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", !on ? 0 : t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : wxsatSummary(t).c_str()); ImGui::PopFont();
}

template<int P>
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = wxProfile(P).name;
    if (live(a, P)) l2 = wxsatSummary(a.rx.wxsat);
}

template<int P>
void meters(const App& a, std::vector<ModeMeter>& out) {
    const WxsatTelemetry& t = a.rx.wxsat;
    out.push_back({"SNR  dB", "%.0f", t.snrDb, 0, 25, t.snrDb >= 8 ? 1 : t.snrDb >= 5 ? 2 : 3});
    if (isFiles(P)) out.push_back({"FILES", "%.0f", (double)(t.files.filesComplete + t.files.filesPartial), 0, 500, t.state >= 3 ? 1 : 0});
    else out.push_back({"LINES", "%.0f", (double)t.lines, 0, 4000, t.state >= 3 ? 1 : 0});
}

template<int P>
void tuner(App& a, bool& retune) {
    ImGui::TextDisabled("Frequency"); sameLineIf(90 * gUi, 5 * gUi);
    struct F { const char* name; double mhz; };
    static const F noaaApt[] = {{"NOAA-15 137.620", 137.620}, {"NOAA-18 137.9125", 137.9125}, {"NOAA-19 137.100", 137.100}};
    static const F noaaHrpt[] = {{"NOAA-18 1707.0", 1707.0}, {"NOAA-19 1698.0", 1698.0}, {"NOAA-15 1702.5", 1702.5}, {"POES S-band 2247.5", 2247.5}};
    static const F noaaDsb[] = {{"NOAA-15 / 18 137.35", 137.35}, {"NOAA-19 137.77", 137.77}};
    static const F himawari[] = {{"After a 5150 MHz LNB: 1002.0", 1002.0}, {"Direct: 4148.0", 4148.0}};
    static const F gnc[] = {{"After a 5150 MHz LNB: 1070.0", 1070.0}, {"Direct: 4080.0", 4080.0}};
    const F* f = nullptr; int n = 0;
    if (P == kWxApt) { f = noaaApt; n = 3; } else if (P == kWxNoaaHrpt || P == kWxNoaaGac) { f = noaaHrpt; n = 4; } else if (P == kWxNoaaDsb) { f = noaaDsb; n = 2; }
    else if (P == kWxHimawariCast) { f = himawari; n = 2; } else if (P == kWxGeoNetCast) { f = gnc; n = 2; }
    char cur[32]; snprintf(cur, sizeof cur, "%.3f MHz", a.freqMhz);
    if (f) {
        ImGui::SetNextItemWidth(std::min(250 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##wxf", cur)) {
            for (int i = 0; i < n; i++) if (ImGui::Selectable(f[i].name, std::fabs(a.freqMhz - f[i].mhz) < 0.0005)) { tuneFreq(a, f[i].mhz); retune = true; }
            ImGui::EndCombo();
        }
    } else ImGui::TextDisabled("%s", cur);
    flowEnd();
}

template<int P>
void command(App& a) {
    modepanel::panes({{"##wxp", nullptr, [&a] { tab<P>(a); }, 2.2f, 300.f},
                      {"##wxs", tr("Signal"), [&a] { receiver<P>(a); ImGui::Spacing(); list<P>(a); }, 1.f, 240.f}}, 340.f);
}

template<int P> constexpr ModeUi makeUi() {
    return ModeUi{.sideTitle = "FRAMES", .tabName = "Picture", .tabIcon = Ic::Camera, .tab = tab<P>, .receiver = receiver<P>, .list = list<P>, .panels = panels<P>,
                  .status = status<P>, .summary = summary<P>, .tuner = tuner<P>, .meters = meters<P>, .command = command<P>};
}

} // namespace

extern const ModeUi kWxsatUi[kWxNumProfiles];
const ModeUi kWxsatUi[kWxNumProfiles] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>(), makeUi<10>(), makeUi<11>()};
