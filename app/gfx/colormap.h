// The waterfall colour maps as pure functions: v in 0..1 to 0xAABBGGRR (opaque). Map numbers match Waterfall::Map and are stored in the
// settings: new maps go at the end, never in between. Also the custom gradient (stops as text), the level transform (brightness,
// contrast, gamma) and the trace colour presets. Tested in tests/app/test_plot_util.cpp and tests/app/test_spec_axis.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cmap {
enum Map { Classic, Viridis, Inferno, Grey, Turbo, Cividis, Magma, SdrClassic, Ice, Fire, NightRed, HighContrast, Custom, SdrppSmooth, OxideBlue, NavyHeat, BlueGreen, Amber, Paper, NumMaps };

inline const char* name(int m) {
    static const char* const n[NumMaps] = {"Classic", "Viridis", "Inferno", "Grey", "Turbo", "Cividis", "Magma", "Deep heat", "Ice", "Fire", "Night (red)", "High contrast", "Custom",
                                           "Smooth heat", "Deep blue", "Navy heat", "Blue green", "Amber", "Paper"};
    return n[m < 0 || m >= NumMaps ? 0 : m];
}
// reads the same with the common colour blindnesses (perceptually even, one hue ramp or lightness only)
inline bool colourBlindSafe(int m) { return m == Viridis || m == Cividis || m == Magma || m == Inferno || m == Grey || m == Ice; }
// the rainbow maps: their brightness goes up and down by design (the custom one may be anything)
inline bool hueRamp(int m) { return m == Turbo || m == SdrClassic || m == HighContrast || m == Custom || m == SdrppSmooth || m == OxideBlue || m == NavyHeat || m == BlueGreen; }

inline uint32_t stopsColor(const float (*s)[3], int n, float v) {   // n colour stops (0..255 per channel), blended linearly
    v *= (float)(n - 1);
    const int i = std::min(n - 2, (int)v);
    const float f = v - i;
    auto ch = [&](int k) { return (uint32_t)(s[i][k] + (s[i + 1][k] - s[i][k]) * f + 0.5f); };
    return 0xFF000000u | (ch(2) << 16) | (ch(1) << 8) | ch(0);
}

// A gradient of up to 16 stops at positions 0..1 (sorted), the colours 0..255. As text: "0:000000,0.5:1e90ff,1:ffffff".
struct Gradient {
    static constexpr int Max = 16;
    int n = 0;
    float pos[Max] = {};
    float rgb[Max][3] = {};
    uint32_t at(float v) const {
        if (n <= 0) return 0xFF000000u;
        v = std::min(1.f, std::max(0.f, v));
        int i = 0;
        while (i < n - 1 && pos[i + 1] < v) i++;
        float f = 0;
        if (i < n - 1 && pos[i + 1] > pos[i]) f = std::min(1.f, std::max(0.f, (v - pos[i]) / (pos[i + 1] - pos[i])));
        if (v <= pos[0]) { i = 0; f = 0; }
        const int j = std::min(n - 1, i + 1);
        auto ch = [&](int k) { return (uint32_t)std::min(255.f, std::max(0.f, rgb[i][k] + (rgb[j][k] - rgb[i][k]) * f + 0.5f)); };
        return 0xFF000000u | (ch(2) << 16) | (ch(1) << 8) | ch(0);
    }
    void sort() {   // by position (insertion sort: at most 16)
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && pos[j] < pos[j - 1]; j--) { std::swap(pos[j], pos[j - 1]); for (int k = 0; k < 3; k++) std::swap(rgb[j][k], rgb[j - 1][k]); }
    }
    // false (and the gradient unchanged) when the text has fewer than two good stops
    bool parse(const std::string& s) {
        Gradient g;
        size_t p = 0;
        while (p < s.size() && g.n < Max) {
            size_t e = s.find(',', p); if (e == std::string::npos) e = s.size();
            const std::string t = s.substr(p, e - p);
            p = e + 1;
            const size_t c = t.find(':');
            if (c == std::string::npos || t.size() - c - 1 != 6) continue;
            char* end = nullptr;
            const float ps = std::strtof(t.c_str(), &end);
            if (end == t.c_str() || !std::isfinite(ps)) continue;
            const unsigned long hex = std::strtoul(t.c_str() + c + 1, &end, 16);
            if (*end) continue;
            g.pos[g.n] = std::min(1.f, std::max(0.f, ps));
            g.rgb[g.n][0] = (float)((hex >> 16) & 255); g.rgb[g.n][1] = (float)((hex >> 8) & 255); g.rgb[g.n][2] = (float)(hex & 255);
            g.n++;
        }
        if (g.n < 2) return false;
        g.sort();
        *this = g;
        return true;
    }
    std::string text() const {
        std::string o;
        for (int i = 0; i < n; i++) {
            char b[24];
            snprintf(b, sizeof b, "%s%.3g:%02x%02x%02x", i ? "," : "", pos[i], (int)rgb[i][0], (int)rgb[i][1], (int)rgb[i][2]);
            o += b;
        }
        return o;
    }
};

// Starting points for the custom gradient
struct GradientPreset { const char* name; const char* stops; };
inline const GradientPreset kGradientPresets[] = {
    {"Blue to white", "0:000010,0.35:0a3a8c,0.7:3fa0e0,1:ffffff"},
    {"Green phosphor", "0:000000,0.4:004010,0.75:20c040,1:d0ffd0"},
    {"Amber", "0:000000,0.4:401800,0.75:e08000,1:fff0c0"},
    {"Purple haze", "0:05000a,0.35:3a0a5a,0.7:c040a0,1:ffe0f0"},
    {"Rainbow", "0:000000,0.15:0000ff,0.35:00ffff,0.55:00ff00,0.75:ffff00,0.9:ff0000,1:ffffff"},
    {"Ocean", "0:000005,0.3:002040,0.6:008080,0.85:60e0c0,1:f0fff8"},
};

inline Gradient& custom() {   // the custom map's stops (the setting "wfCustom"; a blue-to-white start)
    static Gradient g = [] { Gradient x; x.parse(kGradientPresets[0].stops); return x; }();
    return g;
}

inline uint32_t color(int map, float v) {
    v = std::min(1.f, std::max(0.f, v));
    if (map == Turbo) {   // Turbo (Google's colour map), with the noise floor fading to black
        const float t = 0.12f + 0.84f * v;
        auto poly = [&](float a0, float a1, float a2, float a3, float a4, float a5) { return std::min(1.f, std::max(0.f, a0 + t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5)))))); };
        const float r = poly(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f, -152.94239396f, 59.28637943f);
        const float g = poly(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f, 4.27729857f, 2.82956604f);
        const float b = poly(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f, -89.90310912f, 27.34824973f);
        const float f = std::min(1.f, v / 0.22f), k = f * f * (3 - 2 * f);   // smoothstep: black at the bottom
        return 0xFF000000u | ((uint32_t)(b * k * 255 + 0.5f) << 16) | ((uint32_t)(g * k * 255 + 0.5f) << 8) | (uint32_t)(r * k * 255 + 0.5f);
    }
    if (map == Custom) return custom().at(v);
    static const float cool[5][3] = {{6, 10, 20}, {16, 38, 70}, {30, 100, 140}, {110, 190, 205}, {240, 250, 250}};   // Classic: dark blue, teal, near white
    static const float grey[5][3] = {{0, 0, 0}, {34, 34, 33}, {96, 95, 92}, {170, 168, 162}, {244, 242, 236}};
    static const float viridis[9][3] = {{68, 1, 84}, {71, 44, 122}, {59, 81, 139}, {44, 113, 142}, {33, 144, 141}, {39, 173, 129}, {92, 200, 99}, {170, 220, 50}, {253, 231, 37}};
    static const float inferno[9][3] = {{0, 0, 4}, {31, 12, 72}, {85, 15, 109}, {136, 34, 106}, {186, 54, 85}, {227, 89, 51}, {249, 140, 10}, {249, 201, 50}, {252, 255, 164}};
    static const float cividis[9][3] = {{0, 32, 77}, {26, 52, 107}, {66, 78, 108}, {97, 104, 111}, {124, 123, 120}, {154, 145, 119}, {187, 167, 108}, {221, 193, 88}, {255, 233, 69}};
    static const float magma[9][3] = {{0, 0, 4}, {28, 16, 68}, {79, 18, 123}, {129, 37, 129}, {181, 54, 122}, {229, 80, 100}, {251, 135, 97}, {254, 194, 135}, {252, 253, 191}};
    static const float sdrpp[13][3] = {{0, 0, 32}, {0, 0, 48}, {0, 0, 80}, {0, 0, 145}, {30, 144, 255}, {255, 255, 255}, {255, 255, 0}, {254, 109, 22}, {255, 0, 0}, {198, 0, 0}, {159, 0, 0}, {117, 0, 0}, {74, 0, 0}};
    static const float ice[6][3] = {{0, 0, 0}, {8, 20, 60}, {20, 70, 150}, {60, 150, 210}, {170, 225, 245}, {255, 255, 255}};
    static const float fire[7][3] = {{0, 0, 0}, {60, 0, 0}, {150, 20, 0}, {220, 80, 0}, {255, 160, 20}, {255, 230, 120}, {255, 255, 240}};
    static const float night[6][3] = {{0, 0, 0}, {40, 0, 0}, {100, 0, 0}, {170, 10, 5}, {230, 40, 20}, {255, 90, 60}};   // keeps the eyes dark-adapted
    static const float hc[6][3] = {{0, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 0}, {255, 0, 0}, {255, 255, 255}};
    // the SDR++ look: black, dark blue, cyan, yellow, orange, white, evenly blended (no hard band)
    static const float sdrs[9][3] = {{0, 0, 0}, {0, 4, 40}, {0, 12, 110}, {0, 60, 200}, {0, 200, 250}, {255, 245, 40}, {255, 150, 0}, {255, 70, 10}, {255, 255, 255}};
    // the SDRoxide look: deep navy, blue, a bright blue floor-to-signal step, cyan, green, pale green
    static const float oxide[8][3] = {{0, 4, 28}, {0, 12, 80}, {6, 30, 170}, {20, 70, 235}, {30, 150, 250}, {40, 220, 225}, {60, 240, 110}, {225, 255, 220}};
    // Navy heat: SDR++'s "Classic" map with its 15 evenly spaced stops (the doubled orange and red included, so the hot end is as wide
    // as there); the pixels of the reference screenshot's waterfall (spectrum-ref-44) lie within 1 RGB step (median) of it
    static const float navy[15][3] = {{0, 0, 32}, {0, 0, 48}, {0, 0, 80}, {0, 0, 145}, {30, 144, 255}, {255, 255, 255}, {255, 255, 0}, {254, 109, 22},
                                      {254, 109, 22}, {255, 0, 0}, {255, 0, 0}, {198, 0, 0}, {159, 0, 0}, {117, 0, 0}, {74, 0, 0}};
    // Blue green: measured on the second reference (spectrum-ref-43, the pixels sorted by brightness, the median colour of each slice): the
    // floor in a mid blue, signals through a light blue and teal to green; the top end goes on to a pale green-yellow
    static const float bgreen[10][3] = {{0, 0, 40}, {0, 2, 92}, {0, 5, 128}, {0, 9, 160}, {0, 14, 180}, {8, 96, 188}, {5, 134, 160},
                                        {2, 154, 102}, {40, 205, 60}, {225, 255, 150}};
    // Amber: black, brown, amber, pale yellow (the Mono Pro look). Paper: white, pale blue, blue, navy (for the Daylight look, the floor is light)
    static const float amber[6][3] = {{0, 0, 0}, {34, 18, 0}, {110, 60, 0}, {214, 130, 0}, {255, 190, 40}, {255, 245, 200}};
    static const float paper[7][3] = {{246, 248, 252}, {214, 228, 246}, {150, 190, 235}, {70, 130, 215}, {30, 80, 170}, {20, 40, 110}, {10, 14, 50}};
    switch (map) {
    case Amber: return stopsColor(amber, 6, v);
    case Paper: return stopsColor(paper, 7, v);
    case NavyHeat: return stopsColor(navy, 15, v);
    case BlueGreen: return stopsColor(bgreen, 10, v);
    case Cividis: return stopsColor(cividis, 9, v);
    case Viridis: return stopsColor(viridis, 9, v);
    case Inferno: return stopsColor(inferno, 9, v);
    case Grey: return stopsColor(grey, 5, v);
    case Magma: return stopsColor(magma, 9, v);
    case SdrClassic: return stopsColor(sdrpp, 13, v);
    case Ice: return stopsColor(ice, 6, v);
    case Fire: return stopsColor(fire, 7, v);
    case NightRed: return stopsColor(night, 6, v);
    case HighContrast: return stopsColor(hc, 6, v);
    case SdrppSmooth: return stopsColor(sdrs, 9, v);
    case OxideBlue: return stopsColor(oxide, 8, v);
    default: return stopsColor(cool, 5, v);
    }
}

// Contrast: v ^ gamma (gamma > 1 darkens the weak signals, < 1 lifts them).
inline float applyGamma(float v, float gamma) { return gamma == 1.f ? v : std::pow(std::min(1.f, std::max(0.f, v)), gamma); }

// The level (0..1 over the dB range) to the colour index: brightness shifts it (-0.5..0.5), contrast stretches it about the middle
// (0.25..4), then gamma. 0 / 1 / 1 leave it as it is (the default).
inline float levelTransform(float v, float brightness, float contrast, float gamma) {
    v = std::min(1.f, std::max(0.f, v));
    if (brightness != 0.f || contrast != 1.f) v = std::min(1.f, std::max(0.f, (v - 0.5f) * contrast + 0.5f + brightness));
    return applyGamma(v, gamma);
}

// Trace colours: index 0 is the usual soft white (dark grey on a light theme); the others read on both. RGB 0..1.
struct TracePreset { const char* name; float dark[3]; float light[3]; };
inline const TracePreset kTracePresets[] = {
    {"Soft white", {0.93f, 0.94f, 0.95f}, {0.18f, 0.19f, 0.21f}},
    {"Amber", {1.00f, 0.75f, 0.30f}, {0.70f, 0.42f, 0.00f}},
    {"Green", {0.45f, 0.95f, 0.55f}, {0.05f, 0.50f, 0.20f}},
    {"Cyan", {0.40f, 0.85f, 1.00f}, {0.00f, 0.45f, 0.65f}},
    {"Yellow", {0.98f, 0.92f, 0.35f}, {0.55f, 0.50f, 0.00f}},
    {"Orange (CB safe)", {0.90f, 0.62f, 0.00f}, {0.75f, 0.45f, 0.00f}},    // Okabe-Ito
    {"Sky blue (CB safe)", {0.34f, 0.71f, 0.91f}, {0.00f, 0.45f, 0.70f}},
    {"Pink", {1.00f, 0.55f, 0.80f}, {0.70f, 0.15f, 0.45f}},
    {"Light blue", {0.70f, 0.84f, 1.00f}, {0.10f, 0.35f, 0.70f}},   // the SDRoxide look
    {"Pale cyan", {0.55f, 0.89f, 0.93f}, {0.00f, 0.42f, 0.55f}},    // the SDR++ look (measured on spectrum-ref-44)
    {"Pale steel", {0.74f, 0.83f, 0.89f}, {0.15f, 0.30f, 0.45f}},   // the Crisp look (spectrum-ref-43)
};
constexpr int kNumTracePresets = (int)(sizeof kTracePresets / sizeof *kTracePresets);
inline const float* traceRgb(int i, bool light) {
    const TracePreset& p = kTracePresets[i < 0 || i >= kNumTracePresets ? 0 : i];
    return light ? p.light : p.dark;
}
} // namespace cmap
