#include "webshare.h"
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/common/thread_name.h"
#include "dect2/engine/updater.h"
#include "../map/pos_predict.h"
#include <atomic>
#include <functional>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace webshare {

namespace {

enum class Phase { Idle, NeedHelper, Downloading, Starting, Sharing, Preview, Listed, Failed };

constexpr int kPort = 8073;

struct State {
    std::mutex m;
    std::atomic<Phase> phase{Phase::Idle};
    std::atomic<long> serverPid{0}, tunnelPid{0};
    std::string url, error, preview, ackCode, helperPath;
    std::atomic<bool> cancel{false};
    char name[64] = "OnAir Web", loc[40] = "", radio[48] = "";
    bool wantPreview = false;
    std::thread worker;
};
State g;

fs::path dir() {
    fs::path d = fs::path(plat::dataDir()) / "webshare";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string exeDirPath() {
    std::string exe = plat::exePath();
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

void fail(const std::string& why) {
    std::lock_guard<std::mutex> l(g.m);
    g.error = why;
    g.phase = Phase::Failed;
}

void writeConf(bool withDirectory, const std::string& ack) {
    std::ostringstream c;
    c << "web=1\nweb_port=" << kPort << "\nweb_name=" << g.name << "\nweb_public_url=" << g.url << "\n";
    if (withDirectory) {
        c << "web_directory=1\nweb_directory_url=https://web.on-air.dev\nweb_directory_loc=" << g.loc << "\nweb_directory_loc_decimals=2\n"
          << "web_directory_radio=" << g.radio << "\nweb_directory_ack=" << ack << "\n";
    }
    std::ofstream f(dir() / "server.conf", std::ios::trunc);
    f << c.str();
}

bool startServer(std::string& err) {
    if (g.serverPid) { plat::stopChild(g.serverPid); g.serverPid = 0; std::this_thread::sleep_for(std::chrono::milliseconds(1500)); }
    const long pid = plat::spawnChild(serverBinary(), {"--config", (dir() / "server.conf").string(), "--web", "--web-port", std::to_string(kPort)},
                                      (dir() / "server.log").string(), err);
    g.serverPid = pid;
    return pid != 0;
}

bool waitFor(const std::function<bool()>& ok, int seconds) {
    for (int i = 0; i < seconds * 10; i++) {
        if (g.cancel) return false;
        if (ok()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void shareWorker() {
    std::string err;
    g.cancel = false;
    g.url.clear();
    g.phase = Phase::Starting;
    g.helperPath = findHelper();
    {   // server first, with the sharing settings and no directory
        std::ofstream(dir() / "server.conf", std::ios::trunc) << "web=1\nweb_port=" << kPort << "\nweb_name=" << g.name << "\n";
        if (!startServer(err)) { fail(err + " (onair-server is part of the OnAir package; it was not found beside the program)"); return; }
    }
    const long tp = plat::spawnChild(g.helperPath, {"tunnel", "--no-autoupdate", "--url", "http://127.0.0.1:" + std::to_string(kPort)}, (dir() / "tunnel.log").string(), err);
    if (!tp) { fail(err); return; }
    g.tunnelPid = tp;
    std::string url;
    if (!waitFor([&] { url = parseTunnelUrl(slurp(dir() / "tunnel.log")); return !url.empty() || !plat::childAlive(tp); }, 60) || url.empty()) {
        if (!g.cancel) fail("The tunnel did not come up. Check the internet connection. The log is in " + (dir() / "tunnel.log").string());
        return;
    }
    {
        std::lock_guard<std::mutex> l(g.m);
        g.url = url;
    }
    // restart the server so it knows its public address
    writeConf(false, "");
    if (!startServer(err)) { fail(err); return; }
    g.phase = Phase::Sharing;
}

void previewWorker() {
    std::string err;
    writeConf(true, "");
    if (!startServer(err)) { fail(err); return; }
    std::string log;
    if (!waitFor([&] { log = slurp(dir() / "server.log"); return !parseAckCode(log).empty(); }, 30)) { if (!g.cancel) fail("The server did not print the listing preview."); return; }
    {
        std::lock_guard<std::mutex> l(g.m);
        g.preview = parsePreview(log);
        g.ackCode = parseAckCode(log);
    }
    g.phase = Phase::Preview;
}

void confirmWorker() {
    std::string err;
    std::string code;
    { std::lock_guard<std::mutex> l(g.m); code = g.ackCode; }
    writeConf(true, code);
    if (!startServer(err)) { fail(err); return; }
    g.phase = Phase::Listed;
}

void downloadWorker() {
    g.phase = Phase::Downloading;
#ifdef _WIN32
    const char* file = "cloudflared-windows-amd64.exe";
    const char* out = "cloudflared.exe";
#elif defined(__APPLE__)
    const char* file = "cloudflared-darwin-arm64.tgz";
    const char* out = "cloudflared.tgz";
#else
    const char* file = "cloudflared-linux-amd64";
    const char* out = "cloudflared";
#endif
    const fs::path dst = dir() / out;
    const int rc = dect2::curlDownloadFile(std::string("https://github.com/cloudflare/cloudflared/releases/latest/download/") + file, dst.string(), 80ull << 20, 300, g.cancel, [] {});
    if (rc != 0) { fail(std::string("Could not download the helper: ") + dect2::curlDownloadError(rc)); return; }
#ifdef __APPLE__
    std::string err;
    const long tp = plat::spawnChild("/usr/bin/tar", {"-xzf", dst.string(), "-C", dir().string()}, (dir() / "untar.log").string(), err);
    if (!tp || !waitFor([&] { return !plat::childAlive(tp); }, 30)) { fail("Could not unpack the helper."); return; }
    std::error_code ec;
    fs::remove(dst, ec);
#endif
#ifndef _WIN32
    std::error_code ec2;
    fs::permissions(dir() / "cloudflared", fs::perms::owner_all, ec2);
#endif
    g.helperPath = findHelper();
    if (g.helperPath.empty()) { fail("The helper did not unpack."); return; }
    shareWorker();
}

void launch(void (*fn)()) {
    if (g.worker.joinable()) g.worker.join();
    g.worker = dect2::namedThread("onair-webshare", fn);
}

} // namespace

void stopAll() {
    g.cancel = true;
    if (g.worker.joinable()) g.worker.join();
    if (g.tunnelPid) plat::stopChild(g.tunnelPid);
    if (g.serverPid) plat::stopChild(g.serverPid);
    g.tunnelPid = g.serverPid = 0;
    g.phase = Phase::Idle;
}

void drawItems(App& a, bool menu) {
    (void)menu;
    const Phase ph = g.phase;
    ImGui::TextDisabled("%s", tr("OnAir Web"));
    if (ph == Phase::Idle || ph == Phase::Failed || ph == Phase::NeedHelper) {
        if (ph == Phase::Failed) {
            std::lock_guard<std::mutex> l(g.m);
            ImGui::TextColored(ImVec4(1, .5f, .4f, 1), "%s", g.error.c_str());
        }
        if (ImGui::Button(tr("1. Share my radio on the web"))) {
            if (a.engine.running()) a.engine.stop();   // the web server needs the radio for itself
            if (findHelper().empty()) g.phase = Phase::NeedHelper;
            else launch(shareWorker);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Starts a web page for your radio and gives it a public address. The radio is used by the web server while it runs."));
        return;
    }
    if (ph == Phase::Downloading || ph == Phase::Starting) {
        ImGui::TextDisabled("%s", ph == Phase::Downloading ? tr("Getting the helper ...") : tr("Starting ..."));
        if (ImGui::SmallButton(tr("Cancel"))) stopAll();
        return;
    }
    std::string url;
    { std::lock_guard<std::mutex> l(g.m); url = g.url; }
    ImGui::Text("%s", url.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Copy"))) ImGui::SetClipboardText(url.c_str());
    if (ph == Phase::Sharing) {
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(tr("Name"), g.name, sizeof g.name);
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(tr("Radio (for example HackRF Pro)"), g.radio, sizeof g.radio);
        ImGui::SetNextItemWidth(220 * gUi);
        ImGui::InputText(tr("Location (lat,lon, optional)"), g.loc, sizeof g.loc);
        const bool ok = cleanLine(g.name, 48) && cleanLine(g.radio, 48) && cleanLine(g.loc, 40) && g.name[0];
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button(tr("2. List it on the public directory"))) launch(previewWorker);
        if (!ok) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("You will see exactly what is sent before anything is listed. The location is rounded to about a kilometre."));
    } else if (ph == Phase::Listed) {
        ImGui::TextColored(ImVec4(.4f, 1, .5f, 1), "%s", tr("Listed on web.on-air.dev"));
    } else if (ph == Phase::Preview) {
        ImGui::TextDisabled("%s", tr("Check the listing ..."));
    }
    if (ImGui::SmallButton(tr("Stop sharing"))) stopAll();
}

void drawPopups(App& a) {
    (void)a;
    if (g.phase == Phase::NeedHelper) { ImGui::OpenPopup("##wsHelper"); }
    if (ImGui::BeginPopupModal("##wsHelper", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr("To give your radio a public web address OnAir needs a small free helper program, cloudflared by Cloudflare (no account needed)."));
        ImGui::TextWrapped("%s", tr("It will be downloaded once from github.com/cloudflare/cloudflared into OnAir's folder. While you share, traffic to your page goes through Cloudflare."));
        if (ImGui::Button(tr("Download and continue"))) { ImGui::CloseCurrentPopup(); launch(downloadWorker); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) { ImGui::CloseCurrentPopup(); g.phase = Phase::Idle; }
        ImGui::EndPopup();
    }
    if (g.phase == Phase::Preview) ImGui::OpenPopup("##wsPreview");
    if (ImGui::BeginPopupModal("##wsPreview", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", tr("This is exactly what will be sent to the public directory every 5 minutes. Never sent: listener addresses or what anyone listens to."));
        {
            std::lock_guard<std::mutex> l(g.m);
            ImGui::BeginChild("##wsPrev", ImVec2(520 * gUi, 220 * gUi), true);
            ImGui::TextUnformatted(g.preview.c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button(tr("List it"))) { ImGui::CloseCurrentPopup(); launch(confirmWorker); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Not now"))) {
            ImGui::CloseCurrentPopup();
            g.phase = Phase::Sharing;
            launch([] { std::string e; writeConf(false, ""); startServer(e); });
        }
        ImGui::EndPopup();
    }
}

} // namespace webshare
