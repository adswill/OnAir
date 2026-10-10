// HF digital screens: the shell. One main tab with a view per decoder (RTTY, SSTV, FreeDV); each view is drawn by that decoder's own
// file (hfdig_rtty_ui.cpp, hfdig_sstv_ui.cpp, hfdig_freedv_ui.cpp). The shell also keeps the sound controls in step for FreeDV's speech,
// lists what happened (ACTIVITY, on the right) and draws the spectrum and waterfall of the 8 kHz audio at the bottom, to tune the tones by eye.
#include "app.h"
#include "gfx.h"
#include "dect2/fftutil.h"
#include "dect2/hfdig_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// the views of the decoders, each in its own file
void hfdigRttyTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigSstvTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigFreedvTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigFtxTab(App& a, const dect2::HfdigTelemetry& t);
void hfdigFtxPresetFt8(App& a);   // the FT8 shortcut of the mode list: decode FT8, show only FT8

namespace {

struct State {
    int view = 0;                        // 0 RTTY, 1 SSTV, 2 FreeDV, 3 FT8 / FT4 / FT2 / WSPR
    bool wasRunning = false;
    float pushedVol = -1;
    bool pushedMute = false;
    bool tapSet = false;
    // the audio spectrum (UI thread): 1024-point frames every 512 samples, four of them averaged into one display row
    std::vector<float> pend;             // audio not yet analysed
    std::vector<float> acc;              // power sum of the frames of the row being built (bins 0 .. 4000 Hz)
    int accN = 0;
    std::vector<float> spec;             // the last averaged row, dB
    static constexpr int kBins = 512, kRows = 200;
    std::vector<uint32_t> wfPx;          // kRows rows of kBins pixels, newest row first
    float lo = -80, hi = -20;            // colour range of the waterfall, dB (follows the noise floor slowly)
    gfx::Image* wf = nullptr;
    bool wfDirty = false;
    // what happened
    std::string rttyText, fdvText;
    double rttyT = 0, fdvT = 0;          // wall clock seconds of the last change
    int fdvMode = -1;
    struct Pic { uint64_t id; gfx::Image* tex; int w, h; };
    std::vector<Pic> pics;               // thumbnails of the newest finished pictures, newest first
};
State S;

// the audio from the receiver thread
std::mutex gAudMu;
std::vector<float> gAud;

double wallNow() { return (double)time(nullptr); }

void clock(int64_t t, char* out, size_t n) {
    const time_t tt = (time_t)t;
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &tt);
#else
    localtime_r(&tt, &lt);
#endif
    strftime(out, n, "%H:%M:%S", &lt);
}

// takes the new audio into spectrum frames
void analyse() {
    {
        std::lock_guard<std::mutex> lk(gAudMu);
        S.pend.insert(S.pend.end(), gAud.begin(), gAud.end());
        gAud.clear();
    }
    constexpr int N = 1024, hop = 512;
    static dect2::Fft fft(N);
    static std::vector<float> win;
    if (win.empty()) { win.resize(N); for (int i = 0; i < N; i++) win[i] = 0.5f - 0.5f * std::cos(2 * 3.14159265f * i / N); }
    if (S.pend.size() > 16 * N) S.pend.erase(S.pend.begin(), S.pend.end() - 16 * N);   // the window was hidden for a while: skip ahead
    if (S.acc.empty()) S.acc.assign(State::kBins, 0.f);
    size_t used = 0;
    std::vector<dect2::cf32> buf(N);
    while (S.pend.size() - used >= (size_t)N) {
        for (int i = 0; i < N; i++) buf[i] = dect2::cf32(S.pend[used + i] * win[i], 0);
        fft.forward(buf.data());
        for (int k = 0; k < State::kBins; k++) S.acc[k] += std::norm(buf[k]) * (16.f / ((float)N * N));   // a full-scale sine reads 0 dB
        used += hop;
        if (++S.accN < 4) continue;
        S.spec.resize(State::kBins);
        for (int k = 0; k < State::kBins; k++) { S.spec[k] = 10 * std::log10(S.acc[k] / 4 + 1e-12f); S.acc[k] = 0; }
        S.accN = 0;
        // the colours follow the middle of the spectrum (the noise) so that a signal stands out
        std::vector<float> srt = S.spec;
        std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
        const float med = srt[srt.size() / 2];
        S.lo += (med - 5 - S.lo) * 0.2f;
        S.hi += (med + 45 - S.hi) * 0.2f;
        if (S.wfPx.empty()) S.wfPx.assign((size_t)State::kBins * State::kRows, 0xFF000000u);
        std::copy_backward(S.wfPx.begin(), S.wfPx.end() - State::kBins, S.wfPx.end());   // everything one row down
        for (int k = 0; k < State::kBins; k++) {
            const float v = (S.spec[k] - S.lo) / std::max(1.f, S.hi - S.lo);
            S.wfPx[k] = Waterfall::jet(v);
        }
        S.wfDirty = true;
    }
    S.pend.erase(S.pend.begin(), S.pend.begin() + (long)std::min(used, S.pend.size()));
}

// keeps the activity entries and the thumbnails in step with the telemetry
void follow(const dect2::HfdigTelemetry& t) {
    if (t.rtty.text != S.rttyText) { S.rttyText = t.rtty.text; S.rttyT = wallNow(); }
    if (t.freedv.mode >= 0) S.fdvMode = t.freedv.mode;
    if (t.freedv.text != S.fdvText) { S.fdvText = t.freedv.text; S.fdvT = wallNow(); }
    for (size_t k = std::min<size_t>(t.sstv.history.size(), 3); k-- > 0;) {   // the newest three pictures, oldest first so that the newest ends in front
        const auto& h = t.sstv.history[k];
        if (!h || !gGfx || h->width <= 0 || h->height <= 0 || h->rgb.size() < (size_t)h->width * h->height * 3) continue;
        bool have = false;
        for (const auto& p : S.pics) if (p.id == h->id) have = true;
        if (have) continue;
        std::vector<uint32_t> px((size_t)h->width * h->height);
        for (size_t i = 0; i < px.size(); i++) px[i] = 0xFF000000u | ((uint32_t)h->rgb[i * 3 + 2] << 16) | ((uint32_t)h->rgb[i * 3 + 1] << 8) | h->rgb[i * 3];
        gfx::Image* tex = gGfx->createImage(h->width, h->height, 0xFF000000u);
        if (!tex) continue;
        tex->update(0, 0, h->width, h->height, px.data());
        S.pics.insert(S.pics.begin(), {h->id, tex, h->width, h->height});
        while (S.pics.size() > 3) { delete S.pics.back().tex; S.pics.pop_back(); }
    }
}

bool live(const App& a) { return a.engine.running() && a.rx.standard == 26; }   // the engine reports its standard code minus one

void tick(App& a) {
    if (a.modePreset == 1) {   // picked as "FT8 / FT4 / WSPR" in the mode list: open the FT8 / WSPR view
        a.modePreset = 0;
        S.view = 3;
        plat::prefs().setI("hfdigView", 3);
        hfdigFtxPresetFt8(a);
    }
    const bool run = a.engine.running();
    if (run && a.rx.standard == 26 && (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute)) {   // the sound controls are shared with the other modes
        a.engine.hfdig().setVolume(a.volume);
        a.engine.hfdig().setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    if (run && a.rx.standard == 26) {
        if (!S.tapSet) {   // the 8 kHz audio for the spectrum, handed over from the receiver thread
            S.tapSet = true;
            a.engine.hfdig().setAudioTap([](const float* x, size_t n) {
                std::lock_guard<std::mutex> lk(gAudMu);
                if (gAud.size() < 32768) gAud.insert(gAud.end(), x, x + n);
            });
        }
        analyse();
        follow(a.rx.hfdig);
    } else if (!run) S.tapSet = false;
    S.wasRunning = run;
}

void tab(App& a) {
    static bool viewLoaded = false;   // the view is remembered
    if (!viewLoaded) { viewLoaded = true; S.view = (int)std::max(0L, std::min(3L, plat::prefs().getI("hfdigView", 0))); }
    const int was = S.view;
    subNav("hfdv", S.view, {"RTTY", "SSTV", "FreeDV", "FT8 / WSPR"});
    if (S.view != was) plat::prefs().setI("hfdigView", S.view);
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    if (S.view == 0) hfdigRttyTab(a, t);
    else if (S.view == 1) hfdigSstvTab(a, t);
    else if (S.view == 2) hfdigFreedvTab(a, t);
    else hfdigFtxTab(a, t);
}

void list(App& a) {
    if (!live(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver");
        ImGui::PopTextWrapPos();
        return;
    }
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    struct Ev { double t; int kind; size_t pic; };
    std::vector<Ev> ev;
    if (!S.rttyText.empty()) ev.push_back({S.rttyT, 0, 0});
    for (size_t i = 0; i < t.sstv.history.size() && i < 3; i++) if (t.sstv.history[i]) ev.push_back({(double)t.sstv.history[i]->unixTime, 1, i});
    if (!S.fdvText.empty()) ev.push_back({S.fdvT, 2, 0});
    for (size_t i = t.ftx.decodes.size(), n = 0; i-- > 0 && n < 8; n++) ev.push_back({t.ftx.decodes[i].slotUtc, 3, i});   // the newest decodes
    if (ev.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("nothing yet: listening"); ImGui::PopTextWrapPos(); return; }
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& x, const Ev& y) { return x.t > y.t; });   // newest first
    ImGui::PushTextWrapPos(0);
    for (const Ev& e : ev) {
        ImGui::PushID((int)(e.kind * 8 + e.pic));
        if (e.kind == 0) {
            std::string x = S.rttyText;
            for (char& c : x) if (c == '\n' || c == '\r') c = ' ';
            if (x.size() > 60) x = x.substr(x.size() - 60);
            ImGui::TextColored(pal::heading(), "RTTY:"); ImGui::SameLine(0, 5 * gUi);
            ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(x.c_str()); ImGui::PopFont();
        } else if (e.kind == 1) {
            const auto& im = *t.sstv.history[e.pic];
            char tm[16];
            clock(im.unixTime, tm, sizeof tm);
            ImGui::TextColored(pal::heading(), "SSTV:"); ImGui::SameLine(0, 5 * gUi);
            ImGui::Text("%s picture finished %s", im.mode.c_str(), tm);
            for (const auto& p : S.pics) if (p.id == im.id) {
                const float th = 64 * gUi, z = std::min(th / (float)p.w, th / (float)p.h);
                const ImVec2 q = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(p.w * z, p.h * z));
                ImGui::GetWindowDrawList()->AddImage(p.tex->texture(), q, ImVec2(q.x + p.w * z, q.y + p.h * z));
            }
        } else if (e.kind == 3) {
            const auto& d = t.ftx.decodes[e.pic];
            ImGui::TextColored(pal::heading(), "%s:", dect2::ftxModeName(d.mode)); ImGui::SameLine(0, 5 * gUi);
            ImGui::PushFont(a.mono, 0);
            if (d.cq) ImGui::TextColored(pal::okGreen(), "%s", d.msg.c_str()); else ImGui::TextUnformatted(d.msg.c_str());
            ImGui::PopFont();
        } else {
            ImGui::TextColored(pal::heading(), "FreeDV:"); ImGui::SameLine(0, 5 * gUi);
            ImGui::Text("%s sync, text '%s'", S.fdvMode >= 0 ? dect2::freedvModeName(S.fdvMode) : "no", S.fdvText.c_str());
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::PopTextWrapPos();
}

// the audio spectrum (top) and its waterfall (below), 0 .. 4000 Hz, with the tones to tune to
void panels(App& a) {
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    const bool on = live(a);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    if (av.x < 80 || av.y < 80) return;
    const float cap = ImGui::GetTextLineHeightWithSpacing();
    captionFit(av.x, "Audio 0 to 4000 Hz (upper sideband): the RTTY tones, the SSTV signal and the FT8 / WSPR decodes of the last slot");
    const float h1 = std::max(40.f, (av.y - 2 * cap) * 0.45f), h2 = std::max(40.f, av.y - 2 * cap - h1 - 6);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float l = p0.x + 40 * gUi, r = p0.x + av.x - 8 * gUi;
    const float top = p0.y + 4 * gUi, bot = p0.y + h1 - 18 * gUi;
    const float wt = p0.y + h1 + 4, wb = wt + h2 - 18 * gUi;
    if (r - l < 60 || bot - top < 20) { ImGui::Dummy(ImVec2(av.x, h1 + h2 + 4)); return; }
    auto X = [&](double hz) { return (float)(l + hz / 4000.0 * (r - l)); };
    const ImU32 grid = IM_COL32(48, 52, 54, 255), txt = IM_COL32(120, 124, 128, 255);
    dl->AddRectFilled(p0, ImVec2(p0.x + av.x, p0.y + h1), IM_COL32(8, 9, 10, 255));
    dl->AddRectFilled(ImVec2(p0.x, wt - 4), ImVec2(p0.x + av.x, wt + h2), IM_COL32(8, 9, 10, 255));
    char b[32];
    const bool have = on && S.spec.size() == (size_t)State::kBins;
    float yTop = -20, yBot = -100;
    if (have) {
        float mx = -200, mn = 200;
        for (float v : S.spec) { mx = std::max(mx, v); mn = std::min(mn, v); }
        yTop = std::ceil((mx + 5) / 10) * 10; yBot = std::max(yTop - 80, std::floor(mn / 10) * 10);
    }
    auto Y = [&](float db) { return bot - (db - yBot) / std::max(1.f, yTop - yBot) * (bot - top); };
    for (int hz = 0; hz <= 4000; hz += 500) {   // the frequency grid, shared by both plots
        dl->AddLine(ImVec2(X(hz), top), ImVec2(X(hz), bot), grid);
        dl->AddLine(ImVec2(X(hz), wt), ImVec2(X(hz), wb), grid);
        snprintf(b, sizeof b, "%d", hz);
        dl->AddText(ImVec2(X(hz) - (hz == 4000 ? 24 : 12) * gUi, wb + 3 * gUi), txt, b);
    }
    if (have) {
        const float lh = ImGui::GetTextLineHeight();
        float step = 10;   // labels at least a line apart
        while (step < 80 && (Y(yBot) - Y(yBot + step)) < lh + 2) step += 10;
        for (float db = yBot; db <= yTop; db += 10) {
            dl->AddLine(ImVec2(l, Y(db)), ImVec2(r, Y(db)), grid);
            if (std::fmod(db - yBot, step) != 0 || Y(db) - lh * 0.5f < top - 1 || Y(db) + lh * 0.5f > bot + 1) continue;
            snprintf(b, sizeof b, "%.0f", db);
            dl->AddText(ImVec2(p0.x + 4 * gUi, Y(db) - lh * 0.5f), txt, b);
        }
    }
    // the SSTV signal's fixed tones, faint, and the RTTY tones once found
    struct Mark { double hz; const char* label; int row; };
    const Mark sstv[] = {{1200, "SSTV sync", 0}, {1500, "black", 1}, {1900, "leader", 0}, {2300, "white", 1}};
    const ImU32 cs = IM_COL32(110, 170, 235, 90);
    for (const Mark& m : sstv) {
        for (int k = 0; k < 2; k++) {
            const float y0 = k ? wt : top, y1 = k ? wb : bot;
            dl->AddLine(ImVec2(X(m.hz), y0), ImVec2(X(m.hz), y1), cs, 1.f);
        }
        if (bot - top > 5 * ImGui::GetTextLineHeight())   // room for these and the RTTY labels at the bottom
            dl->AddText(ImVec2(X(m.hz) + 3 * gUi, top + 2 * gUi + m.row * ImGui::GetTextLineHeight()), IM_COL32(110, 170, 235, 150), m.label);
    }
    if (on && t.rtty.state >= 1 && t.rtty.markHz > 0) {
        const ImU32 cm = IM_COL32(120, 230, 140, 230), csp = IM_COL32(230, 180, 70, 230);
        for (int k = 0; k < 2; k++) {
            const float y0 = k ? wt : top, y1 = k ? wb : bot;
            dl->AddLine(ImVec2(X(t.rtty.markHz), y0), ImVec2(X(t.rtty.markHz), y1), cm, 2.f);
            dl->AddLine(ImVec2(X(t.rtty.spaceHz), y0), ImVec2(X(t.rtty.spaceHz), y1), csp, 2.f);
        }
        dl->AddText(ImVec2(X(t.rtty.markHz) + 3 * gUi, bot - ImGui::GetTextLineHeight() - 2 * gUi), cm, "RTTY mark");
        dl->AddText(ImVec2(X(t.rtty.spaceHz) + 3 * gUi, bot - 2 * ImGui::GetTextLineHeight() - 2 * gUi), csp, "space");
    }
    if (on) {   // the decodes of the newest slot of each mode, as ticks along the top of the waterfall
        double newest[dect2::kFtxModes] = {};
        for (const auto& d : t.ftx.decodes) newest[d.mode] = std::max(newest[d.mode], d.slotUtc);
        for (const auto& d : t.ftx.decodes) {
            if (d.slotUtc != newest[d.mode] || d.hz <= 0 || d.hz >= 4000) continue;
            const float x = X(d.hz);
            dl->AddLine(ImVec2(x, wt), ImVec2(x, wt + 8 * gUi), IM_COL32(240, 200, 90, 230), 2.f);
        }
    }
    if (have) {
        std::vector<ImVec2> pts(S.spec.size());
        for (size_t i = 0; i < pts.size(); i++) pts[i] = ImVec2(X((double)i * 4000.0 / (double)pts.size()), std::max(top, std::min(bot, Y(S.spec[i]))));
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(140, 230, 160, 255), 0, 1.3f);
    } else {
        dl->AddText(ImVec2(l + 10, top + 10), txt, on ? "no audio yet" : "start the receiver");
    }
    // the waterfall: newest row at the top
    if (gGfx && !S.wf) S.wf = gGfx->createImage(State::kBins, State::kRows, 0xFF000000u);
    if (S.wf && S.wfDirty && !S.wfPx.empty()) { S.wf->update(0, 0, State::kBins, State::kRows, S.wfPx.data()); S.wfDirty = false; }
    if (S.wf && !S.wfPx.empty()) dl->AddImage(S.wf->texture(), ImVec2(l, wt), ImVec2(r, wb));
    ImGui::Dummy(ImVec2(av.x, h1 + h2 + 4));
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("state", "%s", dect2::hfdigSummary(t).c_str());
    kv("input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kv("sideband", "%.1f dBFS (200 - 3800 Hz above the dial frequency)", t.audioDb);
    kv("signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    const dect2::HfdigTelemetry& t = a.rx.hfdig;
    lamp("RTTY", on && t.rtty.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("SSTV", on && t.sstv.state == 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("FreeDV", on && t.freedv.mode >= 0 ? 1 : 0); flowNext(10 * gUi);
    lamp("FT8/WSPR", on && t.ftx.total > 0 ? (t.ftx.clockWarn ? 2 : 1) : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::hfdigSummary(t).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "HF digital";
    if (live(a)) l2 = dect2::hfdigSummary(a.rx.hfdig);
}

} // namespace

extern const ModeUi kHfdigUi;
const ModeUi kHfdigUi = {
    .sideTitle = "ACTIVITY",
    .tabName = "Decoders",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .panels = panels,
    .status = status,
    .summary = summary,
    .tick = tick,
};
