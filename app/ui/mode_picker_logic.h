// The Mode Browser's pure logic (no ImGui, no mode table): tags, the filter, the grid layout, keyboard moves, the recent list and the
// "does my radio fit this mode" check. app/ui/mode_picker.cpp draws it; tests/app/test_mode_picker.cpp tests it.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace modepick {

// ---- tags (the chips of the browser) ----
enum Tag : uint32_t {
    TBroadcast = 1u << 0, TAviation = 1u << 1, TMarine = 1u << 2, TVoice = 1u << 3, TSatellite = 1u << 4, TAmateur = 1u << 5,
    TData = 1u << 6, TTv = 1u << 7, TWeather = 1u << 8, THf = 1u << 9, TSensors = 1u << 10, TPlugins = 1u << 11,
};
constexpr int kNumTags = 12;
inline const char* tagName(int i) {
    static const char* const n[kNumTags] = {"Broadcast", "Aviation", "Marine", "Voice", "Satellite", "Amateur", "Data", "TV", "Weather", "HF", "Sensors", "Plugins"};
    return i >= 0 && i < kNumTags ? n[i] : "";
}
// "needs" bits: shown as a note on the preview card, never blocking
enum Need : uint8_t { NDish = 1, NWideRadio = 2, NPlugin = 4, NCodec = 8, NCable = 16, NActiveAnt = 32 };

// kGroupNames order (mode_table.cpp): 0 TV, 1 BROADCAST RADIO, 2 AVIATION, 3 MARITIME, 4 SATELLITE & NAVIGATION, 5 TWO-WAY RADIO,
// 6 CELLULAR & RAIL, 7 PAGERS, 8 IoT & SENSORS, 9 WEATHER & SCIENCE, 10 AMATEUR - DIGITAL, 11 AMATEUR - VOICE & PACKET, 12 HF UTILITY,
// 13 GENERAL & TOOLS, 14 PLUGINS, 15 Kris custom modes, 16 CUSTOM MODES
inline uint32_t tagsOfGroup(int g) {
    switch (g) {
        case 0: return TTv | TBroadcast;
        case 1: return TBroadcast;
        case 2: return TAviation | TData;
        case 3: return TMarine | TData;
        case 4: return TSatellite;
        case 5: return TVoice;
        case 6: return TData;
        case 7: return TData;
        case 8: return TSensors | TData;
        case 9: return TWeather;
        case 10: return TAmateur | TData | THf;
        case 11: return TAmateur | TVoice;
        case 12: return THf | TData;
        case 14: return TPlugins;
        case 15: return TData;
        default: return 0;
    }
}
// a few modes whose tags differ from their group's, by name
inline uint32_t tagsOfName(const std::string& n) {
    if (n == "Marine VHF") return TVoice | TMarine;
    if (n == "Airband" || n == "Airband voice") return TVoice | TAviation;
    if (n == "VOR / ILS" || n == "NDB") return TAviation;
    if (n == "NAVTEX" || n == "DSC" || n == "Weather fax") return TMarine | THf;
    if (n == "Meteor-M LRPT" || n == "GOES HRIT" || n == "Elektro-L") return TSatellite | TWeather;
    if (n == "GNSS") return TSatellite;
    if (n == "FM" || n == "DAB") return TBroadcast;
    return 0;
}
inline uint32_t tagsOf(int group, const std::string& name) {
    const uint32_t o = tagsOfName(name);
    return o ? o : tagsOfGroup(group);
}

// ---- items and the filter ----
struct Item {
    std::string name, group, text;   // text: lower-case name + sub + blurb + keys (+ the translated sub), for the search
    int groupIdx = 0;
    uint32_t tags = 0;
    bool active = false, hidden = false, fav = false, fits = true;
};
inline std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
// the one matcher of the browser, shared with the lists: every word of the query must occur in the text
inline bool textMatches(const std::string& lowerText, const std::string& query) {
    size_t b = 0;
    const std::string q = lower(query);
    bool any = false;
    while (b < q.size()) {
        while (b < q.size() && q[b] == ' ') b++;
        size_t e = q.find(' ', b);
        if (e == std::string::npos) e = q.size();
        if (e > b) { any = true; if (lowerText.find(q.substr(b, e - b)) == std::string::npos) return false; }
        b = e;
    }
    (void)any;
    return true;
}

enum Tab { TabAll, TabFav, TabRecent, TabFit, kNumTabs };
struct Query {
    std::string text;
    int tab = TabAll;
    uint32_t tags = 0;       // chips: all must be set on the item (AND)
    bool showHidden = false;
    int group = -1;          // the rail: one group, or -1
    const std::vector<std::string>* recent = nullptr;   // for TabRecent, most recent first
};
// Does the item pass everything but the tab? (the tab counts use this too)
inline bool passes(const Item& it, const Query& q, bool ignoreTags = false) {
    const bool searching = !q.text.empty();
    if (it.hidden && !it.active && !searching && !q.showHidden) return false;   // the active mode is never hidden; a search finds hidden ones
    if (q.group >= 0 && it.groupIdx != q.group) return false;
    if (!ignoreTags && (it.tags & q.tags) != q.tags) return false;
    return textMatches(it.text, q.text);
}
inline bool inTab(const Item& it, int tab, const Query& q) {
    switch (tab) {
        case TabFav: return it.fav;
        case TabRecent: return q.recent && std::find(q.recent->begin(), q.recent->end(), it.name) != q.recent->end();
        default: return true;   // All, and Compatible (which greys, never removes)
    }
}
// indices of the items to draw: table order (groups in order), or the recent order for the Recent tab
inline std::vector<int> filter(const std::vector<Item>& items, const Query& q) {
    std::vector<int> out;
    for (int i = 0; i < (int)items.size(); i++) if (passes(items[i], q) && inTab(items[i], q.tab, q)) out.push_back(i);
    if (q.tab == TabRecent && q.recent) {
        auto rank = [&](int i) { return (int)(std::find(q.recent->begin(), q.recent->end(), items[i].name) - q.recent->begin()); };
        std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return rank(a) < rank(b); });
    }
    return out;
}
// chip counts for dimming: how many items a chip would leave in the current tab
inline int countWithTag(const std::vector<Item>& items, const Query& q, int tagIdx) {
    Query c = q; c.tags |= (1u << tagIdx);
    int n = 0;
    for (const Item& it : items) if (passes(it, c) && inTab(it, q.tab, q)) n++;
    return n;
}
inline int countFit(const std::vector<Item>& items, const Query& q) {
    int n = 0;
    for (const Item& it : items) if (passes(it, q) && it.fits) n++;
    return n;
}

// ---- recent list: most recent first, no duplicates, capped ----
constexpr size_t kRecentCap = 12;
inline void recentPush(std::vector<std::string>& r, const std::string& name) {
    r.erase(std::remove(r.begin(), r.end(), name), r.end());
    r.insert(r.begin(), name);
    if (r.size() > kRecentCap) r.resize(kRecentCap);
}
inline std::vector<std::string> recentParse(const std::string& s) {   // "a|b|c", order kept
    std::vector<std::string> v;
    size_t b = 0;
    while (b <= s.size()) {
        size_t e = s.find('|', b);
        if (e == std::string::npos) e = s.size();
        if (e > b) v.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    if (v.size() > kRecentCap) v.resize(kRecentCap);
    return v;
}
inline std::string recentJoin(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) { if (!out.empty()) out += "|"; out += s; }
    return out;
}

// ---- layout: columns of tiles, and moving a focus cell over sections (family headers) ----
struct Cols { int n = 1; float tileW = 150; };
inline Cols columns(float availW, float gap, float minW, float maxW) {
    Cols c;
    availW = std::max(availW, minW);
    c.n = std::max(1, (int)std::floor((availW + gap) / (minW + gap)));
    c.tileW = (availW - gap * (c.n - 1)) / c.n;
    if (c.tileW > maxW) {   // wide window: more columns of at most maxW if that fits, else stretch no more than maxW
        c.n = std::max(c.n, (int)std::ceil((availW + gap) / (maxW + gap)));
        c.tileW = (availW - gap * (c.n - 1)) / c.n;
    }
    return c;
}
// sizes: the tile count of each section in draw order. Moves the flat index; the result stays inside [0, total).
enum Move { Left, Right, Up, Down, Home, End, PageUp, PageDown };
inline int moveFocus(const std::vector<int>& sizes, int cols, int idx, Move m) {
    int total = 0;
    for (int s : sizes) total += s;
    if (total == 0) return 0;
    idx = std::clamp(idx, 0, total - 1);
    cols = std::max(1, cols);
    // locate: section start, local index
    int sec = 0, start = 0;
    while (sec + 1 < (int)sizes.size() && idx >= start + sizes[sec]) { start += sizes[sec]; sec++; }
    auto secStart = [&](int s) { int t = 0; for (int i = 0; i < s; i++) t += sizes[i]; return t; };
    auto prevNonEmpty = [&](int s) { while (--s >= 0 && sizes[s] == 0) {} return s; };
    auto nextNonEmpty = [&](int s) { while (++s < (int)sizes.size() && sizes[s] == 0) {} return s < (int)sizes.size() ? s : -1; };
    int local = idx - start, col = local % cols, row = local / cols;
    auto up = [&](int steps) {
        for (int k = 0; k < steps; k++) {
            if (row > 0) { row--; continue; }
            int p = prevNonEmpty(sec);
            if (p < 0) { return; }
            sec = p; start = secStart(sec);
            row = (sizes[sec] - 1) / cols;
            if (row * cols + col >= sizes[sec]) { col = (sizes[sec] - 1) % cols; }   // the last row is short: the nearest tile
        }
    };
    auto down = [&](int steps) {
        for (int k = 0; k < steps; k++) {
            const int lastRow = (sizes[sec] - 1) / cols;
            if (row < lastRow) {
                row++;
                if (row * cols + col >= sizes[sec]) col = (sizes[sec] - 1) % cols;
                continue;
            }
            int n = nextNonEmpty(sec);
            if (n < 0) return;
            sec = n; start = secStart(sec); row = 0;
            if (col >= sizes[sec]) col = sizes[sec] - 1;
        }
    };
    switch (m) {
        case Left: return std::max(0, idx - 1);
        case Right: return std::min(total - 1, idx + 1);
        case Home: return 0;
        case End: return total - 1;
        case Up: up(1); break;
        case Down: down(1); break;
        case PageUp: up(4); break;
        case PageDown: down(4); break;
    }
    return std::clamp(start + row * cols + col, 0, total - 1);
}

// ---- does the radio fit the mode (never blocks: the caller greys the tile and shows the reason) ----
struct Radio { bool known = false; double minHz = 0, maxHz = 0, maxRateHz = 0; };   // known=false: no radio selected (a test signal, a file, a sound card)
struct ModeNeed { double minRateHz = 0, defMhz = 0; };                              // 0 = unknown
struct Fit { bool ok = true; std::string reason; };
inline std::string msps(double hz) {
    char b[32];
    snprintf(b, sizeof b, hz >= 1e7 || std::fabs(hz / 1e6 - std::round(hz / 1e6)) < 0.05 ? "%.0f" : "%.1f", hz / 1e6);
    return b;
}
inline Fit fit(const Radio& r, const ModeNeed& n) {
    Fit f;
    if (!r.known) return f;
    if (n.minRateHz > 0 && r.maxRateHz > 0 && r.maxRateHz + 1 < n.minRateHz) { f.ok = false; f.reason = "needs " + msps(n.minRateHz) + " Msps radio"; return f; }
    if (n.defMhz > 0 && r.maxHz > 0) {
        const double mhz = n.defMhz;
        if (mhz * 1e6 < r.minHz || mhz * 1e6 > r.maxHz) {
            char b[64];
            snprintf(b, sizeof b, mhz < 30 ? "radio does not reach %.3g MHz" : "radio does not reach %.0f MHz", mhz);
            f.ok = false; f.reason = b;
        }
    }
    return f;
}

}  // namespace modepick
