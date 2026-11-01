// The pure parts of the per-mode Simple / Advanced view (docs/SIMPLE-ADVANCED.md): the preference key of a mode, the stored text, the
// default for a profile, and the pane widths of the layout helper. No ImGui here, so tests/app/test_mode_panel.cpp can check them.
#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace modepanel {

enum class Level { Simple = 0, Advanced = 1 };

// "uiLevel.<family>" or "uiLevel.<family>.<sub>" (sub 0 is the family's own row)
inline std::string prefKey(int family, int sub) {
    char b[64];
    if (sub > 0) std::snprintf(b, sizeof b, "uiLevel.%d.%d", family, sub);
    else std::snprintf(b, sizeof b, "uiLevel.%d", family);
    return b;
}

inline const char* levelText(Level l) { return l == Level::Simple ? "simple" : "advanced"; }

// anything unknown (empty, an older version's value) gives the default
inline Level parseLevel(const std::string& v, Level def) {
    if (v == "simple") return Level::Simple;
    if (v == "advanced") return Level::Advanced;
    return def;
}

// A profile that has been used before keeps the look it knew (Advanced = the tabs); a new profile starts with the Simple view.
inline Level profileDefault(bool profileUsedBefore) { return profileUsedBefore ? Level::Advanced : Level::Simple; }

// Widths of the panes of the layout helper. weights: relative widths; minW: the narrowest a pane may be. When all panes do not fit side by
// side, `columns` says how many go on the first row (the rest wrap to the next rows); a single column stacks them all.
struct PaneLayout { int perRow = 1; std::vector<float> widths; };
inline PaneLayout layoutPanes(float avail, float gap, const std::vector<float>& weights, float minW) {
    PaneLayout r;
    const int n = (int)weights.size();
    if (n == 0) return r;
    int per = n;
    while (per > 1 && avail - gap * (per - 1) < minW * per) per--;
    r.perRow = per;
    for (int i = 0; i < n; i++) {
        const int first = (i / per) * per, last = std::min(n, first + per);
        float sum = 0;
        for (int j = first; j < last; j++) sum += std::max(0.01f, weights[j]);
        const float room = avail - gap * (float)(last - first - 1);
        r.widths.push_back(std::max(1.f, room * std::max(0.01f, weights[i]) / sum));
    }
    return r;
}

}  // namespace modepanel
