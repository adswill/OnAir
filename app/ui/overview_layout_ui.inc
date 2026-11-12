// The sidebar layout's main area with the panels sized to their content (View > Fit panels to their content; the decisions are in
// overview_layout.h, the reasons and the per-mode list in docs/LAYOUT-4.0.md). Each frame it measures what the list on the right and the
// lower analysis row drew, and the next frame places them by that: an empty row is hidden (and drawn out of sight to see when content
// arrives), a small plot goes into the right column, a list of a few lines becomes a strip above the spectrum. The sizes the user drags
// are kept per mode in App::panelSizes. Included by ui2.cpp inside namespace ui2.
#include "overview_layout.h"
#include "setpos_guard.h"
#include <map>
#include <sstream>

namespace {

struct PanelSize { float right = 0, bottom = 0; bool set = false; };   // unscaled points, 0 = automatic; set: dragged in this mode

std::map<int, PanelSize>& panelSizeMap(App& a) {
    static std::map<int, PanelSize> m;
    static std::string seen;
    static bool loaded = false;
    if (!loaded || seen != a.panelSizes) {   // the first use, or the string changed under us (Reset layout, a loaded workspace)
        loaded = true;
        seen = a.panelSizes;
        m.clear();
        std::stringstream ss(a.panelSizes);
        std::string item;
        while (std::getline(ss, item, ';')) {
            int fam = 0; float r = 0, b = 0;
            if (sscanf(item.c_str(), "%d=%f,%f", &fam, &r, &b) == 3) m[fam] = PanelSize{std::max(0.f, r), std::max(0.f, b), true};
        }
    }
    return m;
}

void panelSizeStore(App& a, int fam, const PanelSize& p) {
    std::map<int, PanelSize>& m = panelSizeMap(a);
    m[fam] = p;
    m[fam].set = true;
    std::string out;
    char b[64];
    for (const auto& kv : m) { snprintf(b, sizeof b, "%d=%.0f,%.0f;", kv.first, kv.second.right, kv.second.bottom); out += b; }
    a.panelSizes = out;
    panelSizeMap(a);   // takes the new string as seen
    savePrefs(a);
}

// the sizes for this mode: its own when dragged in it, else the ones dragged before 4.0 (one pair for every mode)
PanelSize panelSizeOf(App& a, int fam) {
    const std::map<int, PanelSize>& m = panelSizeMap(a);
    const auto it = m.find(fam);
    if (it != m.end()) return it->second;
    return PanelSize{a.splitRight, a.splitBottom, false};
}

// what the current child window drew: from its first cursor position to the furthest item
ovl::Extent drawnExtent() {
    const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
    if (!w) return {};
    return {std::max(0.f, w->DC.CursorMaxPos.x - w->DC.CursorStartPos.x), std::max(0.f, w->DC.CursorMaxPos.y - w->DC.CursorStartPos.y)};
}

// draws a panel once more out of sight (clipped away, no input) in a box the size it would have, and returns what it drew
template <class F> ovl::Extent probePanel(const char* id, ImVec2 at, ImVec2 size, F draw) {
    const ImVec2 back = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(at);
    ImGui::PushClipRect(at, at, false);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7 * gUi, 4 * gUi));   // as beginPane
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoInputs);
    draw();
    const ovl::Extent e = drawnExtent();
    ui::closeCursorMove();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopClipRect();
    ImGui::SetCursorScreenPos(back);
    return e;
}

struct FitState { ovl::State st; ovl::Extent right, bottom; };

}  // namespace

// the part of bodyParent under the status pane: the spectrum (the main tabs), the list and the lower row, in the boxes of ovl::layout()
static void fittedArea(App& a, bool tx) {
    static std::map<int, FitState> fits;   // per mode, and the transmit view apart
    FitState& f = fits[a.family * 2 + (tx ? 1 : 0)];
    const float gap = 4 * gUi;
    const ImVec2 o = ImGui::GetCursorScreenPos(), av = ImGui::GetContentRegionAvail();
    PanelSize ps = panelSizeOf(a, a.family);
    ovl::In in;
    in.W = std::floor(av.x); in.H = std::floor(av.y); in.gap = gap;
    in.lineH = ImGui::GetTextLineHeightWithSpacing();
    in.padX = 2 * 7 * gUi; in.padY = 2 * 4 * gUi;   // beginPane's padding
    in.now = ImGui::GetTime();
    in.adaptive = a.fitPanels;
    in.bottomAllowed = !a.atsc3Mode && !tx;
    in.rightAutoW = std::floor(std::max(236.f * gUi, std::min(330.f * gUi, av.x * 0.30f)));
    in.bottomAutoH = std::floor(std::max(144.f * gUi, std::min(215.f * gUi, av.y - gap - 240.f * gUi)));
    in.userRightW = ps.right * gUi; in.userBottomH = ps.bottom * gUi;
    in.minMainW = 260 * gUi; in.minMainH = 150 * gUi; in.minRightW = 150 * gUi; in.minBottomH = 90 * gUi;
    in.right = f.right; in.bottom = f.bottom;
    f.right = f.bottom = ovl::Extent{};
    const ovl::Out L = ovl::layout(in, f.st);
    auto at = [&](const ovl::Box& b) { return ImVec2(o.x + b.x, o.y + b.y); };
    auto sz = [](const ovl::Box& b) { return ImVec2(b.w, b.h); };

    // ---- the list as a strip above the spectrum
    if (L.strip.shown()) {
        ImGui::SetCursorScreenPos(at(L.strip));
        beginPane("##right2s", sz(L.strip));
        tgRect(a, TgRight);
        rightPanel(a);
        endPane();
    }
    // ---- the spectrum and the mode's tabs
    ImGui::SetCursorScreenPos(at(L.main));
    beginPane("##main2", sz(L.main));
    tgRect(a, TgMain);
    mainTabs(a);
    endPane();
    // ---- the column on the right: the list, and a small plot under it
    const float colW = in.W - L.main.w - gap;
    if (colW > 0.5f) {
        const float colH = L.main.y + L.main.h;
        ImGui::SetCursorScreenPos(ImVec2(o.x + L.main.w, o.y));
        float v = ps.right;
        if (splitter("##splitRight", true, gap, colH, v, colW, 150 * gUi, in.W - gap - 260 * gUi, -1, a)) { ps.right = v; panelSizeStore(a, a.family, ps); }
    }
    if (L.right.shown()) {
        ImGui::SetCursorScreenPos(at(L.right));
        beginPane("##right2", sz(L.right));
        tgRect(a, TgRight);
        rightPanel(a);
        if (!L.probeRight) f.right = drawnExtent();
        endPane();
    }
    if (!L.right.shown() && !L.strip.shown()) a.tgMin[TgRight] = a.tgMax[TgRight] = ImVec2(0, 0);
    // ---- the lower row (or the plot in the column)
    if (L.bottom.shown()) {
        const bool row = L.bottom.x < 0.5f && L.bottom.w >= in.W - 0.5f;
        if (row) {
            ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + L.bottom.y - gap));
            float v = ps.bottom;
            if (splitter("##splitBottom", false, gap, in.W, v, L.bottom.h, 90 * gUi, in.H - gap - 150 * gUi, -1, a)) { ps.bottom = v; panelSizeStore(a, a.family, ps); }
        }
        ImGui::SetCursorScreenPos(at(L.bottom));
        beginPane("##const2", sz(L.bottom));
        tgRect(a, TgConst);
        constellationsTab(a);
        f.bottom = drawnExtent();
        endPane();
    } else a.tgMin[TgConst] = a.tgMax[TgConst] = ImVec2(0, 0);
    // ---- out of sight: the panels that are not drawn in a box of their own size, so the next frame knows what they would show
    const bool settling = in.now - f.st.born < ovl::kSettle;
    const bool probeNow = settling || ImGui::GetFrameCount() % 3 == 0;   // every third frame is enough for the hold times
    if (L.probeRight && probeNow) f.right = probePanel("##right2p", at(L.probeRightBox), sz(L.probeRightBox), [&a] { rightPanel(a); });
    if (L.probeBottom && probeNow) f.bottom = probePanel("##const2p", at(L.probeBottomBox), sz(L.probeBottomBox), [&a] { constellationsTab(a); });
    // the whole area is this layout's: the parent's content ends at its bottom
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(in.W, in.H));
}
