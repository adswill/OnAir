// Developer build only (cmake -DDECT2_UI_LAYOUT_CHECK=ON; run by tools/ui_layout_check.sh): finds controls and text that overlap each other,
// run past the edge of their window or are squashed narrower than their label. Dear ImGui's test-engine hooks report every item with its
// rectangle, and the copy of imgui_draw.cpp that CMake makes for this build reports every piece of text drawn. DECT2_LAYOUT_AT seconds after
// the start (default 3, at the latest at frame 390) one frame is checked and the findings are written to DECT2_LAYOUT_REPORT; DECT2_LAYOUT_EXIT=1 then ends the program.
#ifdef DECT2_UI_LAYOUT_CHECK
#include "app.h"
#include "imgui_internal.h"
#include <cstdarg>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Item { ImGuiWindow* win; ImGuiID id; ImRect rect, nav, clip; ImGuiItemFlags flags; bool menu; };
struct Text { ImGuiWindow* win; ImRect ink, clip; bool fine; int owner; std::string s; };
std::vector<Item> gItems;
std::vector<Text> gTexts;
std::unordered_map<ImGuiID, std::string> gLabel;           // from ItemInfo; kept across frames for FindItemDebugLabel
std::unordered_map<ImGuiID, ImGuiItemStatusFlags> gInfo;
std::unordered_map<ImGuiWindow*, int> gLast;                // the last item of each window: the control the text drawn after it belongs to
bool gOn = false, gDone = false;

// Intentional overlaps and clipping: canvases that draw their own text (plots, the waterfall, maps, the picture) and controls that scroll
// their text. Matched as substrings: the window path, then either side of the finding ("" matches anything).
struct Allow { const char* kind; const char* win; const char* what; };
const Allow kAllow[] = {
    // the maps and the sky plot: the labels of aircraft, ships, satellites and nodes sit where their positions are, over each other, under
    // the zoom buttons and past the edge of the map (any finding that involves text in these panes)
    {"", "##adsb_l", "\""}, {"", "##ais_l", "\""}, {"", "##iri_l", "\""}, {"", "##gnss_l", "\""}, {"", "##mesh_l", "\""}, {"", "##sonde_l", "\""},
};

bool allowed(const char* kind, const std::string& win, const std::string& a, const std::string& b) {
    for (const Allow& al : kAllow) {
        if (al.kind[0] && strcmp(al.kind, kind) != 0) continue;
        if (al.win[0] && win.find(al.win) == std::string::npos) continue;
        if (al.what[0] && a.find(al.what) == std::string::npos && b.find(al.what) == std::string::npos) continue;
        return true;
    }
    return false;
}

// "##root/##content_1A2B3C4D/##main2_5E6F7A8B" -> "##root/##content/##main2"
std::string winName(const ImGuiWindow* w) {
    std::string s = w->Name, out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '_' && i + 9 <= s.size() && (i + 9 == s.size() || s[i + 9] == '/')) {
            bool hex = true;
            for (size_t k = 1; k <= 8; k++) hex = hex && isxdigit((unsigned char)s[i + k]);
            if (hex) { i += 9; continue; }
        }
        out += s[i++];
    }
    return out;
}

std::string visible(const std::string& label) { return std::string(label.c_str(), ImGui::FindRenderedTextEnd(label.c_str())); }

std::string itemName(const Item& it) {
    for (ImGuiWindow* w : GImGui->Windows)
        if (w->ParentWindow == it.win && w->ChildId == it.id && (w->Flags & ImGuiWindowFlags_ChildWindow)) {
            std::string n = winName(w);
            return "[pane " + n.substr(n.rfind('/') + 1) + "]";
        }
    auto l = gLabel.find(it.id);
    if (l != gLabel.end()) return "[" + l->second + "]";
    char b[24]; snprintf(b, sizeof b, "[item %08X", it.id);   // no label: name the labelled item before it, to find it in the code
    for (const Item* p = &it; p > gItems.data();) {
        --p;
        if (p->win != it.win) continue;
        auto pl = gLabel.find(p->id);
        if (pl != gLabel.end()) return std::string(b) + " after " + pl->second + "]";
    }
    return std::string(b) + "]";
}

// numbers vary from run to run: the key of a finding has them replaced by '#'
std::string keyOf(std::string s) { for (char& c : s) if (c >= '0' && c <= '9') c = '#'; return s; }

bool contains(const ImRect& outer, const ImRect& in, float tol = 1.f) {
    return in.Min.x >= outer.Min.x - tol && in.Min.y >= outer.Min.y - tol && in.Max.x <= outer.Max.x + tol && in.Max.y <= outer.Max.y + tol;
}
ImRect meet(const ImRect& a, const ImRect& b) { return ImRect(ImMax(a.Min, b.Min), ImMin(a.Max, b.Max)); }
bool degenerate(const ImRect& r) { return r.GetWidth() < 1.f || r.GetHeight() < 1.f; }
// the window's own clipping rectangle, before a parent clips it (a parent that scrolls hides part of a child without cutting it off)
ImRect ownClip(const ImGuiWindow* w) {
    ImRect r = w->InnerRect;
    r.Min.x = ImFloor(0.5f + r.Min.x + w->WindowBorderSize * 0.5f); r.Max.x = ImFloor(r.Max.x - w->WindowBorderSize * 0.5f);
    r.Min.y = ImFloor(0.5f + r.Min.y + w->WindowBorderSize * 0.5f); r.Max.y = ImFloor(r.Max.y - w->WindowBorderSize * 0.5f);
    return r;
}
// a side of c that is the window's edge as clipped by a parent goes back to the window's own edge
ImRect unclip(const ImRect& c, const ImGuiWindow* w) {
    const ImRect own = ownClip(w), ic = w->InnerClipRect;
    ImRect r = c;
    if (std::fabs(c.Min.x - ic.Min.x) < 0.5f && ic.Min.x > own.Min.x + 0.5f) r.Min.x = own.Min.x;
    if (std::fabs(c.Min.y - ic.Min.y) < 0.5f && ic.Min.y > own.Min.y + 0.5f) r.Min.y = own.Min.y;
    if (std::fabs(c.Max.x - ic.Max.x) < 0.5f && ic.Max.x < own.Max.x - 0.5f) r.Max.x = own.Max.x;
    if (std::fabs(c.Max.y - ic.Max.y) < 0.5f && ic.Max.y < own.Max.y - 0.5f) r.Max.y = own.Max.y;
    return r;
}
bool skipWindow(const ImGuiWindow* w) { return (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) != 0 || w->LastFrameActive != GImGui->FrameCount || w->Hidden; }

struct Finding { std::string kind, win, a, b, detail; };

void check(std::vector<Finding>& out) {
    ImGuiContext& g = *GImGui;
    const float fs = g.FontSize;
    auto add = [&](const char* kind, const ImGuiWindow* w, const std::string& a, const std::string& b, const char* fmt, ...) {
        const std::string wn = winName(w);
        if (allowed(kind, wn, a, b)) return;
        char d[256];
        va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
        out.push_back({kind, wn, a, b, d});
    };
    // 1. windows whose contents do not fit and cannot be scrolled
    for (ImGuiWindow* w : g.Windows) {
        const ImRect own = ownClip(w);
        if (skipWindow(w) || degenerate(own)) continue;
        const float oy = w->DC.CursorMaxPos.y - own.Max.y, ox = w->DC.CursorMaxPos.x - own.Max.x;
        if (!w->ScrollbarY && oy > 1.f) add("CUT", w, "[window contents]", "", "%.0f px taller than the window (%.0f px), no scroll bar", oy, own.GetHeight());
        if (!w->ScrollbarX && ox > 1.f) add("CUT", w, "[window contents]", "", "%.0f px wider than the window (%.0f px), no scroll bar", ox, own.GetWidth());
    }
    // 2. items: cut off, squashed, overlapping each other
    std::unordered_map<ImGuiWindow*, std::vector<int>> byWin;
    for (int i = 0; i < (int)gItems.size(); i++) if (!skipWindow(gItems[i].win) && !degenerate(gItems[i].rect)) byWin[gItems[i].win].push_back(i);
    for (auto& [w, idx] : byWin) {
        for (int i : idx) {
            const Item& it = gItems[i];
            const ImRect& r = it.rect;
            const ImRect clip = it.menu ? it.clip : meet(unclip(it.clip, w), ownClip(w));   // the menu bar is outside the inner rectangle
            if (!w->ScrollbarX && (r.Max.x > clip.Max.x + 1.f || r.Min.x < clip.Min.x - 1.f))
                add("CUT", w, itemName(it), "", "runs %.0f px past the %s edge: (%.0f,%.0f)-(%.0f,%.0f)", r.Max.x > clip.Max.x + 1.f ? r.Max.x - clip.Max.x : clip.Min.x - r.Min.x, r.Max.x > clip.Max.x + 1.f ? "right" : "left", r.Min.x, r.Min.y, r.Max.x, r.Max.y);
            if (!w->ScrollbarY && (r.Max.y > clip.Max.y + 1.f || r.Min.y < clip.Min.y - 1.f))
                add("CUT", w, itemName(it), "", "runs %.0f px past the %s edge: (%.0f,%.0f)-(%.0f,%.0f)", r.Max.y > clip.Max.y + 1.f ? r.Max.y - clip.Max.y : clip.Min.y - r.Min.y, r.Max.y > clip.Max.y + 1.f ? "bottom" : "top", r.Min.x, r.Min.y, r.Max.x, r.Max.y);
            auto l = gLabel.find(it.id);
            if (l != gLabel.end()) {
                const std::string v = visible(l->second);
                const float tw = v.empty() ? 0.f : ImGui::CalcTextSize(v.c_str()).x;
                if (tw > 0 && r.GetWidth() + 1.f < tw) add("SQUASH", w, itemName(it), "", "%.0f px wide, the label needs %.0f px", r.GetWidth(), tw);
            }
            if (l != gLabel.end() && l->second == "##plot" && (r.GetHeight() < fs * 3.f || r.GetWidth() < fs * 3.f))
                add("SQUASH", w, itemName(it), "", "the plot area is only %.0f x %.0f px", r.GetWidth(), r.GetHeight());
            const bool frame = it.nav.GetWidth() != r.GetWidth() || (gInfo.count(it.id) && (gInfo[it.id] & ImGuiItemStatusFlags_Inputable));
            if (frame && it.nav.GetWidth() < fs * 1.5f) add("SQUASH", w, itemName(it), "", "the control is only %.0f px wide", it.nav.GetWidth());
        }
        for (size_t x = 0; x < idx.size(); x++) for (size_t y = x + 1; y < idx.size(); y++) {
            const Item& p = gItems[idx[x]];
            const Item& q = gItems[idx[y]];
            if ((p.flags | q.flags) & ImGuiItemFlags_AllowOverlap) continue;
            const ImRect m = meet(p.rect, q.rect);
            // a thin strip is the spacing a Selectable claims around itself (the rows of a list): only a real overlap counts
            if (m.GetWidth() < 2.f || m.GetHeight() < std::max(2.f, 0.35f * std::min(p.rect.GetHeight(), q.rect.GetHeight())) || contains(p.rect, q.rect) || contains(q.rect, p.rect)) continue;
            add("OVERLAP", w, itemName(p), itemName(q), "%.0f x %.0f px: (%.0f,%.0f)-(%.0f,%.0f) and (%.0f,%.0f)-(%.0f,%.0f)", m.GetWidth(), m.GetHeight(),
                p.rect.Min.x, p.rect.Min.y, p.rect.Max.x, p.rect.Max.y, q.rect.Min.x, q.rect.Min.y, q.rect.Max.x, q.rect.Max.y);
        }
    }
    // 3. text: cut off by its window, cut to fit its control, on top of other text or controls
    std::unordered_map<ImGuiWindow*, std::vector<int>> textWin;
    for (int i = 0; i < (int)gTexts.size(); i++) if (!skipWindow(gTexts[i].win)) textWin[gTexts[i].win].push_back(i);
    for (auto& [w, idx] : textWin) {
        for (int i : idx) {
            Text t = gTexts[i];
            t.clip = unclip(t.clip, w);
            const std::string name = "\"" + t.s + "\"";
            const float ovR = t.ink.Max.x - t.clip.Max.x, ovL = t.clip.Min.x - t.ink.Min.x, ovB = t.ink.Max.y - t.clip.Max.y;
            const bool vis = t.ink.Max.y > t.clip.Min.y && t.ink.Min.y < t.clip.Max.y && t.ink.Max.x > t.clip.Min.x && t.ink.Min.x < t.clip.Max.x;
            if (t.fine && (ovR > 1.f || ovL > 1.f || ovB > 1.f) && vis) add("SQUASH", w, name, t.owner >= 0 ? itemName(gItems[t.owner]) : "", "the text is cut to fit its control (%.0f px short)", std::max(std::max(ovR, ovL), ovB));
            else if (!t.fine && !w->ScrollbarX && (ovR > 1.f || ovL > 1.f) && t.ink.Max.y > t.clip.Min.y && t.ink.Min.y < t.clip.Max.y) add("CUT", w, name, "", "runs %.0f px past the %s edge", ovR > 1.f ? ovR : ovL, ovR > 1.f ? "right" : "left");
            else if (!t.fine && !w->ScrollbarY && ovB > 1.f && t.ink.Max.x > t.clip.Min.x && t.ink.Min.x < t.clip.Max.x) add("CUT", w, name, "", "runs %.0f px past the bottom edge", ovB);
        }
        for (size_t x = 0; x < idx.size(); x++) {
            const Text& t = gTexts[idx[x]];
            const ImRect tv = meet(t.ink, t.clip);
            if (degenerate(tv)) continue;
            const std::string name = "\"" + t.s + "\"";
            auto items = byWin.find(w);
            if (items != byWin.end()) for (int k : items->second) {
                const Item& it = gItems[k];
                const ImRect m = meet(tv, it.rect);
                if (m.GetWidth() < 2.f || m.GetHeight() < std::max(2.f, tv.GetHeight() * 0.3f) || contains(it.rect, tv)) continue;
                if (k == t.owner) add("SQUASH", w, name, itemName(it), "the text is %.0f px wider than its control", tv.GetWidth() - it.rect.GetWidth());
                else add("OVERLAP", w, name, itemName(it), "%.0f x %.0f px: (%.0f,%.0f)-(%.0f,%.0f) and (%.0f,%.0f)-(%.0f,%.0f)", m.GetWidth(), m.GetHeight(),
                         tv.Min.x, tv.Min.y, tv.Max.x, tv.Max.y, it.rect.Min.x, it.rect.Min.y, it.rect.Max.x, it.rect.Max.y);
            }
            for (size_t y = x + 1; y < idx.size(); y++) {
                const Text& u = gTexts[idx[y]];
                const ImRect uv = meet(u.ink, u.clip);
                if (degenerate(uv)) continue;
                const ImRect m = meet(tv, uv);
                if (m.GetWidth() < 2.f || m.GetHeight() < std::max(2.f, std::min(tv.GetHeight(), uv.GetHeight()) * 0.3f)) continue;
                if (t.s == u.s && std::fabs(t.ink.Min.x - u.ink.Min.x) <= 2.f && std::fabs(t.ink.Min.y - u.ink.Min.y) <= 2.f) continue;   // a shadow or bold copy
                add("OVERLAP", w, name, "\"" + u.s + "\"", "%.0f x %.0f px: (%.0f,%.0f)-(%.0f,%.0f) and (%.0f,%.0f)-(%.0f,%.0f)", m.GetWidth(), m.GetHeight(),
                    tv.Min.x, tv.Min.y, tv.Max.x, tv.Max.y, uv.Min.x, uv.Min.y, uv.Max.x, uv.Max.y);
            }
        }
    }
    // 4. tabs: shrunk below their label, or the bar is too narrow and scrolls
    for (int n = 0; n < g.TabBars.GetMapSize(); n++) {
        ImGuiTabBar* tb = g.TabBars.TryGetMapData(n);
        if (!tb || tb->CurrFrameVisible != g.FrameCount || !tb->Window || skipWindow(tb->Window)) continue;
        for (ImGuiTabItem& tab : tb->Tabs) {
            if (tab.LastFrameVisible != g.FrameCount) continue;
            const std::string nm = std::string("[tab ") + ImGui::TabBarGetTabName(tb, &tab) + "]";
            const float lw = ImGui::CalcTextSize(ImGui::TabBarGetTabName(tb, &tab), nullptr, true).x, room = tab.Width - 2 * tb->FramePadding.x;   // the padding of the bar is drawn
            if (tab.Width + 0.5f < tab.ContentWidth || lw > room + 0.5f) add("SQUASH", tb->Window, nm, "", "%.0f px wide, the label needs %.0f px", tab.Width, std::max(tab.ContentWidth, lw + 2 * tb->FramePadding.x));
            if (tb->BarRect.Min.x + tab.Offset - tb->ScrollingAnim < tb->BarRect.Min.x - 1.f || tb->BarRect.Min.x + tab.Offset + tab.Width - tb->ScrollingAnim > tb->BarRect.Max.x + 1.f)
                add("CUT", tb->Window, nm, "", "the tab bar is too narrow: this tab is scrolled out of view");
        }
    }
}

void report() {
    ImGuiContext& g = *GImGui;
    std::vector<Finding> f;
    check(f);
    std::set<std::string> seen;
    const char* path = getenv("DECT2_LAYOUT_REPORT");
    FILE* fp = path ? fopen(path, "w") : stderr;
    if (!fp) fp = stderr;
    fprintf(fp, "# display %.0fx%.0f scale %.2f logical %.0fx%.0f frame %d tag %s\n", g.IO.DisplaySize.x, g.IO.DisplaySize.y, gUi, g.IO.DisplaySize.x / gUi, g.IO.DisplaySize.y / gUi, g.FrameCount, getenv("DECT2_LAYOUT_TAG") ? getenv("DECT2_LAYOUT_TAG") : "");
    std::string tabs;
    for (int n = 0; n < g.TabBars.GetMapSize(); n++) {
        ImGuiTabBar* tb = g.TabBars.TryGetMapData(n);
        if (!tb || tb->CurrFrameVisible != g.FrameCount) continue;
        if (ImGuiTabItem* t = ImGui::TabBarFindTabByID(tb, tb->SelectedTabId)) { if (!tabs.empty()) tabs += ", "; tabs += ImGui::TabBarGetTabName(tb, t); }
        if (getenv("DECT2_LAYOUT_TABDEBUG")) for (ImGuiTabItem& t : tb->Tabs) fprintf(fp, "# tab %s w %.1f cw %.1f vis %d win %p\n", ImGui::TabBarGetTabName(tb, &t), t.Width, t.ContentWidth, t.LastFrameVisible == g.FrameCount, (void*)tb->Window);
    }
    fprintf(fp, "# tabs %s%s%s\n", tabs.c_str(), gForceTab.empty() ? "" : "; missing tab ", gForceTab.c_str());
    int n = 0;
    for (const Finding& x : f) {
        const std::string key = x.kind + "\t" + x.win + "\t" + keyOf(x.a) + "\t" + keyOf(x.b);
        if (!seen.insert(key).second) continue;
        fprintf(fp, "%s\t%s\t%s\t%s\t%s\n", x.kind.c_str(), x.win.c_str(), x.a.c_str(), x.b.c_str(), x.detail.c_str());
        n++;
    }
    fprintf(fp, "# %d findings\n", n);
    if (fp != stderr) fclose(fp);
}

void endFrame(ImGuiContext* ctx, ImGuiContextHook*) {
    static const double at = getenv("DECT2_LAYOUT_AT") ? atof(getenv("DECT2_LAYOUT_AT")) : 3.0;
    if (!gDone && (ImGui::GetTime() >= at || ctx->FrameCount >= 390) && ctx->FrameCount > 30) {   // at the latest just before the --shot (frame 400)
        report();
        gDone = true;
        if (getenv("DECT2_LAYOUT_EXIT") && gWindow) glfwSetWindowShouldClose(gWindow, 1);
    }
    gItems.clear(); gTexts.clear(); gLast.clear();
}
} // namespace

// Dear ImGui's test-engine hooks (imgui_internal.h)
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* d) {
    ImGuiWindow* w = ctx->CurrentWindow;
    if (!gOn || !d || !w || id == w->MoveId) return;   // no item data: scroll bars, resize grips and table borders
    if (id == ImGui::GetWindowScrollbarID(w, ImGuiAxis_X) || id == ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y)) return;   // scroll bars lie over the contents
    if (ImGuiTable* t = ctx->CurrentTable)   // the column borders of a table (drag to resize): drawn over the cells on purpose
        for (int c = 0; c < t->ColumnsCount; c++) if (id == ImGui::TableGetColumnResizeID(t, c, t->InstanceCurrent)) return;
    gLast[w] = (int)gItems.size();
    if (!gLabel.count(id) && id == w->GetID("##plot")) gLabel[id] = "##plot";   // plot.cpp: the plot area
    gItems.push_back({w, id, d->Rect, bb, w->ClipRect, d->ItemFlags, w->DC.NavLayerCurrent == ImGuiNavLayer_Menu});
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (!gOn) return;
    gLabel[id] = label ? label : "";
    gInfo[id] = flags;
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) { auto l = gLabel.find(id); return l == gLabel.end() ? nullptr : l->second.c_str(); }

// called by the build's copy of ImDrawList::AddText(): every piece of text, with the ink rectangle of its glyphs
void Dect2LayoutCheckText(ImDrawList* dl, ImFont* font, float size, const ImVec2& pos, const char* b, const char* e, float wrap, const ImVec4* fine) {
    ImGuiContext* g = GImGui;
    if (!gOn || !g || !g->WithinFrameScope || !font) return;
    ImGuiWindow* w = g->CurrentWindow && g->CurrentWindow->DrawList == dl ? g->CurrentWindow : nullptr;
    if (!w) for (ImGuiWindow* x : g->Windows) if (x->DrawList == dl) { w = x; break; }
    if (!w) return;   // the foreground and background lists
    if (!e) e = b + strlen(b);
    ImRect ink(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    if (wrap > 0) {
        const ImVec2 s = font->CalcTextSizeA(size, FLT_MAX, wrap, b, e);
        ink = ImRect(pos, ImVec2(pos.x + s.x, pos.y + s.y));
    } else {
        ImFontBaked* baked = font->GetFontBaked(size);
        const float scale = size / baked->Size;
        float x = IM_TRUNC(pos.x), y = IM_TRUNC(pos.y);
        const float x0 = x;
        for (const char* s = b; s < e;) {
            unsigned int c = (unsigned char)*s;
            if (c < 0x80) s += 1; else s += ImTextCharFromUtf8(&c, s, e);
            if (c == '\n') { x = x0; y += size; continue; }
            if (c < 32) continue;
            const ImFontGlyph* gl = baked->FindGlyph((ImWchar)c);
            if (!gl) continue;
            if (gl->Visible) ink.Add(ImRect(x + gl->X0 * scale, y + gl->Y0 * scale, x + gl->X1 * scale, y + gl->Y1 * scale));
            x += gl->AdvanceX * scale;
        }
    }
    if (ink.Min.x > ink.Max.x) return;   // only blanks
    ImRect clip(dl->_CmdHeader.ClipRect.x, dl->_CmdHeader.ClipRect.y, dl->_CmdHeader.ClipRect.z, dl->_CmdHeader.ClipRect.w);
    if (fine) clip = meet(clip, ImRect(fine->x, fine->y, fine->z, fine->w));
    std::string s(b, std::min<size_t>((size_t)(e - b), 60));
    for (char& c : s) if (c == '\n' || c == '\t') c = ' ';
    auto o = gLast.find(w);
    gTexts.push_back({w, ink, clip, fine != nullptr, o == gLast.end() ? -1 : o->second, s});
}

void layoutCheckInstall() {
    ImGuiContext& g = *GImGui;
    g.TestEngineHookItems = true;
    gOn = true;
    ImGuiContextHook h;
    h.Type = ImGuiContextHookType_EndFramePost;
    h.Callback = endFrame;
    ImGui::AddContextHook(&g, &h);
}
#endif
