// The Ham view (third choice of the RX / TX / Ham switch): operation like WSJT-X on one screen. Always on top: the mode and band bars, the
// station, the licence box, Halt TX and Arm. Then the tabs: Operate (meters, the slot timeline, the decode list coloured by CQ / new DXCC /
// new grid / worked before, the QSO panel with the auto-sequence steps, the six messages, macros), Log, Awards, Map, Spots, Contest, Guide
// (ham_log_ui.cpp, ham_map_ui.cpp, ham_setup_ui.cpp). The logic is in core/include/dect2/ham/ (tested). Transmission goes only through the
// slot scheduler below and HamSlotTx, behind the gate hamTxAllowed (explicit Arm; no licence, a band outside the amateur bands, a country plan miss are warnings): docs/HAM_PLAN.md.
#include "../app.h"
#include "ham_kbd_ui.h"
#include "ham_ops_ui.h"
#include "ham_station_ui.h"
#include "../platform/platform.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_upload.h"
#include "dect2/audio/audioout.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/hamlib_rig.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace dect2;

namespace {
// The real rig: Hamlib PTT (when a rig is chosen; none = VOX / the radio keys itself from audio) and the chosen sound card output.
struct RealIo : ham::HamRigIo {
    HamlibLink link;
    int rigIdx = -1;            // index into hamlibRigs(), -1: no CAT PTT
    std::string device;         // audio output ("" = system default)
    AudioOut out;
    bool drained = false;
    bool ensureOpen(std::string& err) {
        if (rigIdx < 0) { err = "no rig chosen"; return false; }
        if (!link.isOpen()) {
            const auto rigs = hamlibRigs();
            if (rigIdx >= (int)rigs.size()) { err = "the chosen rig is not in the rig list"; return false; }
            HamlibConn c; c.model = rigs[rigIdx].model; c.path = rigs[rigIdx].path; c.baud = rigs[rigIdx].baud;
            if (!link.open(c, err)) return false;
        }
        return true;
    }
    bool ptt(bool on, std::string& err) override {
        if (rigIdx < 0) return true;
        if (!ensureOpen(err)) return false;
        return link.setPtt(on, err);
    }
    bool beginAudio(int rate, std::string& err) override {
        out.setDevice(device);
        drained = false;
        if (!out.start(rate)) { err = "cannot open the sound card output"; return false; }
        out.setStartThreshold(rate / 20);
        return true;
    }
    int write(const float* m, int n) override {
        std::vector<float> st((size_t)n * 2);
        for (int i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = m[i];
        return out.write(st.data(), n);
    }
    int pending() override {
        const int b = out.bufferedFrames();
        if (b == 0 && !drained) { drained = true; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }   // the device's own buffer
        return b;
    }
    void flushAudio() override { out.flush(); }
    void endAudio() override { out.stop(); }
};

struct HamUi {
    bool loaded = false, view = false, armed = false, licence = false, askLicence = false;
    char call[16] = {}, grid[8] = {};
    int mode = 0;                       // hfdig mode number: 0 FT8, 1 FT4, 2 FT2, 3 WSPR (beacon), 4 JT65, 5 JT9, 10 JT4
    int sub = 0;                        // JT65 A B C, JT4 A .. G
    int kbdIdx = -1;                    // a keyboard mode (ham::kbdPresets index) instead of the slot modes
    ham::WsprBeacon beacon;
    bool txBeacon = false;              // the running burst is a beacon one
    float txHz = 1500;
    ham::QsoEngine eng;
    ham::WorkedLog worked;
    std::string logPath, status, lastTx, lastLog;
    double lastFed = 0, lastSpot = 0;
    long lastTxSlot = -1;
    int logged = 0;
    RealIo io;
    ham::HamSlotTx tx{io};
    std::thread worker;
    std::atomic<int> txState{0};      // 0 none, 1 running, 2 sent, 3 failed
    std::mutex mu;
    std::string txErr, txMsg;
    double txSlot = 0;
    int rigSel = -1;
    float level = 0.4f;
    bool fakeIt = false;                // 4.0: WSJT-X "fake it" split: the rig's dial moves in 500 Hz steps so the TX audio stays at 1500 .. 2000 Hz
    ham::HamUploader up;                // logbook uploads: off by default, see uploadSection()
    ham::UpConfig upCfg;
    char qrzKey[64] = {}, clubEmail[64] = {}, clubPw[64] = {}, clubApi[64] = {}, eqslUser[32] = {}, eqslPw[64] = {};
    bool upFieldsInit = false;
    // the operating screen
    int tab = HamTabOperate;
    std::string selMsg, selCall, selGrid;   // the decode picked in the list
    double selSlot = 0; int selSnr = 0; float selHz = 0;
    std::string dxGrid;                 // the grid of the station of the running QSO (from its decodes)
    std::string override;               // a message the operator chose for the next own slot (Tx1 .. Tx6, free text, a macro)
    bool txOverride = false;            // the running burst is that message (the engine does not count it)
    char freeText[40] = {};
    std::map<long, char> slots;         // slot index -> 'T' sent, 'F' not sent, 'R' decodes heard
    long long lastLogId = 0;
    ~HamUi() { tx.abort(); if (worker.joinable()) worker.join(); }
};
HamUi& H() { static HamUi h; return h; }

struct HamMode { int num; const char* name; const char* subId; };
const HamMode kHam[] = {{0, "FT8", "ft8"}, {1, "FT4", "ft4"}, {2, "FT2", "ft2"}, {6, "Q65", "q65"}, {7, "FST4", "fst4"}, {8, "MSK144", "msk144"},
                        {4, "JT65", "jt65"}, {5, "JT9", "jt9"}, {10, "JT4", "jt4"}, {3, "WSPR beacon", "wspr"}};
// the catalog id of the mode on screen (ham_station.h): the frequency memory is kept per band and per this id
std::string modeIdOf(const HamUi& h) {
    if (h.kbdIdx >= 0) {
        const auto& kp = ham::kbdPresets();
        const ham::HamModeInfo* m = ham::hamModeByKbd(kp[(size_t)h.kbdIdx].id);
        return m ? m->id : kp[(size_t)h.kbdIdx].id;
    }
    const ham::HamModeInfo* m = ham::hamModeBySlot(h.mode);
    return m ? m->id : "ft8";
}
// after a mode change: the frequency remembered for this band and the new mode, else its usual one
void tuneForMode(App& a, const HamUi& h) {
    const std::string b = ham::rxBandNameFor(a.freqMhz * 1e6);
    if (b.empty()) return;
    const double usual = (h.kbdIdx >= 0 ? ham::usualDialMhz(b, modeIdOf(h), hamOps().region) : ham::dialFor(b, h.mode)) * 1e6;
    const double t = hamStation().mem.recall(b, modeIdOf(h), usual);
    if (t > 0 && std::fabs(t - a.freqMhz * 1e6) > 1) tuneFreq(a, t / 1e6);
}

void logRecord(HamUi& h, const ham::QsoRecord& r) {
    std::string err;
    ham::LogQso q = hamFromRecord(r);
    if (hamLogQso(q, err)) {
        h.worked.add(r); h.logged++; h.lastLog = r.call;
        h.lastLogId = hamOps().log.qsos().empty() ? 0 : hamOps().log.qsos().back().id;
        mapHamRemember(r.myCall, r.myGrid); mapHamRemember(r.call, r.grid);   // the public map's ham contacts (only if chosen): callsigns, locators, mode, frequency; never the RST or notes
        mapHamHeard(r.mode.c_str(), r.freqHz, r.myCall, std::string(), r.call, r.endUtc > 0 ? r.endUtc : (double)time(nullptr));
        hamToast("Logged " + r.call);
    } else h.status = err;
    h.up.enqueue(r, (long long)time(nullptr));   // queued only; nothing leaves unless a service is on and auto upload is ticked
}

void load(HamUi& h) {
    if (h.loaded) return;
    h.loaded = true;
    hamOpsLoad();
    plat::Prefs& p = plat::basePrefs();
    snprintf(h.call, sizeof h.call, "%s", p.getS("ham.call", "").c_str());
    snprintf(h.grid, sizeof h.grid, "%s", p.getS("ham.grid", "").c_str());
    h.licence = p.getB("ham.licence", false);
    h.rigSel = (int)p.getI("ham.rig", -1);
    h.io.device = p.getS("ham.audioOut", "");
    h.txHz = (float)p.getD("ham.txHz", 1500);
    h.fakeIt = p.getB("ham.fakeIt", false);
    h.logPath = plat::dataDir() + "/onair_ham.adi";
    h.worked.load(h.logPath);
    h.eng.onLog = [&h](const ham::QsoRecord& q) { logRecord(h, q); };
    h.upCfg.autoUpload = p.getB("ham.up.auto", false);
    h.upCfg.on[0] = p.getB("ham.up.qrz", false); h.upCfg.on[1] = p.getB("ham.up.club", false); h.upCfg.on[2] = p.getB("ham.up.eqsl", false);
    h.upCfg.qrzKey = p.getS("ham.up.qrzKey", ""); h.upCfg.clubEmail = p.getS("ham.up.clubEmail", ""); h.upCfg.clubPassword = p.getS("ham.up.clubPw", "");
    h.upCfg.clubApiKey = p.getS("ham.up.clubApi", ""); h.upCfg.eqslUser = p.getS("ham.up.eqslUser", ""); h.upCfg.eqslPassword = p.getS("ham.up.eqslPw", "");
    h.up.load(plat::dataDir() + "/onair_ham_upload.tsv");
    h.up.setConfig(h.upCfg);
    h.up.startWorker();
    h.eng.onEvent = [&h](const std::string& e) { h.status = e == "watchdog" ? "Watchdog: no operator action for too long, TX halted" : "No answer: gave up the call"; h.armed = false; };
    if (!hamOps().setupDone) hamOps().openWizard = true;   // the first visit: the station setup
}
void save(HamUi& h) {
    plat::Prefs& p = plat::basePrefs();
    p.setS("ham.call", h.call); p.setS("ham.grid", h.grid); p.setB("ham.licence", h.licence); p.setD("ham.txHz", h.txHz);
    p.flush();
}
void uploadSection(HamUi& h) {
    if (!ImGui::CollapsingHeader(trw("Logbook uploads (off by default)"))) return;
    plat::Prefs& p = plat::basePrefs();
    auto fld = [](char* b, size_t n, const std::string& v) { snprintf(b, n, "%s", v.c_str()); };
    if (!h.upFieldsInit) {
        h.upFieldsInit = true;
        fld(h.qrzKey, sizeof h.qrzKey, h.upCfg.qrzKey); fld(h.clubEmail, sizeof h.clubEmail, h.upCfg.clubEmail); fld(h.clubPw, sizeof h.clubPw, h.upCfg.clubPassword);
        fld(h.clubApi, sizeof h.clubApi, h.upCfg.clubApiKey); fld(h.eqslUser, sizeof h.eqslUser, h.upCfg.eqslUser); fld(h.eqslPw, sizeof h.eqslPw, h.upCfg.eqslPassword);
    }
    ImGui::TextWrapped("%s", tr("Privacy: when a service is switched on, each logged QSO is sent to it over HTTPS: the other station's callsign and grid, mode, band and frequency, the reports, the UTC time, and your own callsign and grid. Nothing else, and nothing at all while a service is off. Your keys and passwords stay on this computer and go only to that service. LoTW is not supported (it needs TQSL certificates): export the ADIF log and sign it with TQSL."));
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Upload automatically after logging"), &h.upCfg.autoUpload);
    ImGui::Separator();
    ch |= ImGui::Checkbox("QRZ.com Logbook", &h.upCfg.on[0]);
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputText(tr("QRZ API key"), h.qrzKey, sizeof h.qrzKey, ImGuiInputTextFlags_Password)) { h.upCfg.qrzKey = h.qrzKey; ch = true; }
    ch |= ImGui::Checkbox("Club Log", &h.upCfg.on[1]);
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log email"), h.clubEmail, sizeof h.clubEmail)) { h.upCfg.clubEmail = h.clubEmail; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log app password"), h.clubPw, sizeof h.clubPw, ImGuiInputTextFlags_Password)) { h.upCfg.clubPassword = h.clubPw; ch = true; }
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::InputText(tr("Club Log API key"), h.clubApi, sizeof h.clubApi, ImGuiInputTextFlags_Password)) { h.upCfg.clubApiKey = h.clubApi; ch = true; }
    ch |= ImGui::Checkbox("eQSL", &h.upCfg.on[2]);
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("eQSL user"), h.eqslUser, sizeof h.eqslUser)) { h.upCfg.eqslUser = h.eqslUser; ch = true; }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::InputText(tr("eQSL password"), h.eqslPw, sizeof h.eqslPw, ImGuiInputTextFlags_Password)) { h.upCfg.eqslPassword = h.eqslPw; ch = true; }
    if (ch) {
        p.setB("ham.up.auto", h.upCfg.autoUpload); p.setB("ham.up.qrz", h.upCfg.on[0]); p.setB("ham.up.club", h.upCfg.on[1]); p.setB("ham.up.eqsl", h.upCfg.on[2]);
        p.setS("ham.up.qrzKey", h.upCfg.qrzKey); p.setS("ham.up.clubEmail", h.upCfg.clubEmail); p.setS("ham.up.clubPw", h.upCfg.clubPassword);
        p.setS("ham.up.clubApi", h.upCfg.clubApiKey); p.setS("ham.up.eqslUser", h.upCfg.eqslUser); p.setS("ham.up.eqslPw", h.upCfg.eqslPassword);
        p.flush();
        h.up.setConfig(h.upCfg);
    }
    if (ImGui::Button(tr("Upload now"))) h.up.uploadNow((long long)time(nullptr));
    ImGui::SameLine(); ImGui::TextDisabled("%s: %d", tr("Waiting"), (int)h.up.pendingCount());
    // result per QSO (newest first, last 12): U uploaded, F failed with the reason, . waiting, - skipped (service was off)
    const auto its = h.up.items();
    int shown = 0;
    for (int i = (int)its.size() - 1; i >= 0 && shown < 12; i--, shown++) {
        std::string line = its[i].q.call;
        for (int k = 0; k < ham::kUpServices; k++) {
            const ham::UpState& s = its[i].s[k];
            line += std::string("  ") + ham::upServiceName((ham::UpService)k) + ": " + (s.st == 'u' ? "uploaded" : s.st == 'f' ? "failed: " + s.reason : s.st == 'n' ? "not sent" : s.reason.empty() ? "waiting" : "retrying: " + s.reason);
        }
        ImGui::TextUnformatted(line.c_str());
    }
}

std::string hhmmss(double utc) {
    char b[16];
    const time_t t = (time_t)utc;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    snprintf(b, sizeof b, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

// a level bar: 0 .. 1, green / yellow / red
void meter(const char* label, float v, bool on, const char* text) {
    ImGui::TextUnformatted(label); ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos(), sz(110 * gUi, ImGui::GetTextLineHeight());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), IM_COL32(40, 40, 40, 255), 2);
    v = std::max(0.f, std::min(1.f, v));
    const ImU32 c = !on ? IM_COL32(90, 90, 90, 255) : v > 0.9f ? IM_COL32(230, 70, 60, 255) : v > 0.75f ? IM_COL32(230, 200, 60, 255) : IM_COL32(80, 200, 100, 255);
    if (v > 0) dl->AddRectFilled(p, ImVec2(p.x + sz.x * v, p.y + sz.y), c, 2);
    dl->AddText(ImVec2(p.x + 4 * gUi, p.y), IM_COL32(255, 255, 255, 230), text);
    ImGui::Dummy(sz);
}

// the slot timeline: the last 8 slots and the next 4 (blue heard, red sent, orange not sent, outline: own TX slots while a QSO runs)
void timeline(HamUi& h, double now, double per, bool planned) {
    const long cur = ham::slotIndex(now, per);
    const int past = 8, ahead = 4, n = past + 1 + ahead;
    const float w = std::max(240 * gUi, ImGui::GetContentRegionAvail().x), bw = w / n, ht = ImGui::GetTextLineHeight() * 1.4f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; i++) {
        const long s = cur - past + i;
        const ImVec2 a(p.x + i * bw + 1, p.y), b(p.x + (i + 1) * bw - 1, p.y + ht);
        const bool even = ham::slotEven(ham::slotStart(s, per), per), mine = even == h.eng.config().txEven;
        auto it = h.slots.find(s);
        const char k = it == h.slots.end() ? 0 : it->second;
        ImU32 fill = IM_COL32(45, 45, 50, 255);
        if (k == 'T') fill = IM_COL32(200, 60, 55, 255); else if (k == 'F') fill = IM_COL32(210, 130, 40, 255); else if (k == 'R') fill = IM_COL32(50, 100, 170, 255);
        dl->AddRectFilled(a, b, fill, 2);
        if (s == cur) {   // progress through the current slot
            const float f = (float)((now - ham::slotStart(s, per)) / per);
            dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * std::min(1.f, f), b.y), (h.txState == 1 ? IM_COL32(255, 80, 70, 120) : IM_COL32(255, 255, 255, 45)), 2);
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 200), 2, 0, 1.5f * gUi);
        }
        if (s > cur && planned && mine) dl->AddRect(a, b, IM_COL32(230, 90, 80, 230), 2, 0, 2 * gUi);
        char t[8]; snprintf(t, sizeof t, "%s", s == cur ? "now" : even ? "E" : "O");
        dl->AddText(ImVec2(a.x + 3 * gUi, a.y + 2 * gUi), IM_COL32(230, 230, 230, 200), t);
    }
    ImGui::Dummy(ImVec2(w, ht));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Slots: blue heard, red sent, orange not sent; outlined: your next transmissions; E / O even / odd"));
    while (!h.slots.empty() && h.slots.begin()->first < cur - 64) h.slots.erase(h.slots.begin());
}

// the CQ worth answering most in the newest slot: new DXCC, new on band, new grid, not worked, then the strongest
const FtxDecode* bestCq(const HfdigFtxTelemetry& f, int mode, const std::string& band, const std::string& me) {
    HamOps& o = hamOps();
    double newest = 0;
    for (const auto& d : f.decodes) if (d.mode == mode) newest = std::max(newest, d.slotUtc);
    const FtxDecode* best = nullptr;
    double bs = -1e9;
    for (const auto& d : f.decodes) {
        if (d.mode != mode || d.slotUtc < newest || !d.cq || d.call.empty() || d.call == me) continue;
        const std::string md = ham::hamModeName(mode);
        if (o.contestOn && o.contest.dupe(o.log, d.call, band, md)) continue;
        const ham::DecodeMarks m = o.worked.mark(d.call, d.grid, band, md, hamDxccKey(d.call));
        const double s = (m.newDxcc ? 1000 : 0) + (m.newDxccBand ? 500 : 0) + (m.newGrid ? 200 : 0) + (m.newGridBand ? 50 : 0) + (m.workedCallBand ? 0 : 100) + d.snrDb;
        if (s > bs) { bs = s; best = &d; }
    }
    return best;
}

// the free text the operator typed (or a macro): a standard message stays, anything else is FT8 free text (13 characters)
std::string txText(const std::string& s) {
    std::string u = s;
    for (char& c : u) c = (char)toupper((unsigned char)c);
    while (!u.empty() && u.back() == ' ') u.pop_back();
    if (u.empty()) return "";
    return ham::parseMessage(u).kind == ham::ParsedMsg::Other ? ham::msgFree(u) : u;
}
}  // namespace

bool& hamViewFlag() { static bool init = (H().view = getenv("DECT2_HAMVIEW") != nullptr, true); (void)init; return H().view; }

void hamPanel(App& a) {
    HamUi& h = H();
    HamOps& o = hamOps();
    load(h);
    hamStationLoad();
    const double now = (double)time(nullptr);
    ham::QsoConfig& c = h.eng.config();
    c.myCall = h.call; c.myGrid = h.grid; c.mode = h.mode;
    h.beacon.cfg.myCall = h.call; h.beacon.cfg.myGrid = h.grid;
    o.myCall = h.call; o.myGrid = h.grid;
    const bool isBeacon = h.mode == 3, isKbd = h.kbdIdx >= 0;
    const std::string band = ham::bandNameFor(a.freqMhz * 1e6), modeName = ham::hamModeName(h.mode), rxBand = ham::rxBandNameFor(a.freqMhz * 1e6);   // rxBand: also names 11 m (CB), which can transmit with a warning
    c.cqMod = hamStation().cqMod;   // CQ DX / CQ POTA ... (4.0)
    c.contest = hamStation().ftxContest; c.myExch = hamStation().ftxExch;   // FT8 Field Day / RTTY Roundup exchange (4.0)
    if (c.contest && c.myExch.empty()) c.contest = 0;
    h.eng.priority = [band, modeName](const std::string& cs) { return hamNeedOf(cs, "", band, modeName, false).level; };   // several callers: the most needed first
    if (o.workedDirty) { o.worked.build(o.log.qsos()); o.workedDirty = false; }
    const bool stationOk = ham::hamCallValid(h.call) && ham::hamGridValid(h.grid);
    const bool canCq = stationOk;   // a callsign and a locator are needed to build the messages; the licence box is a reminder, not a lock

    HfdigFtxTelemetry f;
    a.engine.hfdig().ftx().telemetry(f);
    auto halt = [&] { h.eng.halt(); h.beacon.stop(); h.armed = false; h.tx.abort(); h.override.clear(); h.status = "Halted"; };
    auto callIt = [&](const std::string& call, const std::string& grid, int snr, double slot, double hz) {
        if (!canCq || isBeacon || isKbd || call.empty()) return;
        h.eng.callStation(now, call, grid, snr, slot, hz);
        h.dxGrid = grid; h.status.clear(); h.override.clear();
    };
    auto answerBest = [&] {
        const FtxDecode* d = bestCq(f, h.mode, band, h.call);
        if (d) callIt(d->call, d->grid, (int)std::lround(d->snrDb), d->slotUtc, d->hz); else hamToast("No CQ to answer in the last slot");
    };
    auto setBand = [&](const std::string& b) {
        const std::string mid = modeIdOf(h);
        const double mhz = hamStation().mem.recall(b, mid, ham::dialFor(b, h.mode) * 1e6) / 1e6;   // where you were last on that band in this mode (4.0)
        if (mhz <= 0) return;
        if (!rxBand.empty()) hamStation().mem.remember(rxBand, mid, a.freqMhz * 1e6);
        if (h.eng.active()) h.eng.halt();
        tuneFreq(a, mhz);
        h.lastFed = 0;
    };

    // ---- keyboard shortcuts ----
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool typing = io.WantTextInput;
        const bool focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (!popup && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) halt();
        if (!popup && !typing) {
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) o.wantTab = HamTabGuide;
            if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) o.openWizard = true;
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) o.wantTab = HamTabLog;
            if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) { h.eng.halt(); h.override.clear(); h.dxGrid.clear(); h.status = "DX call cleared"; }
            if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) o.wantTab = HamTabMap;
            if (ImGui::IsKeyPressed(ImGuiKey_F6, false) && canCq && !isBeacon && !isKbd) { h.eng.startCq(now); c.txHz = h.txHz; h.status.clear(); }
            if (ImGui::IsKeyPressed(ImGuiKey_F7, false) && !isBeacon && !isKbd) answerBest();
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) o.wantTab = HamTabSpots;
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L, false)) { o.wantTab = HamTabLog; o.openAdd = true; }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) { o.wantTab = HamTabLog; o.focusSearch = true; }
            if (focus && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && o.log.undo()) { std::string e; o.log.save(o.logPath, e); o.workedDirty = true; hamToast("Undone"); }
            if (focus && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false) && o.log.redo()) { std::string e; o.log.save(o.logPath, e); o.workedDirty = true; hamToast("Redone"); }
            if (focus && io.KeyAlt && !isKbd && (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))) {
                const auto& bd = ham::bandDials();
                int cur = -1;
                for (int i = 0; i < (int)bd.size(); i++) if (rxBand == bd[(size_t)i].band) cur = i;
                const int step = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : -1;
                for (int i = cur + step; i >= 0 && i < (int)bd.size(); i += step) if (ham::dialFor(bd[(size_t)i].band, h.mode) > 0) { setBand(bd[(size_t)i].band); break; }
            }
        }
    }

    // ---- the frequency and the meter (4.0) ----
    {
        const ham::HamModeInfo* mi = ham::hamModeInfo(modeIdOf(h));
        hamStationHeader(a, isKbd ? 0.0 : c.txHz, mi ? ham::modeClass(mi->adifMode) : std::string(), a.rx.hfdig.audioDb);
    }
    // ---- the mode bar ----
    for (const HamMode& hm : kHam) {
        if (&hm != kHam) ImGui::SameLine();
        if (ImGui::RadioButton(hm.name, h.kbdIdx < 0 && h.mode == hm.num) && (h.mode != hm.num || h.kbdIdx >= 0)) {
            hamRememberFreq(a, modeIdOf(h));
            h.kbdIdx = -1;
            h.mode = hm.num; h.sub = 0; h.eng.halt(); h.beacon.stop(); h.lastFed = 0; h.override.clear();
            for (const auto& m : allModes()) if (m.family == 25 && m.subId && !strcmp(m.subId, hm.subId)) { selectMode(a, m.family, m.subId); break; }
            a.engine.hfdig().ftx().setModeOption(h.mode, 0, 0);
            tuneForMode(a, h);
        }
    }
    {   // keyboard modes
        const auto& kp = ham::kbdPresets();
        const std::string cur = h.kbdIdx >= 0 ? kp[(size_t)h.kbdIdx].name : "Keyboard modes";
        ImGui::SameLine(); ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##kbdmode", cur.c_str())) {
            for (int i = 0; i < (int)kp.size(); i++)
                if (ImGui::Selectable(kp[(size_t)i].name, h.kbdIdx == i)) {
                    hamRememberFreq(a, modeIdOf(h));
                    h.kbdIdx = i; h.eng.halt(); h.beacon.stop(); h.override.clear(); hamKbdSelect(a, i);
                    tuneForMode(a, h);
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        hamMoreModesCombo(a);   // JS8Call, SSTV, digital voice, packet, SSB / AM / FM: their own screens
    }
    if (h.kbdIdx < 0 && (h.mode == 4 || h.mode == 6 || h.mode == 10)) {   // sub-mode: JT65 A B C, Q65 A .. E, JT4 A .. G
        int sb = h.sub;
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
        if (ImGui::Combo(tr("Sub-mode"), &sb, h.mode == 4 ? "A\0B\0C\0" : h.mode == 6 ? "A\0B\0C\0D\0E\0" : "A\0B\0C\0D\0E\0F\0G\0") && sb != h.sub) {
            h.sub = sb; h.eng.halt();
            a.engine.hfdig().ftx().setModeOption(h.mode, sb, 0);
        }
    }

    // ---- the band bar: one click per band, the frequency remembered per band and mode (4.0, ham_station_ui.cpp) ----
    if (hamBandBar(a, modeIdOf(h), isKbd ? -1 : h.mode)) { if (h.eng.active()) h.eng.halt(); h.lastFed = 0; }
    if (!isKbd && o.region > 0 && !band.empty() && !ham::regionAllows(o.region, a.freqMhz * 1e6 + c.txHz, ham::hamModeBwHz(h.mode, h.sub))) {
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "outside the Region %d band plan", o.region);
    } else if (!isKbd && band.empty()) {
        ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "%s", tr("not an amateur band: you are responsible for being allowed to transmit here"));
    }

    // ---- station ----
    ImGui::SetNextItemWidth(110 * gUi);
    bool ch = ImGui::InputText(tr("Callsign"), h.call, sizeof h.call, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    ch |= ImGui::InputText(tr("Grid"), h.grid, sizeof h.grid, ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
    if (ch) save(h);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Setup"))) o.openWizard = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Station setup: callsign, grid, licence, band plan, power, antenna, rig and audio (F2)"));
    ImGui::SameLine();
    if (ImGui::SmallButton("?")) o.wantTab = HamTabGuide;
    if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); hamShortcutsHelp(); ImGui::EndTooltip(); }
    if (!stationOk) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Enter your callsign and a 4 or 6 character grid locator."));

    // licence confirmation, stored; asked before the first TX
    if (ImGui::Checkbox(tr("I hold an amateur licence for this callsign and will follow its rules"), &h.licence)) save(h);
    if (!h.licence) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("No licence set: you are responsible for being allowed to transmit.")); }

    auto rigWidgets = [&h] {   // rig and audio output, level (shared with the keyboard modes)
        const auto rigs = hamlibRigs();
        const std::string curS = h.rigSel >= 0 && h.rigSel < (int)rigs.size() ? hamlibRigDeviceName(rigs[h.rigSel]) : "none (VOX / audio only)";
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::BeginCombo(tr("PTT rig"), curS.c_str())) {
            if (ImGui::Selectable("none (VOX / audio only)", h.rigSel < 0)) { h.rigSel = -1; plat::basePrefs().setI("ham.rig", -1); }
            for (int i = 0; i < (int)rigs.size(); i++) if (ImGui::Selectable(hamlibRigDeviceName(rigs[i]).c_str(), h.rigSel == i)) { h.rigSel = i; plat::basePrefs().setI("ham.rig", i); h.io.link.close(); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(220 * gUi);
        const std::string dn = h.io.device.empty() ? "system default" : h.io.device;
        if (ImGui::BeginCombo(tr("Audio out"), dn.c_str())) {
            if (ImGui::Selectable("system default", h.io.device.empty())) { h.io.device.clear(); plat::basePrefs().setS("ham.audioOut", ""); }
            for (const auto& d : AudioOut::devices()) if (!d.empty() && ImGui::Selectable(d.c_str(), d == h.io.device)) { h.io.device = d; plat::basePrefs().setS("ham.audioOut", d); }
            ImGui::EndCombo();
        }
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
        ImGui::SliderFloat(tr("Level"), &h.level, 0.05f, 0.9f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Audio level into the rig. Set it so the rig shows no ALC action and the power stays low."));
        ImGui::SameLine();
        ImGui::BeginDisabled(h.rigSel < 0);
        if (ImGui::Checkbox(tr("Split (fake it)"), &h.fakeIt)) { plat::basePrefs().setB("ham.fakeIt", h.fakeIt); plat::basePrefs().flush(); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Slot modes with a CAT rig: for each transmission the rig's dial moves in 500 Hz steps so your audio stays between 1500 and 2000 Hz (cleaner, no harmonics in the passband), and comes back after. The RF frequency is the same."));
    };
    hamSetupWizard(a, h.call, sizeof h.call, h.grid, sizeof h.grid, h.licence, rigWidgets, [&h] { save(h); });

    // ---- Halt and Arm: always on screen (the keyboard modes have theirs in their own panel) ----
    if (!isKbd) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.1f, 1));
        if (ImGui::Button(tr("Halt TX"))) halt();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stops at once: the burst is cut, PTT released, the QSO and the beacon stopped, TX disarmed (Esc)"));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, h.armed ? ImVec4(0.75f, 0.1f, 0.1f, 1) : ImVec4(0.3f, 0.3f, 0.3f, 1));
        if (ImGui::Button(h.armed ? tr("ARMED: click to disarm") : tr("Arm TX"))) { h.armed = !h.armed; h.eng.userAction(now); }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Nothing is sent unless this is armed. Without a licence, outside the amateur bands or outside your country's plan OnAir warns you and still sends: you are responsible. Disarmed again when you halt or the watchdog fires."));
        ImGui::SameLine();
        if (h.txState == 1) ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "TX  %s", h.txMsg.c_str());
        else if (!h.status.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", h.status.c_str());
    }
    if (!o.toast.empty() && now - o.toastAt < 6) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.5f, 0.85f, 1, 1), "%s", o.toast.c_str()); }

    // ---- decodes in: the engine, the spots list, the slot timeline ----
    const double per = ham::slotPeriod(h.mode);
    if (!isKbd) {
        double newest = h.lastFed;
        for (const auto& d : f.decodes) {
            if (d.mode != h.mode || d.slotUtc <= h.lastFed) continue;
            h.eng.onDecode(now, d.msg, (int)std::lround(d.snrDb), d.slotUtc, d.hz);
            if (!d.call.empty() && d.call == h.eng.dxCall() && !d.grid.empty()) h.dxGrid = d.grid;
            {   // the public map's ham contacts (Server > Share ham contacts; ignored unless chosen): who is talking to whom, from the message text only
                const double rf = (d.dialHz > 0 ? d.dialHz : a.freqMhz * 1e6) + d.hz;
                mapHamRemember(h.call, h.grid);
                if (d.mode == 3) { if (!d.call.empty() && !d.grid.empty()) mapHamHeard("WSPR", rf, d.call, d.grid, h.call, d.slotUtc); }
                else {
                    const ham::ParsedMsg pm = ham::parseMessage(d.msg);
                    if (!d.call.empty() && !d.grid.empty()) mapHamRemember(d.call, d.grid);
                    if (pm.kind != ham::ParsedMsg::Cq && pm.kind != ham::ParsedMsg::Other && !pm.from.empty() && !pm.to.empty())
                        mapHamHeard(ham::hamModeName(d.mode), rf, pm.from, pm.kind == ham::ParsedMsg::Grid ? pm.grid : std::string(), pm.to, d.slotUtc);
                }
            }
            { char& k = h.slots[ham::slotIndex(d.slotUtc + 0.01, per)]; if (k != 'T' && k != 'F') k = 'R'; }
            newest = std::max(newest, d.slotUtc);
        }
        h.lastFed = newest;
    }
    {
        double newest = h.lastSpot;
        for (const auto& d : f.decodes) {
            if (d.slotUtc <= h.lastSpot || d.call.empty()) continue;
            const double dial = d.dialHz > 0 ? d.dialHz : a.freqMhz * 1e6;
            o.spots.heard(d.call, d.grid, ham::bandNameFor(dial), ham::hamModeName(d.mode), d.slotUtc, dial, d.hz, (int)std::lround(d.snrDb), d.cq, d.msg);
            hamHopSpot(ham::bandNameFor(dial), d.slotUtc);   // the band hopper's "auto" follows your own decodes
            newest = std::max(newest, d.slotUtc);
        }
        h.lastSpot = newest;
    }
    o.dxCall = h.eng.dxCall(); o.dxGrid = h.eng.dxCall().empty() ? "" : h.dxGrid;

    // a finished transmission: only now does the engine advance
    if (h.txState == 2 || h.txState == 3) {
        if (h.worker.joinable()) h.worker.join();
        std::lock_guard<std::mutex> l(h.mu);
        if (h.txState == 2) {
            if (h.txBeacon) h.beacon.txSent(); else if (!h.txOverride) h.eng.txSent(h.txSlot);
            h.lastTx = "Sent: " + h.txMsg;
        } else h.lastTx = "Not sent (" + h.txErr + "): " + h.txMsg;
        h.slots[ham::slotIndex(h.txSlot + 0.01, per)] = h.txState == 2 ? 'T' : 'F';
        h.txState = 0; h.txOverride = false;
    }
    if (!h.armed && h.tx.busy()) h.tx.abort();   // disarm, watchdog and Halt stop a running burst and release PTT at once
    h.io.rigIdx = h.rigSel;
    // band hopping (4.0): only while nothing runs, at a slot boundary
    if (!isKbd && !isBeacon && !h.eng.active() && h.override.empty() && h.txState == 0) {
        HamStation& st = hamStation();
        const long hi = ham::slotIndex(now, per);
        if (st.hopOn && hi != st.lastHopSlot) {
            st.lastHopSlot = hi;
            const std::string nb = hamHopBand(ham::slotStart(hi, per), per, rxBand);
            if (!nb.empty() && ham::dialFor(nb, h.mode) > 0) { setBand(nb); hamToast("Band hop: " + nb); }
        }
    }
    // the slot scheduler: the one place a slot-mode transmission starts, always through the gate
    if (!isKbd) {
        const long idx = ham::slotIndex(now, per);
        const bool due = isBeacon ? h.beacon.active() : (h.eng.active() || !h.override.empty());
        if (due && idx != h.lastTxSlot && now - ham::slotStart(idx, per) >= ham::txStartDelay(h.mode)) {
            h.lastTxSlot = idx;
            const double ss = ham::slotStart(idx, per);
            std::string msg;
            bool ovr = false;
            if (!isBeacon && !h.override.empty()) { if (ham::slotEven(ss, per) == c.txEven) { msg = h.override; ovr = true; h.override.clear(); } }
            else msg = isBeacon ? h.beacon.messageForSlot(ss) : h.eng.messageForSlot(ss);
            if (!msg.empty() && h.txState == 0) {
                ham::HamTxCheck g;
                g.licenceConfirmed = h.licence; g.armed = h.armed; g.call = h.call; g.grid = h.grid;
                g.dialHz = a.freqMhz * 1e6; g.audioHz = isBeacon ? h.beacon.audioHz() : c.txHz; g.bwHz = ham::hamModeBwHz(h.mode, h.sub);
                std::string why;
                if (!ham::hamTxAllowed(g, why)) { h.lastTx = "Not sent (" + why + "): " + msg; h.slots[idx] = 'F'; }
                else {
                    if (!why.empty()) h.status = "Warning: " + why;   // sent anyway: the operator is responsible
                    h.txMsg = msg; h.txSlot = ss; h.txState = 1; h.txBeacon = isBeacon; h.txOverride = ovr;
                    const double hz = g.audioHz, lv = h.level; const int md = h.mode, sb = h.sub;
                    const bool fake = h.fakeIt && h.rigSel >= 0 && !isBeacon;
                    h.worker = std::thread([&h, g, md, msg, hz, lv, sb, fake] {
                        std::string err;
                        ham::HamTxCheck gg = g;
                        double ahz = hz;
                        bool moved = false;
                        if (fake) {   // the same RF frequency: dial + shift, audio - shift (4.0)
                            const ham::SplitTx sp = ham::fakeItSplit(hz);
                            std::string e2;
                            if (sp.dialShiftHz != 0 && h.io.ensureOpen(e2) && h.io.link.setFreq(g.dialHz + sp.dialShiftHz, e2)) { gg.dialHz = g.dialHz + sp.dialShiftHz; ahz = sp.audioHz; moved = true; }
                        }
                        const bool ok = h.tx.send(gg, md, msg, ahz, lv, err, sb);
                        if (moved) { std::string e2; h.io.link.setFreq(g.dialHz, e2); }
                        std::lock_guard<std::mutex> l(h.mu);
                        h.txErr = err;
                        h.txState = ok ? 2 : 3;
                    });
                }
            }
        }
    }

    // ---- the tabs ----
    if (!ImGui::BeginTabBar("##hamtabs")) return;
    static const char* kTab[] = {"Operate", "Log", "Awards", "Map", "Spots", "Cluster", "Contest", "Activate", "Guide"};   // the order of HamTab
    const int want = o.wantTab;
    o.wantTab = -1;
    for (int t = 0; t < HamTabCount; t++) {
        char lbl[48];
        snprintf(lbl, sizeof lbl, "%s%s###hamtab%d", tr(kTab[t]), t == HamTabContest && o.contestOn ? " (on)" : "", t);
        if (!ImGui::BeginTabItem(lbl, nullptr, want == t ? ImGuiTabItemFlags_SetSelected : 0)) continue;
        h.tab = t;
        ImGui::BeginChild("##hamtabbody", ImVec2(0, 0));
        switch (t) {
        case HamTabLog: hamLogTab(a); break;
        case HamTabAwards: hamAwardsTab(a); break;
        case HamTabMap: hamMapTab(a); break;
        case HamTabSpots: hamSpotsTab(a, [&](const std::string& cs, const std::string& gr) {
            for (size_t i = f.decodes.size(); i-- > 0;) if (f.decodes[i].call == cs && f.decodes[i].mode == h.mode) { callIt(cs, gr, (int)std::lround(f.decodes[i].snrDb), f.decodes[i].slotUtc, f.decodes[i].hz); o.wantTab = HamTabOperate; return; }
            hamToast(cs + " was not heard in this mode");
        }); break;
        case HamTabContest: hamContestTab(a); break;
        case HamTabGuide: hamGuideTab(a); break;
        case HamTabCluster: hamClusterTab(a, [&](const std::string& cs, const std::string& gr) {
            h.selCall = cs; h.selGrid = gr; o.selCall = cs; o.selGrid = gr; h.selMsg.clear();
            hamToast(cs + " picked: it is in the QSO panel of Operate");
        }); break;
        case HamTabActivate: hamActivateTab(a); break;
        default: break;
        }
        if (t != HamTabOperate) { ImGui::EndChild(); ImGui::EndTabItem(); continue; }

        // ================= Operate =================
        if (isKbd) {   // keyboard modes: their own panel, the same station, licence, Arm and transmitter
            HamKbdCtx kc{a, h.call, h.grid, stationOk, h.licence, h.armed, h.level, h.tx, h.worker, h.txState, h.mu, h.txErr, h.status, h.worked, rigWidgets,
                         [&h](const ham::QsoRecord& q) { logRecord(h, q); }};
            h.io.rigIdx = h.rigSel;
            hamKbdPanel(kc, h.kbdIdx);
            ImGui::EndChild(); ImGui::EndTabItem();
            continue;
        }
        // TX controls
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::SliderFloat(tr("TX Hz"), &h.txHz, 200, 3000, "%.0f")) { c.txHz = h.txHz; h.eng.userAction(now); save(h); }
        ImGui::SameLine(); ImGui::Checkbox(tr("Hold"), &c.hold);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Hold TX frequency: calling a station does not move your TX frequency to theirs."));
        ImGui::SameLine(); ImGui::Checkbox(tr("Auto-seq"), &c.autoSeq);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Answers the replies to your CQ or call by itself: report, R-report, RR73, 73."));
        int par = c.txEven ? 0 : 1;
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
        if (comboTr(tr("TX slot"), &par, {N_("Even"), N_("Odd")})) { c.txEven = par == 0; h.eng.userAction(now); }
        if (!h.eng.active()) c.txHz = h.txHz; else h.txHz = (float)c.txHz;
        if (isBeacon) {
            // WSPR beacon: transmit only. Power, how often (never more than every second slot), then Start; the gate and Arm apply as always.
            static const int kDbm[] = {0, 3, 7, 10, 13, 17, 20, 23, 27, 30, 33, 37, 40};
            int di = 3;
            for (int i = 0; i < 13; i++) if (kDbm[i] == h.beacon.cfg.dbm) di = i;
            char dlb[16]; snprintf(dlb, sizeof dlb, "%d dBm", kDbm[di]);
            ImGui::SetNextItemWidth(90 * gUi);
            if (ImGui::BeginCombo(tr("Power"), dlb)) {
                for (int i = 0; i < 13; i++) { snprintf(dlb, sizeof dlb, "%d dBm", kDbm[i]); if (ImGui::Selectable(dlb, i == di)) h.beacon.cfg.dbm = kDbm[i]; }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
            int nth = std::max(1, h.beacon.cfg.everyNth);
            if (ImGui::SliderInt(tr("1 slot in"), &nth, 1, 20)) h.beacon.cfg.everyNth = nth;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Duty cycle: one 2 minute slot in N. 1 sends in every slot: a long duty cycle can damage the radio."));
            h.beacon.cfg.audioHz = 1500;
            ImGui::SameLine();
            ImGui::BeginDisabled(!canCq);
            if (ImGui::Button(h.beacon.active() ? tr("Stop beacon") : tr("Start beacon"))) { if (h.beacon.active()) h.beacon.stop(); else { h.beacon.start(); h.status.clear(); } h.beacon.userAction(); }
            ImGui::EndDisabled();
            if (!h.beacon.stopReason().empty()) h.status = "Beacon stopped after its limit of transmissions";
        } else {
            ImGui::BeginDisabled(!canCq);
            if (ImGui::Button(tr("CQ"))) { h.eng.startCq(now); c.txHz = h.txHz; h.status.clear(); h.override.clear(); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Call CQ in your TX slots (F6)"));
            ImGui::SameLine();
            if (ImGui::Button(tr("Answer best CQ"))) answerBest();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Calls the most wanted CQ of the last slot: a new DXCC first, then new on the band, a new grid, not worked before, the strongest (F7)"));
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Clear DX"))) { h.eng.halt(); h.override.clear(); h.dxGrid.clear(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ends the QSO without sending anything more (F4)"));
            {   // CQ DX / CQ POTA ...: the CQ the engine sends (4.0)
                static const char* kMods[] = {"CQ", "CQ DX", "CQ POTA", "CQ SOTA", "CQ WWFF", "CQ TEST", "CQ NA", "CQ EU", "CQ FD", "CQ RU"};
                static const char* kIds[] = {"", "DX", "POTA", "SOTA", "WWFF", "TEST", "NA", "EU", "FD", "RU"};
                HamStation& st = hamStation();
                int cur = 0;
                for (int i = 0; i < 10; i++) if (st.cqMod == kIds[i]) cur = i;
                ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
                if (ImGui::Combo("##cqmod", &cur, kMods, 10)) { st.cqMod = kIds[cur]; hamStationSave(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("What your CQ says: CQ DX asks for distant stations only, CQ POTA for park hunters ..."));
                // the WSJT-X contest exchanges: the sequence sends the exchange instead of grid and report
                int ct = st.ftxContest;
                ImGui::SameLine(); ImGui::SetNextItemWidth(130 * gUi);
                if (ImGui::Combo("##ftxcontest", &ct, "no contest\0ARRL Field Day\0RTTY Roundup\0") && ct != st.ftxContest) {
                    st.ftxContest = ct;
                    st.cqMod = ct == 1 ? "FD" : ct == 2 ? "RU" : "";
                    h.eng.halt();
                    hamStationSave();
                }
                if (st.ftxContest) {
                    static char ex[24] = {};
                    static bool exInit = false;
                    if (!exInit) { exInit = true; snprintf(ex, sizeof ex, "%s", st.ftxExch.c_str()); }
                    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
                    if (ImGui::InputTextWithHint("##ftxexch", st.ftxContest == 1 ? "2B EMA" : "579 MA", ex, sizeof ex, ImGuiInputTextFlags_CharsUppercase)) { st.ftxExch = ex; hamStationSave(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.ftxContest == 1 ? tr("Field Day: your class and ARRL / RAC section, e.g. 2B EMA") : tr("RTTY Roundup: 579 and your state / province, or a serial number from DX, e.g. 579 MA or 579 0001"));
                }
            }
        }
        if (ImGui::CollapsingHeader(trw("Rig and audio"))) rigWidgets();
        if (ImGui::CollapsingHeader(trw("Automation: band hopping"))) hamHopUi();

        // meters and the timeline
        const HfdigTelemetry& ht = a.rx.hfdig;
        char mt[32];
        const bool rxOn = ht.audioDb > -150;
        snprintf(mt, sizeof mt, rxOn ? "%.0f dBFS" : "no audio", ht.audioDb);
        meter("RX", rxOn ? (float)((ht.audioDb + 90) / 90) : 0.f, rxOn, mt);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Receive audio before the AGC. Keep it out of the red: about -30 to -10 dBFS is good."));
        ImGui::SameLine(0, 16 * gUi);
        snprintf(mt, sizeof mt, h.txState == 1 ? "%.0f %%" : "idle", h.level * 100);
        meter("TX", h.txState == 1 ? h.level / 0.9f : 0.f, h.txState == 1, mt);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The audio level sent to the rig while transmitting (the Level slider). The rig's own ALC and power meters are what count."));
        ImGui::SameLine(0, 16 * gUi);
        static const char* kState[] = {"Idle", "Calling CQ", "Calling: sent grid", "Sent report", "Sent R-report", "Sent RR73", "Sent 73"};
        if (isBeacon) ImGui::Text("%s: %s", tr("State"), h.beacon.active() ? "Beacon running" : "Beacon off");
        else ImGui::Text("%s: %s %s   %s: %d", tr("State"), kState[h.eng.state()], h.eng.dxCall().c_str(), tr("Logged"), h.logged);
        timeline(h, now, per, isBeacon ? h.beacon.active() : (h.eng.active() || !h.override.empty()));
        if (!h.lastTx.empty()) ImGui::TextDisabled("%s", h.lastTx.c_str());

        // the decode list (left) and the QSO panel (right)
        const float avail = ImGui::GetContentRegionAvail().x;
        const float right = std::min(380 * gUi, avail * 0.42f);
        ImGui::BeginChild("##hamdec", ImVec2(avail - right - 6 * gUi, 0));
        const ImGuiTableFlags fl = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
        // the rows of this mode, newest first, for the arrow keys
        std::vector<size_t> rows;
        for (size_t i = f.decodes.size(); i-- > 0;) if (f.decodes[i].mode == h.mode) rows.push_back(i);
        if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !rows.empty()) {
            int cur = -1;
            for (int i = 0; i < (int)rows.size(); i++) if (f.decodes[rows[(size_t)i]].msg == h.selMsg && f.decodes[rows[(size_t)i]].slotUtc == h.selSlot) cur = i;
            int nx = cur;
            if (!ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) nx = std::min((int)rows.size() - 1, cur + 1);
            if (!ImGui::GetIO().KeyAlt && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) nx = std::max(0, cur - 1);
            if (nx != cur && nx >= 0) { const FtxDecode& d = f.decodes[rows[(size_t)nx]]; h.selMsg = d.msg; h.selSlot = d.slotUtc; h.selCall = d.call; h.selGrid = d.grid; h.selSnr = (int)std::lround(d.snrDb); h.selHz = d.hz; o.selCall = d.call; o.selGrid = d.grid; }
            if (cur >= 0 && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) { const FtxDecode& d = f.decodes[rows[(size_t)cur]]; callIt(d.call, d.grid, (int)std::lround(d.snrDb), d.slotUtc, d.hz); }
        }
        if (ImGui::BeginTable("##hamd", 7, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("UTC"); ImGui::TableSetupColumn("dB"); ImGui::TableSetupColumn("DT"); ImGui::TableSetupColumn("Hz");
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Country"); ImGui::TableSetupColumn("Info");
            ImGui::TableHeadersRow();
            double lastSlot = -1;
            for (size_t ri = 0; ri < rows.size(); ri++) {
                const FtxDecode& d = f.decodes[rows[ri]];
                ImGui::TableNextRow();
                if (lastSlot >= 0 && d.slotUtc != lastSlot) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(255, 255, 255, 10));
                lastSlot = d.slotUtc;
                ImGui::PushID((int)rows[ri]);
                const std::string country = d.call.empty() ? "" : hamCountry(d.call);
                const ham::DecodeMarks m = o.worked.mark(d.call, d.grid, band, modeName, d.call.empty() ? "" : hamDxccKey(d.call));
                const bool dupe = o.contestOn && !d.call.empty() && o.contest.dupe(o.log, d.call, band, modeName);
                const bool toMe = !c.myCall.empty() && d.msg.rfind(c.myCall + " ", 0) == 0;
                const ham::NeedResult nr = d.call.empty() ? ham::NeedResult() : hamNeedOf(d.call, d.grid, band, modeName, true);   // zones, state, prefix (4.0); alerts
                if (m.newDxcc) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(150, 40, 150, 90));
                else if (m.newDxccBand) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(170, 80, 140, 60));
                else if (m.newGrid) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(40, 120, 170, 70));
                ImGui::TableNextColumn();
                const bool sel = d.msg == h.selMsg && d.slotUtc == h.selSlot;
                if (ImGui::Selectable(hhmmss(d.slotUtc).c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    h.selMsg = d.msg; h.selSlot = d.slotUtc; h.selCall = d.call; h.selGrid = d.grid; h.selSnr = (int)std::lround(d.snrDb); h.selHz = d.hz;
                    o.selCall = d.call; o.selGrid = d.grid;
                    if (ImGui::IsMouseDoubleClicked(0)) callIt(d.call, d.grid, (int)std::lround(d.snrDb), d.slotUtc, d.hz);
                }
                if (ImGui::BeginPopupContextItem("##dctx")) {
                    const bool can = canCq && !d.call.empty() && !isBeacon;
                    if (ImGui::MenuItem(d.cq ? tr("Answer this CQ") : tr("Call this station"), "Enter", false, can)) callIt(d.call, d.grid, (int)std::lround(d.snrDb), d.slotUtc, d.hz);
                    if (ImGui::MenuItem(tr("Set my TX frequency here"))) { h.txHz = d.hz; c.txHz = d.hz; save(h); }
                    if (ImGui::MenuItem(tr("Show on the map"), nullptr, false, !d.grid.empty())) { o.selCall = d.call; o.selGrid = d.grid; o.wantTab = HamTabMap; }
                    if (ImGui::MenuItem(tr("QSOs with this call in the log"), nullptr, false, !d.call.empty())) o.wantTab = HamTabLog;
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn(); ImGui::Text("%+3.0f", d.snrDb);
                ImGui::TableNextColumn(); ImGui::Text("%4.1f", d.dt);
                ImGui::TableNextColumn(); ImGui::Text("%5.0f", d.hz);
                ImGui::TableNextColumn();
                if (toMe) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", d.msg.c_str());
                else if (m.workedCallBand && !m.newDxccBand) ImGui::TextDisabled("%s", d.msg.c_str());
                else if (d.cq) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "%s", d.msg.c_str());
                else ImGui::TextUnformatted(d.msg.c_str());
                if (!d.call.empty() && hamCountryInMessages()) {   // the country and zones beside the message (setting)
                    const std::string tag = hamCountryTag(d.call);
                    if (!tag.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("(%s)", tag.c_str()); }
                }
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", country.c_str());
                ImGui::TableNextColumn();
                if (dupe) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "DUPE");
                else if (m.newDxcc) ImGui::TextColored(ImVec4(1, 0.5f, 1, 1), "NEW DXCC");
                else if (m.newDxccBand) ImGui::TextColored(ImVec4(1, 0.65f, 0.9f, 1), "new band");
                else if (nr.level == 3 || nr.level == 4) ImGui::TextColored(ImVec4(1, 0.78f, 0.4f, 1), "%s", nr.label.c_str());
                else if (m.newGrid) ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "new grid");
                else if (m.workedCallBand) ImGui::TextDisabled("B4");
                else if (m.workedCall) ImGui::TextDisabled("b4 other band");
                else if (nr.has(ham::NeedPrefix)) ImGui::TextDisabled("new prefix");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // ---- the QSO panel ----
        ImGui::BeginChild("##hamqso", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const std::string dx = !h.eng.dxCall().empty() ? h.eng.dxCall() : h.selCall;
        const std::string dxg = !h.eng.dxCall().empty() ? h.dxGrid : h.selGrid;
        if (!dx.empty()) {
            ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.3f);
            ImGui::TextUnformatted(dx.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%s %s", dxg.c_str(), hamCountry(dx).c_str());
            ham::PathInfo pi;
            if (ham::pathBetween(h.grid, dxg, pi)) ImGui::TextDisabled("%.0f km, %.0f deg (long path %.0f deg)", pi.km, pi.bearing, pi.longBearing);
            const int dupeIdx = o.log.findDupe(dx, band, modeName);
            if (dupeIdx >= 0) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "%s %s", tr("Worked before:"), o.log.qsos()[(size_t)dupeIdx].band.c_str());
            hamCallbookPanel(dx);   // name, QTH, grid from the cache, the log or the online callbook (4.0)
            {
                HamStation& st = hamStation();
                if (st.splitTxHz > 0 && st.splitFor == dx) {
                    ImGui::TextColored(ImVec4(0.5f, 0.85f, 1, 1), "%s %.3f MHz", tr("Split: they listen on"), st.splitTxHz / 1e6);
                    if (h.rigSel >= 0) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Set the rig's split"))) { std::string err; h.io.rigIdx = h.rigSel; hamToast(h.io.ensureOpen(err) && h.io.link.setSplit(true, st.splitTxHz, err) ? "Split set on the rig" : "Split not set: " + err); }
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Split off"))) { std::string err; h.io.rigIdx = h.rigSel; hamToast(h.io.ensureOpen(err) && h.io.link.setSplit(false, 0, err) ? "Split off" : "Split not changed: " + err); }
                    }
                }
            }
            if (h.eng.dxCall().empty()) {
                ImGui::BeginDisabled(!canCq || isBeacon || h.selCall.empty());
                if (ImGui::Button(tr("Call"), ImVec2(-1, 0))) callIt(h.selCall, h.selGrid, h.selSnr, h.selSlot, h.selHz);
                ImGui::EndDisabled();
            }
        } else ImGui::TextDisabled("%s", tr("Pick a station in the list, or call CQ"));
        // the auto-sequence steps, the current one lit
        ImGui::SeparatorText(tr("Sequence"));
        {
            const int st = h.eng.state();
            const char* names[] = {"CQ", "Tx1 grid", "Tx2 rpt", "Tx3 R-rpt", "Tx4 RR73", "Tx5 73"};
            for (int i = 0; i < 6; i++) {
                if (i) ImGui::SameLine(0, 4 * gUi);
                const bool on = st == i + 1;
                if (on) ImGui::TextColored(h.txState == 1 ? ImVec4(1, 0.35f, 0.3f, 1) : ImVec4(0.4f, 0.85f, 1, 1), "[%s]", names[i]);
                else ImGui::TextDisabled("%s", names[i]);
            }
            // the message of the next own slot (a copy of the engine is asked, so that nothing changes)
            if (h.eng.active() || !h.override.empty()) {
                const long idx = ham::slotIndex(now, per);
                long nx = idx + 1;
                if (ham::slotEven(ham::slotStart(nx, per), per) != c.txEven) nx++;
                std::string next = h.override;
                if (next.empty()) { ham::QsoEngine cp = h.eng; cp.onLog = nullptr; cp.onEvent = nullptr; next = cp.messageForSlot(ham::slotStart(nx, per)); }
                ImGui::Text("%s %s: %s", tr("Next"), hhmmss(ham::slotStart(nx, per)).c_str(), next.empty() ? "-" : next.c_str());
                if (!h.armed) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", tr("Not armed: nothing will be sent"));
            }
        }
        // the six standard messages for the station; a click sends it in the next own slot (Ctrl+1 .. Ctrl+6)
        ImGui::SeparatorText(tr("Messages"));
        {
            const std::string me = h.call;
            int snr = h.selSnr;   // the report: how the station was heard last
            for (size_t r : rows) if (f.decodes[r].call == dx) { snr = (int)std::lround(f.decodes[r].snrDb); break; }
            std::string msgs[6];
            if (!dx.empty()) {
                msgs[0] = ham::msgGrid(dx, me, h.grid); msgs[1] = ham::msgReport(dx, me, snr, false); msgs[2] = ham::msgReport(dx, me, snr, true);
                msgs[3] = ham::hamModeUsesRRR(h.mode) ? ham::msgRRR(dx, me) : ham::msgRR73(dx, me); msgs[4] = ham::msg73(dx, me);
            }
            msgs[5] = ham::msgCq(me, h.grid);
            const bool can = canCq && !isBeacon;
            for (int i = 0; i < 6; i++) {
                ImGui::PushID(i);
                char lb[64]; snprintf(lb, sizeof lb, "Tx%d", i + 1);
                ImGui::BeginDisabled(!can || msgs[i].empty());
                const bool key = can && !msgs[i].empty() && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false);
                if (ImGui::SmallButton(lb) || key) { h.override = msgs[i]; h.eng.userAction(now); }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (h.override == msgs[i] && !msgs[i].empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "%s", msgs[i].c_str());
                else ImGui::TextUnformatted(msgs[i].empty() ? "-" : msgs[i].c_str());
                ImGui::PopID();
            }
            // free text and macros
            ImGui::SetNextItemWidth(-90 * gUi);
            ImGui::InputTextWithHint("##free", tr("free text or a macro"), h.freeText, sizeof h.freeText, ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            ham::MacroVars mv; mv.myCall = h.call; mv.myGrid = h.grid; mv.call = dx; mv.grid = dxg; mv.snr = ham::reportText(h.selSnr); mv.rst = mv.snr;
            char sb[8]; snprintf(sb, sizeof sb, "%03d", o.contest.nextSerial); mv.serial = sb; mv.exch = o.contestOn ? o.contest.sent("") : "";
            const std::string ft = txText(ham::expandMacro(h.freeText, mv));
            ImGui::BeginDisabled(!can || ft.empty());
            if (ImGui::Button(tr("Send next"))) { h.override = ft; h.eng.userAction(now); }
            ImGui::EndDisabled();
            if (!ft.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", ft.c_str());
            if (!h.override.empty()) { if (ImGui::SmallButton(tr("Cancel the chosen message"))) h.override.clear(); }
            for (size_t i = 0; i < o.macros.size(); i++) {
                ImGui::PushID((int)i + 100);
                if (ImGui::SmallButton("x")) { o.macros.erase(o.macros.begin() + (long)i); hamSaveStation(); ImGui::PopID(); break; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Delete this macro"));
                ImGui::SameLine();
                if (ImGui::Selectable(o.macros[i].c_str())) snprintf(h.freeText, sizeof h.freeText, "%s", o.macros[i].c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("-> %s", txText(ham::expandMacro(o.macros[i], mv)).c_str());
                ImGui::PopID();
            }
            if (h.freeText[0] && ImGui::SmallButton(tr("Keep as a macro"))) { o.macros.push_back(h.freeText); hamSaveStation(); }
        }
        if (o.contestOn) { ImGui::SeparatorText(tr("Contest")); ImGui::Text("%s  %s: %s", ham::contestDefs()[(size_t)o.contest.def].id, tr("sent"), o.contest.sent("599").c_str()); }
        if (h.lastLogId && o.log.find(h.lastLogId)) {
            ImGui::Separator();
            ImGui::TextDisabled("%s %s", tr("Last logged:"), o.log.find(h.lastLogId)->call.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Open the log"))) o.wantTab = HamTabLog;
        }
        uploadSection(h);
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}
