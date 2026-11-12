// The General receiver screen of 4.0 (docs/GENERAL-4.0.md): the pure logic under it, with no ImGui, so tests/general40 can check it.
// The layout state (sidebar width and visibility, the spectrum / waterfall split, the drawer, which sidebar sections are open, snap)
// and its text form in the preferences; the sidebar width a window of a given width can give; the VFO hit test and the drag maths.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace genlayout {

// The sidebar's sections, top to bottom. Each one's open state is one bit of Layout::open.
enum Section : int { SecStatus, SecSource, SecRadio, SecAudio, SecDisplay, SecRecording, SecBookmarks, SecSignals, SecChannels, SecMode, SecTools, SecLog, SecCount };
constexpr uint32_t bit(int s) { return 1u << (unsigned)s; }
constexpr uint32_t kDefaultOpen = bit(SecStatus) | bit(SecSource) | bit(SecRadio) | bit(SecAudio);
constexpr uint32_t kAllSections = (1u << SecCount) - 1;

// sizes in unscaled points (times the text scale when drawn)
constexpr float kSideDefault = 300, kSideMin = 210, kSideMax = 640;
constexpr float kPlotMinW = 360;                  // the spectrum keeps at least this much width before the sidebar hides itself
constexpr float kFracMin = 0.2f, kFracMax = 0.8f; // the spectrum's share of the plot height (the shared plot clamps to the same)
constexpr float kDrawerDefault = 240, kDrawerMin = 110;

struct Layout {
    float sideW = kSideDefault;   // the sidebar's width
    bool sideOpen = true;         // shown (the button at its top or Ctrl+B / Cmd+B hide it)
    float specFrac = 0.42f;       // the spectrum's share of the plot; the waterfall has the rest (the bar between them is dragged)
    bool drawerOpen = false;      // the drawer under the waterfall: Receiver, History, Memory, Frequencies, a second mode
    float drawerH = kDrawerDefault;
    uint32_t open = kDefaultOpen; // the open sections
    bool snap = true;             // VFO clicks and drags snap to the step
    bool operator==(const Layout& o) const {
        return sideW == o.sideW && sideOpen == o.sideOpen && specFrac == o.specFrac && drawerOpen == o.drawerOpen && drawerH == o.drawerH && open == o.open && snap == o.snap;
    }
};

inline Layout clamped(Layout L) {
    if (!std::isfinite(L.sideW)) L.sideW = kSideDefault;
    if (!std::isfinite(L.specFrac)) L.specFrac = Layout{}.specFrac;
    if (!std::isfinite(L.drawerH)) L.drawerH = kDrawerDefault;
    L.sideW = std::clamp(L.sideW, kSideMin, kSideMax);
    L.specFrac = std::clamp(L.specFrac, kFracMin, kFracMax);
    L.drawerH = std::clamp(L.drawerH, kDrawerMin, 2000.f);
    L.open &= kAllSections;
    return L;
}

// The preferences text: "v=1;w=300;o=1;f=0.420;d=0;dh=240;s=f;n=1". Unknown keys are skipped and missing ones keep their default, so a
// later version can add keys and an older one still reads what it knows.
inline std::string encode(const Layout& in) {
    const Layout L = clamped(in);
    char b[160];
    std::snprintf(b, sizeof b, "v=1;w=%.0f;o=%d;f=%.3f;d=%d;dh=%.0f;s=%x;n=%d", L.sideW, L.sideOpen ? 1 : 0, L.specFrac, L.drawerOpen ? 1 : 0, L.drawerH, L.open, L.snap ? 1 : 0);
    return b;
}

// false (and the defaults) for empty or foreign text
inline bool decode(const std::string& s, Layout& out) {
    Layout L;
    bool v1 = false;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        const std::string kv = s.substr(i, j - i);
        i = j + 1;
        const size_t eq = kv.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        if (v.empty()) continue;
        char* e = nullptr;
        if (k == "s") { const unsigned long m = std::strtoul(v.c_str(), &e, 16); if (e != v.c_str()) L.open = (uint32_t)m; continue; }
        const double d = std::strtod(v.c_str(), &e);
        if (e == v.c_str()) continue;
        if (k == "v") v1 = d >= 1;
        else if (k == "w") L.sideW = (float)d;
        else if (k == "o") L.sideOpen = d != 0;
        else if (k == "f") L.specFrac = (float)d;
        else if (k == "d") L.drawerOpen = d != 0;
        else if (k == "dh") L.drawerH = (float)d;
        else if (k == "n") L.snap = d != 0;
    }
    out = v1 ? clamped(L) : Layout{};
    return v1;
}

// The width the sidebar is drawn at for a content area availW wide (points), 0 when it is not drawn. A window too narrow for the sidebar
// and kPlotMinW of spectrum hides it (autoHidden); `forced` (the button pressed while hidden that way) shows it anyway, narrower.
inline float sideWidth(float availW, const Layout& L, bool forced, bool* autoHidden = nullptr) {
    if (autoHidden) *autoHidden = false;
    if (!L.sideOpen || availW <= 0) return 0;
    const float want = std::clamp(L.sideW, kSideMin, kSideMax);
    const float room = availW - kPlotMinW;
    if (room >= kSideMin) return std::min(want, room);
    if (forced) return std::max(0.f, std::min(want, availW * 0.7f));
    if (autoHidden) *autoHidden = true;
    return 0;
}

// The sidebar button (and Ctrl+B): hide it when shown; show it when hidden by hand; when the window hid it, show it narrower.
inline void toggleSide(Layout& L, bool& forced, bool autoHidden) {
    if (!L.sideOpen) { L.sideOpen = true; forced = autoHidden; return; }
    if (autoHidden && !forced) { forced = true; return; }
    L.sideOpen = false;
    forced = false;
}

// ---- the VFO on the spectrum and the waterfall
enum class Hit { None, Body, EdgeLo, EdgeHi };

// What the mouse at mx is over: a filter edge (within grab pixels, only the edges that can move), the passband or the centre line (Body:
// a drag moves the whole VFO), or nothing. A band narrower than 2.5 grabs on screen can only be moved: zoom in to drag its edges.
inline Hit hitTest(float mx, float xLo, float xHi, float xCentre, float grab, bool canLo, bool canHi) {
    if (xLo > xHi) std::swap(xLo, xHi);
    const bool tiny = xHi - xLo < 2.5f * grab;
    if (!tiny) {
        const float dLo = std::fabs(mx - xLo), dHi = std::fabs(mx - xHi);
        const bool nLo = canLo && dLo < grab, nHi = canHi && dHi < grab;
        if (nLo && nHi) return dHi < dLo ? Hit::EdgeHi : Hit::EdgeLo;
        if (nLo) return Hit::EdgeLo;
        if (nHi) return Hit::EdgeHi;
    }
    const float pad = tiny ? grab : 0.f;
    if (mx > xLo - pad && mx < xHi + pad) return Hit::Body;
    if (std::fabs(mx - xCentre) < grab) return Hit::Body;   // USB / LSB: the centre line sits outside the passband
    return Hit::None;
}

inline double snapHz(double hz, double stepHz) { return stepHz > 0 ? std::round(hz / stepHz) * stepHz : hz; }

// A drag of the VFO's body: the mouse frequency less where in the VFO it was grabbed, snapped to the step when snap is on
inline double dragCentreHz(double mouseHz, double grabOffsetHz, double stepHz, bool snap) {
    const double c = mouseHz - grabOffsetHz;
    return snap ? snapHz(c, stepHz) : c;
}

// A drag of a filter edge: the filter width for the mouse d Hz from the VFO centre. kind 0: symmetric (NFM, WFM, AM, CW), 1 USB, 2 LSB
// (their inner edge sits ssbLowCut from the centre). Not clamped: the caller clamps to the demodulator's range.
inline double edgeWidthHz(int kind, double d, double ssbLowCut) {
    if (kind == 1) return d - ssbLowCut;
    if (kind == 2) return -d - ssbLowCut;
    return 2 * std::fabs(d);
}

// The wheel over the VFO: n notches move it n steps from the nearest step (the same rule as the wheel tuning of #409)
inline double wheelTuneHz(double hz, int n, double stepHz) { return stepHz > 0 ? snapHz(hz, stepHz) + n * stepHz : hz; }

// ---- Auto demodulator (off by default): the demodulator and the filter for the tuned frequency, from the band plan's service of the band
// there ("fm", "am", "air", "pmr", "marine", "ham", ...; docs/band-plans.md) or, with no plan band, from builtInService(). The demodulator
// numbers are dect2::GeneralDemod's (core/include/dect2/general/general_rx.h): 0 NFM, 1 WFM, 2 AM, 3 USB, 4 LSB, 5 CW.
struct AutoPick { bool ok = false; int demod = 0; double widthHz = 0; const char* why = ""; };

// the service of a few well-known ranges, for when the band plan has nothing at hz ("" when none)
inline const char* builtInService(double hz) {
    struct R { double lo, hi; const char* s; };
    static const R r[] = {
        {148.5e3, 283.5e3, "am"}, {526.5e3, 1710e3, "am"},
        {1.8e6, 2.0e6, "ham"}, {3.5e6, 4.0e6, "ham"}, {5.3515e6, 5.3665e6, "ham"}, {7.0e6, 7.3e6, "ham"}, {10.1e6, 10.15e6, "ham"},
        {14.0e6, 14.35e6, "ham"}, {18.068e6, 18.168e6, "ham"}, {21.0e6, 21.45e6, "ham"}, {24.89e6, 24.99e6, "ham"}, {28.0e6, 29.7e6, "ham"},
        {50.0e6, 54.0e6, "ham"}, {87.5e6, 108.0e6, "fm"}, {108.0e6, 137.0e6, "air"}, {144.0e6, 148.0e6, "ham"}, {156.0e6, 162.05e6, "marine"},
        {430.0e6, 440.0e6, "ham"}, {446.0e6, 446.2e6, "pmr"}};
    for (const R& x : r) if (hz >= x.lo && hz <= x.hi) return x.s;
    return "";
}

// amateur bands: CW in the CW sub-bands, then voice by the usual habit (lower sideband under 10 MHz except 60 m, upper above, FM on the
// FM parts of 10 m, 6 m, 2 m and 70 cm). The IARU segments differ a little by region; the ham voice buttons follow the region exactly.
inline AutoPick hamPick(double hz) {
    static const double cw[][2] = {{1.81e6, 1.838e6}, {3.5e6, 3.57e6}, {7.0e6, 7.04e6}, {10.1e6, 10.15e6}, {14.0e6, 14.07e6}, {18.068e6, 18.095e6},
                                   {21.0e6, 21.07e6}, {24.89e6, 24.915e6}, {28.0e6, 28.07e6}, {50.0e6, 50.1e6}, {144.0e6, 144.15e6}, {432.0e6, 432.1e6}};
    for (const auto& b : cw) if (hz >= b[0] && hz < b[1]) return {true, 5, 500, "ham CW segment"};
    if (hz < 30e6) {
        if (hz >= 5.3e6 && hz < 5.5e6) return {true, 3, 2700, "60 m: upper sideband"};
        return hz < 10e6 ? AutoPick{true, 4, 2700, "ham voice under 10 MHz: lower sideband"} : AutoPick{true, 3, 2700, "ham voice over 10 MHz: upper sideband"};
    }
    if ((hz >= 29.5e6 && hz < 29.7e6) || (hz >= 51.0e6 && hz < 54.0e6) || (hz >= 144.5e6 && hz < 148.0e6) || (hz >= 433.0e6 && hz < 440.0e6))
        return {true, 0, 12500, "ham FM segment"};
    return {true, 3, 2700, "ham SSB segment"};
}

inline AutoPick autoPick(const std::string& service, double hz, double bandWidthHz) {
    const double bw = bandWidthHz > 0 ? bandWidthHz : 0;
    if (service == "fm") return {true, 1, bw >= 50e3 && bw <= 220e3 ? bw : 180e3, "FM broadcast"};
    if (service == "am") return {true, 2, bw >= 1e3 && bw <= 20e3 ? bw : 9e3, "AM broadcast"};
    if (service == "air") return {true, 2, hz >= 118e6 ? 6e3 : 9e3, "airband: AM"};
    if (service == "pmr" || service == "marine") return {true, 0, bw >= 2e3 && bw <= 30e3 ? bw : 12.5e3, "narrow FM"};
    if (service == "ham") return hamPick(hz);
    return {};
}

}  // namespace genlayout
