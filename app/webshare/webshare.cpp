// Server > Share on web: the OnAir Web setup window. Step 1 shares the radio on the web (onair-server --web plus a free Cloudflare quick tunnel), step 2 lists
// it on the public directory after an exact preview. The owner chooses the radio, the centre and span, the decoders, who may listen and what the listing says
// (webshare_core.h holds those choices and the logic; this file is the window and the child programs).
//
// Nothing here waits on the UI thread: every file wait, process start and network request runs in a job thread with a time limit and a cancel flag, the
// window only reads the state those jobs leave behind. Everything runs as child programs with logs under <data>/webshare/, so closing OnAir ends the sharing.
// Docs: docs/ONAIR-WEB-SHARING.md and docs/ONAIR-WEB.md.
#include "webshare.h"
#include "webshare_core.h"
#include "qr.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../ui/typography.h"
#include "dect2/common/thread_name.h"
#include "dect2/engine/updater.h"
#include "web/web_json.h"
#include "web/web_policy.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <cstring>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace webshare {

namespace {

enum class Phase { Idle, NeedHelper, Downloading, Starting, Sharing, Stopping, Failed };

struct Job { std::thread th; std::shared_ptr<std::atomic<bool>> done; };

struct G {
    std::mutex m;                       // the strings and structs below
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<long> serverPid{0}, tunnelPid{0}, dirPid{0};
    std::atomic<bool> cancel{false}, shareRunning{false};
    std::atomic<int> testRunning{0}, previewRunning{0};
    ShareSettings S;
    bool loaded = false, dirty = false, saveFailed = false;
    double dirtyAt = 0;
    std::string stage, url, error, advice, helperPath, appliedConf, appliedDirSig, adminToken;
    std::string demoNote;
    int port = 8073, apiPort = 8765;
    Step2State s2;
    std::string s2Sig;                  // the listing settings the preview / the running listing was made from
    std::vector<TestStep> test;
    bool testDone = false;
    ShareStats stats;
    double statsAt = 0, publicAt = 0;
    int publicState = -1;               // -1 unknown, 0 not answering, 1 answers
    std::string publicNote;
    std::string applyNote;
    std::mutex jm;
    std::vector<Job> jobs;
    bool open = false, demo = false;
    int tab = 0, selectTab = -1;
    int lastDrawFrame = -1;
} g;

// ---------------------------------------------------------------- files and programs
fs::path dir() {
    fs::path d = fs::path(plat::dataDir()) / "webshare";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string slurp(const fs::path& p, size_t maxBytes = 1 << 20) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s = s.substr(s.size() - maxBytes);
    return s;
}

void writeFile(const fs::path& p, const std::string& text, bool secret) {
    std::error_code ec;
    {
        std::ofstream f(p, std::ios::trunc | std::ios::binary);
        f << text;
    }
    if (secret) fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);
}

std::string lastLines(const std::string& s, int n) {
    size_t end = s.find_last_not_of("\r\n ");
    if (end == std::string::npos) return {};
    size_t pos = end;
    int lines = 0;
    while (pos > 0) { if (s[pos] == '\n' && ++lines == n) { pos++; break; } pos--; }
    std::string r = s.substr(pos, end - pos + 1);
    if (r.size() > 400) r = r.substr(r.size() - 400);
    return r;
}

std::string exeDirPath() {
    const std::string exe = plat::exePath();
    const size_t cut = exe.find_last_of("/\\");
    return cut == std::string::npos ? std::string(".") : exe.substr(0, cut);
}

std::string serverBinary() {
#ifdef _WIN32
    const char* nm = "onair-server.exe";
#else
    const char* nm = "onair-server";
#endif
    return (fs::path(exeDirPath()) / nm).string();
}

// cloudflared: an installed one, else the one we downloaded (with consent) into <data>/webshare/
std::string findHelper() {
#ifdef _WIN32
    const char* nm = "cloudflared.exe";
    const char* cands[] = {"C:\\Program Files (x86)\\cloudflared\\cloudflared.exe", "C:\\Program Files\\cloudflared\\cloudflared.exe"};
#else
    const char* nm = "cloudflared";
    const char* cands[] = {"/opt/homebrew/bin/cloudflared", "/usr/local/bin/cloudflared", "/usr/bin/cloudflared"};
#endif
    std::error_code ec;
    for (const char* c : cands) if (fs::is_regular_file(c, ec)) return c;
    const fs::path mine = dir() / nm;
    if (fs::is_regular_file(mine, ec)) return mine.string();
    return {};
}

void spawnJob(std::function<void()> fn) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t = dect2::namedThread("onair-webshare", [fn, done] { fn(); *done = true; });
    std::lock_guard<std::mutex> l(g.jm);
    g.jobs.push_back(Job{std::move(t), done});
}

void reapJobs() {   // joins only the jobs that have already ended: never waits
    std::lock_guard<std::mutex> l(g.jm);
    for (size_t i = 0; i < g.jobs.size();) {
        if (*g.jobs[i].done) { if (g.jobs[i].th.joinable()) g.jobs[i].th.join(); g.jobs.erase(g.jobs.begin() + (long)i); }
        else i++;
    }
}

bool sleepOrCancel(int ms) {   // false when cancelled
    for (int i = 0; i < ms / 50; i++) { if (g.cancel) return false; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return !g.cancel;
}

void fail(const std::string& why, const std::string& advice) {
    std::lock_guard<std::mutex> l(g.m);
    g.error = why;
    g.advice = advice;
    g.phase = Phase::Failed;
}

void setStage(const std::string& s) { std::lock_guard<std::mutex> l(g.m); g.stage = s; }

// a stop that ends a program politely, then firmly, never longer than ~4 s
void endChild(std::atomic<long>& pid) {
    const long p = pid.exchange(0);
    if (p <= 0) return;
    plat::stopChild(p);
    for (int i = 0; i < 40 && plat::childAlive(p); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (plat::childAlive(p)) { plat::killChild(p); for (int i = 0; i < 20 && plat::childAlive(p); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
}

// leftovers of a crashed OnAir (a server or tunnel still running from the last time) are ended before a new start
void endLeftovers() {
    const fs::path pf = dir() / "pids.txt";
    std::istringstream in(slurp(pf, 4096));
    long p;
    std::string nm;
    while (in >> p >> nm) {
        const std::string now = plat::childName(p);
        if (!now.empty() && now.find(nm) != std::string::npos) { plat::stopChild(p); for (int i = 0; i < 30 && plat::childAlive(p); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100)); if (plat::childAlive(p)) plat::killChild(p); }
    }
    std::error_code ec;
    fs::remove(pf, ec);
}
void notePids() {
    std::ostringstream o;
    if (g.serverPid) o << g.serverPid.load() << " onair-server\n";
    if (g.tunnelPid) o << g.tunnelPid.load() << " cloudflared\n";
    if (g.dirPid) o << g.dirPid.load() << " onair-server\n";
    writeFile(dir() / "pids.txt", o.str(), false);
}

// ---------------------------------------------------------------- curl helpers (time-limited, run in jobs)
std::string curlGet(const std::string& url, int seconds, int* status = nullptr, const std::string& headerFile = {}) {
    std::vector<std::string> a = {"-sS", "-i", "--max-time", std::to_string(seconds), "--connect-timeout", std::to_string(std::min(seconds, 6)), "--max-redirs", "0"};
    if (!headerFile.empty()) { a.push_back("-H"); a.push_back("@" + headerFile); }
    a.push_back(url);
    std::string out;
    std::atomic<bool> never{false};
    const int rc = dect2::curlRun(a, &out, never);
    if (status) *status = (rc == 0 && out.compare(0, 5, "HTTP/") == 0) ? atoi(out.c_str() + 9) : 0;
    const size_t b = out.find("\r\n\r\n");
    return (rc == 0 && b != std::string::npos) ? out.substr(b + 4) : std::string();
}

std::string curlPostJson(const std::string& url, const std::string& json, const std::string& headerFile, int* status) {
    const fs::path body = dir() / "post.json";
    writeFile(body, json, true);
    std::vector<std::string> a = {"-sS", "-i", "--max-time", "5", "--connect-timeout", "3", "-X", "POST", "-H", "Content-Type: application/json", "-H", "@" + headerFile, "--data-binary", "@" + body.string(), url};
    std::string out;
    std::atomic<bool> never{false};
    const int rc = dect2::curlRun(a, &out, never);
    std::error_code ec;
    fs::remove(body, ec);
    *status = (rc == 0 && out.compare(0, 5, "HTTP/") == 0) ? atoi(out.c_str() + 9) : 0;
    const size_t b = out.find("\r\n\r\n");
    return b == std::string::npos ? std::string() : out.substr(b + 4);
}

std::string newToken() {
    static const char* hex = "0123456789abcdef";
    std::string t;
    FILE* f = fopen("/dev/urandom", "rb");
    unsigned char b[16] = {0};
    if (f) { if (fread(b, 1, 16, f) != 16) b[0] = 0; fclose(f); }
    else { std::srand((unsigned)std::time(nullptr) ^ (unsigned)(size_t)&t); for (auto& c : b) c = (unsigned char)(std::rand() & 255); }
    for (unsigned char c : b) { t += hex[c >> 4]; t += hex[c & 15]; }
    return t;
}

// ---------------------------------------------------------------- settings
void loadSettingsOnce() {
    if (g.loaded) return;
    g.loaded = true;
    g.S = settingsFromText(slurp(dir() / "settings.conf", 1 << 16));
}
void markDirty() { g.dirty = true; g.dirtyAt = ImGui::GetTime(); }
void saveSettingsIfDue() {
    if (!g.dirty || ImGui::GetTime() - g.dirtyAt < 0.6 || g.demo) return;
    g.dirty = false;
    writeFile(dir() / "settings.conf", settingsToText(g.S), true);
}

RigCaps currentRig(const std::vector<RigCaps>& rigs, const ShareSettings& S) {
    for (const auto& r : rigs) if (r.name == S.rig) return r;
    if (!rigs.empty() && S.rig.empty()) return rigs.front();
    RigCaps r;
    r.name = S.rig;
    return r;
}

// ---------------------------------------------------------------- step 1
std::string serverConfText(const ShareSettings& S, const Plan& plan) { return buildServerConf(S, plan, g.port, g.apiPort, "", g.adminToken); }

std::string stripPolicy(const std::string& conf) {   // the lines the running server can take live
    std::istringstream in(conf);
    std::string l, o;
    while (std::getline(in, l)) {
        if (l.compare(0, 11, "web_demods=") == 0 || l.compare(0, 17, "web_decoders_off=") == 0 || l.compare(0, 16, "web_decoders_on=") == 0 || l.compare(0, 14, "web_legal_ack=") == 0) continue;
        o += l + "\n";
    }
    return o;
}

bool waitForServer(long pid, int seconds, std::string& why) {
    for (int i = 0; i < seconds * 10; i++) {
        if (g.cancel) return false;
        if (!plat::childAlive(pid)) {
            const std::string log = lastLines(slurp(dir() / "server.log", 8192), 4);
            why = log.empty() ? "The web server program stopped right after it started." : log;
            return false;
        }
        if (i % 5 == 0) {
            int st = 0;
            const std::string body = curlGet("http://127.0.0.1:" + std::to_string(g.port) + "/web/v1/info", 2, &st);
            if (st == 200 && body.find("\"proto\"") != std::string::npos) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    why = "The web server did not answer within " + std::to_string(seconds) + " seconds. " + lastLines(slurp(dir() / "server.log", 8192), 3);
    return false;
}

bool startServer(const ShareSettings& S, const Plan& plan, std::string& err) {
    endChild(g.serverPid);
    const std::string conf = serverConfText(S, plan);
    writeFile(dir() / "server.conf", conf, true);
    std::string e;
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "server.conf").string(), "--web", "--web-port", std::to_string(g.port), "--port", std::to_string(g.apiPort)},
                                      (dir() / "server.log").string(), e);
    if (!pid) { err = e + ": onair-server is part of the OnAir package; it was not found beside the program (" + serverBinary() + ")"; return false; }
    g.serverPid = pid;
    notePids();
    { std::lock_guard<std::mutex> l(g.m); g.appliedConf = conf; }
    return true;
}

void writeAdminHeader() {
    writeFile(dir() / "admin.hdr", "Authorization: Bearer " + g.adminToken + "\n", true);
}

void shareWorker(ShareSettings S, RigCaps rig, Plan plan) {
    g.shareRunning = true;
    g.cancel = false;
    auto bail = [](const std::string& why, const std::string& adv) { fail(why, adv); endChild(g.tunnelPid); endChild(g.dirPid); endChild(g.serverPid); g.shareRunning = false; };
    { std::lock_guard<std::mutex> l(g.m); g.url.clear(); g.error.clear(); g.advice.clear(); g.stats = ShareStats{}; g.publicState = -1; g.publicNote.clear(); g.applyNote.clear(); }
    g.phase = Phase::Starting;
    setStage(tr("Cleaning up an earlier run ..."));
    endLeftovers();
    g.port = freeLocalPort(8073);
    g.apiPort = freeLocalPort(g.port + 1 > 8765 ? g.port + 1 : 8765);
    if (!g.port || !g.apiPort) { bail(tr("No free port on this computer."), tr("Close the programs that use ports 8073 to 8112, or restart the computer, and try again.")); return; }
    g.adminToken = newToken();
    writeAdminHeader();
    setStage(tr("Opening the radio and starting the web server ..."));
    std::string err;
    if (!startServer(S, plan, err)) { bail(err, tr("Reinstall OnAir, or build the onair-server target.")); return; }
    if (!waitForServer(g.serverPid, 30, err)) {
        if (!g.cancel) bail(tr("The web server did not start."), err + "  " + tr("Is the radio used by another program? Close it, or choose another radio on the Radio tab."));
        else g.shareRunning = false;
        return;
    }
    std::string url;
    if (S.link == "tunnel") {
        setStage(tr("Asking Cloudflare for a public address ..."));
        std::string helper = findHelper();
        if (helper.empty()) { bail(tr("The tunnel helper is missing."), tr("Press Start sharing again and allow the download, or use Access > Link > Only my network.")); return; }
        std::string e2;
        const long tp = plat::spawnChild(helper, {"tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:" + std::to_string(g.port)}, (dir() / "tunnel.log").string(), e2);
        if (!tp) { bail(tr("The tunnel helper could not be started."), e2); return; }
        g.tunnelPid = tp;
        notePids();
        for (int i = 0; i < 900 && url.empty() && !g.cancel; i++) {   // 90 s: the helper asks Cloudflare, which sometimes needs a while
            url = parseTunnelUrl(slurp(dir() / "tunnel.log", 65536));
            if (!url.empty()) break;
            if (!plat::childAlive(tp)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g.cancel) { g.shareRunning = false; return; }
        if (url.empty()) {
            const std::string log = slurp(dir() / "tunnel.log", 65536);
            const std::string adv = tunnelAdvice(log);
            bail(tr("The public address did not come up."), (adv.empty() ? tr("Check the internet connection and try again.") : adv) + "  [" + lastLines(log, 2) + "]");
            return;
        }
        setStage(tr("Waiting until the public address answers ..."));
        bool up = false;
        for (int i = 0; i < 40 && !g.cancel && !up; i++) {   // a new quick-tunnel name needs a few seconds before it works from outside
            int st = 0;
            const std::string body = curlGet(url + "/web/v1/info", 6, &st);
            up = st == 200 && body.find("\"proto\"") != std::string::npos;
            if (!up && !sleepOrCancel(1500)) break;
        }
        std::lock_guard<std::mutex> l(g.m);
        g.publicState = up ? 1 : 0;
        g.publicNote = up ? "" : tr("The public address does not answer yet. It can take a minute; press Test connection.");
    } else if (S.link == "own") {
        url = S.ownUrl;
        while (!url.empty() && url.back() == '/') url.pop_back();
    } else {
        const std::string ip = lanAddress();
        url = "http://" + (ip.empty() ? std::string("<this computer's address>") : ip) + ":" + std::to_string(g.port);
    }
    if (g.cancel) { g.shareRunning = false; return; }
    {
        std::lock_guard<std::mutex> l(g.m);
        g.url = url;
        g.stage.clear();
    }
    g.phase = Phase::Sharing;
    g.shareRunning = false;
}

void restartServerWorker(ShareSettings S, Plan plan) {
    g.shareRunning = true;
    std::string err;
    setStage(tr("Applying the changes (the public link stays the same) ..."));
    g.phase = Phase::Starting;
    if (!startServer(S, plan, err) || !waitForServer(g.serverPid, 30, err)) {
        if (!g.cancel) fail(tr("The web server did not restart."), err);
        g.shareRunning = false;
        return;
    }
    { std::lock_guard<std::mutex> l(g.m); g.stage.clear(); g.applyNote.clear(); }
    g.phase = Phase::Sharing;
    g.shareRunning = false;
}

void liveApplyWorker(ShareSettings S, Plan plan, ShareSettings old) {
    // decoders and listening modes can change while people listen
    const std::string url = "http://127.0.0.1:" + std::to_string(g.port) + "/web/v1/admin/policy";
    const std::string hdr = (dir() / "admin.hdr").string();
    auto post = [&](const std::string& body) { int st = 0; curlPostJson(url, body, hdr, &st); return st == 200; };
    bool allOk = true;
    std::set<std::string> dOld = {}, dNew = {};
    const std::vector<DecoderRow> was = decoderRows(old, plan), now = decoderRows(S, plan);
    for (size_t i = 0; i < now.size() && i < was.size(); i++) {
        if (was[i].on == now[i].on) continue;
        std::string body = std::string("{\"") + (now[i].isDemod ? "demod" : "decoder") + "\":\"" + now[i].kind + "\",\"on\":" + (now[i].on ? "true" : "false");
        if (now[i].restricted && now[i].on) body += std::string(",\"accept_legal_notice\":\"") + onair_web::kLegalNoticeVersion + "\"";
        body += "}";
        allOk = post(body) && allOk;
    }
    std::lock_guard<std::mutex> l(g.m);
    g.appliedConf = serverConfText(S, plan);
    g.applyNote = allOk ? std::string() : tr("Some decoder changes could not be applied live. Press Apply again, or stop and start sharing.");
}

void stopWorker() {
    g.cancel = true;
    for (int i = 0; i < 200 && g.shareRunning; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));   // a start in progress ends first (it checks the flag every 50 ms)
    endChild(g.dirPid);
    endChild(g.tunnelPid);
    endChild(g.serverPid);
    std::error_code ec;
    fs::remove(dir() / "pids.txt", ec);
    fs::remove(dir() / "admin.hdr", ec);
    {
        std::lock_guard<std::mutex> l(g.m);
        g.url.clear(); g.stage.clear(); g.stats = ShareStats{}; g.publicState = -1;
        g.s2 = Step2State{};
    }
    g.phase = Phase::Idle;
    g.cancel = false;
}

void downloadWorker(ShareSettings S, RigCaps rig, Plan plan) {
    g.shareRunning = true;
    g.cancel = false;
    g.phase = Phase::Downloading;
    setStage(tr("Getting the tunnel helper ..."));
#ifdef _WIN32
    const char* file = "cloudflared-windows-amd64.exe";
    const char* out = "cloudflared.exe";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    const char* file = "cloudflared-darwin-arm64.tgz";
#else
    const char* file = "cloudflared-darwin-amd64.tgz";
#endif
    const char* out = "cloudflared.tgz";
#else
#if defined(__aarch64__)
    const char* file = "cloudflared-linux-arm64";
#else
    const char* file = "cloudflared-linux-amd64";
#endif
    const char* out = "cloudflared";
#endif
    const fs::path dst = dir() / out;
    const int rc = dect2::curlDownloadFile(std::string("https://github.com/cloudflare/cloudflared/releases/latest/download/") + file, dst.string(), 80ull << 20, 300, g.cancel, [] {});
    if (rc != 0) { if (!g.cancel) fail(tr("Could not download the helper."), std::string(dect2::curlDownloadError(rc)) + "  " + tr("You can also install cloudflared yourself (brew install cloudflared), or use Access > Link > Only my network.")); g.shareRunning = false; return; }
#ifdef __APPLE__
    std::string err;
    const long tp = plat::spawnChild("/usr/bin/tar", {"-xzf", dst.string(), "-C", dir().string()}, (dir() / "untar.log").string(), err);
    for (int i = 0; i < 300 && tp && plat::childAlive(tp); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::error_code ec;
    fs::remove(dst, ec);
#endif
#ifndef _WIN32
    std::error_code ec2;
    fs::permissions(dir() / "cloudflared", fs::perms::owner_all, ec2);
#endif
    if (findHelper().empty()) { fail(tr("The helper did not unpack."), tr("Install cloudflared yourself, or use Access > Link > Only my network.")); g.shareRunning = false; return; }
    g.shareRunning = false;
    shareWorker(S, rig, plan);
}

// ---------------------------------------------------------------- step 2
std::string dirSigOf(const ShareSettings& S, const Plan& plan, const std::string& url) {
    return buildDirectoryConf(S, plan, g.port, url, "", (dir() / "web_directory.key").string());
}

void previewWorker(ShareSettings S, Plan plan, std::string url) {
    g.previewRunning = 1;
    const std::string conf = dirSigOf(S, plan, url);
    writeFile(dir() / "directory.conf", conf, true);
    std::string err;
    const fs::path log = dir() / "preview.log";
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "directory.conf").string(), "--directory-preview"}, log.string(), err);
    std::string out;
    if (pid) {
        for (int i = 0; i < 300 && plat::childAlive(pid) && !g.cancel; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (plat::childAlive(pid)) plat::killChild(pid);
        out = slurp(log, 16384);
    } else out = "error=" + err;
    std::lock_guard<std::mutex> l(g.m);
    step2PreviewResult(g.s2, out, out.compare(0, 4, "ack=") == 0);
    g.s2Sig = conf;
    g.previewRunning = 0;
}

void listWorker(ShareSettings S, Plan plan, std::string url, std::string ack) {
    endChild(g.dirPid);
    std::error_code ec;
    fs::remove(dir() / "web_directory_status.json", ec);
    writeFile(dir() / "directory.conf", buildDirectoryConf(S, plan, g.port, url, ack, (dir() / "web_directory.key").string()), true);
    std::string err;
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "directory.conf").string(), "--directory-run"}, (dir() / "directory.log").string(), err);
    if (!pid) {
        std::lock_guard<std::mutex> l(g.m);
        g.s2.s = Step2::ListFailed;
        g.s2.problem = Problem{"start", tr("The listing program could not be started."), err, false};
        return;
    }
    g.dirPid = pid;
    notePids();
}

void removeWorker(ShareSettings S, Plan plan, std::string url) {
    endChild(g.dirPid);
    writeFile(dir() / "directory.conf", buildDirectoryConf(S, plan, g.port, url, "", (dir() / "web_directory.key").string()), true);
    std::string err;
    const fs::path log = dir() / "remove.log";
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "directory.conf").string(), "--directory-remove"}, log.string(), err);
    for (int i = 0; i < 400 && pid && plat::childAlive(pid); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (pid && plat::childAlive(pid)) plat::killChild(pid);
    const std::string out = slurp(log, 4096);
    std::error_code ec;
    fs::remove(dir() / "web_directory_status.json", ec);
    std::lock_guard<std::mutex> l(g.m);
    g.s2 = Step2State{};
    g.s2Sig.clear();
    if (out.find("removed from the directory") == std::string::npos) {
        g.s2.s = Step2::PreviewFailed;
        g.s2.problem = Problem{"remove", tr("The directory could not be told to remove the listing."), lastLines(out, 3) + "  " + tr("The entry disappears by itself about 15 minutes after the last heartbeat."), false};
    }
}

void testWorker(ShareSettings S, Plan plan, std::string url) {
    g.testRunning = 1;
    { std::lock_guard<std::mutex> l(g.m); g.test.clear(); g.testDone = false; }
    writeFile(dir() / "directory.conf", dirSigOf(S, plan, url), true);
    std::string err;
    const fs::path log = dir() / "test.log";
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "directory.conf").string(), "--directory-test"}, log.string(), err);
    size_t seen = 0;
    for (int i = 0; i < 800 && !g.cancel; i++) {   // 40 s at most
        const std::string out = slurp(log, 65536);
        size_t pos = seen;
        for (size_t nl; (nl = out.find('\n', pos)) != std::string::npos; pos = nl + 1) {
            TestStep ts;
            if (parseTestLine(out.substr(pos, nl - pos), ts)) { std::lock_guard<std::mutex> l(g.m); if (ts.done) g.testDone = true; else g.test.push_back(ts); }
        }
        seen = pos;
        if (g.testDone) break;
        if (!pid || !plat::childAlive(pid)) { if (out.size() <= seen + 0) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (pid && plat::childAlive(pid)) plat::killChild(pid);
    {
        std::lock_guard<std::mutex> l(g.m);
        if (!g.testDone) {
            TestStep ts;
            ts.step = "directory"; ts.code = "silent"; ts.ok = false;
            ts.message = pid ? tr("The test did not finish in time.") : tr("The test program could not be started.");
            ts.advice = pid ? tr("The network is very slow or blocks the connection. Try another network.") : err;
            if (g.test.empty()) g.test.push_back(ts);
            g.testDone = true;
        }
    }
    g.testRunning = 0;
}

void statsWorker(std::string url, bool checkPublic) {
    int st = 0;
    const std::string body = curlGet("http://127.0.0.1:" + std::to_string(g.port) + "/web/v1/admin/stats", 3, &st, (dir() / "admin.hdr").string());
    ShareStats s;
    const bool ok = st == 200 && parseStats(body, s);
    int pub = -1;
    if (checkPublic && !url.empty()) {
        int ps = 0;
        const std::string pb = curlGet(url + "/web/v1/info", 8, &ps);
        pub = ps == 200 && pb.find("\"proto\"") != std::string::npos ? 1 : 0;
    }
    std::lock_guard<std::mutex> l(g.m);
    if (ok) g.stats = s;
    if (pub >= 0) { g.publicState = pub; g.publicNote = pub ? "" : tr("The public address does not answer right now."); }
}

// ---------------------------------------------------------------- demo (screenshots of the window: DECT2_WEBSHARE_DEMO=step1|dropdown|decoders|preview|error)
std::vector<RigCaps> demoRigs() {
    std::vector<RigCaps> v;
    auto add = [&](const char* n, double lo, double hi, double rate, bool net, bool test) { RigCaps r; r.name = n; r.minHz = lo; r.maxHz = hi; r.maxRate = rate; r.network = net; r.test = test; v.push_back(r); };
    add("RTL-SDR Blog V4 (serial 00000001)", 24e6, 1766e6, 3.2e6, false, false);
    add("HackRF Pro (serial 0000a0)", 1e6, 6e9, 20e6, false, false);
    add("Airspy R2 (serial 7a31)", 24e6, 1800e6, 10e6, false, false);
    add("SDRplay RSPdx (serial 22010)", 1e3, 2e9, 10e6, false, false);
    add("rtl_tcp 192.168.1.20:1234 (network)", 24e6, 1766e6, 2.4e6, true, false);
    add("Test signal (no radio)", 0, 0, 0, false, true);
    return v;
}

void setupDemo(const char* mode) {
    g.demo = true;
    g.open = true;
    g.loaded = true;
    g.S = ShareSettings{};
    g.S.rig = "HackRF Pro (serial 0000a0)";
    g.S.name = "Roof antenna, Vilnius";
    g.S.loc = "54.6872,25.2797";
    g.S.desc = "FM, airband and ADS-B from the roof";
    g.S.centreMHz = 97.75; g.S.wholeSpan = true;
    std::string m = mode ? mode : "step1";
    g.tab = 0;
    if (m == "decoders") g.tab = 1;
    if (m == "preview" || m == "error") g.tab = 2;
    if (m == "decoders") { g.S.wholeSpan = false; g.S.centreMHz = 127.5; g.S.spanMHz = 19; g.S.dataOff.insert("ft8"); }
    if (m == "preview" || m == "error") {
        g.phase = Phase::Sharing;
        g.url = "https://quiet-river-lamp-7.trycloudflare.com";
        g.stats.ok = true; g.stats.users = 3; g.stats.maxUsers = 8; g.stats.kbps = 412; g.stats.maxKbps = 8000; g.stats.cpuPct = 18.5; g.stats.centreHz = 97.75e6; g.stats.rateHz = 20e6;
        g.publicState = 1;
    }
    if (m == "preview") {
        g.s2.s = Step2::PreviewReady;
        g.s2.ack = "9f3c01a7b2de";
        g.s2.preview = "OnAir Web directory: every 300 s this server will send an HTTPS POST to\n  https://web.on-air.dev/api/v1/heartbeat\nwith exactly this JSON (time, users and capture coverage change at each beat):\n"
                       "  {\"v\":1,\"action\":\"heartbeat\",\"id\":\"onair-5e2a91c0d4b7f318\",\"pub\":\"...\",\"ts\":1793440000,\"name\":\"Roof antenna, Vilnius\",\"url\":\"https://quiet-river-lamp-7.trycloudflare.com/\",\n"
                       "   \"loc\":\"54.69,25.28\",\"freq\":[[87750000,107250000]],\"radio\":\"HackRF Pro\",\"cap\":8,\"users\":0,\"interval\":300,\"ver\":\"3.5\",\"contact\":\"\",\"desc\":\"FM, airband and ADS-B from the roof\",\"reach\":0,\"sig\":\"<signature made with this server's key>\"}\n"
                       "Nothing else is sent: no listener addresses, names or what anyone listens to, only the number of users.\n";
    }
    if (m == "error") {
        g.s2.s = Step2::ListFailed;
        g.s2.problem = Problem{"resolving", "The directory cannot find your public address in DNS yet.", "A new tunnel address can take up to a minute to appear. OnAir keeps trying by itself; if it still fails after 5 minutes, stop sharing and share again.", true};
        g.s2.fails = 3;
        TestStep a; a.step = "directory"; a.ok = true; a.ms = 184; a.message = "The directory web.on-air.dev answers (184 ms).";
        TestStep b; b.step = "local"; b.ok = true; b.ms = 6; b.message = "The OnAir web server on this computer answers.";
        TestStep c; c.step = "public"; c.ok = false; c.ms = 12004; c.message = "Your public address https://quiet-river-lamp-7.trycloudflare.com/ does not answer (timeout).";
        c.advice = "If you use the built-in tunnel: stop sharing and share again to get a fresh address. If you use your own address: your router or firewall blocks the port. Use the built-in tunnel, UPnP, or forward the port by hand (docs/ONAIR-WEB.md, 'Port forwarding').";
        g.test = {a, b, c};
        g.testDone = true;
        g.publicState = 0;
    }
}

} // namespace

// ================================================================ the window
namespace {

ImVec4 okCol() { return ImVec4(.4f, 1.f, .5f, 1.f); }
ImVec4 warnCol() { return ImVec4(1.f, .75f, .3f, 1.f); }
ImVec4 badCol() { return ImVec4(1.f, .5f, .4f, 1.f); }

void textField(const char* label, const char* id, std::string& s, size_t cap, float width, const char* hint = nullptr, ImGuiInputTextFlags fl = 0) {
    char b[320];
    snprintf(b, sizeof b, "%s", s.c_str());
    ImGui::SetNextItemWidth(width * gUi);
    if (label && *label) { ImGui::TextUnformatted(label); }
    const bool ch = hint ? ImGui::InputTextWithHint(id, hint, b, std::min(sizeof b, cap + 1), fl) : ImGui::InputText(id, b, std::min(sizeof b, cap + 1), fl);
    if (ch) { s = b; markDirty(); }
}

bool dblField(const char* id, double& v, double step, const char* fmt, float width = 110) {
    ImGui::SetNextItemWidth(width * gUi);
    double x = v;
    if (ImGui::InputDouble(id, &x, step, step * 10, fmt)) { if (x > 0 && x < 1e9) { v = x; markDirty(); return true; } }
    return false;
}

void cpuDots(int cpu) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = 7 * gUi, gap = 3 * gUi;
    for (int i = 0; i < 5; i++) {
        const ImVec2 a(p.x + i * (s + gap), p.y + 4 * gUi);
        const ImU32 col = i < cpu ? IM_COL32(120, 200, 140, 255) : IM_COL32(110, 110, 110, 90);
        dl->AddRectFilled(a, ImVec2(a.x + s, a.y + s), col, 1.5f * gUi);
    }
    ImGui::Dummy(ImVec2(5 * (s + gap), ImGui::GetTextLineHeight()));
}

void drawQr(const std::string& text, float px) {
    QrCode q;
    if (!qrEncode(text, q)) { ImGui::TextDisabled("%s", tr("The link is too long for a QR code.")); return; }
    const int quiet = 2;
    const float m = px / (float)(q.size + 2 * quiet);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + px, p.y + px), IM_COL32(255, 255, 255, 255));
    for (int y = 0; y < q.size; y++)
        for (int x = 0; x < q.size; x++)
            if (q.dark(x, y)) dl->AddRectFilled(ImVec2(p.x + (x + quiet) * m, p.y + (y + quiet) * m), ImVec2(p.x + (x + quiet + 1) * m, p.y + (y + quiet + 1) * m), IM_COL32(0, 0, 0, 255));
    ImGui::Dummy(ImVec2(px, px));
}

void wrapped(const ImVec4& col, const std::string& s) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", s.c_str());
    ImGui::PopStyleColor();
}

void problemBlock(const Problem& p, const char* headline) {
    ImGui::PushStyleColor(ImGuiCol_Text, p.temporary ? warnCol() : badCol());
    ImGui::TextWrapped("%s %s", headline, p.message.c_str());
    ImGui::PopStyleColor();
    if (!p.advice.empty()) { ImGui::Indent(12 * gUi); ImGui::TextWrapped("%s %s", tr("What to do:"), p.advice.c_str()); ImGui::Unindent(12 * gUi); }
}

std::string currentUrl() { std::lock_guard<std::mutex> l(g.m); return g.url; }

// ---- tab 1: radio and range
void tabRadio(App& a, const std::vector<RigCaps>& rigs, const RigCaps& rig, const Plan& plan, bool running) {
    struct IdG { explicit IdG(const char* s) { ImGui::PushID(s); } ~IdG() { ImGui::PopID(); } } idg("wsRadio");
    ShareSettings& S = g.S;
    if (running) ImGui::BeginDisabled();
    ImGui::TextUnformatted(tr("Radio (rig)"));
    ImGui::SetNextItemWidth(380 * gUi);
    const std::string preview = S.rig.empty() ? (rigs.empty() ? std::string(tr("No radio found")) : rigs.front().name) : S.rig;
    if (ImGui::BeginCombo("##wsRig", preview.c_str())) {
        for (const auto& r : rigs) {
            const bool sel = r.name == S.rig || (S.rig.empty() && &r == &rigs.front());
            if (ImGui::Selectable((r.name + "##rig").c_str(), sel)) {
                S.rig = r.name;
                if (!S.wholeSpan || true) {
                    const double lo = r.minHz / 1e6, hi = r.maxHz / 1e6;
                    if (hi > 0 && (S.centreMHz < lo || S.centreMHz > hi)) S.centreMHz = defaultCentreMHz(r);
                }
                markDirty();
            }
            ImGui::SameLine(); ImGui::TextDisabled("  %s", capsLine(r).c_str());
        }
        if (rigs.empty()) ImGui::TextDisabled("%s", tr("Plug in a radio and press Refresh."));
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Refresh##wsRefresh"))) { if (!g.demo) refreshDevices(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Look for radios again (a radio that is in use by OnAir's own receiver is listed as it was)."));
    ImGui::TextDisabled("%s", capsLine(rig).c_str());
    if (!S.rig.empty() && std::none_of(rigs.begin(), rigs.end(), [&](const RigCaps& r) { return r.name == S.rig; })) wrapped(warnCol(), tr("The radio you chose is not connected. Plug it in and press Refresh, or choose another one."));
    ImGui::Spacing();

    ImGui::SeparatorText(tr("Frequency range"));
    ImGui::TextUnformatted(tr("Quick choice"));
    ImGui::SetNextItemWidth(300 * gUi);
    if (ImGui::BeginCombo("##wsPreset", S.wholeSpan ? tr("Whole span of the radio (default)") : tr("Custom or a quick choice ..."))) {
        if (ImGui::Selectable(tr("Whole span of the radio (default)"), S.wholeSpan)) { S.wholeSpan = true; S.rateAuto = true; if (rig.maxHz > 0 && (S.centreMHz * 1e6 < rig.minHz || S.centreMHz * 1e6 > rig.maxHz)) S.centreMHz = defaultCentreMHz(rig); markDirty(); }
        for (const auto& p : rangePresets()) if (ImGui::Selectable(tr(p.label), false)) { applyPreset(S, p); markDirty(); }
        ImGui::EndCombo();
    }
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Centre frequency (MHz)"));
    dblField("##wsCentre", S.centreMHz, 0.1, "%.4f");
    if (!S.wholeSpan) {
        ImGui::SameLine(); ImGui::TextUnformatted(tr("Span (MHz)")); ImGui::SameLine();
        dblField("##wsSpan", S.spanMHz, 0.5, "%.3f");
    }
    if (ImGui::Checkbox(tr("Whole span of the radio"), &S.wholeSpan)) markDirty();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Use everything the radio can see at once around the centre frequency (its full sample rate)."));
    if (ImGui::TreeNode(trw("Advanced: sample rate and gain"))) {
        if (ImGui::Checkbox(tr("Choose the sample rate myself"), &S.rateAuto)) { S.rateAuto = !S.rateAuto; markDirty(); }
        if (!S.rateAuto) { ImGui::SameLine(); double r = S.rateMsps > 0 ? S.rateMsps : plan.rateHz / 1e6; ImGui::SetNextItemWidth(100 * gUi); if (ImGui::InputDouble("Msps##wsRate", &r, 0.5, 2, "%.3f") && r > 0) { S.rateMsps = r; markDirty(); } }
        if (ImGui::Checkbox(tr("Set the gain myself"), &S.gainManual)) markDirty();
        if (S.gainManual) { ImGui::SameLine(); ImGui::SetNextItemWidth(120 * gUi); float gdb = (float)S.gainDb; if (ImGui::SliderFloat("dB##wsGain", &gdb, 0, 80, "%.0f")) { S.gainDb = gdb; markDirty(); } }
        else ImGui::TextDisabled("%s", tr("Gain: OnAir's default for the radio."));
        ImGui::TreePop();
    }
    if (running) ImGui::EndDisabled();
    ImGui::Spacing();
    char line[200];
    snprintf(line, sizeof line, tr("Listeners will see %s MHz, captured at %.2f Msps around %.4f MHz."), rangeSpecMHz(plan.loHz, plan.hiHz).c_str(), plan.rateHz / 1e6, plan.centreHz / 1e6);
    ImGui::TextWrapped("%s", line);
    if (!plan.warning.empty()) wrapped(warnCol(), plan.warning);
    if (running) ImGui::TextDisabled("%s", tr("To change the radio or range, stop sharing first (changes to decoders and access can be applied while sharing)."));
}

// ---- tab 2: decoders
void tabDecoders(const Plan& plan) {
    ShareSettings& S = g.S;
    const std::vector<DecoderRow> rows = decoderRows(S, plan);
    ImGui::TextWrapped("%s", tr("Choose what the listeners of your page can do. Everything is on by default except digital voice, pagers and other message traffic."));
    std::string group;
    int idx = 0;
    for (const auto& r : rows) {
        if (r.group != group) {
            group = r.group;
            ImGui::Spacing();
            ImGui::SeparatorText(tr(group.c_str()));
            const bool restricted = r.restricted;
            if (restricted) {
                ImGui::TextWrapped("%s", tr("These can reveal private messages. Read the notice, then switch on only what you are allowed to share."));
                if (ImGui::TreeNode(trw("Read the legal notice"))) { ImGui::TextWrapped("%s", onair_web::legalNoticeText()); ImGui::TreePop(); }
                if (ImGui::Checkbox(tr("I have read it and take the responsibility"), &S.legalAck)) { if (!S.legalAck) S.restrictedOn.clear(); markDirty(); }
            }
            ImGui::PushID(group.c_str());
            if (!restricted || S.legalAck) {
                if (ImGui::SmallButton(tr("All on"))) { setGroup(S, group, true); markDirty(); }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("All off"))) { setGroup(S, group, false); markDirty(); }
            }
            ImGui::PopID();
        }
        ImGui::PushID(idx++);
        const bool locked = r.restricted && !S.legalAck;
        if (locked || !r.possible) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, locked ? 0.45f : 0.7f);
        if (locked) ImGui::BeginDisabled();
        bool on = r.on;
        if (ImGui::Checkbox(r.label.c_str(), &on)) {
            if (r.isDemod) {
                std::set<std::string> cur;
                std::string cc, tok;
                for (char c : S.demods + ",") { if (c == ',') { if (!tok.empty()) cur.insert(tok); tok.clear(); } else tok += c; }
                if (on) cur.insert(r.kind); else cur.erase(r.kind);
                S.demods.clear();
                for (const auto& k : cur) S.demods += (S.demods.empty() ? "" : ",") + k;
            } else if (r.restricted) { if (on) S.restrictedOn.insert(r.kind); else S.restrictedOn.erase(r.kind); }
            else { if (on) S.dataOff.erase(r.kind); else S.dataOff.insert(r.kind); }
            markDirty();
        }
        if (locked) ImGui::EndDisabled();
        if (!r.isDemod) {
            ImGui::SameLine(300 * gUi);
            cpuDots(r.cpu);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Rough CPU cost of one running decoder (1 light, 5 heavy)."));
            if (r.needsCodec) { ImGui::SameLine(); ImGui::TextColored(warnCol(), "%s", tr("codec")); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Needs a voice codec that you install yourself (docs/CODECS.md). Without it the decoder runs but stays silent.")); }
            if (!r.possible) { ImGui::SameLine(); ImGui::TextDisabled("%s", (std::string("(") + r.why + ")").c_str()); }
        }
        if (locked || !r.possible) ImGui::PopStyleVar();
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::SeparatorText(tr("Limits"));
    ImGui::SetNextItemWidth(110 * gUi);
    int mu = S.maxUsers;
    if (ImGui::InputInt(tr("Most listeners at once"), &mu, 1, 5)) { S.maxUsers = std::clamp(mu, 1, 1000); markDirty(); }
    ImGui::SetNextItemWidth(110 * gUi);
    int kb = S.userKbps;
    if (ImGui::InputInt(tr("Most kbit/s per listener (0 = default 512)"), &kb, 32, 128)) { S.userKbps = std::clamp(kb, 0, 100000); markDirty(); }
    if (ImGui::Checkbox(tr("Audio only (no spectrum or waterfall for listeners)"), &S.audioOnly)) markDirty();
    if (ImGui::Checkbox(tr("Listeners may tune only inside the shared range"), &S.limitToRange)) markDirty();
    char est[160];
    snprintf(est, sizeof est, tr("Estimated load: decoders %d points, up to %d listeners. A listener costs roughly a quarter of one core."), cpuScore(rows), S.maxUsers);
    ImGui::TextDisabled("%s", est);
}

// ---- tab 3: access and listing
void testBlock(const ShareSettings& S, const Plan& plan, const std::string& url, bool sharing) {
    struct IdG { explicit IdG(const char* s) { ImGui::PushID(s); } ~IdG() { ImGui::PopID(); } } idg("wsTest");
    const bool busy = g.testRunning > 0;
    if (busy) ImGui::BeginDisabled();
    if (ImGui::Button(tr("Test connection"))) { if (!g.demo) spawnJob([S, plan, url] { testWorker(S, plan, url); }); }
    if (busy) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Checks the directory, the web server on this computer and your public address, one after the other. Nothing is listed or changed."));
    if (busy) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Testing ...")); }
    std::vector<TestStep> steps;
    { std::lock_guard<std::mutex> l(g.m); steps = g.test; }
    for (const auto& t : steps) {
        ImGui::TextColored(t.ok ? okCol() : badCol(), "%s", t.ok ? tr("OK") : tr("FAILED"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s", tr(testStepTitle(t.step)), t.message.c_str());
        if (!t.ok && !t.advice.empty()) { ImGui::Indent(24 * gUi); ImGui::TextWrapped("%s %s", tr("What to do:"), t.advice.c_str()); ImGui::Unindent(24 * gUi); }
    }
    (void)sharing;
}

void tabAccess(App& a, const Plan& plan, bool running) {
    struct IdG { explicit IdG(const char* s) { ImGui::PushID(s); } ~IdG() { ImGui::PopID(); } } idg("wsAccess");
    (void)a;
    ShareSettings& S = g.S;
    const std::string url = currentUrl();
    ImGui::SeparatorText(tr("Who can listen"));
    textField(tr("Password (optional, 8 characters or more; empty = anyone with the link)"), "##wsPw", S.password, 64, 260, nullptr, ImGuiInputTextFlags_Password);
    if (!S.password.empty() && S.password.size() < 8) wrapped(warnCol(), tr("The password is too short: the server would not start with it."));
    if (ImGui::TreeNode(trw("Advanced: how listeners reach you"))) {
        if (running) ImGui::BeginDisabled();
        int mode = S.link == "own" ? 1 : S.link == "lan" ? 2 : 0;
        if (ImGui::RadioButton(tr("Free public address (Cloudflare tunnel, nothing to set up)"), mode == 0)) { S.link = "tunnel"; markDirty(); }
        if (ImGui::RadioButton(tr("My own https address (a reverse proxy on this computer, for example Caddy or nginx)"), mode == 1)) { S.link = "own"; markDirty(); }
        if (ImGui::RadioButton(tr("Only my network (a phone on the same Wi-Fi)"), mode == 2)) { S.link = "lan"; markDirty(); }
        if (S.link == "own") textField(nullptr, "##wsOwn", S.ownUrl, 190, 340, "https://radio.example.com");
        if (running) ImGui::EndDisabled();
        ImGui::TextDisabled("%s", tr("Behind a router that blocks incoming connections, use the free public address: it needs no open port."));
        ImGui::TreePop();
    }

    ImGui::Spacing();
    ImGui::SeparatorText(tr("Step 2: list it on the public directory (optional)"));
    ImGui::TextWrapped("%s", tr("The directory (web.on-air.dev) is a public list of OnAir Web servers. Nothing is sent until you have seen exactly what will be sent and pressed List it."));
    textField(tr("Name"), "##wsName", S.name, 48, 280);
    textField(tr("Description (optional)"), "##wsDesc", S.desc, 280, 380);
    textField(tr("Location (optional): latitude,longitude or a locator such as KO24"), "##wsLoc", S.loc, 40, 220, "54.69,25.28");
    {
        ImGui::SetNextItemWidth(260 * gUi);
        static const char* kRound[] = {"About 100 km (very private)", "About 10 km", "About 1 km (default)", "About 100 m", "About 10 m", "About 1 m"};
        int d = std::clamp(S.locDecimals, 0, 5);
        if (ImGui::Combo(tr("Location is rounded to##wsRound"), &d, kRound, 6)) { S.locDecimals = d; markDirty(); }
    }
    textField(tr("Radio shown in the list (optional)"), "##wsRadioLabel", S.radioLabel, 48, 260, plan.device.c_str());
    textField(tr("Contact (optional, shown publicly)"), "##wsContact", S.contact, 80, 260);
    if (ImGui::Checkbox(tr("Let the directory check once an hour that my server answers"), &S.reach)) markDirty();

    std::string why;
    const bool valid = settingsValid(S, why);
    if (!valid) wrapped(warnCol(), why);
    Step2State s2;
    std::string sig;
    { std::lock_guard<std::mutex> l(g.m); s2 = g.s2; sig = g.s2Sig; }
    const bool sharing = !url.empty() && g.phase == Phase::Sharing;
    const bool stale = (s2.s == Step2::PreviewReady || s2.s == Step2::Listing || s2.s == Step2::Listed) && !sig.empty() && !g.demo && sig != dirSigOf(S, plan, url);
    ImGui::Spacing();
    if (!sharing) ImGui::TextDisabled("%s", tr("Start sharing (step 1) first: the directory lists the public address."));
    const bool previewing = g.previewRunning > 0;
    if (!sharing || !valid || previewing) ImGui::BeginDisabled();
    if (s2.s == Step2::Off || s2.s == Step2::PreviewFailed || s2.s == Step2::ListFailed || stale) {
        if (ImGui::Button(stale ? tr("Preview the new listing") : tr("Preview what will be sent"))) {
            { std::lock_guard<std::mutex> l(g.m); g.s2 = Step2State{}; g.s2.s = Step2::Previewing; }
            const ShareSettings Sc = S;
            if (!g.demo) spawnJob([Sc, plan, url] { previewWorker(Sc, plan, url); });
        }
    }
    if (!sharing || !valid || previewing) ImGui::EndDisabled();
    if (previewing || s2.s == Step2::Previewing) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Making the preview ...")); }
    if (stale) wrapped(warnCol(), tr("You changed something that is part of the listing. Preview it again before it is sent."));

    if (s2.s == Step2::PreviewFailed) problemBlock(s2.problem, tr("The preview failed."));
    if (s2.s == Step2::PreviewReady && !stale) {
        ImGui::TextWrapped("%s", tr("This is exactly what will be sent to the public directory every 5 minutes. Never sent: listener addresses or what anyone listens to."));
        ImGui::BeginChild("##wsPrev", ImVec2(0, 150 * gUi), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(s2.preview.c_str());
        ImGui::EndChild();
        if (ImGui::Button(tr("List it"))) {
            { std::lock_guard<std::mutex> l(g.m); step2Confirm(g.s2, (long long)std::time(nullptr)); }
            const ShareSettings Sc = S;
            const std::string ack = s2.ack;
            if (!g.demo) spawnJob([Sc, plan, url, ack] { listWorker(Sc, plan, url, ack); });
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Not now"))) { std::lock_guard<std::mutex> l(g.m); step2Cancel(g.s2); }
    }
    if (s2.s == Step2::Listing) {
        ImGui::TextColored(warnCol(), "%s", tr("Listing ... (waiting for the directory's answer)"));
        if (s2.fails > 0) problemBlock(s2.problem, tr("Not yet:"));
    }
    if (s2.s == Step2::Listed) {
        ImGui::TextColored(okCol(), "%s", tr("Listed on the public directory."));
        if (s2.lastOk) { char b[64]; time_t t = (time_t)s2.lastOk; struct tm tmv;
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);
#endif
            strftime(b, sizeof b, "%H:%M:%S", &tmv); ImGui::SameLine(); ImGui::TextDisabled("%s %s", tr("Last check:"), b); }
        if (ImGui::SmallButton(tr("Remove from the directory"))) { const ShareSettings Sc = S; if (!g.demo) spawnJob([Sc, plan, url] { removeWorker(Sc, plan, url); }); }
    }
    if (s2.s == Step2::ListFailed) {
        problemBlock(s2.problem, tr("The listing did not work."));
        if (!s2.problem.temporary || true) {
            if (ImGui::SmallButton(tr("Remove from the directory"))) { const ShareSettings Sc = S; if (!g.demo) spawnJob([Sc, plan, url] { removeWorker(Sc, plan, url); }); }
        }
    }
    ImGui::Spacing();
    testBlock(S, plan, url, sharing);
}

// ---- the status block (always at the top)
void statusBlock(App& a, const std::vector<RigCaps>& rigs, const RigCaps& rig, const Plan& plan) {
    struct IdG { explicit IdG(const char* s) { ImGui::PushID(s); } ~IdG() { ImGui::PopID(); } } idg("wsStatus");
    ShareSettings& S = g.S;
    const Phase ph = g.phase;
    std::string url, stage, error, advice, applyNote, publicNote;
    ShareStats st;
    int pub;
    { std::lock_guard<std::mutex> l(g.m); url = g.url; stage = g.stage; error = g.error; advice = g.advice; st = g.stats; pub = g.publicState; applyNote = g.applyNote; publicNote = g.publicNote; }
    std::string why;
    const bool valid = settingsValid(S, why);
    if (ph == Phase::Idle || ph == Phase::Failed || ph == Phase::NeedHelper) {
        if (ph == Phase::Failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, badCol()); ImGui::TextWrapped("%s", error.c_str()); ImGui::PopStyleColor();
            if (!advice.empty()) { ImGui::Indent(12 * gUi); ImGui::TextWrapped("%s %s", tr("What to do:"), advice.c_str()); ImGui::Unindent(12 * gUi); }
        } else ImGui::TextDisabled("%s", tr("Not sharing."));
        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button(tr("Start sharing"), ImVec2(180 * gUi, 0))) {
            const ShareSettings Sc = S;
            const RigCaps rc = rig;
            const Plan pl = plan;
            if (g.demo) { g.phase = Phase::Sharing; g.url = "https://quiet-river-lamp-7.trycloudflare.com"; }
            else {
                const bool sameRadio = a.engine.running() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && (rc.test ? a.devices[(size_t)a.devIdx].kind == dect2::DeviceInfo::Synthetic : a.devices[(size_t)a.devIdx].name == rc.name);
                if (sameRadio) a.engine.stop();   // one radio, one user: only when OnAir's own receiver is on this very radio
                if (S.link == "tunnel" && findHelper().empty()) g.phase = Phase::NeedHelper;
                else spawnJob([Sc, rc, pl] { shareWorker(Sc, rc, pl); });
            }
        }
        if (!valid) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", valid ? tr("Starts a web page for your radio and gives it a public address. Choose the radio, range and decoders in the tabs below first, or just press this for the defaults.") : why.c_str());
        (void)rigs;
        return;
    }
    if (ph == Phase::Downloading || ph == Phase::Starting || ph == Phase::Stopping) {
        ImGui::TextColored(warnCol(), "%s", ph == Phase::Stopping ? tr("Stopping ...") : stage.empty() ? tr("Starting ...") : stage.c_str());
        if (ph != Phase::Stopping && ImGui::SmallButton(tr("Cancel"))) { g.phase = Phase::Stopping; spawnJob([] { stopWorker(); }); }
        return;
    }
    // sharing
    ImGui::TextColored(okCol(), "%s", tr("Sharing on the web"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Stop sharing"))) { g.phase = Phase::Stopping; if (!g.demo) spawnJob([] { stopWorker(); }); else { g.phase = Phase::Idle; g.url.clear(); } }
    ImGui::Text("%s", url.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Copy link"))) ImGui::SetClipboardText(url.c_str());
    static bool showQr = true;
    ImGui::SameLine();
    ImGui::Checkbox(tr("QR code"), &showQr);
    if (showQr && !url.empty()) { drawQr(linkQrText(url), 128 * gUi); }
    if (st.ok) {
        ImGui::Text(tr("Listeners: %d of %d    Sending to listeners: %.0f of %.0f kbit/s    CPU: %.0f%%"), st.users, st.maxUsers, st.kbps, st.maxKbps, st.cpuPct);
        ImGui::TextDisabled(tr("Radio input: %.2f Msps around %.4f MHz%s"), st.rateHz / 1e6, st.centreHz / 1e6, st.iqDropped > 0 ? tr(" (the computer dropped samples: lower the sample rate)") : "");
    } else ImGui::TextDisabled("%s", tr("Waiting for the first numbers ..."));
    if (pub == 1) ImGui::TextColored(okCol(), "%s", tr("The public address answers."));
    else if (pub == 0) { wrapped(warnCol(), publicNote.empty() ? tr("The public address does not answer.") : publicNote); }
    // pending changes
    const std::string now = serverConfText(S, plan);
    std::string applied;
    { std::lock_guard<std::mutex> l(g.m); applied = g.appliedConf; }
    if (!g.demo && !applied.empty() && now != applied) {
        const bool onlyPolicy = stripPolicy(now) == stripPolicy(applied);
        wrapped(warnCol(), onlyPolicy ? tr("Decoder changes are not applied yet.") : tr("Changes are not applied yet. Applying restarts the web server for a few seconds; the public link stays the same."));
        if (ImGui::Button(tr("Apply changes"))) {
            const ShareSettings Sc = S; const Plan pl = plan;
            if (onlyPolicy) {
                // the old settings are what the applied conf says; rebuild them from the text of the last apply
                ShareSettings prev = S;
                prev.dataOff.clear(); prev.restrictedOn.clear(); prev.demods.clear(); prev.legalAck = false;
                std::istringstream in(applied);
                std::string l;
                while (std::getline(in, l)) {
                    auto val = [&](const char* k) { const size_t n = strlen(k); return l.compare(0, n, k) == 0 ? l.substr(n) : std::string(); };
                    const std::string dm = val("web_demods="), off = val("web_decoders_off="), on = val("web_decoders_on=");
                    if (l.compare(0, 11, "web_demods=") == 0) prev.demods = dm;
                    if (l.compare(0, 17, "web_decoders_off=") == 0) { std::string t; for (char c : off + ",") { if (c == ',') { if (!t.empty()) prev.dataOff.insert(t); t.clear(); } else t += c; } }
                    if (l.compare(0, 16, "web_decoders_on=") == 0) { std::string t; for (char c : on + ",") { if (c == ',') { if (!t.empty()) prev.restrictedOn.insert(t); t.clear(); } else t += c; } }
                    if (l.compare(0, 14, "web_legal_ack=") == 0) prev.legalAck = true;
                }
                spawnJob([Sc, pl, prev] { liveApplyWorker(Sc, pl, prev); });
            } else spawnJob([Sc, pl] { restartServerWorker(Sc, pl); });
        }
    }
    if (!applyNote.empty()) wrapped(warnCol(), applyNote);
}

} // namespace

// ---------------------------------------------------------------- tick and window
namespace {

void tickState(const std::vector<RigCaps>&, const RigCaps&, const Plan&) {
    reapJobs();
    saveSettingsIfDue();
    if (g.demo) return;
    const double now = ImGui::GetTime();
    if (g.phase == Phase::Sharing && now - g.statsAt > 2.0) {
        g.statsAt = now;
        bool inJob;
        { std::lock_guard<std::mutex> l(g.jm); inJob = g.jobs.size() > 3; }
        if (!inJob) {
            const std::string url = currentUrl();
            const bool pub = g.S.link != "lan" && now - g.publicAt > 30.0;
            if (pub) g.publicAt = now;
            spawnJob([url, pub] { statsWorker(url, pub); });
        }
        if (!plat::childAlive(g.serverPid)) {
            if (g.serverPid) { g.serverPid = 0; fail(tr("The web server stopped."), lastLines(slurp(dir() / "server.log", 8192), 4) + "  " + tr("Press Start sharing to try again.")); endChild(g.tunnelPid); endChild(g.dirPid); }
        }
    }
    // step 2: read the listing program's status file (small, quick)
    Step2 s;
    { std::lock_guard<std::mutex> l(g.m); s = g.s2.s; }
    if (s == Step2::Listing || s == Step2::Listed || s == Step2::ListFailed) {
        static double lastRead = 0;
        if (now - lastRead > 1.0) {
            lastRead = now;
            const std::string js = slurp(dir() / "web_directory_status.json", 4096);
            const bool alive = g.dirPid && plat::childAlive(g.dirPid);
            std::lock_guard<std::mutex> l(g.m);
            step2Status(g.s2, js, alive, (long long)std::time(nullptr));
        }
    }
}

} // namespace

void stopAll() {
    g.cancel = true;
    {
        std::lock_guard<std::mutex> l(g.jm);
        for (auto& j : g.jobs) if (j.th.joinable()) j.th.join();
        g.jobs.clear();
    }
    for (auto* p : {&g.dirPid, &g.tunnelPid, &g.serverPid}) { const long pid = p->exchange(0); if (pid > 0) plat::stopChild(pid); }
    std::error_code ec;
    fs::remove(dir() / "pids.txt", ec);
    g.phase = Phase::Idle;
}

void drawItems(App& a, bool menu) {
    (void)a;
    const Phase ph = g.phase;
    ImGui::TextDisabled("%s", tr("OnAir Web"));
    const char* label = ph == Phase::Sharing ? tr("Share on web ... (sharing now)") : tr("Share on web ...");
    if (menu ? ImGui::MenuItem(label) : ImGui::Button(label)) g.open = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Choose the radio, the range and the decoders, then share it on the web with a link. Step 2 lists it on the public directory."));
    if (ph == Phase::Sharing) {
        const std::string url = currentUrl();
        ImGui::TextDisabled("%s", url.c_str());
    }
}

void drawPopups(App& a) {
    loadSettingsOnce();
    static bool demoChecked = false;
    if (!demoChecked) { demoChecked = true; if (const char* d = getenv("DECT2_WEBSHARE_DEMO")) setupDemo(d); }
    if (g.lastDrawFrame == ImGui::GetFrameCount()) return;   // the Server menu and the Server popup both call this
    g.lastDrawFrame = ImGui::GetFrameCount();

    // the helper download consent (a modal, as before)
    if (g.phase == Phase::NeedHelper) ImGui::OpenPopup("##wsHelper");
    if (ImGui::BeginPopupModal("##wsHelper", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr("To give your radio a public web address OnAir needs a small free helper program, cloudflared by Cloudflare (no account needed)."));
        ImGui::TextWrapped("%s", tr("It will be downloaded once from github.com/cloudflare/cloudflared into OnAir's folder (about 30 MB). While you share, traffic to your page goes through Cloudflare."));
        if (ImGui::Button(tr("Download and continue"))) {
            ImGui::CloseCurrentPopup();
            const std::vector<RigCaps> rigs = shareableRigs(a.devices);
            const RigCaps rc = currentRig(rigs, g.S);
            const ShareSettings Sc = g.S; const Plan pl = resolvePlan(g.S, rc);
            spawnJob([Sc, rc, pl] { downloadWorker(Sc, rc, pl); });
            g.phase = Phase::Downloading;
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##wsHelperCancel"))) { ImGui::CloseCurrentPopup(); g.phase = Phase::Idle; }
        ImGui::EndPopup();
    }
    if (!g.open) { reapJobs(); return; }

    std::vector<RigCaps> rigs = g.demo ? demoRigs() : shareableRigs(a.devices);
    if (g.S.rig.empty() && !rigs.empty()) g.S.rig = rigs.front().name;
    const RigCaps rig = currentRig(rigs, g.S);
    if (g.S.link != "own" && g.S.centreMHz <= 0) g.S.centreMHz = defaultCentreMHz(rig);
    const Plan plan = resolvePlan(g.S, rig);
    tickState(rigs, rig, plan);

    firstWindowSize(700 * gUi, 760 * gUi);
    ImGui::SetNextWindowPos(ImVec2(60 * gUi, 50 * gUi), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Share on web##wsWindow", &g.open)) {
        statusBlock(a, rigs, rig, plan);
        ImGui::Separator();
        const bool running = g.phase == Phase::Sharing || g.phase == Phase::Starting || g.phase == Phase::Downloading || g.phase == Phase::Stopping;
        if (ImGui::BeginTabBar("##wsTabs")) {
            if (ImGui::BeginTabItem(trw("Radio and range"), nullptr, g.tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) { if (ImGui::IsItemVisible()) {} tabRadio(a, rigs, rig, plan, running); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Decoders and limits"), nullptr, g.tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) { tabDecoders(plan); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(trw("Access and listing"), nullptr, g.tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) { tabAccess(a, plan, running); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        g.tab = -1;   // the demo / first-open selection applies once
    }
    ImGui::End();
}

} // namespace webshare
