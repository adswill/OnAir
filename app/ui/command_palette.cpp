// The command palette (Ctrl+K): see command_palette.h. Every entry calls a function the menus and buttons already use.
#include "command_palette.h"
#include "../i18n/i18n.h"
#include "scale.h"
#include "input_map.h"
#include "../voice/voice_monitor_ui.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <vector>

namespace {

struct Cmd {
    std::string label;   // English text, translated when drawn
    const char* kind;    // "Tab", "Panel", "Mode" or "Action"
    std::function<void(App&)> run;
    bool hidden = false;   // a mode left out by Modes shown: listed only once something is typed, tagged "hidden"
};

// the tabs the tab bar drew in the current frame (paletteNoteTab)
std::vector<std::string> gTabNames;
int gTabFrame = -1;

// View-menu panels: each is a bool of App that its menu item toggles
struct Panel { const char* name; bool App::*on; };
const Panel kPanels[] = {
    {"Passes", &App::passesOpen}, {"Scheduler", &App::schedulerOpen}, {"Recordings", &App::recmgrOpen}, {"SatNOGS transmitters", &App::satnogsOpen}, {"Direction finder", &App::dfOpen}, {"Interference hunter", &App::rfiOpen},
    {"Passive radar", &App::passiveRadarOpen}, {"Images", &App::galleryOpen}, {"Data feeds...", &App::feedsOpen},
    {"Messages...", &App::msgdbOpen}, {"Protocol lab...", &App::protolabOpen}, {"TX studio...", &App::txStudioOpen}, {"Mode Builder...", &App::modeBuilderOpen}, {"Signal Lab...", &App::signallabOpen}, {"Watchlists...", &App::watchlistOpen},
    {"Automations...", &App::automationOpen}, {"Plugins...", &App::pluginsOpen}, {"Memory...", &App::memoryOpen},
    {"Datasets...", &App::datasetsOpen}, {"Data packs...", &App::packsOpen}, {"Frequencies...", &App::freqOpen}, {"Map", &App::mapOpen}, {"Propagation...", &App::propOpen},
    {"Sweep scanner", &App::sweepOpen}, {"Station health", &App::stationHealthOpen}, {"Reception doctor", &App::doctorOpen}, {"Antenna rotator...", &App::rotatorOpen},
    {"Antenna designer...", &App::antdOpen},
};

// every entry for this frame
std::vector<Cmd> collect(App& a) {
    std::vector<Cmd> c;
    if (gTabFrame == ImGui::GetFrameCount()) for (const std::string& t : gTabNames) c.push_back({t, "Tab", [t](App& x) { routeTab(x, t); }});
    c.push_back({a.engine.running() ? "Stop receiver" : "Start receiver", "Action",
                 [](App& x) { if (x.engine.running()) x.engine.stop(); else startReceiver(x); }});
    c.push_back({a.muted ? "Unmute" : "Mute", "Action", [](App& x) {   // the same switch as the sound controls
        x.muted = !x.muted; x.engine.player().setMuted(x.muted); x.engine.dabAudio().setMuted(x.muted);
    }});
    for (const Panel& p : kPanels) c.push_back({p.name, "Panel", [p](App& x) { x.*p.on = !(x.*p.on); }});
    c.push_back({"Controls (keys and mouse)", "Action", [](App&) { inputmap::windowOpen() = true; }});
    c.push_back({"Modes shown", "Action", [](App& x) { x.modeVisOpen = true; }});
    for (int i = 0; i < kNumModes; i++) c.push_back({modeHiddenNow(a, kModes[i]) ? std::string(kModes[i].name) + " (hidden)" : std::string(kModes[i].name), "Mode", [f = kModes[i].family, s = kModes[i].subId](App& x) { selectMode(x, f, s); }, modeHiddenNow(a, kModes[i])});
    c.push_back({"Voice codecs", "Action", [](App& x) { x.voiceCodecsOpen = true; }});
    c.push_back({"Voice monitor", "Action", [](App&) { voiceui::open(); }});
    c.push_back({"Voice keys", "Action", [](App& x) { x.voiceKeysOpen = true; }});
    c.push_back({"Credits", "Action", [](App& x) { x.creditsOpen = true; x.creditsTab = 0; }});
    c.push_back({"Credits and licences", "Action", [](App& x) { x.creditsOpen = true; x.creditsTab = 1; }});
    c.push_back({"Documentation", "Action", [](App&) { dect2::openUrl("https://on-air.dev/docs/"); }});
    c.push_back({"Troubleshooting", "Action", [](App&) { dect2::openUrl("https://on-air.dev/docs/troubleshooting/"); }});
    return c;
}

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// every word of the (lower-case) query appears in the text, ignoring case
bool matchAll(const std::string& text, const std::string& query) {
    const std::string t = lower(text);
    size_t p = 0;
    while (p < query.size()) {
        while (p < query.size() && query[p] == ' ') p++;
        size_t e = query.find(' ', p);
        if (e == std::string::npos) e = query.size();
        if (e > p && t.find(query.substr(p, e - p)) == std::string::npos) return false;
        p = e;
    }
    return true;
}

}  // namespace

void paletteNoteTab(const char* name) {
    if (gTabFrame != ImGui::GetFrameCount()) { gTabFrame = ImGui::GetFrameCount(); gTabNames.clear(); }
    if (std::find(gTabNames.begin(), gTabNames.end(), name) == gTabNames.end()) gTabNames.push_back(name);
}

void commandPalette(App& a) {
    static char q[64] = "";
    static int sel = 0;
    static bool closeReq = false, focusSearch = false;
    ImGuiIO& io = ImGui::GetIO();
    const char* id = trw("Command palette");
    if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_K, false)) {   // Ctrl+K (Cmd+K): open, or close when open
        if (ImGui::IsPopupOpen(id)) closeReq = true;
        else { ImGui::OpenPopup(id); q[0] = 0; sel = 0; focusSearch = true; }
    }
    if (!ImGui::IsPopupOpen(id)) return;

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(520 * gUi, io.DisplaySize.x * 0.9f), std::min(440 * gUi, io.DisplaySize.y * 0.8f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    if (closeReq || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        closeReq = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::InputTextWithHint("##cmdq", tr("Type a name"), q, sizeof q)) sel = 0;

    std::vector<Cmd> cmds = collect(a);
    const std::string query = lower(q);
    // decoded messages that match the text (type "Search"): looked up when the text changes, not every frame
    static std::string lastQ;
    static std::vector<std::pair<std::string, std::string>> hits;
    if (query != lastQ) { lastQ = query; hits.clear(); if (query.size() >= 3) hits = msgdbQuickSearch(query, 6); }
    const size_t nCmds = cmds.size();
    for (const auto& h : hits) { const std::string t = h.second; cmds.push_back({h.first, "Search", [t](App& x) { msgdbSearchFor(x, t); }}); }
    std::vector<int> shown;
    for (int i = 0; i < (int)cmds.size(); i++)
        if ((size_t)i >= nCmds || ((!cmds[i].hidden || !query.empty()) && (matchAll(cmds[i].label, query) || matchAll(tr(cmds[i].label.c_str()), query)))) shown.push_back(i);

    const int n = (int)shown.size();
    sel = std::max(0, std::min(sel, n - 1));
    bool moved = false;
    if (n && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { sel = std::min(sel + 1, n - 1); moved = true; }
    if (n && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { sel = std::max(sel - 1, 0); moved = true; }
    int chosen = -1;
    if (n && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) chosen = shown[sel];

    ImGui::Separator();
    ImGui::BeginChild("##cmdlist", ImVec2(0, 0), ImGuiChildFlags_None);
    if (!n) ImGui::TextDisabled("%s", tr("Nothing matches"));
    for (int k = 0; k < n; k++) {
        const Cmd& c = cmds[shown[k]];
        ImGui::PushID(k);
        if (ImGui::Selectable(tr(c.label.c_str()), k == sel)) chosen = shown[k];
        if (k == sel && moved) ImGui::SetScrollHereY(0.5f);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(tr(c.kind)).x);
        ImGui::TextDisabled("%s", tr(c.kind));
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (chosen >= 0) {
        const std::function<void(App&)> run = cmds[chosen].run;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        run(a);   // after the popup closes, so that what it opens is not hidden by it
        return;
    }
    ImGui::EndPopup();
}
