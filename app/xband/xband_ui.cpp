// Screens of the X-band links (families 530 - 549, one receiver class): the channels and products of the pass, Save, and the main path: play a recording through the receiver.
// docs/SAT-XBAND-3.8.md.
#include "ui/kit/mode_panel.h"
#include "app.h"
#include "gfx/gfx.h"
#include "panels/gallery.h"
#include "platform/platform.h"
#include "dect2/xband/xband_rx.h"
#include "dect2/xband/xband_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace dect2;

namespace {

struct State {
    int view = 0;                     // 0: first product, then channels
    gfx::Image* tex = nullptr;
    int texW = 0, texH = 0;
    uint64_t shownId = 0;
    std::string msg;
    XbFileJob job;
    double rateMsps = 0;
    char path[1024] = {};
    std::unique_ptr<XbandReceiver> fileRx;
    std::unique_ptr<XbFileRunner> runner;
    XbTelemetry fileTel;
    uint64_t fileSeq = 0;
    std::vector<XbProductImage> products;
    double lastProd = 0;
    int view2 = 0;
};
State gS[kXbNumProfiles];

bool live(const App& a, int P) { return a.engine.running() && a.rx.standard == 531 + P; }

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

// The telemetry shown: the recording being played, or the live receiver
const XbTelemetry& shown(App& a, int P, State& s) {
    if (s.runner && (s.runner->running() || s.fileRx)) return s.fileTel;
    return a.rx.xband;
}

void showImage(State& s, const uint8_t* rgba8888, int w, int h, uint64_t id) {
    if (!gGfx || w <= 0 || h <= 0) return;
    if (s.shownId == id && s.tex && s.texW == w && s.texH == h) return;
    if (!s.tex || s.texW != w || s.texH != h) { delete s.tex; s.tex = gGfx->createImage(w, h, 0xFF000000u); s.texW = w; s.texH = h; }
    if (s.tex) s.tex->update(0, 0, w, h, reinterpret_cast<const uint32_t*>(rgba8888));
    s.shownId = id;
}

std::string saveFolder(int P) {
    const std::string dir = plat::dataDir() + "/xband/" + xbLink(P).id;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(dir.c_str())), ec);
    return dir;
}

std::string saveAll(App& a, int P, State& s, XbandReceiver& rx) {
    char ts[32];
    const time_t tt = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
    const std::string dir = saveFolder(P);
    int n = 0;
    const auto save = [&](const XbProductImage& p, const std::string& kind) {
        std::string nm = p.name;
        for (auto& c : nm) if (c == ' ' || c == '/' || c == '(' || c == ')' || c == ',') c = '_';
        const std::string file = dir + "/" + ts + "-" + kind + "-" + nm + ".png";
        if (gfx::writePng(file.c_str(), reinterpret_cast<const uint8_t*>(p.rgba.data()), p.width, p.height)) n++;
    };
    for (const auto& p : rx.products(0)) { save(p, "product"); gallery::publish(std::string(xbLink(P).id) + "-" + ts + "-" + p.name, xbLink(P).name, (double)tt, p.width, p.height, reinterpret_cast<const uint8_t*>(p.rgba.data())); }
    for (const auto& p : rx.channelImages()) save(p, "channel");
    (void)a; (void)s;
    return n ? "saved " + std::to_string(n) + " pictures to " + dir : "nothing to save yet";
}

void startFile(App& a, int P, State& s) {
    s.job.path = s.path;
    s.job.sampleRate = s.rateMsps * 1e6;
    s.fileRx = std::make_unique<XbandReceiver>(P);
    s.runner = std::make_unique<XbFileRunner>();
    s.fileSeq = 0; s.fileTel = XbTelemetry{}; s.fileTel.profile = P; s.shownId = 0;
    std::string err;
    if (!s.runner->start(*s.fileRx, s.job, &err)) { s.msg = err; s.runner.reset(); s.fileRx.reset(); }
    else s.msg = "decoding the recording";
    (void)a;
}

template<int P>
void tab(App& a) {
    State& s = gS[P];
    const XbLink& L = xbLink(P);
    if (s.runner && s.fileRx && s.fileRx->telemetry(s.fileTel, s.fileSeq)) s.fileSeq = s.fileTel.seq;
    XbTelemetry dummy;
    const XbTelemetry& t = shown(a, P, s);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s. %s. Instruments: %s.", tr("A recording is the usual way to use this link"), L.needs, L.instruments);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // the recording: the main path
    ImGui::SetNextItemWidth(std::min(330 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##xbpath", tr("recording: .cadu, .wav, .sigmf-meta, .cs8 .cu8 .cs16 .cf32"), s.path, sizeof s.path);
    flowNext(5 * gUi);
    if (ImGui::Button(tr("Choose..."))) { std::string f = plat::openFileDialog(); if (!f.empty()) snprintf(s.path, sizeof s.path, "%s", f.c_str()); }
    flowNext(5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));
    ImGui::InputDouble("##xbrate", &s.rateMsps, 0, 0, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sample rate of a raw recording in Msps (WAV and SigMF carry their own)"));
    flowNext(5 * gUi);
    bool fileBusy = s.runner && s.runner->running();
    if (!fileBusy) { if (ImGui::Button(tr("Decode recording"))) startFile(a, P, s); }
    else { if (ImGui::Button(tr("Stop"))) { s.runner->stop(); } }
    flowEnd();
    if (s.runner) { ImGui::ProgressBar(s.runner->progress(), ImVec2(-1, 0), s.runner->status().empty() ? "" : s.runner->status().c_str()); }
    // what is on screen
    ImGui::SetNextItemWidth(std::min(240 * gUi, ImGui::GetContentRegionAvail().x));
    int nCh = (int)t.channels.size();
    int nProd = (int)s.products.size();
    std::string cur = "-";
    if (s.view < nProd) cur = s.products[(size_t)s.view].name;
    else if (s.view - nProd < nCh && t.channels[(size_t)s.view - nProd]) cur = t.channels[(size_t)s.view - nProd]->name;
    if (ImGui::BeginCombo("##xbview", cur.c_str())) {
        for (int i = 0; i < nProd; i++) if (ImGui::Selectable(s.products[(size_t)i].name.c_str(), i == s.view)) { s.view = i; s.shownId = 0; }
        for (int i = 0; i < nCh; i++) if (t.channels[(size_t)i] && ImGui::Selectable(t.channels[(size_t)i]->name.c_str(), nProd + i == s.view)) { s.view = nProd + i; s.shownId = 0; }
        ImGui::EndCombo();
    }
    flowNext(6 * gUi);
    XbandReceiver& rx = s.fileRx ? *s.fileRx : a.engine.xband(P);
    if (ImGui::Button(tr("Update products"))) { s.products = rx.products(1600); if (s.view >= (int)s.products.size() + nCh) s.view = 0; s.shownId = 0; }
    flowNext(5 * gUi);
    if (ImGui::Button(tr("Save all"))) s.msg = saveAll(a, P, s, rx);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Products and every channel at full size, in the data folder, and the products in the gallery"));
    flowNext(5 * gUi);
    if (ImGui::Button(tr("New pass"))) { rx.clearImage(); s.products.clear(); s.view = 0; s.shownId = 0; s.msg.clear(); }
    flowEnd();
    if (!s.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", s.msg.c_str()); ImGui::PopTextWrapPos(); }
    // the picture
    const auto preview = [&]() -> void {
        if (s.view < nProd) {
            const auto& p = s.products[(size_t)s.view];
            showImage(s, reinterpret_cast<const uint8_t*>(p.rgba.data()), p.width, p.height, 1000000u + (uint64_t)s.view);
            if (!p.note.empty()) ImGui::TextDisabled("%s", p.note.c_str());
        } else {
            const int ci = s.view - nProd;
            if (ci < nCh && t.channels[(size_t)ci]) {
                const auto& c = t.channels[(size_t)ci];
                std::vector<uint32_t> rgba((size_t)c->width * c->height);
                for (size_t k = 0; k < rgba.size(); k++) { const uint32_t g = c->px[k]; rgba[k] = 0xFF000000u | g << 16 | g << 8 | g; }
                showImage(s, reinterpret_cast<const uint8_t*>(rgba.data()), c->width, c->height, c->id);
                ImGui::TextDisabled("%s, %.3f um, %d x %d", c->name.c_str(), c->um, c->fullWidth, c->fullHeight);
            } else {
                ImGui::TextDisabled("%s", tr("Decoded channels and products appear here."));
                return;
            }
        }
        if (s.tex) {
            const float aw = std::max(50.f, ImGui::GetContentRegionAvail().x);
            const float sh = aw * s.texH / std::max(1, s.texW);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddImage(s.tex->texture(), p0, ImVec2(p0.x + aw, p0.y + sh), ImVec2(0, 0), ImVec2(1, 1));
            ImGui::Dummy(ImVec2(aw, sh));
        }
    };
    preview();
    (void)dummy;
}

template<int P>
void receiver(App& a) {
    const XbLink& L = xbLink(P);
    State& s = gS[P];
    const XbTelemetry& t = shown(a, P, s);
    kvRow(a, "state", "%s", xbandSummary(t).c_str());
    kvRow(a, "link", "%.3f Msym/s, %s%s", L.symbolRate / 1e6, L.mod == kXbBpsk ? "BPSK" : L.mod == kXbQpsk ? "QPSK" : "OQPSK", L.fec == kXbFecConv ? ", K=7 r=1/2" : L.fec == kXbFecLdpc ? ", LDPC 7/8" : L.fec == kXbFecTurbo ? ", turbo" : "");
    kvRow(a, "carrier offset", "%.0f Hz", t.cfoHz);
    kvRow(a, "SNR", "%.1f dB", t.snrDb);
    kvRow(a, "frames", "%llu good, %llu bad, %llu sync words", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.asmHits);
    kvRow(a, "Reed-Solomon", "%llu symbols corrected", (unsigned long long)t.rsCorrected);
    if (!t.satName.empty()) kvRow(a, "satellite", "%s", t.satName.c_str());
    else if (t.scid >= 0) kvRow(a, "spacecraft id", "%d", t.scid);
    if (t.vcid >= 0) kvRow(a, "virtual channel", "%d", t.vcid);
    kvRow(a, "packets", "%llu", (unsigned long long)t.packets);
    kvRow(a, "lines", "%llu", (unsigned long long)t.lines);
    kvRow(a, "needs", "%s; %.0f Msps or more", L.needs, L.minRate / 1e6);
    if (t.rateTooLow) kvRow(a, "note", "the sample rate is below %.1f Msps", L.minRate / 1e6);
}

template<int P>
void list(App& a) {
    State& s = gS[P];
    const XbTelemetry& t = shown(a, P, s);
    ImGui::TextDisabled("%s", tr("Virtual channels"));
    for (int v = 0; v < 64; v++) if (t.vcidFrames[v]) ImGui::Text("VCID %d: %llu frames", v, (unsigned long long)t.vcidFrames[v]);
}

template<int P>
void panels(App& a) {
    State& s = gS[P];
    const XbTelemetry& t = shown(a, P, s);
    const float H = ImGui::GetContentRegionAvail().y;
    const float sz = std::max(50.f, H - ImGui::GetTextLineHeightWithSpacing() - 6);
    ImGui::BeginGroup();
    captionFit(sz, "Symbols");
    std::vector<cf32> pts;
    for (const auto& p : t.constellation) pts.emplace_back(p.first, p.second);
    scatter("##xbc", pts, ImVec2(sz, sz), 2.0, pal::okGreen());
    ImGui::EndGroup();
}

template<int P>
void status(App& a) {
    const bool run = a.engine.running();
    State& s = gS[P];
    const XbTelemetry& t = shown(a, P, s);
    StatusPanel panel;
    lamp("Carrier", t.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Frames", t.state >= 3 ? 1 : t.state == 2 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(run || s.runner ? xbandSummary(t).c_str() : "stopped"); ImGui::PopFont();
}

template<int P>
void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = xbLink(P).name;
    if (live(a, P)) l2 = xbandSummary(a.rx.xband);
}

template<int P>
void meters(const App& a, std::vector<ModeMeter>& out) {
    const XbTelemetry& t = a.rx.xband;
    out.push_back({"SNR  dB", "%.0f", t.snrDb, 0, 25, t.snrDb >= 8 ? 1 : t.snrDb >= 4 ? 2 : 3});
    out.push_back({"FRAMES", "%.0f", (double)t.blocksOk, 0, 100000, t.state >= 3 ? 1 : 0});
}

template<int P>
void tuner(App& a, bool&) {
    char cur[32]; snprintf(cur, sizeof cur, "%.3f MHz", a.freqMhz);
    ImGui::TextDisabled("Frequency"); sameLineIf(90 * gUi, 5 * gUi);
    ImGui::TextDisabled("%s", cur);
    flowEnd();
}

template<int P>
void command(App& a) {
    modepanel::panes({{"##xbp", nullptr, [&a] { tab<P>(a); }, 2.2f, 300.f},
                      {"##xbs", tr("Signal"), [&a] { receiver<P>(a); ImGui::Spacing(); list<P>(a); }, 1.f, 240.f}}, 340.f);
}

template<int P> constexpr ModeUi makeUi() {
    return ModeUi{.sideTitle = "FRAMES", .tabName = "Pictures", .tabIcon = Ic::Camera, .tab = tab<P>, .receiver = receiver<P>, .list = list<P>, .panels = panels<P>,
                  .status = status<P>, .summary = summary<P>, .tuner = tuner<P>, .meters = meters<P>, .command = command<P>};
}

} // namespace

extern const ModeUi kXbandUi[kXbNumProfiles];
const ModeUi kXbandUi[kXbNumProfiles] = {makeUi<0>(), makeUi<1>(), makeUi<2>(), makeUi<3>(), makeUi<4>(), makeUi<5>(), makeUi<6>(), makeUi<7>(), makeUi<8>(), makeUi<9>(), makeUi<10>(), makeUi<11>(), makeUi<12>(), makeUi<13>(), makeUi<14>(), makeUi<15>(), makeUi<16>(), makeUi<17>(), makeUi<18>(), makeUi<19>()};
