// The Memory panel: the frequencies the user keeps with their mode and settings. A window (View > Memory, Ctrl+M), an optional tab of the main
// screen, and the "M" menu of the quick list. The model and the files are in core (dect2/channels/memory_bank.h), what is captured from and
// applied to a receiver in memory_options.cpp, the edit dialog in memory_edit.cpp.
#include <string>
#include "memory_ui.h"
#include "../state/results_export.h"
#include "../ui/ui_common.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include "../ui/input_map.h"

namespace {

using dect2::Memory;
using dect2::MemoryBank;
using dect2::MemorySort;

struct U {
    char query[64] = "";
    int sort = 0;                 // MemorySort as the combo shows it (0 = manual order)
    bool desc = false, grouped = true, starsOnly = false, loaded = false;
    uint64_t sel = 0, pendingDel = 0;   // a delete asked for inside a row waits until the table is done
    int target = 0;               // 0: the receiver on screen, else the receiver id
    std::set<std::string> folded; // groups folded (by name)
    // delete with undo
    Memory undo; size_t undoPos = 0; bool haveUndo = false; double undoT = 0;
    std::string msg; double msgT = 0; bool msgBad = false;
    bool tabLoaded = false;
} u;

void say(const std::string& s, bool bad = false) { u.msg = s; u.msgBad = bad; u.msgT = ImGui::GetTime(); }

App* targetApp(App& a) {
    if (u.target > 0) if (App* r = receiverById(u.target)) return r;
    u.target = 0;
    return &a;
}

void recall(App& a, uint64_t id, bool withMode) {
    MemoryBank& b = memoryBank();
    const Memory* m = b.find(id);
    if (!m) return;
    App* t = targetApp(a);
    std::string note;
    const Memory copy = *m;   // the bank may change under a recall that saves
    if (memoryRecallApp(*t, copy, withMode, note)) {
        b.touch(id, 0);
        memoryChanged();
        char bf[200];
        snprintf(bf, sizeof bf, tr("RX%d: %s"), t->rxId, memui::memoryTitle(copy).c_str());
        say(std::string(bf) + (note.empty() ? "" : "  (" + note + ")"));
    } else say(note.empty() ? std::string(tr("Could not apply this memory.")) : note, true);
}

void storeCurrent(App& a) {
    App* t = targetApp(a);
    Memory m = memoryCapture(*t);
    m.name = memorySuggestName(*t);
    memui::openEdit(m, true);
}

void loadPrefsOnce() {
    if (u.loaded) return;
    u.loaded = true;
    plat::Prefs& p = plat::prefs();
    u.sort = (int)std::max(0L, std::min(5L, p.getI("memSort", 0)));
    u.desc = p.getB("memDesc", false);
    u.grouped = p.getB("memGrouped", true);
}
void savePrefsUi() {
    plat::Prefs& p = plat::prefs();
    p.setI("memSort", u.sort); p.setB("memDesc", u.desc); p.setB("memGrouped", u.grouped);
}

std::string lastUsedText(int64_t t) { return t > 0 ? fmtLocal(t, "%Y-%m-%d %H:%M") : std::string("-"); }
std::string bwText(double hz) {
    if (hz <= 0) return "-";
    char b[32];
    if (hz >= 1e6) snprintf(b, sizeof b, "%.3g MHz", hz / 1e6); else snprintf(b, sizeof b, "%.4g kHz", hz / 1e3);
    return b;
}

const char* const kSortNames[] = {N_("Manual order"), N_("Name"), N_("Frequency"), N_("Last used"), N_("Group"), N_("Mode")};

// one row; returns true when the row was dragged onto another (from, to are bank positions)
void row(App& a, MemoryBank& b, size_t idx, bool allowDrag, int& dragFrom, int& dragTo) {
    const Memory& m = b.all()[idx];
    ImGui::PushID((int)m.id);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();   // star
    {
        const bool fav = m.favourite;
        if (iconFlat(Ic::Star, tr(fav ? "Favourite: kept at the top. Click to remove" : "Click to make this a favourite"), fav)) { Memory c = m; c.favourite = !fav; b.update(m.id, c); memoryChanged(); }
    }
    ImGui::TableNextColumn();   // name: the whole row is one selectable
    const bool sel = u.sel == m.id;
    if (m.colour) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x - 3 * gUi, p.y), ImVec2(p.x - 1 * gUi, p.y + ImGui::GetTextLineHeight()), IM_COL32((m.colour >> 16) & 255, (m.colour >> 8) & 255, m.colour & 255, 255));
    }
    char nm[160];
    snprintf(nm, sizeof nm, "%s###r", m.name.empty() ? tr("(no name)") : m.name.c_str());
    if (ImGui::Selectable(nm, sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
        u.sel = m.id;
        if (ImGui::IsMouseDoubleClicked(0)) recall(a, m.id, true);
    }
    if (ImGui::IsItemHovered() && (!m.notes.empty() || !m.radio.empty())) {
        ImGui::BeginTooltip();
        if (!m.notes.empty()) ImGui::TextUnformatted(m.notes.c_str());
        if (!m.radio.empty()) ImGui::TextDisabled(tr("stored with: %s"), m.radio.c_str());
        ImGui::EndTooltip();
    }
    if (allowDrag && ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
        const int from = (int)idx;
        ImGui::SetDragDropPayload("onair_memory", &from, sizeof from);
        ImGui::TextUnformatted(m.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (allowDrag && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("onair_memory")) { dragFrom = *(const int*)p->Data; dragTo = (int)idx; }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem("##ctx")) {
        u.sel = m.id;
        if (ImGui::MenuItem(tr("Recall"))) recall(a, m.id, true);
        if (ImGui::MenuItem(tr("Recall frequency only"))) recall(a, m.id, false);
        if (ImGui::MenuItem(tr("Edit..."))) memui::openEdit(m, false);
        if (ImGui::MenuItem(tr("Include in the memory scan"), nullptr, m.scan)) { Memory c = m; c.scan = !m.scan; b.update(m.id, c); memoryChanged(); }
        if (ImGui::MenuItem(tr("Delete"))) u.pendingDel = m.id;
        ImGui::EndPopup();
    }
    ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(memui::freqReadout(m.freqHz).c_str()); ImGui::PopFont();
    ImGui::TableNextColumn();
    { const int mi = memoryModeIndex(m.mode); ImGui::TextUnformatted(m.mode.empty() ? "-" : mi >= 0 ? kModes[mi].name : m.mode.c_str()); }
    ImGui::TableNextColumn(); ImGui::TextUnformatted(bwText(m.bandwidthHz).c_str());
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s%s", m.tags.c_str(), m.scan ? (m.tags.empty() ? "scan" : "  scan") : "");
    ImGui::TableNextColumn(); ImGui::TextDisabled("%s", lastUsedText(m.lastUsed).c_str());
    ImGui::PopID();
}

void table(App& a, MemoryBank& b, float h) {
    dect2::MemoryQuery q;
    q.text = u.query; q.favouritesOnly = u.starsOnly;
    const MemorySort sort = (MemorySort)u.sort;
    std::vector<size_t> v = b.view(q, sort, u.desc, true);
    v.erase(std::remove_if(v.begin(), v.end(), [&](size_t i) { return !memui::memoryListEnabled(b.all()[i].group); }), v.end());   // lists switched off in "My lists"
    int dragFrom = -1, dragTo = -1;
    std::vector<std::string> order;   // groups in the order they appear
    if (u.grouped) {
        for (size_t i : v) { const std::string& g = b.all()[i].group; if (std::find(order.begin(), order.end(), g) == order.end()) order.push_back(g); }
        std::stable_sort(order.begin(), order.end(), [](const std::string& x, const std::string& y) { return x.empty() != y.empty() ? y.empty() : false; });   // "no group" last
        if (order.size() == 1 && order[0].empty()) order.clear();   // no groups used: a plain list
    }
    const bool allowDrag = sort == MemorySort::Manual && !*u.query && !u.starsOnly && order.empty();   // a reorder only makes sense on the whole list in its own order
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable | ImGuiTableFlags_SortTristate;   // tristate: no heading is sorted until one is clicked
    if (ImGui::BeginTable("##memtable", 7, tf, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoHide, 24 * gUi);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn(tr("Frequency"), ImGuiTableColumnFlags_WidthFixed, 112 * gUi);
        ImGui::TableSetupColumn(tr("Mode"), ImGuiTableColumnFlags_WidthFixed, 120 * gUi);
        ImGui::TableSetupColumn(tr("Bandwidth"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 76 * gUi);
        ImGui::TableSetupColumn(tr("Tags"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.5f);
        ImGui::TableSetupColumn(tr("Last used"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsDirty) {   // a click on a heading
            static const int colSort[7] = {0, 1, 2, 5, 0, 0, 3};   // Name, Frequency, Mode, Last used
            const int c = sp->SpecsCount > 0 ? sp->Specs[0].ColumnIndex : 0;
            if (sp->SpecsCount > 0 && colSort[c]) { u.sort = colSort[c]; u.desc = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending; savePrefsUi(); }
            sp->SpecsDirty = false;
        }
        if (order.empty()) for (size_t i : v) row(a, b, i, allowDrag, dragFrom, dragTo);
        else for (const std::string& g : order) {
            int n = 0;
            for (size_t i : v) if (b.all()[i].group == g) n++;
            ImGui::TableNextRow(ImGuiTableRowFlags_None);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            const bool fold = u.folded.count(g) > 0;
            char gl[160];
            snprintf(gl, sizeof gl, "%s  (%d)###g%s", g.empty() ? tr("No group") : g.c_str(), n, g.c_str());
            ImGui::SetNextItemOpen(!fold, ImGuiCond_Always);
            const bool open = ImGui::TreeNodeEx(gl, ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            if (open == fold) { if (open) u.folded.erase(g); else u.folded.insert(g); }
            if (open) for (size_t i : v) if (b.all()[i].group == g) row(a, b, i, false, dragFrom, dragTo);
        }
        ImGui::EndTable();
    }
    if (dragFrom >= 0 && dragTo >= 0 && dragFrom != dragTo) { b.move((size_t)dragFrom, (size_t)dragTo); memoryChanged(); }
    if (u.pendingDel) {
        u.haveUndo = b.remove(u.pendingDel, &u.undo, &u.undoPos);
        u.undoT = ImGui::GetTime(); u.pendingDel = 0;
        if (u.haveUndo) memoryChanged();
    }
}

void importExport(MemoryBank& b) {
    memui::importButton(b);   // memory_lists_ui.cpp: opens a preview of what a file holds before anything is added
    ImGui::SameLine();
    memui::exportAs(b);
    memui::importPopup(b);
}

// the memory scan: the Scanner steps through the flagged memories (frequency, mode and dwell) and stops on activity; it is the one that knows
// each mode's lock / SNR telemetry, so nothing is repeated here
void scanButton(App& a, MemoryBank& b) {
    int n = 0;
    for (const Memory& m : b.all()) n += m.scan && !m.mode.empty() && memoryModeIndex(m.mode) >= 0;
    ImGui::BeginDisabled(n == 0);
    char l[80];
    snprintf(l, sizeof l, "%s (%d)###memscan", tr("Scan memories"), n);
    if (ImGui::Button(l)) {
        std::vector<ScanSeed> list;
        for (const Memory& m : b.all()) if (m.scan && !m.mode.empty() && memoryModeIndex(m.mode) >= 0) list.push_back({(double)m.freqHz / 1e6, m.mode, m.name, m.dwellSec});
        scannerLoadList(*targetApp(a), list);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapTooltip(380 * gUi, tr("Steps through the memories marked 'Include in the memory scan' with the Scanner: it tunes each one, switches the mode, listens for its dwell time and stops while the receiver reports a signal or a lock. Memories without a mode are left out."));
}

void panelBody(App& a, bool inWindow) {
    loadPrefsOnce();
    MemoryBank& b = memoryBank();
    if (ImGui::GetTime() - u.undoT > 20) u.haveUndo = false;
    // controls
    if (iconButton(Ic::Save, tr("Store current"), pal::remap(IM_COL32(32, 96, 140, 255)), pal::remap(IM_COL32(44, 124, 178, 255)))) storeCurrent(a);
    if (ImGui::IsItemHovered()) wrapTooltip(380 * gUi, tr("Keeps this receiver's frequency, mode, sample rate, gains and the mode's options. The name is the station's when the mode reports one."));
    const Memory* sel = b.find(u.sel);
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button(tr("Recall"))) recall(a, u.sel, true);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) wrapTooltip(380 * gUi, tr("Tunes to the frequency, switches to the stored mode and applies its options and gains. Double-click a row does the same. A running receiver restarts when the mode changes."));
    ImGui::SameLine();
    if (ImGui::Button(tr("Frequency only"))) recall(a, u.sel, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Tunes to the frequency and keeps the mode as it is"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Edit..."))) memui::openEdit(*sel, false);
    ImGui::SameLine();
    if (ImGui::Button(tr("Delete"))) { u.undo = *sel; u.haveUndo = b.remove(u.sel, &u.undo, &u.undoPos); u.undoT = ImGui::GetTime(); memoryChanged(); }
    ImGui::EndDisabled();
    if (u.haveUndo) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Undo delete"))) { b.restore(u.undo, u.undoPos); u.haveUndo = false; u.sel = u.undo.id; memoryChanged(); }
    }
    {   // the receiver a recall goes to
        const std::vector<App*> rs = receiversAll();
        if (rs.size() > 1) {
            ImGui::SameLine(0, 12 * gUi);
            ImGui::SetNextItemWidth(110 * gUi);
            char cur[24];
            snprintf(cur, sizeof cur, u.target ? "RX%d" : tr("This receiver"), u.target);
            if (ImGui::BeginCombo("##memtarget", cur)) {
                if (ImGui::Selectable(tr("This receiver"), u.target == 0)) u.target = 0;
                for (App* r : rs) { char l[16]; snprintf(l, sizeof l, "RX%d", r->rxId); if (ImGui::Selectable(l, u.target == r->rxId)) u.target = r->rxId; }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The receiver Store current and Recall work on"));
        }
    }
    // search and order
    ImGui::SetNextItemWidth(std::max(120.f * gUi, ImGui::GetContentRegionAvail().x - 340 * gUi));
    ImGui::InputTextWithHint("##memq", tr("Search name, tag, mode, frequency (162.4)"), u.query, sizeof u.query);
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::BeginCombo("##memsort", tr(kSortNames[u.sort]))) {
        for (int i = 0; i < 6; i++) if (ImGui::Selectable(tr(kSortNames[i]), u.sort == i)) { u.sort = i; savePrefsUi(); }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click a heading of the table to sort by it too"));
    ImGui::SameLine(); if (ImGui::Checkbox(tr("Groups"), &u.grouped)) savePrefsUi();
    ImGui::SameLine(); ImGui::Checkbox(tr("Stars only"), &u.starsOnly);
    // the table
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2 + 6 * gUi;
    if (b.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("No memories yet. Tune to a frequency you want to keep and press Store current; or import a list."));
        ImGui::PopTextWrapPos();
    } else table(a, b, std::max(80.f * gUi, ImGui::GetContentRegionAvail().y - footer));
    // the bottom line
    importExport(b);
    ImGui::SameLine(); scanButton(a, b);
    memui::listsSection(b);
    memui::statusLine();
    ImGui::SameLine(0, 12 * gUi); ImGui::TextDisabled(tr("%zu memories"), b.size());
    if (!u.msg.empty() && ImGui::GetTime() - u.msgT < 12) { ImGui::PushTextWrapPos(0); ImGui::TextColored(u.msgBad ? pal::badRed() : pal::okGreen(), "%s", u.msg.c_str()); ImGui::PopTextWrapPos(); }
    if (const uint64_t id = memui::editDialogs()) { u.sel = id; u.query[0] = 0; }
    (void)inWindow;
}

// the lines of the quick list: favourites, then the ten most recent that are not favourites
void quickItems(App& a) {
    MemoryBank& b = memoryBank();
    bool any = false;
    auto item = [&](const Memory& m) { any = true; ImGui::PushID((int)m.id); if (ImGui::MenuItem(memui::memoryTitle(m).c_str())) recall(a, m.id, true); ImGui::PopID(); };
    for (const Memory& m : b.all()) if (m.favourite) item(m);
    const std::vector<size_t> r = b.recent(10);
    bool head = false;
    for (size_t i : r) {
        const Memory& m = b.all()[i];
        if (m.favourite) continue;
        if (!head) { if (any) ImGui::Separator(); ImGui::TextDisabled("%s", tr("Recent")); head = true; }
        item(m);
    }
    if (!any) ImGui::TextDisabled("%s", tr("(nothing stored or used yet)"));
}

}  // namespace

namespace { int gDevShow = 0; }   // --memdev: 1 opens the Store current dialog, 2 draws the quick list as a window (a headless run cannot open a menu)
void memoryDev(int what) { gDevShow = what; }

void memoryWindow(App& a) {
    if (gDevShow == 1) { gDevShow = 0; a.memoryOpen = true; storeCurrent(a); }
    if (gDevShow == 2) {
        ImGui::SetNextWindowPos(ImVec2(500 * gUi, 90 * gUi), ImGuiCond_Once);
        if (ImGui::Begin("Quick list (dev)")) quickItems(a);
        ImGui::End();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (inputmap::pressed(inputmap::Action::MemoryWin)) a.memoryOpen = !a.memoryOpen;   // Ctrl+M by default
    (void)io;
    if (!u.tabLoaded) { u.tabLoaded = true; a.memoryTab = a.memoryTab || plat::prefs().getB("memTab", false); }
    if (!a.memoryOpen) return;
    firstWindowSize(720 * gUi, 460 * gUi);
    if (!ImGui::Begin(trw("Memory"), &a.memoryOpen)) { ImGui::End(); return; }
    panelBody(a, true);
    ImGui::End();
}

void memoryTabItem(App& a) {
    if (!a.memoryTab) return;
    if (tabItem(N_("Memory"), Ic::Star)) { panelBody(a, false); ImGui::EndTabItem(); }
}

void memoryMenu(App& a) {
    if (ImGui::BeginMenu((std::string(tr("Memory")) + "###memmenu").c_str())) {
        if (ImGui::MenuItem(tr("Memory panel..."), inputmap::shortcut(inputmap::Action::MemoryWin).c_str(), a.memoryOpen)) a.memoryOpen = !a.memoryOpen;
        if (ImGui::MenuItem(tr("Store current..."))) { a.memoryOpen = true; storeCurrent(a); }
        if (ImGui::MenuItem(tr("Show Memory as a tab"), nullptr, a.memoryTab)) { a.memoryTab = !a.memoryTab; plat::prefs().setB("memTab", a.memoryTab); }
        ImGui::Separator();
        quickItems(a);
        ImGui::EndMenu();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", inputmap::tip(tr("Memories: the frequencies you keep, with mode and settings (Ctrl+M)"), inputmap::Action::MemoryWin).c_str());
}

void memoryToolbarButton(App& a) {
    if (ImGui::SmallButton((std::string(tr("Memory")) + "##memtb").c_str())) ImGui::OpenPopup("##memquick");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", inputmap::tip(tr("Memories: the frequencies you keep, with mode and settings (Ctrl+M)"), inputmap::Action::MemoryWin).c_str());
    if (ImGui::BeginPopup("##memquick")) {
        if (ImGui::MenuItem(tr("Memory panel..."), inputmap::shortcut(inputmap::Action::MemoryWin).c_str(), a.memoryOpen)) a.memoryOpen = !a.memoryOpen;
        if (ImGui::MenuItem(tr("Store current..."))) { a.memoryOpen = true; storeCurrent(a); }
        ImGui::Separator();
        quickItems(a);
        ImGui::EndPopup();
    }
}
