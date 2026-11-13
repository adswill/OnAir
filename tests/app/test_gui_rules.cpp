// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Interface rules that can be checked on the sources of app/ without running the program (docs/GUI-AUDIT.md lists what was checked by hand):
//  1. no SetCursorPos / SetCursorScreenPos right before the end of a window, child, group or function: ImGui reports "Code uses SetCursorPos()
//     to extend window/parent boundaries" (##main2). Restore a saved spot with restoreCursor(), or submit a Dummy().
//  2. windows, tabs, tree nodes, collapsing headers and modal popups are named with trw(), not tr(): their open state and saved place are kept
//     by the ID, which tr() changes with the language (i18n.h)
//  3. no bare pixel sizes where a scaled one is meant: SetNextItemWidth(<number>), WidthFixed columns, SetNextWindowSize(ImVec2(<n>, <n>));
//     first-use window sizes go through firstWindowSize(), which fits them to the display
//  4. no two widgets with the same literal label (the same ImGui ID) in one function without PushID, except in branches that never run together
//  5. every modal popup that is begun with a literal name is opened somewhere with that name
//  6. the translations of short labels stay within a length budget (they sit in buttons, pills and table headers)
//  7. the logic of app/ui/window_fit.h: windows moved back on screen, first-use sizes fitted to the display
#include "ui/window_fit.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Src { std::string rel, text; };

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// comments become spaces (line numbers kept); string and character literals are kept as they are
static std::string stripComments(const std::string& t) {
    std::string o;
    o.reserve(t.size());
    const size_t n = t.size();
    for (size_t i = 0; i < n;) {
        if (t.compare(i, 2, "//") == 0) {
            while (i < n && t[i] != '\n') { o += ' '; i++; }
        } else if (t.compare(i, 2, "/*") == 0) {
            const size_t e = t.find("*/", i + 2);
            const size_t end = e == std::string::npos ? n : e + 2;
            for (; i < end; i++) o += t[i] == '\n' ? '\n' : ' ';
        } else if (t[i] == '"' || t[i] == '\'') {
            const char q = t[i];
            size_t j = i + 1;
            while (j < n && t[j] != q && t[j] != '\n') j += t[j] == '\\' ? 2 : 1;
            j = std::min(n, j + 1);
            o.append(t, i, j - i);
            i = j;
        } else o += t[i++];
    }
    return o;
}

static int lineOf(const std::string& t, size_t pos) { return 1 + (int)std::count(t.begin(), t.begin() + (long)std::min(pos, t.size()), '\n'); }

static std::vector<Src> appSources(const fs::path& root) {
    std::vector<Src> v;
    for (const auto& e : fs::recursive_directory_iterator(root / "app")) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".mm") continue;
        v.push_back({fs::relative(e.path(), root).generic_string(), stripComments(readFile(e.path()))});
    }
    std::sort(v.begin(), v.end(), [](const Src& a, const Src& b) { return a.rel < b.rel; });
    return v;
}

// ---------------------------------------------------------------- 1
static void ruleSetCursorAtEnd(const std::vector<Src>& srcs) {
    const std::regex re(R"(SetCursor(Screen)?Pos[XY]?\s*\()");
    int n = 0;
    for (const Src& s : srcs) {
        const std::string& c = s.text;
        for (auto it = std::sregex_iterator(c.begin(), c.end(), re); it != std::sregex_iterator(); ++it) {
            size_t j = (size_t)(it->position() + it->length());
            for (int dp = 1; j < c.size() && dp; j++) dp += c[j] == '(' ? 1 : c[j] == ')' ? -1 : 0;
            const size_t semi = c.find(';', j);
            if (semi == std::string::npos) continue;
            size_t k = semi + 1;
            bool newline = false;
            while (k < c.size() && (c[k] == ' ' || c[k] == '\t' || c[k] == '\n')) { newline |= c[k] == '\n'; k++; }
            const std::string next = c.substr(k, 20);
            const bool end = next.rfind("ImGui::End(", 0) == 0 || next.rfind("ImGui::EndChild(", 0) == 0 || next.rfind("ImGui::EndGroup(", 0) == 0 ||
                             next.rfind("endPane(", 0) == 0 || (newline && next.rfind("}", 0) == 0 && k > 0 && c[k - 1] == '\n');   // a function's last line
            if (end) { n++; CHECK(false, "%s:%d: SetCursorPos right before the end of a window or function (restoreCursor() or a Dummy())", s.rel.c_str(), lineOf(c, (size_t)it->position())); }
        }
    }
    std::printf("rule 1: %d cursor moves at an end\n", n);
}

// ---------------------------------------------------------------- 2
static void ruleTrwForStateful(const std::vector<Src>& srcs) {
    const std::regex re(R"(\b(ImGui::Begin|BeginTabItem|TreeNode|TreeNodeEx|CollapsingHeader|BeginPopupModal|OpenPopup)\s*\(\s*tr\()");
    for (const Src& s : srcs)
        for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), re); it != std::sregex_iterator(); ++it)
            CHECK(false, "%s:%d: %s(tr(...)): use trw() so the ID is the same in every language", s.rel.c_str(), lineOf(s.text, (size_t)it->position()), (*it)[1].str().c_str());
}

// ---------------------------------------------------------------- 3
static void ruleScaledSizes(const std::vector<Src>& srcs) {
    const std::regex width(R"(SetNextItemWidth\(\s*[0-9]{2,}(\.[0-9]*)?f?\s*\))");
    const std::regex column(R"(WidthFixed\s*,\s*[0-9]{2,}(\.[0-9]*)?f?\s*\))");
    const std::regex window(R"(SetNextWindowSize\(\s*ImVec2\(\s*[0-9]{2,}(\.[0-9]*)?f?\s*,\s*[0-9]{2,}(\.[0-9]*)?f?\s*\))");
    const std::regex firstUse(R"(SetNextWindowSize\([^;]*ImGuiCond_FirstUseEver)");
    for (const Src& s : srcs) {
        for (const std::regex* r : {&width, &column, &window})
            for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), *r); it != std::sregex_iterator(); ++it)
                CHECK(false, "%s:%d: a size in bare pixels (multiply by gUi): %s", s.rel.c_str(), lineOf(s.text, (size_t)it->position()), it->str().c_str());
        if (s.rel == "app/ui/widgets.cpp") continue;   // firstWindowSize() itself
        for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), firstUse); it != std::sregex_iterator(); ++it)
            CHECK(false, "%s:%d: a first-use window size that may not fit the display: use firstWindowSize()", s.rel.c_str(), lineOf(s.text, (size_t)it->position()));
    }
}

// ---------------------------------------------------------------- 4
// branches that never run together (switch cases, if / else, a menu or a button form of the same control)
struct AllowDup { const char* file; const char* id; };
static const AllowDup kAllowDup[] = {
    {"app/gfx/plots.cpp", "Automatic"}, {"app/gfx/plots.cpp", "Manual"},                       // DVB, FM and ISDB-T bandwidth popups
    {"app/hfdig/hfdig_sstv_ui.cpp", "##sstvpic"}, {"app/hfdig/hfdig_sstv_ui.cpp", "##sstvside"},   // wide and narrow layouts
    {"app/marine/marine_ui.cpp", "##msv"},
    {"app/mesh/mesh_ui.cpp", "all presets"},                                                     // one protocol entry or all
    {"app/panels/tx_step_settings.cpp", "Output level (dB)"},                                    // slider or field
    {"app/rooms/rooms_send.cpp", "Go live (Live rooms)..."},                                     // disabled or live form
    {"app/scan/scan_db_ui.cpp", "Cancel"},                                                       // dialog stages
    {"app/scan/scan_outputs.cpp", "##range"},
    {"app/ui/toolbar_source.cpp", "##ssnr"}, {"app/ui/toolbar_source.cpp", "##scfo"},            // the simulator rows per family
};

static void ruleDuplicateIds(const std::vector<Src>& srcs) {
    const std::regex call(R"(ImGui::(Button|SmallButton|Checkbox|RadioButton|InputText|InputTextWithHint|InputTextMultiline|SliderFloat|SliderInt|DragFloat|DragInt|Combo|BeginCombo|InputInt|InputFloat|InputDouble|ColorEdit3|ColorEdit4|TreeNode|CollapsingHeader|BeginTabItem|InvisibleButton|BeginChild)\(\s*(?:(?:tr|trw)\(\s*)?"((?:\\.|[^"\\])*)\")");
    const std::regex header(R"(^[A-Za-z_].*\)\s*(const\s*)?\{\s*$)");
    std::set<std::string> allowUsed;
    for (const Src& s : srcs) {
        std::vector<std::string> lines;
        { std::stringstream ss(s.text); std::string l; while (std::getline(ss, l)) lines.push_back(l); }
        for (size_t i = 0; i < lines.size(); i++) {
            const std::string& h = lines[i];
            if (!std::regex_match(h, header)) continue;
            for (const char* kw : {"namespace", "struct", "class", "enum", "extern", "if", "for", "while", "switch"}) if (h.rfind(kw, 0) == 0) goto next;
            {
                size_t j = i + 1;
                while (j < lines.size() && lines[j] != "}") j++;
                std::string body;
                for (size_t k = i; k < j; k++) { body += lines[k]; body += '\n'; }
                if (body.find("PushID") == std::string::npos && body.find("PushOverrideID") == std::string::npos) {
                    std::map<std::string, std::vector<int>> seen;
                    for (auto it = std::sregex_iterator(body.begin(), body.end(), call); it != std::sregex_iterator(); ++it) {
                        std::string lbl = (*it)[2].str();
                        if (lbl.empty() || lbl.find('%') != std::string::npos) continue;
                        const size_t t = lbl.find("###");
                        if (t != std::string::npos) lbl = lbl.substr(t);
                        seen[lbl].push_back((int)i + lineOf(body, (size_t)it->position()));
                    }
                    for (const auto& [id, at] : seen) {
                        if (at.size() < 2) continue;
                        bool ok = false;
                        for (const AllowDup& al : kAllowDup) if (s.rel == al.file && id == al.id) { ok = true; allowUsed.insert(s.rel + "|" + id); }
                        CHECK(ok, "%s:%d: \"%s\" is used %zu times in one function without PushID (one ImGui ID: they act as one widget)", s.rel.c_str(), at[0], id.c_str(), at.size());
                    }
                }
                i = j;
            }
        next:;
        }
    }
    for (const AllowDup& al : kAllowDup) CHECK(allowUsed.count(std::string(al.file) + "|" + al.id), "stale allow-list entry %s \"%s\"", al.file, al.id);
}

// ---------------------------------------------------------------- 5
static void ruleModalsOpened(const std::vector<Src>& srcs) {
    const std::regex begin(R"(BeginPopupModal\(\s*((?:trw|tr)\(\s*)?"((?:\\.|[^"\\])*)\")"), open(R"(OpenPopup\(\s*((?:trw|tr)\(\s*)?"((?:\\.|[^"\\])*)\")");
    std::set<std::string> opened;
    for (const Src& s : srcs)
        for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), open); it != std::sregex_iterator(); ++it) opened.insert((*it)[1].str() + "|" + (*it)[2].str());
    int n = 0;
    for (const Src& s : srcs)
        for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), begin); it != std::sregex_iterator(); ++it) {
            n++;
            const std::string key = (*it)[1].str() + "|" + (*it)[2].str();
            CHECK(opened.count(key), "%s:%d: the modal \"%s\" is never opened with the same name (OpenPopup with the same tr / trw form)", s.rel.c_str(), lineOf(s.text, (size_t)it->position()), (*it)[2].str().c_str());
        }
    std::printf("rule 5: %d modals with a literal name\n", n);
}

// ---------------------------------------------------------------- 6
// display columns of UTF-8 text: CJK and full-width characters count two
static int columns(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        unsigned cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 7; len = 4; } else if (c >= 0xE0) { cp = c & 15; len = 3; } else if (c >= 0xC0) { cp = c & 31; len = 2; }
        for (int k = 1; k < len && i + (size_t)k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 63);
        i += (size_t)len;
        const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                          (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
    }
    return w;
}

static void ruleTranslationBudget(const fs::path& root) {
    int n = 0;
    for (const auto& e : fs::directory_iterator(root / "app" / "i18n")) {
        if (e.path().extension() != ".txt") continue;
        std::stringstream ss(readFile(e.path()));
        std::string l;
        int ln = 0;
        while (std::getline(ss, l)) {
            ln++;
            if (l.empty() || l[0] == '#') continue;
            const size_t tab = l.find('\t');
            if (tab == std::string::npos) continue;
            const std::string k = l.substr(0, tab), v = l.substr(tab + 1);
            if (k.size() > 14 || k.find("\\n") != std::string::npos) continue;
            n++;
            const int kw = columns(k), vw = columns(v);
            // a short label: at most 3.5 times its English length or 24 columns more, whichever is more; a very short one at most 18 columns
            const int budget = kw <= 4 ? 18 : std::max((int)(kw * 3.5f), kw + 24);
            CHECK(vw <= budget, "%s:%d: \"%s\" -> \"%s\" is %d columns, the budget for a %d-column label is %d (it sits in a button or a header)",
                  e.path().filename().string().c_str(), ln, k.c_str(), v.c_str(), vw, kw, budget);
        }
    }
    std::printf("rule 6: %d short translations measured\n", n);
}

// ---------------------------------------------------------------- 7
// the menu bar's right-hand items sit at the bar's vertical centre: a SmallButton lowers itself by the line's text-baseline offset (the bar's
// extra frame padding), so every placement there zeroes it (noBaseShift) right after SetCursorPosY(y); else the text touches the bar's bottom line
static void ruleMenuBarCentre(const std::vector<Src>& srcs) {
    for (const Src& s : srcs) {
        if (s.rel != "app/ui/ui2_shell.cpp") continue;
        const size_t a = s.text.find("static void menuBarRight("), b = s.text.find("void restoreDefaultView", a);
        CHECK(a != std::string::npos && b != std::string::npos, "menuBarRight not found");
        if (a == std::string::npos || b == std::string::npos) return;
        const std::string body = s.text.substr(a, b - a);
        size_t at = 0; int n = 0;
        while ((at = body.find("SetCursorPosY(y);", at)) != std::string::npos) {
            at += 17; n++;
            CHECK(body.compare(at, 15, " noBaseShift();") == 0, "ui2_shell.cpp menuBarRight: SetCursorPosY(y) not followed by noBaseShift() (text sits below the bar's centre)");
        }
        CHECK(n >= 3, "menuBarRight: expected 3 placements, found %d", n);
    }
}

// ---------------------------------------------------------------- 8
// ImGui 1.92 PushFont(font, size) takes the size before the interface scale (FontScaleDpi = gUi) and applies it itself: a size from
// GetFontSize() (already scaled) or a pixel size times gUi is scaled twice, 1.5x too large at 150 %, and outgrows its row (#502)
static void ruleFontSizeOnce(const std::vector<Src>& srcs) {
    const std::regex re(R"(PushFont\([^;,]*,[^;]*(GetFontSize\(\)|\bgUi\b))");
    for (const Src& s : srcs)
        for (auto it = std::sregex_iterator(s.text.begin(), s.text.end(), re); it != std::sregex_iterator(); ++it)
            CHECK(false, "%s:%d: PushFont size scaled twice (use fontBase() * k or a bare pixel size, ui/font_size.h): %s", s.rel.c_str(), lineOf(s.text, (size_t)it->position()), it->str().c_str());
}

static void windowFit() {
    using winfit::V2;
    const V2 disp{1280, 720};
    // reachable already: left alone
    V2 p = winfit::reachablePos({100, 100}, {400, 300}, disp, 80, 20);
    CHECK(!winfit::moved(p, {100, 100}), "a visible window moved");
    // past the right edge (a second monitor that is gone): 80 px of it stay on the display
    p = winfit::reachablePos({3000, 100}, {400, 300}, disp, 80, 20);
    CHECK(p.x == 1200 && p.y == 100, "right edge: %.0f %.0f", p.x, p.y);
    // past the left edge
    p = winfit::reachablePos({-1000, 100}, {400, 300}, disp, 80, 20);
    CHECK(p.x == 80 - 400, "left edge: %.0f", p.x);
    // title bar above the top, or below the bottom
    p = winfit::reachablePos({100, -50}, {400, 300}, disp, 80, 20);
    CHECK(p.y == 0, "top: %.0f", p.y);
    p = winfit::reachablePos({100, 2000}, {400, 300}, disp, 80, 20);
    CHECK(p.y == 700, "bottom: %.0f", p.y);
    // a window narrower than the grab width
    p = winfit::reachablePos({-500, 10}, {30, 30}, disp, 80, 20);
    CHECK(p.x == 0, "narrow window: %.0f", p.x);
    // first-use sizes
    V2 s = winfit::firstSize({980, 620}, {1366, 768}, 24);
    CHECK(s.x == 980 && s.y == 620, "fits: %.0f %.0f", s.x, s.y);
    s = winfit::firstSize({1960, 1240}, {1366, 768}, 24);   // 980 x 620 at 200 % on a laptop
    CHECK(s.x == 1366 - 48 && s.y == 768 - 48, "fitted: %.0f %.0f", s.x, s.y);
    s = winfit::firstSize({500, 500}, {10, 10}, 24);
    CHECK(s.x >= 1 && s.y >= 1, "tiny display: %.0f %.0f", s.x, s.y);
}

int main() {
    const fs::path root = DECT2_SOURCE_DIR;
    const std::vector<Src> srcs = appSources(root);
    CHECK(srcs.size() > 100, "only %zu sources found under %s/app", srcs.size(), root.string().c_str());
    ruleSetCursorAtEnd(srcs);
    ruleTrwForStateful(srcs);
    ruleScaledSizes(srcs);
    ruleDuplicateIds(srcs);
    ruleModalsOpened(srcs);
    ruleTranslationBudget(root);
    ruleMenuBarCentre(srcs);
    ruleFontSizeOnce(srcs);
    windowFit();
    if (!failures) std::printf("ok: %zu sources\n", srcs.size());
    return failures ? 1 : 0;
}
