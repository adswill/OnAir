// Workspaces and the last-session file. A record is one line of '|'-separated fields (escaped: \p is '|', \n a new line, \\ a backslash).
// The saved list is the prefs key "workspaces" (one record per line); the last session is dataDir()/last-session.txt:
//   line 1: "clean 0" while running, "clean 1" after a normal exit
//   line 2: the record of the session (the same fields as a workspace)
#include "workspace.h"
#include "../platform/platform.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

// the open windows a workspace remembers: the plain ones (a flag each, no state to rebuild)
struct PanelFlag { const char* name; bool App::*flag; };
const PanelFlag kPanels[] = {
    {"gallery", &App::galleryOpen}, {"direction-finder", &App::dfOpen}, {"interference", &App::rfiOpen},
    {"passive-radar", &App::passiveRadarOpen}, {"passes", &App::passesOpen}, {"station-health", &App::stationHealthOpen}, {"reception-doctor", &App::doctorOpen},
    {"sweep", &App::sweepOpen}, {"scanner", &App::scannerOpen}, {"scanview", &App::scanViewOpen}, {"memory", &App::memoryOpen}, {"packs", &App::packsOpen}, {"datasets", &App::datasetsOpen},
    {"frequencies", &App::freqOpen}, {"map", &App::mapOpen}, {"propagation", &App::propOpen}, {"rotator", &App::rotatorOpen}, {"feeds", &App::feedsOpen},
    {"watchlist", &App::watchlistOpen}, {"messages", &App::msgdbOpen}, {"protocol-lab", &App::protolabOpen}, {"mode-builder", &App::modeBuilderOpen}, {"signal-lab", &App::signallabOpen},
    {"automation", &App::automationOpen},
    {"antenna-designer", &App::antdOpen},
};

std::string escapeField(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '|') o += "\\p";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::vector<std::string> splitRecord(const std::string& line) {
    std::vector<std::string> f(1);
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c == '|') { f.emplace_back(); continue; }
        if (c == '\\' && i + 1 < line.size()) {
            const char n = line[++i];
            f.back() += n == 'p' ? '|' : n == 'n' ? '\n' : n;
            continue;
        }
        f.back() += c;
    }
    return f;
}

std::string encodeRecord(const Workspace& w) {
    char num[160];
    snprintf(num, sizeof num, "%.6f|%.0f|%d|%d|%d", w.freqMhz, w.sampleRateHz, w.lnaDb, w.vgaDb, w.ampOn ? 1 : 0);
    return escapeField(w.name) + "|" + escapeField(w.devName) + "|" + std::to_string(w.family) + "|" + escapeField(w.subId) + "|" + num
         + "|" + escapeField(w.panels);
}

// name|device|family|sub|freq|rate|lna|vga|amp|panels (the panels field may be empty)
bool decodeRecord(const std::string& line, Workspace& w) {
    const std::vector<std::string> f = splitRecord(line);
    if (f.size() < 9) return false;
    w.name = f[0]; w.devName = f[1]; w.family = atoi(f[2].c_str()); w.subId = f[3];
    w.freqMhz = atof(f[4].c_str()); w.sampleRateHz = atof(f[5].c_str());
    w.lnaDb = atoi(f[6].c_str()); w.vgaDb = atoi(f[7].c_str()); w.ampOn = f[8] == "1";
    w.panels = f.size() > 9 ? f[9] : std::string();
    return !w.name.empty();
}

std::string sessionPath() { return plat::dataDir() + "/last-session.txt"; }

void writeSession(bool clean, const Workspace& w) {
    const std::string path = sessionPath(), tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fprintf(f, "clean %d\n%s\n", clean ? 1 : 0, encodeRecord(w).c_str());
    fclose(f);
#ifdef _WIN32
    std::remove(path.c_str());   // rename does not replace an existing file on Windows
#endif
    std::rename(tmp.c_str(), path.c_str());
}

bool readSession(bool& clean, Workspace& w) {
    FILE* f = fopen(sessionPath().c_str(), "rb");
    if (!f) return false;
    std::string all;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
    fclose(f);
    const size_t nl = all.find('\n');
    if (nl == std::string::npos) return false;
    clean = all.compare(0, nl, "clean 1") == 0;
    std::string rec = all.substr(nl + 1);
    while (!rec.empty() && (rec.back() == '\n' || rec.back() == '\r')) rec.pop_back();
    return decodeRecord(rec, w);
}

struct SessionState {
    bool logging = false;        // the file is written from now on (a start with arguments, a dev run, does not)
    bool pending = false;        // a session that did not end is waiting for the dialog
    bool haveWrite = false;
    Workspace recovered;
    std::chrono::steady_clock::time_point lastWrite;
};
SessionState gSession;

}  // namespace

Workspace workspaceCapture(const App& a, const std::string& name) {
    Workspace w;
    w.name = name;
    w.devName = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? a.devices[a.devIdx].name : std::string();
    w.family = a.family;
    if (const SubMode* sm = curSub(a)) w.subId = sm->id;
    w.freqMhz = a.freqMhz;
    w.sampleRateHz = a.tune.sampleRate;
    w.lnaDb = a.tune.lnaDb; w.vgaDb = a.tune.vgaDb; w.ampOn = a.tune.ampOn;
    for (const PanelFlag& p : kPanels) {
        if (!(a.*(p.flag))) continue;
        if (!w.panels.empty()) w.panels += ',';
        w.panels += p.name;
    }
    return w;
}

bool workspaceApply(App& a, const Workspace& w, std::string& why) {
    if (a.engine.running()) { why = "Stop the receiver before loading a workspace."; return false; }
    if (w.family < 0 || w.family >= kNumFamilies) { why = "The workspace's mode is not known to this version."; return false; }
    selectMode(a, w.family, w.subId.empty() ? nullptr : w.subId.c_str());   // the mode and its protocol entry, as the Mode menu sets them
    if (a.family != w.family) { why = "The workspace's mode is not available here."; return false; }
    if (!w.devName.empty()) {   // the radio this workspace was made with, when it is plugged in
        bool found = false;
        for (int i = 0; i < (int)a.devices.size() && !found; i++) if (a.devices[i].name == w.devName) { a.devIdx = i; found = true; }
        if (!found) why = "The radio " + w.devName + " is not connected; the current radio is kept. ";
    }
    if (w.freqMhz > 0) tuneFreq(a, w.freqMhz);   // the frequency, as the tuning field sets it
    if (w.sampleRateHz > 0) saveSampleRate(a, w.sampleRateHz);   // remembered for this mode and radio, as the Rate control does
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) {   // gains after the rate: the rate's setup does not change them
        a.tune.lnaDb = w.lnaDb; a.tune.vgaDb = w.vgaDb; a.tune.ampOn = w.ampOn;
        a.agcOn = false;   // a gain set by a workspace is not the AGC's to change, as with a gain set by hand
        if (const int sl = modeSlot(a); sl >= 0 && sl < kNumSlots) a.famGain[sl] = {w.lnaDb, w.vgaDb, w.ampOn, true};
    }
    {   // the windows: exactly the ones the workspace had open
        std::string rest = "," + w.panels + ",";
        for (const PanelFlag& p : kPanels) a.*(p.flag) = rest.find("," + std::string(p.name) + ",") != std::string::npos;
    }
    savePrefs(a);
    return true;
}

std::vector<Workspace> workspaceList() {
    std::vector<Workspace> v;
    const std::string st = plat::prefs().getS("workspaces", "");
    size_t pos = 0;
    while (pos < st.size()) {
        size_t e = st.find('\n', pos);
        if (e == std::string::npos) e = st.size();
        Workspace w;
        if (decodeRecord(st.substr(pos, e - pos), w)) v.push_back(w);
        pos = e + 1;
    }
    return v;
}

void workspaceStore(const std::vector<Workspace>& v) {
    std::string st;
    for (const Workspace& w : v) st += encodeRecord(w) + "\n";
    plat::prefs().setS("workspaces", st);
    plat::prefs().flush();
}

void workspaceSessionStart(const App& a, bool offerRestore) {
    if (!offerRestore) return;   // a run with arguments (the dev and test runs) neither reads nor writes the file
    bool clean = true;
    Workspace w;
    if (readSession(clean, w) && !clean) { gSession.pending = true; gSession.recovered = w; }
    gSession.logging = true;
    if (!gSession.pending) { writeSession(false, workspaceCapture(a, "last session")); gSession.haveWrite = true; gSession.lastWrite = std::chrono::steady_clock::now(); }
}

void workspaceSessionTick(const App& a) {
    if (!gSession.logging || gSession.pending) return;   // the crash record is kept until the dialog is answered
    const auto now = std::chrono::steady_clock::now();
    if (gSession.haveWrite && now - gSession.lastWrite < std::chrono::seconds(5)) return;
    writeSession(false, workspaceCapture(a, "last session"));
    gSession.haveWrite = true;
    gSession.lastWrite = now;
}

void workspaceSessionEnd(const App& a) {
    if (!gSession.logging || gSession.pending) return;
    writeSession(true, workspaceCapture(a, "last session"));
    gSession.logging = false;
}

bool workspaceRestorePending() { return gSession.pending; }
const Workspace& workspaceRecovered() { return gSession.recovered; }

void workspaceRestoreAnswer(App& a, bool restore, std::string& why) {
    if (!gSession.pending) return;
    gSession.pending = false;
    if (restore) { workspaceApply(a, gSession.recovered, why); }
    else why.clear();
    gSession.haveWrite = false;   // the next tick writes the session as it is now
}
