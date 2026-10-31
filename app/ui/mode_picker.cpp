// The Mode Browser: a large window with every receiver mode as a button, grouped by family (docs/MODE-PICKER.md). The lists (combo, segmented
// bar, Mode menu) stay; this is the visual way to pick. The logic (filter, layout, keys, radio fit) is in mode_picker_logic.h and is unit-tested.
#include "../app.h"
#include "../i18n/i18n.h"
#include "dect2/engine/mode_tuning.h"
#include "mode_picker_logic.h"
#include "mode_visibility.h"
#include "scale.h"
#include <cstring>

namespace {

using namespace modepick;

struct State {
    bool want = false, open = false;
    char q[48] = "";
    int tab = TabAll, group = -1;
    uint32_t tags = 0;
    bool showHidden = false;
    int focus = 0;                 // flat index over the tiles drawn
    bool kbd = false;              // the keyboard moved the focus (show it, show the card)
    bool scrollTo = false, focusActive = false, focusSearch = false;
    float scroll = 0;
    int hoverFlat = -1;
    double hoverT = 0;
    std::vector<int> lastSizes, lastFlat;   // last frame's layout, for the keys
    int lastCols = 1;
};
State S;
bool gDevKeepQuery = false;   // the DECT2_MODEPICK test hook sets the search text before the window opens

std::string trimTo(const char* s, float maxW) {   // cut with ".." so the text fits
    if (!s) return {};
    if (ImGui::CalcTextSize(s).x <= maxW) return s;
    std::string t = s;
    while (!t.empty()) {
        do t.pop_back(); while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80);
        if (ImGui::CalcTextSize((t + "..").c_str()).x <= maxW) break;
    }
    return t + "..";
}

void drawStar(ImDrawList* dl, ImVec2 c, float r, bool on, ImU32 col) {
    ImVec2 pt[10];
    for (int i = 0; i < 10; i++) {
        const float ang = -1.5708f + i * 0.62832f, rr = (i & 1) ? r * 0.42f : r;
        pt[i] = ImVec2(c.x + cosf(ang) * rr, c.y + sinf(ang) * rr);
    }
    if (on) {
        ImVec2 mid[5];
        for (int i = 0; i < 5; i++) mid[i] = pt[i * 2 + 1];
        dl->AddConvexPolyFilled(mid, 5, col);
        for (int i = 0; i < 5; i++) dl->AddTriangleFilled(pt[i * 2], pt[(i * 2 + 9) % 10], pt[i * 2 + 1], col);
    }
    dl->AddPolyline(pt, 10, col, ImDrawFlags_Closed, 1.2f * gUi);
}

void badge(ImDrawList* dl, ImVec2& right, float y, const char* txt, ImU32 col) {   // drawn right to left
    const ImVec2 ts = ImGui::CalcTextSize(txt);
    const float pad = 3 * gUi, h = ts.y + 1 * gUi;
    const ImVec2 a(right.x - ts.x - 2 * pad, y - h), b(right.x, y);
    dl->AddRectFilled(a, b, (col & 0x00FFFFFF) | 0x38000000, 3 * gUi);
    dl->AddText(ImVec2(a.x + pad, a.y + 0.5f * gUi), col, txt);
    right.x = a.x - 3 * gUi;
}

std::vector<std::string> recentOf(const App& a) { return recentParse(a.modeRecent); }

void closeIt() { S.open = false; ImGui::CloseCurrentPopup(); }

void pick(App& a, const ModeDef& m, bool keep) {
    selectMode(a, m.family, m.subId);
    if (!keep) closeIt();
}

void toggleFav(App& a, const ModeDef& m) {
    std::set<std::string> f = modevis::parseList(a.modeFavs);
    if (!f.erase(m.name)) f.insert(m.name);
    a.modeFavs = modevis::joinList(f);
    savePrefs(a);
}

void card(const App& a, const ModeDef& m, const ModeFit& fit, float conf, bool detected) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(380 * gUi);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGroupCols[m.group]), "%s", m.name);
    ImGui::SameLine(); ImGui::TextDisabled("%s", tr(kGroupNames[m.group]));
    ImGui::TextUnformatted(m.blurb ? m.blurb : "");
    if (const dect2::ModeTuning* t = dect2::modeTuning(m.family + 2)) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s %.4g MHz   %s %.3g Msps   %s %.3g MHz", tr("Starts at"), t->defMhz, tr("Rate"), t->sampleRate / 1e6, tr("Width"), t->bandwidthMhz);
    }
    std::string needs;
    auto add = [&](bool on, const char* s) { if (on) { if (!needs.empty()) needs += ", "; needs += tr(s); } };
    add(m.needs & NDish, "a dish with an LNA"); add(m.needs & NActiveAnt, "an active antenna"); add(m.needs & NCable, "a cable feed");
    add(m.needs & NWideRadio, "a radio with a wide sample rate"); add(m.needs & NPlugin, "a plugin"); add(m.needs & NCodec, "a voice codec you install");
    if (!needs.empty()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1), "%s: %s", tr("Needs"), needs.c_str());
    if (!fit.ok) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "%s: %s. %s", tr("Your radio"), tr(fit.reason.c_str()), tr("You can still pick it."));
    if (detected) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "%s (%.0f%%)", tr("Signal detected"), conf * 100);
    const std::string cr = modeCreditsLine(m.name);
    if (!cr.empty()) ImGui::TextDisabled("%s", cr.c_str() + 2);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    (void)a;
}

struct Sec { std::string title; ImU32 col; std::vector<int> idx; };

}  // namespace

void modeRecordRecent(App& a, int family, const char* subId) {
    for (const auto& m : allModes()) {
        if (m.family != family) continue;
        if (subId ? (m.subId && !strcmp(m.subId, subId)) : (m.subId == nullptr)) {
            std::vector<std::string> r = recentOf(a);
            recentPush(r, m.name);
            a.modeRecent = recentJoin(r);
            return;
        }
    }
}

void modePickerOpen(App&) { S.want = true; }
bool modePickerWantsKeys() { return S.open; }

void modePickerButton(App& a, const char* id) {
    const std::string lbl = std::string(tr("Modes")) + id;
    if (ImGui::Button(lbl.c_str())) modePickerOpen(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Browse all modes as buttons (M)"));
}

void modePicker(App& a) {
    ImGuiIO& io = ImGui::GetIO();
    modeInfoFill();
    if (!S.open && !S.want && ImGui::IsKeyPressed(ImGuiKey_M, false) && !io.WantTextInput && !io.KeyCtrl && !io.KeySuper && !io.KeyAlt && !io.KeyShift
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        S.want = true;
    {   // dev/test only: DECT2_MODEPICK="tab=3,group=2,q=adsb,hidden=1,tags=2" opens the window on frame 3 of a hidden --shot run
        static const char* env = getenv("DECT2_MODEPICK");
        static int frame = 0;
        if (env && ++frame == 3) {
            std::string e = env;
            auto val = [&](const char* k) { size_t p = e.find(std::string(k) + "="); if (p == std::string::npos) return std::string(); p += strlen(k) + 1; size_t q = e.find(',', p); return e.substr(p, q == std::string::npos ? q : q - p); };
            S.want = true;
            S.tab = val("tab").empty() ? 0 : atoi(val("tab").c_str());
            S.group = val("group").empty() ? -1 : atoi(val("group").c_str());
            S.tags = (uint32_t)atoi(val("tags").c_str());
            S.showHidden = val("hidden") == "1";
            snprintf(S.q, sizeof S.q, "%s", val("q").c_str());
            gDevKeepQuery = true;
        }
    }
    static const char* kTitle = "###modepicker";
    const std::string title = std::string(tr("Choose a mode")) + kTitle;
    if (S.want) {
        S.want = false;
        ImGui::OpenPopup(kTitle);
        S.open = true; S.focusSearch = true; S.focusActive = true; S.kbd = false;
        if (!gDevKeepQuery) S.q[0] = 0;
        gDevKeepQuery = false;
    }
    if (!S.open && !ImGui::IsPopupOpen(kTitle)) return;

    const ImVec2 disp = io.DisplaySize;
    const bool full = a.touchUi || disp.x < 700 * gUi;
    ImVec2 sz(std::min(disp.x, std::max(disp.x * 0.8f, 640 * gUi)), std::min(disp.y, std::max(disp.y * 0.8f, 420 * gUi)));
    if (full) sz = disp;
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(full ? ImVec2(0, 0) : ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, full ? ImVec2(0, 0) : ImVec2(0.5f, 0.5f));
    bool openFlag = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &openFlag, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) { S.open = false; return; }
    if (!openFlag) { closeIt(); ImGui::EndPopup(); return; }
    {   // a click outside the window closes it
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        if (!full && ImGui::IsMouseClicked(0) && !ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false)) { closeIt(); ImGui::EndPopup(); return; }
    }
    S.open = true;

    // ---- items: one per mode-table row, same order ----
    static std::vector<Item> items;
    static std::vector<std::string> baseText;
    static int baseN = 0;
    if (baseN != kNumModes) {
        baseText.assign((size_t)kNumModes, std::string());
        for (int i = 0; i < kNumModes; i++) {
            const ModeDef& m = kModes[i];
            auto z = [](const char* t) { return std::string(t ? t : ""); };
            baseText[i] = lower(z(m.name) + " " + z(m.sub) + " " + (m.sub ? z(tr(m.sub)) : std::string()) + " " + z(m.blurb) + " " + z(m.keys) + " " + z(kGroupNames[m.group]) + " " + squash(z(m.name) + z(m.sub)));   // "adsb" finds "ADS-B"
        }
        baseN = kNumModes;
    }
    const std::set<std::string> favs = modevis::parseList(a.modeFavs);
    items.resize((size_t)kNumModes);
    for (int i = 0; i < kNumModes; i++) {
        const ModeDef& m = kModes[i];
        Item& it = items[i];
        it.name = m.name; it.groupIdx = m.group; it.tags = m.tags; it.text = baseText[i];
        it.active = modeSelected(a, m); it.hidden = modeHiddenNow(a, m); it.fav = favs.count(m.name) > 0; it.fits = modeFit(a, m).ok;
    }
    const std::vector<std::string> recent = recentOf(a);
    Query q;
    q.text = S.q; q.tab = S.tab; q.tags = S.tags; q.showHidden = S.showHidden; q.group = S.group; q.recent = &recent;

    // ---- header: search, show hidden ----
    ImGui::AlignTextToFramePadding();
    ImGui::SetNextItemWidth(std::max(160.f * gUi, ImGui::GetContentRegionAvail().x - 150 * gUi));
    if (S.focusSearch) { ImGui::SetKeyboardFocusHere(); S.focusSearch = false; }
    ImGui::InputTextWithHint("##mpq", tr("search: FT8, LoRa, APRS, 1090..."), S.q, sizeof S.q, ImGuiInputTextFlags_EscapeClearsAll);
    const bool searchActive = ImGui::IsItemActive();
    ImGui::SameLine(0, 12 * gUi);
    ImGui::Checkbox(tr("Show hidden"), &S.showHidden);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("List the modes you hid in Modes shown (a search finds them anyway)"));

    // ---- tabs ----
    Query qn = q; qn.tab = TabAll; qn.tags = 0;   // counts for the tab labels: everything but the tab and the chips
    int nAll = 0, nFav = 0, nRecent = 0, nFit = 0;
    for (const Item& it : items) if (passes(it, qn)) { nAll++; nFav += it.fav; nRecent += inTab(it, TabRecent, q); nFit += it.fits; }
    const int counts[kNumTabs] = {nAll, nFav, nRecent, nFit};
    const char* const tabNames[kNumTabs] = {N_("All"), N_("Favourites"), N_("Recent"), N_("Compatible with my radio")};
    for (int t = 0; t < kNumTabs; t++) {
        if (t) ImGui::SameLine(0, 4 * gUi);
        const bool on = S.tab == t;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char lbl[96]; snprintf(lbl, sizeof lbl, "%s (%d)###mptab%d", tr(tabNames[t]), counts[t], t);
        if (ImGui::Button(lbl)) S.tab = t;
        if (on) ImGui::PopStyleColor();
    }

    // ---- tag chips ----
    {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 20 * gUi;
        for (int t = 0; t < kNumTags; t++) {
            const char* nm = tr(tagName(t));
            const float w = ImGui::CalcTextSize(nm).x + 16 * gUi;
            if (t && ImGui::GetItemRectMax().x + 4 * gUi + w < right) ImGui::SameLine(0, 4 * gUi);   // else the chip starts a new row
            const bool on = (S.tags >> t) & 1;
            const bool dim = !on && countWithTag(items, q, t) == 0;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, dim ? 0.4f : 1.f);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10 * gUi);
            char lbl[64]; snprintf(lbl, sizeof lbl, "%s##chip%d", nm, t);
            if (ImGui::SmallButton(lbl)) S.tags ^= (1u << t);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
        }
    }
    q.tags = S.tags;

    // ---- body ----
    const float footH = 2 * ImGui::GetTextLineHeightWithSpacing() + 6 * gUi;
    const float bodyH = std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - footH);
    const bool rail = !full;
    std::vector<int> groupCount(kNumGroups, 0);
    {
        Query qg = q; qg.group = -1;
        for (int i = 0; i < kNumModes; i++) if (passes(items[i], qg) && inTab(items[i], q.tab, q)) groupCount[items[i].groupIdx]++;
    }
    int total = 0;
    for (int c : groupCount) total += c;
    if (rail) {
        ImGui::BeginChild("##mprail", ImVec2(250 * gUi, bodyH), ImGuiChildFlags_Borders);
        char lbl[96];
        snprintf(lbl, sizeof lbl, "%s (%d)##railall", tr("All modes"), total);
        if (ImGui::Selectable(lbl, S.group < 0)) { S.group = -1; S.scroll = 0; }
        for (int g = 0; g < kNumGroups; g++) {
            if (!groupCount[g] && S.group != g) continue;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            snprintf(lbl, sizeof lbl, "   %s (%d)##rail%d", tr(kGroupNames[g]), groupCount[g], g);
            if (ImGui::Selectable(lbl, S.group == g)) { S.group = S.group == g ? -1 : g; S.scroll = 0; }
            ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 3 * gUi, p.y + ImGui::GetTextLineHeight()), kGroupCols[g]);
        }
        ImGui::EndChild();
        ImGui::SameLine(0, 6 * gUi);
    } else {   // narrow / touch: the families as a strip of chips above the grid
        ImGui::BeginChild("##mpstrip", ImVec2(0, 34 * gUi), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        char lbl[96];
        snprintf(lbl, sizeof lbl, "%s (%d)##stripall", tr("All"), total);
        if (ImGui::Selectable(lbl, S.group < 0, 0, ImVec2(ImGui::CalcTextSize(lbl).x, 0))) S.group = -1;
        for (int g = 0; g < kNumGroups; g++) {
            if (!groupCount[g] && S.group != g) continue;
            ImGui::SameLine(0, 10 * gUi);
            snprintf(lbl, sizeof lbl, "%s (%d)##strip%d", tr(kGroupNames[g]), groupCount[g], g);
            if (ImGui::Selectable(lbl, S.group == g, 0, ImVec2(ImGui::CalcTextSize(lbl).x, 0))) S.group = S.group == g ? -1 : g;
        }
        ImGui::EndChild();
    }

    // the sections of the grid
    q.group = S.group;
    std::vector<int> vis = filter(items, q);
    if (q.tab != TabRecent) std::stable_sort(vis.begin(), vis.end(), [&](int x, int y) { return items[x].groupIdx < items[y].groupIdx; });
    std::vector<Sec> secs;
    std::vector<float> conf((size_t)kNumModes, 0.f);
    std::vector<char> detected((size_t)kNumModes, 0);
    if (gModeSignalHint) {
        Sec d{tr("Detected"), IM_COL32(110, 220, 130, 255), {}};
        for (int i : vis) { float c = 0; if (gModeSignalHint(kModes[i], &c)) { detected[i] = 1; conf[i] = c; d.idx.push_back(i); } }
        if (!d.idx.empty()) secs.push_back(d);
    }
    if (q.tab == TabRecent) {
        Sec r{tr("Recent"), IM_COL32(150, 162, 178, 255), {}};
        for (int i : vis) if (!detected[i]) r.idx.push_back(i);
        if (!r.idx.empty()) secs.push_back(r);
    } else {
        for (int i : vis) {
            if (detected[i]) continue;
            if (secs.empty() || secs.back().title != tr(kGroupNames[items[i].groupIdx]) || secs.back().col != kGroupCols[items[i].groupIdx])
                secs.push_back({tr(kGroupNames[items[i].groupIdx]), kGroupCols[items[i].groupIdx], {}});
            secs.back().idx.push_back(i);
        }
    }
    std::vector<int> flat, sizes;
    for (const Sec& s : secs) { sizes.push_back((int)s.idx.size()); flat.insert(flat.end(), s.idx.begin(), s.idx.end()); }

    // ---- keys (the layout of last frame) ----
    if (S.focusActive) {
        for (int k = 0; k < (int)flat.size(); k++) if (items[flat[k]].active) { S.focus = k; S.scrollTo = true; break; }
        S.focusActive = false;
    }
    S.focus = flat.empty() ? 0 : std::clamp(S.focus, 0, (int)flat.size() - 1);
    {
        auto go = [&](Move m) { S.focus = moveFocus(sizes, S.lastCols, S.focus, m); S.kbd = true; S.scrollTo = true; };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) go(Down);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) go(Up);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) go(PageDown);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) go(PageUp);
        if (!searchActive) {
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) go(Right);
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) go(Left);
            if (ImGui::IsKeyPressed(ImGuiKey_Home)) go(Home);
            if (ImGui::IsKeyPressed(ImGuiKey_End)) go(End);
            if (ImGui::IsKeyPressed(ImGuiKey_Slash, false)) S.focusSearch = true;
            else if (S.kbd && !flat.empty() && ImGui::IsKeyPressed(ImGuiKey_F, false)) toggleFav(a, kModes[flat[S.focus]]);
            else if (io.InputQueueCharacters.Size > 0 && !io.KeyCtrl && !io.KeySuper) {   // typing starts a search
                for (ImWchar c : io.InputQueueCharacters) if (c > 32 && c < 127 && strlen(S.q) + 1 < sizeof S.q) { size_t n = strlen(S.q); S.q[n] = (char)c; S.q[n + 1] = 0; }
                S.focusSearch = true; S.kbd = false;
            }
        }
        if (!flat.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            pick(a, kModes[flat[S.focus]], io.KeyShift || io.KeyCtrl);
            if (!S.open) { ImGui::EndPopup(); return; }
        }
    }

    // ---- the grid ----
    ImGui::BeginChild("##mpgrid", ImVec2(0, bodyH), ImGuiChildFlags_Borders);
    if (S.scroll > 0 && ImGui::GetScrollY() == 0 && ImGui::IsWindowAppearing()) ImGui::SetScrollY(S.scroll);
    const bool touch = a.touchUi;
    const float gap = (touch ? 12.f : 8.f) * gUi;
    const float tileH = (touch ? 92.f : 74.f) * gUi;   // the name and two lines of subtitle
    const Cols cols = columns(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize, gap, 150 * gUi, 220 * gUi);
    S.lastCols = cols.n;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    int flatI = 0, hoveredFlat = -1;
    for (const Sec& s : secs) {
        ImGui::Spacing();
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(hp, ImVec2(hp.x + 4 * gUi, hp.y + ImGui::GetTextLineHeight()), s.col);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10 * gUi);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(s.col), "%s  (%d)", s.title.c_str(), (int)s.idx.size());
        for (int k = 0; k < (int)s.idx.size(); k++, flatI++) {
            const int mi = s.idx[k];
            const ModeDef& m = kModes[mi];
            const Item& it = items[mi];
            if (k % cols.n) ImGui::SameLine(0, gap);
            ImGui::PushID(mi);
            const ImVec2 p = ImGui::GetCursorScreenPos(), size(cols.tileW, tileH);
            const bool clicked = ImGui::InvisibleButton("##t", size);
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 mp = io.MousePos;
            const float sr = 8 * gUi;
            const ImVec2 starC(p.x + size.x - 12 * gUi, p.y + 12 * gUi);
            const bool onStar = hov && fabsf(mp.x - starC.x) < sr + 3 * gUi && fabsf(mp.y - starC.y) < sr + 3 * gUi;
            const bool kfocus = S.kbd && flatI == S.focus;
            const ModeFit fit = modeFit(a, m);
            const float alpha = fit.ok ? 1.f : 0.55f;
            ImVec4 bgc = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            if (hov || kfocus) { bgc.x += 0.07f; bgc.y += 0.07f; bgc.z += 0.07f; }
            dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::ColorConvertFloat4ToU32(bgc), 5 * gUi);
            const ImU32 gc = kGroupCols[m.group];
            dl->AddRectFilled(p, ImVec2(p.x + 4 * gUi, p.y + size.y), (gc & 0x00FFFFFF) | ((ImU32)(alpha * 255) << 24), 3 * gUi, ImDrawFlags_RoundCornersLeft);
            if (it.active) dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::ColorConvertFloat4ToU32(m.accent), 5 * gUi, 0, 2 * gUi);
            else if (kfocus) dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(230, 230, 240, 220), 5 * gUi, 0, 1.5f * gUi);
            else if (hov) dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), (gc & 0x00FFFFFF) | 0xA0000000, 5 * gUi);
            const float tx = p.x + 12 * gUi, tw = size.x - 12 * gUi - 24 * gUi;
            ImVec4 tc = ImGui::GetStyleColorVec4(ImGuiCol_Text); tc.w *= alpha;
            ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); dc.w *= alpha;
            dl->PushClipRect(p, ImVec2(p.x + size.x, p.y + size.y), true);
            const std::string nm = trimTo(m.name, tw - (it.active ? 16 * gUi : 0));
            dl->AddText(ImVec2(tx, p.y + 6 * gUi), ImGui::ColorConvertFloat4ToU32(tc), nm.c_str());
            const float nw = ImGui::CalcTextSize(nm.c_str()).x;
            if (it.active) {   // a check mark after the name
                const ImVec2 c(tx + nw + 8 * gUi, p.y + 6 * gUi + ImGui::GetTextLineHeight() * 0.55f);
                const ImVec2 ck[3] = {ImVec2(c.x - 4 * gUi, c.y), ImVec2(c.x - 1 * gUi, c.y + 3 * gUi), ImVec2(c.x + 4 * gUi, c.y - 4 * gUi)};
                dl->AddPolyline(ck, 3, ImGui::ColorConvertFloat4ToU32(m.accent), 0, 2 * gUi);
            }
            if (detected[mi]) dl->AddCircleFilled(ImVec2(tx + nw + (it.active ? 24 : 8) * gUi, p.y + 6 * gUi + ImGui::GetTextLineHeight() * 0.5f), 3.5f * gUi, IM_COL32(90, 220, 120, 255));
            const float ly = p.y + 6 * gUi + ImGui::GetTextLineHeightWithSpacing();
            if (!fit.ok) {   // a padlock and the reason
                const ImU32 lc = ImGui::ColorConvertFloat4ToU32(dc);
                dl->AddRectFilled(ImVec2(tx, ly + 5 * gUi), ImVec2(tx + 8 * gUi, ly + 11 * gUi), lc, 1.5f * gUi);
                dl->AddRect(ImVec2(tx + 1.5f * gUi, ly + 1 * gUi), ImVec2(tx + 6.5f * gUi, ly + 6 * gUi), lc, 2 * gUi, 0, 1.2f * gUi);
                dl->PushClipRect(ImVec2(tx + 12 * gUi, ly), ImVec2(tx + tw, ly + 2 * ImGui::GetTextLineHeight() + 1 * gUi), true);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx + 12 * gUi, ly), ImGui::ColorConvertFloat4ToU32(dc), tr(fit.reason.c_str()), nullptr, tw - 12 * gUi);
                dl->PopClipRect();
            } else if (m.sub && *m.sub) {   // the subtitle wraps to two lines (clipped), narrower when badges sit at the bottom right
                const bool badges = it.hidden || (m.needs & NPlugin) || m.isNew || detected[mi] || modeCodecMissing(m);
                const float ww = tw - (badges ? 44 * gUi : 0);
                dl->PushClipRect(ImVec2(tx, ly), ImVec2(tx + ww, ly + 2 * ImGui::GetTextLineHeight() + 1 * gUi), true);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ly), ImGui::ColorConvertFloat4ToU32(dc), tr(m.sub), nullptr, ww);
                dl->PopClipRect();
            }
            ImVec2 br(p.x + size.x - 6 * gUi, p.y + size.y - 5 * gUi);
            if (it.hidden || (q.showHidden && modeHiddenNow(a, m))) badge(dl, br, br.y, tr("hidden"), IM_COL32(170, 170, 180, 255));
            if (m.needs & NPlugin) badge(dl, br, br.y, "PLUGIN", IM_COL32(150, 170, 240, 255));
            if (modeCodecMissing(m)) badge(dl, br, br.y, "CODEC", IM_COL32(240, 170, 90, 255));
            if (m.isNew) badge(dl, br, br.y, "NEW", IM_COL32(110, 220, 130, 255));
            if (detected[mi]) badge(dl, br, br.y, tr("Signal detected"), IM_COL32(110, 220, 130, 255));
            drawStar(dl, starC, sr, it.fav, onStar ? IM_COL32(255, 220, 90, 255) : it.fav ? IM_COL32(240, 200, 70, 255) : IM_COL32(150, 150, 160, hov ? 200 : 110));
            dl->PopClipRect();
            if (S.scrollTo && flatI == S.focus) { ImGui::SetScrollHereY(0.5f); S.scrollTo = false; }
            if (hov) hoveredFlat = flatI;
            if (clicked) {
                if (onStar) toggleFav(a, m);
                else { pick(a, m, io.KeyShift || io.KeyCtrl); }
            }
            const bool showCard = hov ? (hoveredFlat == flatI && S.hoverFlat == flatI && ImGui::GetTime() - S.hoverT > 0.35) : (kfocus && ImGui::GetTime() - S.hoverT > 0.35);
            if (showCard && S.open) card(a, m, fit, conf[mi], detected[mi]);
            ImGui::PopID();
            if (!S.open) break;
        }
        if (!S.open) break;
    }
    ImGui::PopStyleVar();
    if (hoveredFlat != S.hoverFlat) { S.hoverFlat = hoveredFlat; S.hoverT = ImGui::GetTime(); }
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) S.kbd = false;
    if (flat.empty()) {
        const char* msg = q.tab == TabFav ? tr("No favourites yet. Click the star on a mode (or press F on the highlighted one).")
                        : q.tab == TabRecent ? tr("Modes you pick appear here.")
                        : tr("No mode matches. Clear the search or the filters.");
        ImGui::Spacing(); ImGui::TextDisabled("%s", msg);
    }
    if (S.open) S.scroll = ImGui::GetScrollY();
    S.lastSizes = sizes; S.lastFlat = flat;
    ImGui::EndChild();

    // ---- footer ----
    ImGui::Spacing();
    ImGui::TextDisabled("%s", tr("Click = pick, Shift-click = keep open, arrows move, Enter picks, F = favourite, Esc closes"));
    const int shownFlat = hoveredFlat >= 0 ? hoveredFlat : (S.kbd ? S.focus : -1);
    if (shownFlat >= 0 && shownFlat < (int)flat.size()) {
        const ModeDef& m = kModes[flat[shownFlat]];
        const std::string line = std::string(m.name) + ": " + (m.tip ? m.tip : "");
        ImGui::TextUnformatted(trimTo(line.c_str(), ImGui::GetContentRegionAvail().x).c_str());
    } else ImGui::TextDisabled("%d %s", (int)flat.size(), tr("modes shown"));
    ImGui::EndPopup();
}
