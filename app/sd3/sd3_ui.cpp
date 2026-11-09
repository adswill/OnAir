// Screens of the SatDump tier 3 links (families 290 - 299 and 510, one receiver class): the satellite table, the picture (GRB and GVAR), the frame log, and the receiver state.
// docs/SATDUMP-TIER3-3.8.md.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "gfx/gfx.h"
#include "platform/platform.h"
#include "dect2/sd3/sd3_rx.h"
#include "dect2/sd3/sd3_tel.h"
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
    int imageSel = 0, view = 1, link = 0;
    gfx::Image* tex = nullptr;
    int texW = 0, texRows = 0, texCap = 0;
    uint64_t imgId = 0;
    int shownView = -1, shownImg = -1;
    std::vector<const WxLine*> drawn;
    bool follow = true, save = false, autoLink = true;
    std::string msg;
};
State gS[kSd3NumProfiles];

const WxImage* activeImage(const Sd3Telemetry& t, int sel) { return sel == 1 && t.image2 ? t.image2.get() : t.image.get(); }

bool live(const App& a, int P) { return a.engine.running() && a.rx.standard == kSd3Families[P] + 1; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

std::string abiName(const Sd3Telemetry& t) { for (const auto& p : t.products) if (p.key == t.selectedProduct) return p.name; return "?"; }

std::string saveFolder(int P) { return plat::dataDir() + "/sd3/" + sd3Profile(P).id; }

void loadState(App& a, int P) {
    State& s = gS[P];
    if (s.loaded) return;
    s.loaded = true;
    const std::string id = sd3Profile(P).id;
    s.link = std::clamp((int)plat::prefs().getI(("sd3Link" + id).c_str(), 0), 0, sd3LinkCount(P) - 1);
    s.autoLink = plat::prefs().getB(("sd3AutoLink" + id).c_str(), true);
    s.save = plat::prefs().getB(("sd3Save" + id).c_str(), false);
    a.engine.sd3(P).setLink(s.link);
    a.engine.sd3(P).setSaveDir(s.save ? saveFolder(P) : std::string());
}

void update(int P, const Sd3Telemetry& t) {
    State& s = gS[P];
    const WxImage* im = activeImage(t, s.imageSel);
    if (!im || !gGfx) return;
    const int w = im->width;
    const int view = std::clamp(s.view, 0, im->channels - 1);
    const bool fresh = im->id != s.imgId || s.shownView != view || s.shownImg != s.imageSel || s.texW != w;
    if (fresh) { s.imgId = im->id; s.shownView = view; s.shownImg = s.imageSel; s.drawn.clear(); }
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
        for (int i = 0; i < w; i++) { const uint32_t g = cur ? cur->px[(size_t)view * (size_t)w + (size_t)i] : 0; px[(size_t)i] = 0xFF000000u | g << 16 | g << 8 | g; }
        s.tex->update(0, r, w, 1, px.data());
        s.drawn[(size_t)r] = cur;
    }
    s.texRows = im->lines;
}

std::string saveImage(int P, const WxImage* im, int view) {
    if (!im || im->lines <= 0) return "nothing to save yet";
    const int w = im->width;
    view = std::clamp(view, 0, im->channels - 1);
    std::vector<uint32_t> px((size_t)im->lines * (size_t)w);
    for (int r = 0; r < im->lines; r++) {
        const WxLine* l = (size_t)r < im->line.size() ? im->line[(size_t)r].get() : nullptr;
        for (int i = 0; i < w; i++) { const uint32_t g = l ? l->px[(size_t)view * (size_t)w + (size_t)i] : 0; px[(size_t)r * (size_t)w + (size_t)i] = 0xFF000000u | g << 16 | g << 8 | g; }
    }
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
    snprintf(name, sizeof name, "%s-%s-ch%d.png", ts, sd3Profile(P).id, view + 1);
    const std::string dir = saveFolder(P);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    const std::string path = dir + "/" + name;
    return gfx::writePng(path.c_str(), reinterpret_cast<const uint8_t*>(px.data()), w, im->lines) ? "saved " + path : "could not write " + path;
}

void linkPicker(App& a, int P) {
    State& s = gS[P];
    const int n = sd3LinkCount(P);
    if (s.autoLink && n > 1) {
        const int k = sd3LinkForFrequency(P, a.freqMhz);
        if (k >= 0 && k != s.link) { s.link = k; a.engine.sd3(P).setLink(k); plat::prefs().setI((std::string("sd3Link") + sd3Profile(P).id).c_str(), k); plat::prefs().flush(); }
    }
    if (n > 1) {
        const Sd3Link& cur = sd3LinkAt(P, s.link);
        ImGui::SetNextItemWidth(std::min(320 * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##sd3link", cur.name)) {
            for (int i = 0; i < n; i++) {
                const Sd3Link& l = sd3LinkAt(P, i);
                char label[128];
                if (l.mhz > 0) snprintf(label, sizeof label, "%s  (%.3f MHz)", l.name, l.mhz); else snprintf(label, sizeof label, "%s  (no frequency listed)", l.name);
                if (ImGui::Selectable(label, i == s.link)) {
                    s.link = i;
                    a.engine.sd3(P).setLink(i);
                    plat::prefs().setI((std::string("sd3Link") + sd3Profile(P).id).c_str(), i); plat::prefs().flush();
                    if (l.mhz > 0) tuneFreq(a, l.mhz);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The satellite or link. Picking one tunes to its frequency and sets its modulation and frame format."));
        flowNext(8 * gUi);
        if (ImGui::Checkbox(tr("Follow the frequency"), &s.autoLink)) { plat::prefs().setB((std::string("sd3AutoLink") + sd3Profile(P).id).c_str(), s.autoLink); plat::prefs().flush(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Choose the link from the tuned frequency (within 1 MHz) when you tune the radio."));
        flowNext(8 * gUi);
    }
}

template<int P>
void tab(App& a) {
    loadState(a, P);
    State& s = gS[P];
    const Sd3Telemetry& t = a.rx.sd3;
    const bool on = live(a, P);
    linkPicker(a, P);
    if (ImGui::Checkbox(tr("Save files"), &s.save)) {
        a.engine.sd3(P).setSaveDir(s.save ? saveFolder(P) : std::string());
        plat::prefs().setB((std::string("sd3Save") + sd3Profile(P).id).c_str(), s.save); plat::prefs().flush();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Frames, xRIT files and GRB metadata in <data folder>/sd3/%s/"), sd3Profile(P).id);
    flowNext(8 * gUi);
    const bool pictures = P == kSd3Grb || P == kSd3Gvar;
    if (pictures) {
        if (on) update(P, t);
        const WxImage* im = on ? activeImage(t, s.imageSel) : nullptr;
        if (P == kSd3Grb && on && !t.products.empty()) {
            ImGui::SetNextItemWidth(std::min(260 * gUi, ImGui::GetContentRegionAvail().x));
            std::string cur = t.selectedProduct >= 0 ? abiName(t) : std::string(tr("Follow the newest"));
            if (ImGui::BeginCombo("##sd3prod", cur.c_str())) {
                if (ImGui::Selectable(tr("Follow the newest"), false)) a.engine.sd3(P).setProduct(-1);
                for (const auto& p : t.products) if (ImGui::Selectable(p.name.c_str(), p.key == t.selectedProduct)) a.engine.sd3(P).setProduct(p.key);
                ImGui::EndCombo();
            }
            flowNext(8 * gUi);
        }
        if (P == kSd3Gvar && on && t.image && t.image2) {
            ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
            const char* names[2] = {t.image->name.c_str(), t.image2->name.c_str()};
            if (ImGui::Combo("##sd3im", &s.imageSel, names, 2)) s.drawn.clear();
            flowNext(8 * gUi);
        }
        if (im && im->channels > 1) {
            ImGui::SetNextItemWidth(std::min(150 * gUi, ImGui::GetContentRegionAvail().x));
            char cur[32]; snprintf(cur, sizeof cur, "%s %d", tr("Channel"), s.view + 1);
            if (ImGui::BeginCombo("##sd3ch", cur)) {
                for (int i = 0; i < im->channels; i++) { char n[32]; snprintf(n, sizeof n, "%s %d", tr("Channel"), i + 1); if (ImGui::Selectable(n, i == s.view)) s.view = i; }
                ImGui::EndCombo();
            }
            flowNext(8 * gUi);
        }
        if (ImGui::Button(tr("Save PNG"))) s.msg = saveImage(P, im, s.view);
        flowNext(5 * gUi);
        if (runningButton(a, tr("New picture"))) { a.engine.sd3(P).clearImage(); s.msg.clear(); }
    }
    flowEnd();
    if (!s.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (pictures) {
        const WxImage* im = on ? activeImage(t, s.imageSel) : nullptr;
        if (im && im->lines > 0 && s.tex) {
            ImGui::BeginChild("##sd3pic", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
            const float W = std::max(50.f, ImGui::GetContentRegionAvail().x), z = W / s.texW;
            const float h = im->lines * z;
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage(s.tex->texture(), p0, ImVec2(p0.x + W, p0.y + h), ImVec2(0, 0), ImVec2(1, (float)im->lines / (float)s.texCap));
            ImGui::Dummy(ImVec2(W, h));
            if (s.follow) ImGui::SetScrollY(ImGui::GetScrollMaxY());
            ImGui::EndChild();
            return;
        }
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    if (!on) {
        ImGui::TextDisabled("%s", P == kSd3Grb ? tr("GOES-R GRB on 1686.6 MHz: DVB-S2, 8.67 Msym/s. Needs a dish at L-band, an LNA and 12 Msps or more.")
                                 : P == kSd3Uhrit ? tr("GK-2A UHRIT at 8070 MHz: DVB-S2, 15.6 Msym/s. Needs an X-band dish with a down-converter and about 28 Msps or more; recordings work.")
                                 : P == kSd3Gvar ? tr("GOES GVAR on 1685.7 MHz: BPSK 2.11 Msym/s. The GOES-N satellites have been retired, so this mode opens recordings.")
                                 : P >= kSd3Geoscan ? tr("A 70 cm Yagi or a good vertical is enough during a pass; pick the satellite, then start the receiver. Doppler is handled by the demodulator.")
                                 : tr("Pick the satellite. Needs a dish that follows it at S-band or L-band, except the geostationary links."));
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::TextDisabled("%s", sd3Summary(t).c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##sd3frames", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, 0))) {
        ImGui::TableSetupColumn(tr("Time"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("SC"), ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
        ImGui::TableSetupColumn(tr("VC"), ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
        ImGui::TableSetupColumn(tr("Bytes"), ImGuiTableColumnFlags_WidthFixed, 56 * gUi);
        ImGui::TableSetupColumn(tr("First bytes"));
        ImGui::TableHeadersRow();
        ImGui::PushFont(a.mono, 0);
        for (auto it = t.frames.rbegin(); it != t.frames.rend(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%.1f", it->timeSec);
            ImGui::TableNextColumn(); if (it->scid >= 0) ImGui::Text("%d", it->scid);
            ImGui::TableNextColumn(); if (it->vcid >= 0) ImGui::Text("%d", it->vcid);
            ImGui::TableNextColumn(); ImGui::Text("%d", it->bytes);
            ImGui::TableNextColumn();
            if (it->rsOk) ImGui::TextUnformatted(it->hex.c_str()); else ImGui::TextDisabled("%s", it->hex.c_str());
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
}

template<int P>
void list(App& a) {
    if (!live(a, P)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const Sd3Telemetry& t = a.rx.sd3;
    kvRow(a, "frames", "%llu good, %llu failed", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (t.rsCorrected) kvRow(a, "RS corrected", "%llu bytes", (unsigned long long)t.rsCorrected);
    if (t.packets) kvRow(a, "packets", "%llu, %llu CRC failed", (unsigned long long)t.packets, (unsigned long long)t.crcBad);
    if (t.files) kvRow(a, "files", "%llu", (unsigned long long)t.files);
    if (!t.lastFile.empty()) kvRow(a, "last file", "%s", t.lastFile.c_str());
    if (t.lines) kvRow(a, "lines", "%llu", (unsigned long long)t.lines);
    if (!t.lastText.empty()) kvRow(a, "last", "%s", t.lastText.c_str());
    if (!t.hint.empty()) kvRow(a, "note", "%s", t.hint.c_str());
    if (t.scid >= 0) kvRow(a, "spacecraft", "%d", t.scid);
    for (int v = 0; v < 64; v++) if (t.vcidFrames[v]) kvRow(a, "virtual channel", "%d: %llu", v, (unsigned long long)t.vcidFrames[v]);
    if (!t.apids.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("APID");
        for (size_t i = 0; i < std::min<size_t>(t.apids.size(), 12); i++) kvRow(a, "", "0x%03X: %llu", t.apids[i].first, (unsigned long long)t.apids[i].second);
    }
}

template<int P>
void receiver(App& a) {
    loadState(a, P);
    const Sd3Telemetry& t = a.rx.sd3;
    kvRow(a, "state", "%s", live(a, P) ? sd3Summary(t).c_str() : (a.engine.running() ? "starting" : "stopped"));
    if (live(a, P)) { kvRow(a, "SNR", "%.1f dB", t.snrDb); kvRow(a, "offset", "%.0f Hz", t.cfoHz); kvRow(a, "symbol rate", "%.0f", t.symbolRate); }
}

template<int P> void panels(App& a) { receiver<P>(a); }
template<int P> void status(App& a) { const bool on = live(a, P); ImGui::TextUnformatted(!on ? "stopped" : sd3Summary(a.rx.sd3).c_str()); }
template<int P> std::string summary(App& a) { return live(a, P) ? sd3Summary(a.rx.sd3) : std::string(); }
template<int P> void tuner(App&) {}
template<int P> void meters(App&) {}

template<int P>
void command(App& a) {
    modepanel::panes({{"##sd3p", nullptr, [&a] { tab<P>(a); }, 2.2f, 300.f},
                      {"##sd3s", tr("Signal"), [&a] { receiver<P>(a); ImGui::Spacing(); list<P>(a); }, 1.f, 240.f}}, 340.f);
}

template<int P> constexpr ModeUi makeUi() {
    return ModeUi{.sideTitle = "FRAMES", .tabName = "Frames", .tabIcon = Ic::Camera, .tab = tab<P>, .receiver = receiver<P>, .list = list<P>, .panels = panels<P>,
                  .status = status<P>, .summary = summary<P>, .tuner = tuner<P>, .meters = meters<P>, .command = command<P>};
}

} // namespace

extern const ModeUi kSd3Ui[kSd3NumProfiles];
const ModeUi kSd3Ui[kSd3NumProfiles] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>(), makeUi<10>()};
