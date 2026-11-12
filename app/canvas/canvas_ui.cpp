// The Workspace layout (View > Layout > Workspace, docs/WORKSPACE-4.0.md). A blank canvas below the menu bar with a row of workspace
// controls; the user adds blocks from the Add block button or a right click. Each block is a movable, resizable window drawn with the
// panels the other layouts use (the source rows of the toolbar, spectrumWaterfall(), mainTabs(), txPanel(), the Record and Listen groups
// of the Source panel ...); the tool windows that draw themselves (map, passes, rotator, scanner, frequencies, memory) are placed on the
// canvas as they are. A block is drawn with the receiver of the Source block it is linked to (several radios: one Source block per
// receiver, the RX 1 / + receivers of receivers.h). The model, its file form and the templates are canvas_model.h.
#include "canvas.h"
#include "canvas_model.h"
#include "../state/receivers.h"
#include "../state/workspace.h"
#include "../vfo/vfo_state.h"
#include "../ui/icons.h"
#include "../platform/platform.h"
#include "dect2/engine/vfo.h"
#include "dect2/engine/mode_tuning.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace ui2 {   // ui2_widgets.cpp / ui2_shell.cpp (ui2_internal.h is for the ui2 files only)
float freqDigits(App& a, float fontSize);
void volumeControl(App& a, float w);
void logConsole(App& a, float h);
void modeCombo(App& a, float w);
}

namespace canvas {
namespace {

// settings that belong to the whole window, not to one receiver: read and written with the first receiver's keys
struct GlobalPrefs {
    int old;
    GlobalPrefs() : old(plat::gPrefRx) { plat::gPrefRx = 1; }
    ~GlobalPrefs() { plat::gPrefRx = old; }
};
struct RxPrefs {
    int old;
    explicit RxPrefs(int rx) : old(plat::gPrefRx) { plat::gPrefRx = rx; }
    ~RxPrefs() { plat::gPrefRx = old; }
};

struct Drag { bool on = false, resizing = false; };

struct State {
    bool prefsRead = false, on = false;
    bool loaded = false;
    std::vector<Workspace> all;
    int cur = 0;
    bool dirty = false;
    double dirtyAt = 0;
    std::map<int, int> lastOwner;     // source block -> the decoder block that owned it last frame (absent: not known yet)
    std::map<int, double> idleFreq;   // source block -> the frequency to keep when it goes idle
    std::set<int> place;              // blocks (and tab groups, as -group) placed from the model this frame
    std::map<int, Drag> drag;         // a block window being moved or resized by the mouse
    std::set<int> hostedSet;          // hosted blocks whose window flag the canvas set on hostedApp
    std::set<int> hostedSeen;         // hosted windows already placed once
    App* hostedApp = nullptr;
    int moving = 0, dropOn = 0;       // a block dragged by its title over another block's title: they become tabs when it is let go
    ImVec2 origin;                    // the canvas' top left on screen
    ImVec2 addAt;                     // where the Add block menu puts the new block (canvas points)
    std::vector<int> removeQ;
    bool cogOpen = false;
    std::map<int, bool> cog;          // a block's settings row is open
    char nameBuf[96] = "";
    std::string status;               // the last export / import message
    double statusAt = -10;
    std::string devArg;               // --workspace
    std::vector<std::function<void()>> later;   // changes to the list of workspaces: done at the start of the next frame (the frame holds a reference into it)
} S;

std::string filePath() { return plat::dataDir() + "/workspaces-blocks.txt"; }

void markDirty() { if (!S.dirty) S.dirtyAt = ImGui::GetTime(); S.dirty = true; }

void saveNow() {
    std::ofstream f(filePath(), std::ios::binary | std::ios::trunc);
    if (f) f << writeMany(S.all);
    S.dirty = false;
    GlobalPrefs gp;
    if (S.cur >= 0 && S.cur < (int)S.all.size()) plat::prefs().setS("canvasCur", S.all[S.cur].name.c_str());
}

void note(const std::string& s) { S.status = s; S.statusAt = ImGui::GetTime(); }

void freshState() {   // a workspace was switched or loaded: the per-frame bookkeeping starts again
    S.lastOwner.clear(); S.place.clear(); S.drag.clear(); S.hostedSeen.clear(); S.hostedSet.clear(); S.cog.clear();
    S.hostedApp = nullptr;   // a tool window closed while another workspace (or layout) was shown is not taken for a close of its block
    for (const Block& b : S.all[S.cur].blocks) { S.place.insert(b.id); if (b.tab) S.place.insert(-b.tab); }
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    std::ifstream f(filePath(), std::ios::binary);
    if (f) { std::stringstream ss; ss << f.rdbuf(); S.all = parseMany(ss.str()); }
    if (S.all.empty()) { S.all.push_back(makeTemplate(0)); S.all.back().name = "My workspace"; }
    std::string want;
    { GlobalPrefs gp; want = plat::prefs().getS("canvasCur", ""); }
    S.cur = 0;
    for (int i = 0; i < (int)S.all.size(); i++) if (S.all[i].name == want) S.cur = i;
    if (!S.devArg.empty()) {   // --workspace N (a template) or a file
        const std::string a = S.devArg;
        S.devArg.clear();
        if (a.size() == 1 && a[0] >= '0' && a[0] < '0' + kNumTemplates) {
            Workspace t = makeTemplate(a[0] - '0');
            t.name = uniqueName(S.all, t.name);
            S.all.push_back(t);
            S.cur = (int)S.all.size() - 1;
        } else {
            std::ifstream g(a, std::ios::binary);
            std::stringstream ss; ss << g.rdbuf();
            for (Workspace& w : parseMany(ss.str())) { w.name = uniqueName(S.all, w.name); S.all.push_back(w); S.cur = (int)S.all.size() - 1; }
        }
    }
    freshState();
}

Workspace& cur() { return S.all[S.cur]; }

// ---- modes and receivers

std::string modeKey(const App& a) {
    const dect2::SubMode* s = curSub(a);
    return std::to_string(a.family) + (s && s->id ? std::string("/") + s->id : std::string());
}

const ModeDef* modeRow(const std::string& key) {
    const size_t sl = key.find('/');
    const int fam = std::atoi(key.substr(0, sl).c_str());
    const std::string sub = sl == std::string::npos ? std::string() : key.substr(sl + 1);
    const ModeDef* any = nullptr;
    for (int i = 0; i < kNumModes; i++) {
        const ModeDef& m = kModes[i];
        if (m.family != fam) continue;
        if (sub.empty() ? !m.subId : (m.subId && sub == m.subId)) return &m;
        if (!any) any = &m;
    }
    return any;
}

std::string modeLabel(const std::string& key) {
    const ModeDef* m = modeRow(key);
    return m ? m->name : key;
}

void selectKey(App& a, const std::string& key) {
    if (const ModeDef* m = modeRow(key)) selectMode(a, m->family, m->subId);
}

App* rxOf(const Workspace& w, int sourceBlock) {
    const int rx = w.receiverOf(sourceBlock);
    return rx ? receiverById(rx) : nullptr;
}

std::string devName(const App& a) {
    return a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? a.devices[a.devIdx].name : std::string();
}

std::string sourceLabel(const Workspace& w, int sourceBlock) {
    const int rx = w.receiverOf(sourceBlock);
    if (!rx) return tr("no receiver");
    std::string s = "RX " + std::to_string(rx);
    if (const App* r = receiverById(rx)) { const std::string d = devName(*r); if (!d.empty()) s += " " + d; }
    return s;
}

// a Source block's saved radio settings: the receiver's own settings, as File > Workspaces keeps them
void captureSource(Block& b, const App& r) {
    const ::Workspace c = workspaceCapture(r, "");
    b.set("dev", c.devName);
    b.set("mode", std::to_string(c.family) + (c.subId.empty() ? std::string() : "/" + c.subId));
    b.setD("mhz", c.freqMhz); b.setD("rate", c.sampleRateHz);
    b.setI("lna", c.lnaDb); b.setI("vga", c.vgaDb); b.setI("amp", c.ampOn ? 1 : 0);
}

void applySource(const Block& b, App& r) {
    if (r.engine.running() || b.get("mode").empty()) return;
    ::Workspace c = workspaceCapture(r, "");   // the windows and the shown modes stay as they are
    c.devName = b.get("dev");
    const std::string mk = b.get("mode");
    const size_t sl = mk.find('/');
    c.family = std::atoi(mk.substr(0, sl).c_str());
    c.subId = sl == std::string::npos ? std::string() : mk.substr(sl + 1);
    c.freqMhz = b.getD("mhz", c.freqMhz); c.sampleRateHz = b.getD("rate", 0);
    c.lnaDb = b.getI("lna", c.lnaDb); c.vgaDb = b.getI("vga", c.vgaDb); c.ampOn = b.getI("amp", 0) != 0;
    std::string why;
    RxPrefs rp(r.rxId);
    if (!workspaceApply(r, c, why)) r.engine.log("Workspace block: " + why);
}

std::vector<int> openReceivers() {
    std::vector<int> v;
    for (App* r : receiversAll()) v.push_back(r->rxId);
    return v;
}

// every Source block drives a receiver: the one it names, or a free one (made when needed)
void syncSources(Workspace& w) {
    for (Block& b : w.blocks) {
        if (b.kind != Kind::Source) continue;
        int rx = b.getI("rx");
        if (rx > 0 && receiverById(rx)) continue;
        if (rx > 0 && rx <= kMaxReceivers && !receiverById(rx)) {   // the receiver it names is closed (a loaded workspace): open receivers until it is there
            while ((int)receiversAll().size() < kMaxReceivers && !receiverById(rx) && addReceiver()) {}
            if (App* r = receiverById(rx)) { applySource(b, *r); continue; }
        }
        bool create = false;
        b.cfg.erase("rx");
        rx = pickReceiver(w, openReceivers(), kMaxReceivers, create);
        if (!rx) continue;   // every receiver is taken: the block says so
        App* r = create ? addReceiver() : receiverById(rx);
        if (!r) continue;
        b.setI("rx", r->rxId);
        applySource(b, *r);
        markDirty();
    }
}

// the decoder blocks choose their source's mode; a source without one runs the idle mode (the General receiver) at the same frequency
void syncModes(Workspace& w) {
    for (const int s : w.sources()) {
        App* r = rxOf(w, s);
        if (!r) continue;
        const int owner = w.decoderOwner(s);
        const auto it = S.lastOwner.find(s);
        const int last = it == S.lastOwner.end() ? -1 : it->second;
        Block* ob = owner ? w.find(owner) : nullptr;
        const std::string rxMode = modeKey(*r);
        const ModeStep st = modeStep(last, owner, ob ? ob->get("mode") : std::string(), rxMode);
        RxPrefs rp(r->rxId);
        if (st.kind == ModeStep::Select) selectKey(*r, st.mode);
        else if (st.kind == ModeStep::Idle) {
            const double f = r->freqMhz;
            selectKey(*r, st.mode);
            if (r->freqMhz != f && f > 0) tuneFreq(*r, f);
            r->engine.log("Workspace: no decoder block uses RX " + std::to_string(r->rxId) + ": its decoder stopped, the General receiver keeps the spectrum");
        }
        if (ob) { const std::string now = modeKey(*r); if (ob->get("mode") != now) { ob->set("mode", now); markDirty(); } }   // the block follows the receiver (its mode list, the Mode menu)
        S.lastOwner[s] = owner;
    }
}

// ---- hosted tool windows

bool* hostedFlag(App& a, Kind k) {
    switch (k) {
    case Kind::Map: return &a.mapOpen;
    case Kind::Tracker: return &a.passesOpen;
    case Kind::Rotator: return &a.rotatorOpen;
    case Kind::Scanner: return &a.scannerOpen;
    case Kind::Frequencies: return &a.freqOpen;
    case Kind::Bookmarks: return &a.memoryOpen;
    default: return nullptr;
    }
}
const char* hostedName(Kind k) {   // the tool window's ImGui name (its "###id": the same in every language)
    switch (k) {
    case Kind::Map: return "Map";
    case Kind::Tracker: return "###Passes";
    case Kind::Rotator: return "###Antenna rotator";
    case Kind::Scanner: return "###Scanner (highly experimental)";
    case Kind::Frequencies: return "###Frequencies";
    case Kind::Bookmarks: return "###Memory";
    default: return "";
    }
}
bool hostedAutoSize(Kind k) { return k == Kind::Rotator; }   // the rotator window sizes itself

// ---- geometry

ImVec2 toScreen(float x, float y) { return ImVec2(S.origin.x + x * gUi, S.origin.y + y * gUi); }
Rect fromWindow(const ImGuiWindow* win, const Rect& old) {
    Rect r;
    r.x = std::round((win->Pos.x - S.origin.x) / gUi);
    r.y = std::round((win->Pos.y - S.origin.y) / gUi);
    r.w = win->Collapsed ? old.w : std::round(win->SizeFull.x / gUi);
    r.h = win->Collapsed ? old.h : std::round(win->SizeFull.y / gUi);
    return r;
}

// after the window was drawn: its position and size into the model; a move or resize ends with a snap
void readBack(Workspace& w, const std::vector<int>& ids, const ImGuiWindow* win, int key) {
    if (!win || ids.empty()) return;
    Block* b0 = w.find(ids[0]);
    if (!b0) return;
    const Rect now = fromWindow(win, b0->r);
    Drag& d = S.drag[key];
    const bool down = ImGui::IsMouseDown(0);
    if (now != b0->r && down) { d.resizing = d.resizing || now.w != b0->r.w || now.h != b0->r.h; d.on = true; }
    Rect r = now;
    if (!down && d.on) {   // let go: snap
        if (w.snap) r = snapRect(w, ids[0], now, d.resizing, 10);
        d = Drag();
        if (r != now) S.place.insert(key);
        markDirty();
    } else if (now != b0->r && !down) markDirty();   // moved by ImGui (kept on screen) or the keyboard
    for (int id : ids) if (Block* b = w.find(id)) {
        if (b->r != r) b->r = r;
        if (b->collapsed != win->Collapsed && w.shown(id)) { b->collapsed = win->Collapsed; markDirty(); }
    }
}

// ---- block bodies

std::string title(const Workspace& w, const Block& b) {
    std::string t = tr(info(b.kind).name);
    if (b.kind == Kind::Source) t += " - " + sourceLabel(w, b.id);
    else if (b.kind == Kind::Decoder && !b.get("mode").empty()) t += " - " + modeLabel(b.get("mode"));
    if (info(b.kind).needsSource) t += w.sourceMissing(b.id) ? std::string(" - ") + tr("no source") : " - RX " + std::to_string(w.receiverOf(b.source));
    return t;
}

// the first row of a block: the source it uses and the settings cog
void headerRow(Workspace& w, Block& b) {
    const KindInfo& ki = info(b.kind);
    const float cogW = ImGui::GetFrameHeight();
    if (ki.needsSource) {
        ImGui::SetNextItemWidth(std::max(80 * gUi, std::min(240 * gUi, ImGui::GetContentRegionAvail().x - cogW - 8 * gUi)));
        const std::string curL = w.sourceMissing(b.id) ? std::string(tr("Pick a source")) : sourceLabel(w, b.source);
        if (ImGui::BeginCombo("##src", curL.c_str())) {
            for (int s : w.sources()) {
                ImGui::PushID(s);
                if (ImGui::Selectable(sourceLabel(w, s).c_str(), b.source == s)) { w.link(b.id, s); markDirty(); }
                ImGui::PopID();
            }
            if (w.sources().empty()) ImGui::TextDisabled("%s", tr("Add a Radio source block first"));
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The radio source this block uses"));
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, ImGui::GetContentRegionAvail().x - cogW));
    bool& c = S.cog[b.id];
    if (iconFlat(Ic::Gear, tr("Block settings"), c)) c = !c;
    if (!c) return;
    ImGui::Indent(4 * gUi);
    if (b.kind == Kind::Decoder) {
        if (App* r = rxOf(w, b.source)) { RxPrefs rp(r->rxId); toolbarParts(*r, TbDecoder, true); }
    }
    if (b.tab && ImGui::SmallButton(tr("Take out of the tabs"))) { w.leaveTab(b.id); S.place.insert(b.id); markDirty(); }
    if (!ki.hosted && !b.tab) {
        std::vector<int> others;
        for (const Block& o : w.blocks) if (o.id != b.id && !info(o.kind).hosted && w.shown(o.id)) others.push_back(o.id);
        if (!others.empty()) {
            ImGui::SetNextItemWidth(200 * gUi);
            if (ImGui::BeginCombo("##tabwith", tr("Tab with ..."))) {
                for (int o : others) { ImGui::PushID(o); if (ImGui::Selectable(title(w, *w.find(o)).c_str())) { const int g = w.find(o)->tab ? w.find(o)->tab : o; w.joinTab(b.id, o); S.place.insert(-g); markDirty(); } ImGui::PopID(); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Put this block into tabs with another one (or drag it by its title onto the other block's title)"));
        }
    }
    if (ImGui::SmallButton(tr("Remove block"))) S.removeQ.push_back(b.id);
    ImGui::Unindent(4 * gUi);
    ImGui::Separator();
}

int ensureVfo(App& r, Block& b) {
    const int id = b.getI("vfo");
    for (const VfoRow& v : r.vfoRows) if (v.id == id && id > 0) return id;
    const bool panel = r.vfoPanel;
    vfoAddAt(r, r.freqMhz + b.getD("offkhz", 0) / 1000.0);
    r.vfoPanel = panel;   // the channel list window stays as it was
    if (r.vfoRows.empty()) return 0;
    VfoRow& v = r.vfoRows.back();
    if (!b.get("vmode").empty()) v.mode = std::max(0, std::min((int)dect2::VfoMode::Count - 1, b.getI("vmode")));
    b.setI("vfo", v.id);
    markDirty();
    return v.id;
}

void demodBody(Block& b, App& r) {
    const int id = ensureVfo(r, b);
    VfoRow* row = nullptr;
    for (VfoRow& v : r.vfoRows) if (v.id == id) row = &v;
    if (!row) { ImGui::TextDisabled("%s", tr("No channel")); return; }
    bool ch = false;
    ch |= ImGui::Checkbox(tr("On"), &row->on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    if (ImGui::BeginCombo("##vm", dect2::vfoModeName((dect2::VfoMode)row->mode))) {
        for (int m = 0; m < (int)dect2::VfoMode::Count; m++) if (ImGui::Selectable(dect2::vfoModeName((dect2::VfoMode)m), m == row->mode)) { row->mode = m; ch = true; }
        ImGui::EndCombo();
    }
    double off = (row->freqMhz - r.freqMhz) * 1000.0;
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputDouble(tr("kHz offset"), &off, 1, 10, "%.3f")) { row->freqMhz = r.freqMhz + off / 1000.0; ch = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("From the source's tuned frequency; the channel must be inside its capture"));
    ImGui::SetNextItemWidth(110 * gUi);
    ch |= ImGui::InputDouble("MHz##vf", &row->freqMhz, 0, 0, "%.4f");
    ImGui::SetNextItemWidth(110 * gUi);
    ch |= ImGui::InputDouble(tr("kHz wide"), &row->bwKhz, 0, 0, row->bwKhz > 0 ? "%.1f" : "auto");
    const bool dec = dect2::vfoIsDecoder((dect2::VfoMode)row->mode);
    if (!dec) {
        ImGui::SetNextItemWidth(110 * gUi);
        ch |= ImGui::InputDouble(tr("squelch dB"), &row->sqDb, 1, 5, row->sqDb <= -150 ? "open" : "%.0f");
        ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderFloat(tr("volume"), &row->vol, 0.f, 2.f, "%.2f");
        ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderFloat(tr("pan"), &row->pan, -1.f, 1.f, "%.1f");
        ch |= ImGui::Checkbox(tr("Mute"), &row->mute);
    }
    for (const dect2::VfoStatus& st : r.engine.vfoStatus()) if (st.id == row->id) {
        ImGui::TextColored(st.open ? pal::okGreen() : ImVec4(0.6f, 0.6f, 0.6f, 1), "%.0f dBFS%s%s", st.levelDb, st.info.empty() ? "" : "  ", st.info.c_str());
    }
    if (!r.engine.running()) ImGui::TextDisabled("%s", tr("Start the source: the channel shares its capture."));
    const std::string warn = r.engine.vfoWarning(row->id);
    if (!warn.empty() && row->on) ImGui::TextColored(pal::warnAmber(), "%s", warn.c_str());
    if (ch) { b.setD("offkhz", (row->freqMhz - r.freqMhz) * 1000.0); b.setI("vmode", row->mode); markDirty(); savePrefsSoon(r); }
}

void audioBody(Workspace& w, Block& b, App& r) {
    bool heard = receiverHeard() == &r;
    if (ImGui::Checkbox(tr("Hear this source"), &heard)) receiverHear(r.rxId);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only one receiver plays its sound at a time"));
    ui2::volumeControl(r, ImGui::GetContentRegionAvail().x);
    listenGroupUi(r);
    std::vector<Block*> demods;
    for (Block& o : w.blocks) if (o.kind == Kind::Demod && o.source == b.source) demods.push_back(&o);
    if (demods.empty()) return;
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Demodulator blocks mixed in"));
    for (Block* d : demods) {
        const int id = d->getI("vfo");
        for (VfoRow& v : r.vfoRows) if (v.id == id) {
            ImGui::PushID(d->id);
            bool ch = ImGui::Checkbox("##on", &v.on);
            ImGui::SameLine(); ImGui::Text("%.4f MHz %s", v.freqMhz, dect2::vfoModeName((dect2::VfoMode)v.mode));
            if (!dect2::vfoIsDecoder((dect2::VfoMode)v.mode)) {
                ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::SliderFloat("##v", &v.vol, 0.f, 2.f, "%.2f");
                ImGui::SameLine(); ch |= ImGui::Checkbox(tr("Mute"), &v.mute);
            }
            if (ch) savePrefsSoon(r);
            ImGui::PopID();
        }
    }
}

int notesResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* s = (std::string*)d->UserData;
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}

void notesBody(Block& b) {
    static std::map<int, std::string> buf;
    std::string& s = buf[b.id];
    if (s.empty() && !b.get("text").empty()) s = b.get("text");
    if (s.capacity() < 64) s.reserve(64);
    if (ImGui::InputTextMultiline("##notes", s.data(), s.capacity() + 1, ImVec2(-1, -1), ImGuiInputTextFlags_CallbackResize, notesResize, &s)) {
        s.resize(std::strlen(s.c_str()));
        b.set("text", s);
        markDirty();
    }
}

void clockBody(Block& b) {
    const std::time_t t = std::time(nullptr);
    char u[32] = "", l[32] = "", d[32] = "";
    if (const std::tm* g = std::gmtime(&t)) { std::strftime(u, sizeof u, "%H:%M:%S", g); std::strftime(d, sizeof d, "%Y-%m-%d", g); }
    if (const std::tm* lt = std::localtime(&t)) std::strftime(l, sizeof l, "%H:%M:%S", lt);
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 2.2f);
    ImGui::Text("%s", u);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("UTC  %s", d);
    bool local = b.getI("local", 1) != 0;
    if (local) ImGui::Text("%s %s", l, tr("local"));
    if (ImGui::Checkbox(tr("Local time"), &local)) { b.setI("local", local ? 1 : 0); markDirty(); }
}

void sourceBody(Workspace& w, Block& b) {
    App* r = rxOf(w, b.id);
    if (!r) {
        ImGui::TextWrapped("%s", tr("No receiver for this block: OnAir runs four at most. Remove another Radio source block, or close a receiver."));
        return;
    }
    RxPrefs rp(r->rxId);
    const bool run = r->engine.running();
    if (ImGui::Button(run ? tr("Stop") : tr("Start"), ImVec2(70 * gUi, 0))) { if (run) r->engine.stop(); else startReceiver(*r); }
    ImGui::SameLine();
    const bool heard = receiverHeard() == r;
    ImGui::TextColored(run ? pal::okGreen() : ImVec4(0.6f, 0.6f, 0.6f, 1), "RX %d  %s%s", r->rxId, run ? tr("receiving") : tr("stopped"), heard ? "  (heard)" : "");
    ui2::freqDigits(*r, 22.f);
    toolbarParts(*r, TbSource | TbTuner | TbGain, true);
    if (sourceOptionsOpen(*r)) sourceOptions(*r);
    if (ImGui::TreeNode(trw("Status"))) { statusBar(*r); ImGui::TreePop(); }
    // the settings that go with the workspace: refreshed while the receiver is stopped or now and then
    static std::map<int, double> lastCap;
    if (ImGui::GetTime() - lastCap[b.id] > 2.0) {
        lastCap[b.id] = ImGui::GetTime();
        const std::string before = b.get("dev") + b.get("mode") + b.get("mhz") + b.get("rate") + b.get("lna") + b.get("vga") + b.get("amp");
        captureSource(b, *r);
        if (before != b.get("dev") + b.get("mode") + b.get("mhz") + b.get("rate") + b.get("lna") + b.get("vga") + b.get("amp")) markDirty();
    }
}

void decoderBody(Workspace& w, Block& b, App& r) {
    ui2::modeCombo(r, std::min(260 * gUi, ImGui::GetContentRegionAvail().x * 0.5f));
    ImGui::SameLine();
    if (ImGui::Button(b.paused ? tr("Resume") : tr("Pause"))) { b.paused = !b.paused; markDirty(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Paused: the source's decoder stops (it runs the General receiver at the same frequency), the other blocks keep working"));
    const int owner = w.decoderOwner(b.source);
    if (b.paused) { ImGui::TextDisabled("%s", tr("Paused.")); return; }
    if (owner != b.id) {
        ImGui::TextWrapped("%s", tr("Another decoder block uses this source: a receiver runs one mode. Add another Radio source block for a second mode at the same time."));
        return;
    }
    mainTabs(r);
}

void body(App& drawn, Workspace& w, Block& b) {
    (void)drawn;
    headerRow(w, b);
    const KindInfo& ki = info(b.kind);
    App* r = ki.needsSource ? rxOf(w, b.source) : nullptr;
    if (ki.needsSource && !r) {
        ImGui::TextWrapped("%s", w.sourceMissing(b.id) ? tr("This block has no source (it was removed). Pick one above, or add a Radio source block.")
                                                        : tr("The source has no receiver yet."));
        return;
    }
    if (r) {
        RxPrefs rp(r->rxId);
        switch (b.kind) {
        case Kind::Spectrum: {
            float frac = (float)b.getD("split", 0.45);
            spectrumWaterfall(*r, frac, true);
            if (std::fabs(frac - b.getD("split", 0.45)) > 1e-3) { b.setD("split", std::round(frac * 1000) / 1000); markDirty(); }
            break;
        }
        case Kind::Demod: demodBody(b, *r); break;
        case Kind::Audio: audioBody(w, b, *r); break;
        case Kind::Decoder: decoderBody(w, b, *r); break;
        case Kind::Transmitter: txPanel(*r); break;
        case Kind::Recorder: recordGroupUi(*r); break;
        case Kind::Log: ui2::logConsole(*r, 0); break;
        case Kind::History: historyLogTab(*r); break;
        default: break;
        }
        return;
    }
    switch (b.kind) {
    case Kind::Source: sourceBody(w, b); break;
    case Kind::Clock: clockBody(b); break;
    case Kind::Notes: notesBody(b); break;
    default: break;
    }
}

ImGuiWindowFlags flagsFor(Kind k) {
    ImGuiWindowFlags f = ImGuiWindowFlags_NoSavedSettings;
    if (k == Kind::Spectrum || k == Kind::Notes || k == Kind::Log) f |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    return f;
}

ImGuiWindow* windowNamed(const std::string& name) { return ImGui::FindWindowByName(name.c_str()); }

void placeNext(int key, const Rect& r, bool collapsed, const std::string& name) {
    const ImGuiWindow* win = windowNamed(name);
    if (S.place.count(key) || !win || !win->WasActive) {
        ImGui::SetNextWindowPos(toScreen(r.x, r.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(r.w * gUi, r.h * gUi), ImGuiCond_Always);
        ImGui::SetNextWindowCollapsed(collapsed, ImGuiCond_Always);
        S.place.erase(key);
    }
}

// a window of its own
void blockWindow(App& drawn, Workspace& w, Block& b) {
    const std::string name = title(w, b) + "###cvb" + std::to_string(b.id);
    placeNext(b.id, b.r, b.collapsed, "###cvb" + std::to_string(b.id));
    bool open = true;
    ImGui::PushID(b.id);
    if (ImGui::Begin(name.c_str(), &open, flagsFor(b.kind))) body(drawn, w, b);   // collapsed: nothing is drawn
    readBack(w, {b.id}, ImGui::GetCurrentWindow(), b.id);
    ImGui::End();
    ImGui::PopID();
    if (!open) S.removeQ.push_back(b.id);
}

// several blocks as tabs of one window
void groupWindow(App& drawn, Workspace& w, int group) {
    const std::vector<int> m = w.tabMembers(group);
    if (m.empty()) return;
    int act = w.activeTab[group];
    Block* ab = w.find(act);
    if (!ab) { act = m[0]; ab = w.find(act); w.activeTab[group] = act; }
    const std::string id = "###cvg" + std::to_string(group);
    placeNext(-group, ab->r, ab->collapsed, id);
    bool open = true;
    const std::string name = title(w, *ab) + id;
    ImGui::PushID(-group);
    if (ImGui::Begin(name.c_str(), &open, flagsFor(ab->kind))) {
        if (ImGui::BeginTabBar("##cvtabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int bid : m) {
                Block* b = w.find(bid);
                if (!b) continue;
                const std::string lab = std::string(tr(info(b->kind).name)) + "###t" + std::to_string(bid);
                bool keep = true;
                if (ImGui::BeginTabItem(lab.c_str(), &keep)) {
                    if (w.activeTab[group] != bid) { w.activeTab[group] = bid; markDirty(); }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::MenuItem(tr("Take out of the tabs"))) { w.leaveTab(bid); S.place.insert(bid); markDirty(); }
                    if (ImGui::MenuItem(tr("Remove block"))) S.removeQ.push_back(bid);
                    ImGui::EndPopup();
                }
                if (!keep) S.removeQ.push_back(bid);
            }
            ImGui::EndTabBar();
        }
        if (Block* sb = w.find(w.activeTab[group])) { ImGui::PushID(sb->id); body(drawn, w, *sb); ImGui::PopID(); }
    }
    readBack(w, w.tabMembers(group), ImGui::GetCurrentWindow(), -group);
    ImGui::End();
    ImGui::PopID();
    if (!open) S.removeQ.push_back(w.activeTab[group]);
}

// a tool window that draws itself: its flag is set on the receiver drawn now, and it is placed from the model
void hostedBlock(App& drawn, Workspace& w, Block& b) {
    bool* flag = hostedFlag(drawn, b.kind);
    if (!flag) return;
    if (S.hostedApp == &drawn && S.hostedSet.count(b.id) && !*flag) { S.removeQ.push_back(b.id); return; }   // closed by its own close button
    *flag = true;
    S.hostedSet.insert(b.id);
    const char* name = hostedName(b.kind);
    ImGuiWindow* win = windowNamed(name);
    if (!win || !win->WasActive) return;   // it appears this frame (floatingWindows draws it); placed on the next
    if (S.place.count(b.id) || !S.hostedSeen.count(b.id)) {
        ImGui::SetWindowPos(name, toScreen(b.r.x, b.r.y), ImGuiCond_Always);
        if (!hostedAutoSize(b.kind)) ImGui::SetWindowSize(name, ImVec2(b.r.w * gUi, b.r.h * gUi), ImGuiCond_Always);
        ImGui::SetWindowCollapsed(name, b.collapsed, ImGuiCond_Always);
        ImGui::BringWindowToDisplayFront(win);
        S.hostedSeen.insert(b.id);
        S.place.erase(b.id);
        return;
    }
    readBack(w, {b.id}, win, b.id);
}

void removeBlock(App& drawn, Workspace& w, int id) {
    Block* b = w.find(id);
    if (!b) return;
    if (info(b->kind).hosted) { if (bool* f = hostedFlag(drawn, b->kind)) *f = false; S.hostedSet.erase(id); S.hostedSeen.erase(id); }
    if (b->kind == Kind::Demod) if (App* r = rxOf(w, b->source)) {   // its channel goes with it
        const int v = b->getI("vfo");
        r->vfoRows.erase(std::remove_if(r->vfoRows.begin(), r->vfoRows.end(), [v](const VfoRow& x) { return x.id == v; }), r->vfoRows.end());
        savePrefs(*r);
    }
    if (b->kind == Kind::Source) {   // one block per receiver: its receiver closes with it (RX 1 stays open)
        const int rx = w.receiverOf(id);
        if (rx >= 2) closeReceiver(rx);
        S.lastOwner.erase(id);
    }
    w.remove(id);
    markDirty();
}

void addMenuItems(Workspace& w, ImVec2 at) {
    for (int g = 0; g < kNumGroups; g++) {
        ImGui::TextDisabled("%s", tr(kGroupNames[g]));
        for (int k = 0; k < (int)Kind::Count; k++) {
            const KindInfo& ki = info((Kind)k);
            if (ki.group != g) continue;
            bool present = false;
            for (const Block& b : w.blocks) present = present || b.kind == ki.kind;
            const bool one = ki.hosted && present;   // a tool window exists once
            const bool full = ki.kind == Kind::Source && (int)w.sources().size() >= kMaxReceivers;
            if (ImGui::MenuItem(tr(ki.name), nullptr, false, !one && !full)) {
                const int id = w.add(ki.kind, std::max(0.f, at.x), std::max(0.f, at.y));
                if (ki.kind == Kind::Decoder) if (Block* b = w.find(id)) if (App* r = rxOf(w, b->source)) b->set("mode", modeKey(*r));
                S.place.insert(id);
                markDirty();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", one ? tr("This tool window is on the canvas already") : full ? tr("Four receivers at most") : tr(ki.tip));
        }
    }
}

void closeHosted() {
    if (S.hostedApp) for (const Block& b : cur().blocks) if (bool* f = hostedFlag(*S.hostedApp, b.kind)) *f = false;
}

void switchTo(int i) {
    if (i < 0 || i >= (int)S.all.size() || i == S.cur) return;
    closeHosted();   // the old workspace's tool windows close
    S.cur = i;
    freshState();
    markDirty();
}

void toolbarRow(Workspace& w) {
    ImGui::SetCursorPos(ImVec2(8 * gUi, 4 * gUi));
    if (ImGui::Button(tr("Add block"))) { S.addAt = ImVec2(24, 24 + 24 * (float)(w.blocks.size() % 10)); ImGui::OpenPopup("##cvaddbtn"); }
    if (ImGui::BeginPopup("##cvaddbtn")) { addMenuItems(w, S.addAt); ImGui::EndPopup(); }
    ImGui::SameLine(0, 16 * gUi);
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##cvws", w.name.c_str())) {
        for (int i = 0; i < (int)S.all.size(); i++) { ImGui::PushID(i); if (ImGui::Selectable(S.all[i].name.c_str(), i == S.cur)) S.later.push_back([i] { switchTo(i); }); ImGui::PopID(); }
        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("New from a template"));
        for (int t = 0; t < kNumTemplates; t++) {
            ImGui::PushID(100 + t);
            if (ImGui::Selectable(tr(kTemplateNames[t]))) S.later.push_back([t] {
                Workspace n = makeTemplate(t);
                n.name = uniqueName(S.all, tr(kTemplateNames[t]));
                S.all.push_back(n);
                switchTo((int)S.all.size() - 1);
            });
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The saved workspaces (they keep their blocks, places, links and settings); New from a template"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Save as..."))) { std::snprintf(S.nameBuf, sizeof S.nameBuf, "%s", uniqueName(S.all, w.name).c_str()); ImGui::OpenPopup("##cvsaveas"); }
    if (ImGui::BeginPopup("##cvsaveas")) {
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##nm", S.nameBuf, sizeof S.nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button(tr("Save a copy")) || enter) && S.nameBuf[0]) {
            Workspace c = w;
            c.name = uniqueName(S.all, S.nameBuf);
            S.later.push_back([c] { S.all.push_back(c); switchTo((int)S.all.size() - 1); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Rename")) && S.nameBuf[0]) {
            std::vector<Workspace> others;
            for (int i = 0; i < (int)S.all.size(); i++) if (i != S.cur) others.push_back(S.all[i]);
            S.all[S.cur].name = uniqueName(others, S.nameBuf);
            markDirty();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export..."))) {
        const std::string p = plat::saveFileDialog((w.name + ".onairws").c_str());
        if (!p.empty()) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << serialise(w);
            note(f ? std::string(tr("Exported to ")) + p : std::string(tr("Could not write ")) + p);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Import..."))) {
        const std::string p = plat::openFileDialog();
        if (!p.empty()) {
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            const std::vector<Workspace> got = parseMany(ss.str());
            if (!got.empty()) S.later.push_back([got] {
                for (Workspace g : got) { g.name = uniqueName(S.all, g.name); S.all.push_back(g); }
                switchTo((int)S.all.size() - 1);
            });
            note(got.empty() ? std::string(tr("Not a workspace file: ")) + p : std::string(tr("Imported ")) + std::to_string(got.size()));
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(S.all.size() < 2);
    if (ImGui::Button(tr("Delete"))) ImGui::OpenPopup("##cvdel");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##cvdel")) {
        ImGui::Text(tr("Delete the workspace \"%s\"?"), w.name.c_str());
        if (ImGui::Button(tr("Delete it"))) {
            S.later.push_back([] {
                if (S.all.size() < 2) return;
                closeHosted();
                S.all.erase(S.all.begin() + S.cur);
                S.cur = 0;
                freshState();
                markDirty();
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0, 16 * gUi);
    if (ImGui::Checkbox(tr("Snap"), &w.snap)) markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A moved or resized block settles on the grid and against the edges of the others"));
    ImGui::SameLine();
    int grid = (int)w.grid;
    ImGui::SetNextItemWidth(70 * gUi);
    if (ImGui::InputInt(tr("grid"), &grid, 0, 0)) { w.grid = (float)std::max(0, std::min(200, grid)); markDirty(); }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Links"), &w.links)) markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lines from each radio source to the blocks that use it"));
    if (ImGui::GetTime() - S.statusAt < 6) { ImGui::SameLine(0, 16 * gUi); ImGui::TextDisabled("%s", S.status.c_str()); }
}

// the screen rectangle a block shows at (its window, or its tab group's)
bool screenRect(const Workspace& w, const Block& b, ImVec2& p0, ImVec2& p1) {
    const ImGuiWindow* win = nullptr;
    if (info(b.kind).hosted) win = windowNamed(hostedName(b.kind));
    else if (b.tab) win = windowNamed("###cvg" + std::to_string(b.tab));
    else win = windowNamed("###cvb" + std::to_string(b.id));
    if (!win || !win->WasActive) return false;
    (void)w;
    p0 = win->Pos;
    p1 = ImVec2(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
    return true;
}

void drawLinks(const Workspace& w, ImDrawList* dl) {
    if (!w.links) return;
    const ImU32 col = ImGui::GetColorU32(ImVec4(pal::accent().x, pal::accent().y, pal::accent().z, 0.55f));
    for (const Block& b : w.blocks) {
        if (!b.source || !w.shown(b.id)) continue;
        const Block* s = w.find(b.source);
        ImVec2 a0, a1, b0, b1;
        if (!s || !screenRect(w, *s, a0, a1) || !screenRect(w, b, b0, b1)) continue;
        const bool right = b0.x >= a1.x;
        const ImVec2 from(right ? a1.x : a0.x, a0.y + ImGui::GetFrameHeight() * 0.5f);
        const ImVec2 to(right ? b0.x : b1.x, b0.y + ImGui::GetFrameHeight() * 0.5f);
        const float dx = std::max(30.f * gUi, std::fabs(to.x - from.x) * 0.4f) * (right ? 1.f : -1.f);
        dl->AddBezierCubic(from, ImVec2(from.x + dx, from.y), ImVec2(to.x - dx, to.y), to, col, 1.4f * gUi);
        dl->AddCircleFilled(to, 3 * gUi, col);
    }
}

void drawGrid(const Workspace& w, ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    if (w.grid < 4 || !w.snap) return;
    const float step = w.grid * gUi * (w.grid < 12 ? 4 : 2);
    const ImU32 c = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.045f));
    for (float x = p0.x; x < p1.x; x += step)
        for (float y = p0.y; y < p1.y; y += step) dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 1.5f * gUi, y + 1.5f * gUi), c);
}

// a block window dragged by its title over another block's title: the target lights up; let go there and they become tabs
void dragToTabs(Workspace& w) {
    ImGuiContext& g = *GImGui;
    const ImGuiWindow* mv = g.MovingWindow ? g.MovingWindow->RootWindow : nullptr;
    auto blockOf = [&](const ImGuiWindow* win) -> int {
        if (!win) return 0;
        const char* id = std::strstr(win->Name, "###cvb");
        if (id) return std::atoi(id + 6);
        id = std::strstr(win->Name, "###cvg");
        if (id) { const int grp = std::atoi(id + 6); return w.activeTab.count(grp) ? w.activeTab[grp] : 0; }
        return 0;
    };
    const int me = blockOf(mv);
    if (me && ImGui::IsMouseDown(0)) {
        S.moving = me;
        S.dropOn = 0;
        const ImVec2 m = ImGui::GetIO().MousePos;
        for (const Block& b : w.blocks) {
            if (b.id == me || info(b.kind).hosted || !w.shown(b.id)) continue;
            const Block* mb = w.find(me);
            if (mb && mb->tab && mb->tab == b.tab) continue;
            ImVec2 p0, p1;
            if (!screenRect(w, b, p0, p1)) continue;
            const float th = ImGui::GetFrameHeight();
            if (m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p0.y + th) {
                S.dropOn = b.id;
                ImGui::GetForegroundDrawList()->AddRectFilled(p0, ImVec2(p1.x, p0.y + th), ImGui::GetColorU32(ImVec4(pal::accent().x, pal::accent().y, pal::accent().z, 0.35f)));
                ImGui::GetForegroundDrawList()->AddRect(p0, p1, ImGui::GetColorU32(pal::accent()), 0, 0, 2 * gUi);
                ImGui::SetTooltip("%s", tr("Let go to put them into tabs"));
            }
        }
        return;
    }
    if (S.moving && S.dropOn && !ImGui::IsMouseDown(0)) {
        const Block* t = w.find(S.dropOn);
        const int grp = t ? (t->tab ? t->tab : t->id) : 0;
        if (w.joinTab(S.moving, S.dropOn)) { S.place.insert(-grp); S.drag.clear(); markDirty(); }
    }
    if (!ImGui::IsMouseDown(0)) { S.moving = 0; S.dropOn = 0; }
}

}  // namespace

bool on() {
    if (!S.prefsRead) { GlobalPrefs gp; S.on = plat::prefs().getB("canvasOn", false); S.prefsRead = true; }
    return S.on;
}

void setOn(bool v) {
    on();
    if (S.on == v) return;
    S.on = v;
    GlobalPrefs gp;
    plat::prefs().setB("canvasOn", v);
    if (S.loaded && S.dirty) saveNow();
    if (v && S.loaded) freshState();
}

bool active(const App& a) { return on() && a.newUi && !a.touchUi; }

void devStart(const std::string& arg) { S.devArg = arg; S.prefsRead = true; S.on = true; }   // for this run only: not saved

void layoutMenuItem(App& a) {
    (void)a;
    if (ImGui::MenuItem(tr("Workspace"), nullptr, on())) setOn(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A blank canvas: add blocks (radio sources, spectrum, demodulators, audio, decoders, map, satellite tracker, rotator, transmitter ...) and arrange them as you like"));
}

void frame(App& a, ImVec2 disp, float top) {
    load();
    for (auto& f : S.later) f();
    S.later.clear();
    Workspace& w = cur();
    for (int id : S.removeQ) removeBlock(a, w, id);
    S.removeQ.clear();
    syncSources(w);
    syncModes(w);

    // the canvas: the row of workspace controls, then the blocks' area
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(disp.x, std::max(1.f, disp.y - top)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##canvasroot", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    toolbarRow(w);
    const float rowH = ImGui::GetFrameHeight() + 8 * gUi;
    S.origin = ImVec2(0, top + rowH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(disp.x, disp.y);
    dl->AddLine(ImVec2(0, S.origin.y - 1), ImVec2(disp.x, S.origin.y - 1), ImGui::GetColorU32(ImGuiCol_Separator));
    drawGrid(w, dl, S.origin, p1);
    if (w.blocks.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(S.origin.x + 24 * gUi, S.origin.y + 24 * gUi));
        ImGui::PushTextWrapPos(std::min(disp.x - 24 * gUi, 640 * gUi));
        ImGui::TextDisabled("%s", tr("A blank workspace. Add block (or a right click here) puts a block on the canvas: start with a Radio source, then a Spectrum and waterfall, a Decoder, a Demodulator and an Audio output that use it. "
                                     "Drag a block by its title to move it, by a corner to resize it; drop it on another block's title to make tabs. The workspace list on the left has ready-made templates."));
        ImGui::PopTextWrapPos();
    }
    // a right click on the empty canvas: the Add block menu, the new block where the click was
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(1) && ImGui::GetIO().MousePos.y > S.origin.y) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        S.addAt = ImVec2((m.x - S.origin.x) / gUi, (m.y - S.origin.y) / gUi);
        ImGui::OpenPopup("##cvadd");
    }
    if (ImGui::BeginPopup("##cvadd")) { addMenuItems(w, S.addAt); ImGui::EndPopup(); }
    drawLinks(w, dl);
    ImGui::End();

    // the blocks; a hidden tab or a collapsed block draws nothing of its body
    std::set<int> groupsDone;
    for (size_t i = 0; i < w.blocks.size(); i++) {
        Block& b = w.blocks[i];
        if (info(b.kind).hosted) { hostedBlock(a, w, b); continue; }
        if (b.tab) { if (groupsDone.insert(b.tab).second) groupWindow(a, w, b.tab); continue; }
        blockWindow(a, w, b);
    }
    S.hostedApp = &a;
    dragToTabs(w);
    if (S.dirty && ImGui::GetTime() - S.dirtyAt > 1.0 && !ImGui::IsMouseDown(0)) saveNow();
}

}  // namespace canvas
