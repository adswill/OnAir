// Debug report (debug_report.h). Files of the last capture are kept in <logDir>/debug-report until the next one; the zip is written
// only when the user presses Save, to a file the user picks. Nothing here uses the network.
#include "debug_report.h"
#include "report_problem.h"
#include "sysinfo.h"
#include "../app.h"
#include "../main_internal.h"
#include "../platform/platform.h"
#include "../i18n/i18n.h"
#include "../ui/scale.h"
#include "dect2/common/log_tail.h"
#include "dect2/common/thread_name.h"
#include "dect2/engine/crash_report.h"
#include "dect2/engine/debug_dump.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "?"
#endif

namespace fs = std::filesystem;

namespace {

constexpr int64_t kHangMs = 15000;   // no frame for this long: a hang

// ------------------------------------------------------------------ small helpers

fs::path u8path(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string fromPath(const fs::path& p) { const std::u8string u = p.u8string(); return std::string(reinterpret_cast<const char*>(u.data()), u.size()); }

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::string b(32, '\0');
    b.resize(std::strftime(b.data(), b.size(), "%Y-%m-%d %H:%M:%S UTC", &tm));
    return b;
}

std::string fileStamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::string b(32, '\0');
    b.resize(std::strftime(b.data(), b.size(), "%Y%m%d-%H%M%S", &tm));
    return b;
}

std::string jsonText(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { std::string u(8, '\0'); u.resize((size_t)std::snprintf(u.data(), u.size(), "\\u%04x", c)); o += u; }
        else o += (char)c;
    }
    return o + "\"";
}

const char* osTag() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

const char* archTag() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#else
    return "other";
#endif
}

bool debuggerAttached() {
#ifdef _WIN32
    return IsDebuggerPresent() != FALSE;
#elif defined(__linux__)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) if (line.rfind("TracerPid:", 0) == 0) return std::atoi(line.c_str() + 10) != 0;
    return false;
#else
    return false;
#endif
}

int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((size_t)d->BufTextLen);
        d->Buf = s->data();
    }
    return 0;
}

bool textBox(const char* id, std::string& s, float height) {
    return ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, ImVec2(-1, height), ImGuiInputTextFlags_CallbackResize, resizeCb, &s);
}

// ------------------------------------------------------------------ watchdog

std::atomic<int64_t> gBeat{0};          // the last frame (steady ms); 0 before the watchdog may act
std::atomic<bool> gHangDumped{false};   // a hang dump was written and is not shown yet
std::atomic<int64_t> gHangSince{0};
std::atomic<bool> gStop{false};
std::mutex gDogMu;
std::condition_variable gDogCv;
std::thread gDog;
std::string gHangPath;                  // <logDir>/hang.dmp (set before the thread starts)

void watchdogMain() {
    int64_t lastWake = steadyMs();
    std::unique_lock<std::mutex> lk(gDogMu);
    while (!gStop.load()) {
        gDogCv.wait_for(lk, std::chrono::milliseconds(500));
        if (gStop.load()) break;
        const int64_t now = steadyMs();
        if (now - lastWake > 5000) gBeat.store(now);   // the computer slept: that is not a hang
        lastWake = now;
        if (plat::dialogDepth().load() > 0 && gBeat.load() != 0) gBeat.store(now);   // a file dialog of the system is open: waiting is normal
        const int64_t beat = gBeat.load();
        if (beat == 0 || gHangDumped.load() || now - beat < kHangMs) continue;
        if (debuggerAttached()) continue;   // stopped at a breakpoint
        lk.unlock();
        std::string err;
        const bool ok = dect2::dump::write(gHangPath, err);
        dect2::crash::appendLog("watchdog: the main thread has not finished a frame for " + std::to_string((now - beat) / 1000) + " s; " +
                                (ok ? "the stacks of all threads are in " + gHangPath : "no dump: " + err) + "\n");
        gHangSince.store(beat);
        gHangDumped.store(ok);
        if (!ok) gBeat.store(0);   // no second try for this hang
        lk.lock();
    }
}

// ------------------------------------------------------------------ the report

enum class Trigger { CtrlD, Crash, Hang, HangLastRun };

struct State {
    bool open = false;
    bool pendingCapture = false;   // Ctrl+D / menu: capture in this frame
    int openAtFrame = 0;           // the window is drawn from this frame on (not in the screenshot)
    Trigger trigger = Trigger::CtrlD;
    fs::path dir;                  // <logDir>/debug-report
    std::string dumpErr;
    bool haveDump = false, haveShot = false;
    std::string system, log, crashTxt, radio;
    std::string q1, q2, q3;
    bool incDump = true, incShot = true, incSys = true, incLog = true;
    std::string status;
    // what was picked up at start, shown once the crash note is gone
    bool lastRunPending = false;
    Trigger lastRunTrigger = Trigger::Crash;
    std::string lastRunDump, lastRunText;
    // the log file
    std::string lastLogLine;
    int64_t lastLogMs = 0;
};

State& st() { static State s; return s; }

std::string triggerText(Trigger t) {
    switch (t) {
        case Trigger::CtrlD: return "Ctrl+D";
        case Trigger::Crash: return "crash (last run)";
        case Trigger::Hang: return "hang (the window did not respond)";
        case Trigger::HangLastRun: return "hang (last run, OnAir was closed while it did not respond)";
    }
    return "?";
}

std::string cleanLog(const std::string& text) {
    std::string out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        out += reportMayHoldSecret(line) ? std::string("[line left out: it may hold a key, a token or a password]") : reportCleanText(line);
        out += '\n';
        p = e + 1;
    }
    return out;
}

// the logs, oldest first, then cut to 1 MB from the newest end
std::string gatherLog(App& a, bool lastRun) {
    const fs::path d = u8path(dect2::crash::logDir());
    std::string s;
    if (lastRun) {
        const std::string prev = slurp(d / "onair-prev.log");
        s += "== onair-prev.log (the run that crashed or hung) ==\n" + (prev.empty() ? std::string("(empty)\n") : prev);
        if (!s.empty() && s.back() != '\n') s += '\n';
    }
#ifdef _WIN32
    const std::string cur = slurp(d / "onair.log");   // standard error and the program log of this run
    s += "== onair.log (this run) ==\n" + cur;
    if (!s.empty() && s.back() != '\n') s += '\n';
#endif
    size_t total = 0;
    s += "== program log (this run, newest last) ==\n";
    for (const std::string& l : a.engine.logSnapshot(total)) s += l + "\n";
    return dect2::logTail(cleanLog(s), dect2::kReportLogBytes);
}

void clearDir(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const char* f : {"dump.dmp", "screenshot.png"}) fs::remove(dir / f, ec);
}

// everything but the screenshot (taken by the main loop at the end of this frame) and the answers
void capture(App& a, Trigger t, const std::string& existingDump, const std::string& crashText) {
    State& s = st();
    s.trigger = t;
    s.dir = u8path(dect2::crash::logDir()) / "debug-report";
    clearDir(s.dir);
    const fs::path dmp = s.dir / "dump.dmp";
    s.dumpErr.clear();
    if (existingDump.empty()) {
        std::string err;
        s.haveDump = dect2::dump::write(fromPath(dmp), err);
        if (!s.haveDump) s.dumpErr = err;
    } else {
        std::error_code ec;
        fs::copy_file(u8path(existingDump), dmp, fs::copy_options::overwrite_existing, ec);
        s.haveDump = !ec;
        if (ec) s.dumpErr = "could not copy " + existingDump + ": " + ec.message();
    }
    s.haveShot = false;
    if (t == Trigger::CtrlD) {
        gAutomationShotPath = fromPath(s.dir / "screenshot.png");   // saved by the main loop at the end of this frame
        s.haveShot = true;
    }
    s.system = systemInfoReport(a);
    s.radio = reportRadioAndMode(a);
    s.log = gatherLog(a, t == Trigger::Crash || t == Trigger::HangLastRun);
    s.crashTxt = crashText;
    s.q1.clear(); s.q2.clear(); s.q3.clear();
    s.incDump = s.haveDump; s.incShot = true; s.incSys = true; s.incLog = true;
    s.status.clear();
    s.open = true;
    s.openAtFrame = ImGui::GetFrameCount() + 2;   // the screenshot frame stays clean
    a.engine.log(std::string("debug report: ") + triggerText(t) + (s.haveDump ? ", dump written" : ", no dump: " + s.dumpErr));
}

std::string reportText(const State& s) {
    std::string r = "OnAir debug report\n\n";
    r += std::string("Version: ") + ONAIR_VERSION + "\n";
    r += "Trigger: " + triggerText(s.trigger) + "\n";
    r += "Saved: " + utcNow() + "\n";
    r += "System: " + systemInfoSummary() + "\n\n";
    r += "What were you doing?\n" + (s.q1.empty() ? std::string("(no answer)") : s.q1) + "\n\n";
    r += "How can you reproduce it?\n" + (s.q2.empty() ? std::string("(no answer)") : s.q2) + "\n\n";
    r += "What is the core problem, or what do you want changed?\n" + (s.q3.empty() ? std::string("(no answer)") : s.q3) + "\n\n";
    r += "Radio and mode at the time of the report:\n" + s.radio + "\n";
    r += "Files: report.txt (this), dump.json, " + std::string(s.incDump && s.haveDump ? "dump.dmp (stacks of all threads, no heap), " : "") +
         (s.incSys ? "system.txt, " : "") + (s.incLog ? "log.txt (at most 1 MB, newest last), " : "") +
         (s.crashTxt.empty() ? "" : "crash.txt, ") + (s.incShot && s.haveShot ? "screenshot.png" : "") + "\n";
    return r;
}

std::string dumpJson(const State& s) {
    std::string j = "{\n";
    j += "  \"format\": 1,\n";
    j += "  \"version\": " + jsonText(ONAIR_VERSION) + ",\n";
    j += "  \"os\": " + jsonText(osTag()) + ",\n";
    j += "  \"arch\": " + jsonText(archTag()) + ",\n";
    j += "  \"trigger\": " + jsonText(triggerText(s.trigger)) + ",\n";
    j += "  \"saved_utc\": " + jsonText(utcNow()) + ",\n";
    j += "  \"dump\": " + std::string(s.incDump && s.haveDump ? "\"dump.dmp\"" : "null") + ",\n";
    j += "  \"dump_error\": " + jsonText(s.dumpErr) + "\n";
    return j + "}\n";
}

bool saveZip(const State& s, const std::string& path) {
    std::vector<std::pair<std::string, std::string>> files;
    files.push_back({"report.txt", reportText(s)});
    files.push_back({"dump.json", dumpJson(s)});
    if (s.incDump && s.haveDump) files.push_back({"dump.dmp", slurp(s.dir / "dump.dmp")});
    if (s.incSys) files.push_back({"system.txt", s.system});
    if (s.incLog) files.push_back({"log.txt", s.log});
    if (!s.crashTxt.empty()) files.push_back({"crash.txt", reportCleanText(s.crashTxt)});
    if (s.incShot && s.haveShot) {
        const std::string png = slurp(s.dir / "screenshot.png");
        if (!png.empty()) files.push_back({"screenshot.png", png});
    }
    return writeZipFile(path, files);
}

std::string sizeText(uintmax_t bytes) {
    std::string b(32, '\0');
    if (bytes >= (1u << 20)) b.resize((size_t)std::snprintf(b.data(), b.size(), "%.1f MB", (double)bytes / 1048576.0));
    else b.resize((size_t)std::snprintf(b.data(), b.size(), "%.0f KB", (double)bytes / 1024.0));
    return b;
}

void reportWindow(App& a) {
    State& s = st();
    if (!s.open || ImGui::GetFrameCount() < s.openAtFrame) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(720 * gUi, vp->WorkSize.x * 0.95f), std::min(700 * gUi, vp->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::Begin(trw("Debug report"), &open)) { ImGui::End(); if (!open) s.open = false; return; }

    ImGui::PushTextWrapPos(0.0f);
    switch (s.trigger) {
        case Trigger::CtrlD: ImGui::TextUnformatted(tr("OnAir saved its current state (Ctrl+D).")); break;
        case Trigger::Crash: ImGui::TextUnformatted(tr("OnAir crashed the last time it ran. Its state at the crash was saved.")); break;
        case Trigger::Hang: ImGui::TextUnformatted(tr("OnAir did not respond for a while. Its state at that moment was saved.")); break;
        case Trigger::HangLastRun: ImGui::TextUnformatted(tr("OnAir did not respond the last time it ran, and was closed. Its state at that moment was saved.")); break;
    }
    ImGui::TextUnformatted(tr("Answer the three questions and press Save. Attach the zip to a new issue on GitHub (github.com/adswill/OnAir/issues). Nothing is uploaded by OnAir."));
    if (!s.dumpErr.empty()) ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1), "%s: %s", tr("No stack dump"), s.dumpErr.c_str());
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("What were you doing?"));
    textBox("##q1", s.q1, 70 * gUi);
    ImGui::TextUnformatted(tr("How can you reproduce it?"));
    textBox("##q2", s.q2, 70 * gUi);
    ImGui::TextUnformatted(tr("What is the core problem, or what do you want changed?"));
    textBox("##q3", s.q3, 70 * gUi);

    ImGui::Spacing();
    ImGui::SeparatorText(tr("In the zip"));
    std::error_code ec;
    if (s.haveDump) {
        const std::string label = std::string(tr("Debug dump: the stacks of all threads")) + " (" + sizeText(fs::file_size(s.dir / "dump.dmp", ec)) + ")";
        ImGui::Checkbox(label.c_str(), &s.incDump);
    }
    if (s.haveShot) ImGui::Checkbox(tr("Screenshot of the OnAir window"), &s.incShot);
    ImGui::Checkbox(tr("Information about this computer (processor, SIMD, graphics, memory)"), &s.incSys);
    {
        const std::string label = std::string(tr("Log, newest part")) + " (" + sizeText(s.log.size()) + ")";
        ImGui::Checkbox(label.c_str(), &s.incLog);
    }
    if (!s.crashTxt.empty()) ImGui::BulletText("%s", tr("The crash report of the last run"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", tr("The stack memory and the screenshot can hold personal data: keys, passwords, callsigns, positions, messages. Leave them out if that matters. Log lines that may hold a key, a token or a password are left out, and folder paths are shortened."));
    ImGui::PopTextWrapPos();
    if (ImGui::TreeNode(trw("Show the information about this computer"))) {
        ImGui::BeginChild("##sysinfo", ImVec2(0, 160 * gUi), true);
        ImGui::TextUnformatted(s.system.c_str());
        ImGui::EndChild();
        ImGui::TreePop();
    }

    ImGui::Spacing();
    if (ImGui::Button(tr("Save..."))) {
        const std::string path = plat::saveFileDialog(("debug_dump_v" + std::string(ONAIR_VERSION) + "_" + fileStamp() + ".zip").c_str());
        if (!path.empty()) {
            s.status = saveZip(s, path) ? std::string(tr("Saved. Attach this file to a new GitHub issue.")) + " " + path
                                        : std::string(tr("Could not write the zip file."));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) open = false;
    if (!s.status.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(s.status.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
    if (!open) s.open = false;
    (void)a;
}

// new lines of the program log into onair.log, once a second (the log of a crashed run is then there at the next start)
void mirrorLog(App& a) {
    State& s = st();
    const int64_t now = steadyMs();
    if (now - s.lastLogMs < 1000) return;
    s.lastLogMs = now;
    size_t total = 0;
    const std::vector<std::string> lines = a.engine.logSnapshot(total);
    size_t from = 0;
    if (!s.lastLogLine.empty()) {
        for (size_t i = lines.size(); i-- > 0;) if (lines[i] == s.lastLogLine) { from = i + 1; break; }
    }
    if (from >= lines.size()) return;
    std::string out;
    for (size_t i = from; i < lines.size(); i++) out += lines[i] + "\n";
    dect2::crash::appendLog(out);
    s.lastLogLine = lines.back();
}

bool ctrlD() {
    const ImGuiIO& io = ImGui::GetIO();
    return !io.WantTextInput && (io.KeyCtrl || io.KeySuper) && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false);
}

}  // namespace

void debugReportBeat() {
    if (gBeat.load() != 0) gBeat.store(steadyMs());
}

void debugReportRequest() { st().pendingCapture = true; }

void debugReportInit(App& a, bool devRun) {
    State& s = st();
    const fs::path d = u8path(dect2::crash::logDir());
    gHangPath = fromPath(d / "hang.dmp");
    if (devRun) return;
    std::string report;
    std::error_code ec;
    if (dect2::crash::lastRunCrashed(report)) {
        s.lastRunPending = true;
        s.lastRunTrigger = Trigger::Crash;
        s.lastRunDump = dect2::crash::lastCrashDump();
        s.lastRunText = slurp(u8path(report));
    } else if (fs::exists(d / "hang.dmp", ec)) {   // the watchdog wrote it and OnAir never answered again
        s.lastRunPending = true;
        s.lastRunTrigger = Trigger::HangLastRun;
        s.lastRunDump = fromPath(d / "hang.dmp");
    }
    if (!std::getenv("ONAIR_NO_WATCHDOG")) gDog = dect2::namedThread("onair-watchdog", watchdogMain);
    (void)a;
}

void debugReportShutdown() {
    gStop.store(true);
    gDogCv.notify_all();
    if (gDog.joinable()) gDog.join();
}

void debugReportFrame(App& a) {
    State& s = st();
    const int64_t now = steadyMs();
    if (gHangDumped.load()) {   // the main thread answers again after a hang: show what was saved then
        gHangDumped.store(false);
        if (!s.open) {
            capture(a, Trigger::Hang, gHangPath, "");
            std::error_code ec;
            fs::remove(u8path(gHangPath), ec);
        }
    }
    if (dect2::crash::failedStarts() == 0 && ImGui::GetFrameCount() > 120 && gBeat.load() == 0) gBeat.store(now);   // the watchdog acts once the program is up
    mirrorLog(a);

    if (s.lastRunPending && gCrashNote.empty() && !s.open && ImGui::GetFrameCount() > 30) {   // after the crash note has been closed
        s.lastRunPending = false;
        capture(a, s.lastRunTrigger, s.lastRunDump, s.lastRunTrigger == Trigger::Crash ? s.lastRunText : std::string());
        if (s.lastRunTrigger == Trigger::HangLastRun) { std::error_code ec; fs::remove(u8path(s.lastRunDump), ec); }
    }
    if (ctrlD() && !s.open) s.pendingCapture = true;   // not while the window is open: that would throw the answers away
    if (s.pendingCapture) {
        s.pendingCapture = false;
        capture(a, Trigger::CtrlD, "", "");
    }
    reportWindow(a);
}
