// OnAir's side of the Digital video generator, which is its own program since #458 (onair-videogen, app/videogen/):
//  - the link server (vglink.h) runs from the start of OnAir: the generators send their multiplex to it over RTP and the transmitter
//    reads it as "generator:N", as before. It always listens on the local pipe (no ports); with the Transmit panel's "Listen on the
//    network" box (videoGenLinkNet) also on UDP videoGenLinkPort / + 1, on 127.0.0.1 or (videoGenLinkAll) every network card;
//  - Tools > Digital video generator starts onair-videogen from OnAir's own folder with --onair local --token <t> (the UDP port only
//    when the pipe could not be made), or asks the running one to come to the front;
//  - a.videoGenOpen (saved) means "the generator runs": set when it is started, cleared when the user closes the generator window (its
//    RTCP BYE "closed") or it ends otherwise, kept when OnAir closes, so the next start of OnAir starts it again. OnAir closing asks the
//    generator it started to quit (and ends it after 3 s); a generator started by hand keeps running and links again later.
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/tv/vglink.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <random>
#include <string>
#include <thread>

using namespace dect2::vgen;

namespace {

long gPid = 0;                   // the generator OnAir started (0: none)
std::mutex gTokenMu;             // gToken is read on the link's thread (onBye)
std::string gToken;              // the token it was started with
std::atomic<bool> gBye{false};   // its window was closed by the user
bool gServerTried = false;
std::string gServerError;

std::string generatorExe() {
    std::string dir = plat::exePath();
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
#ifdef _WIN32
    return dir + "\\onair_videogen.exe";
#else
    return dir + "/onair-videogen";
#endif
}

std::string newToken() {
    std::random_device rd;
    std::array<char, 24> b{};
    snprintf(b.data(), b.size(), "%08x%08x", rd(), rd());
    return b.data();
}

void startServer() {
    if (gServerTried) return;
    gServerTried = true;
    const bool net = plat::prefs().getB("videoGenLinkNet", false);   // UDP too (off: the local pipe only)
    const int port = (int)plat::prefs().getI("videoGenLinkPort", kVgLinkDefaultPort);
    const bool any = plat::prefs().getB("videoGenLinkAll", true);    // with UDP: every network card (off: 127.0.0.1)
    vgLinkServer().onBye = [](const std::string& tok) { std::lock_guard<std::mutex> lk(gTokenMu); if (!tok.empty() && tok == gToken) gBye = true; };
    gServerError.clear();
    if (!vgLinkServer().start(net, port, any, gServerError)) fprintf(stderr, "video generator link: %s\n", gServerError.c_str());
}

bool ours() { return gPid > 0 && plat::childAlive(gPid); }

std::string token() { std::lock_guard<std::mutex> lk(gTokenMu); return gToken; }

bool spawn(App& a) {
    { std::lock_guard<std::mutex> lk(gTokenMu); gToken = newToken(); }
    vgLinkServer().setOwnToken(token());   // only this generator follows the transmitter's rate (#458)
    std::string err;
    const std::string at = vgLinkServer().listeningLocal() || vgLinkServer().port() == 0 ? std::string("local") : "127.0.0.1:" + std::to_string(vgLinkServer().port());
    std::vector<std::string> args = {"--onair", at, "--token", token()};
    if (const char* shot = getenv("DECT2_VGSHOT")) {   // dev: the generator OnAir starts saves its window too (#458 screenshots)
        const char* delay = getenv("DECT2_VGSHOTDELAY");
        args.insert(args.end(), {"--hidden", "--no-first-run", "--shot", shot, "--shotdelay", delay ? delay : "12"});
    }
    gPid = plat::spawnChild(generatorExe(), args, plat::cacheDir() + "/onair-videogen.log", err);
    if (gPid > 0) return true;
    a.videoGenOpen = false;
    gServerError = "could not start " + generatorExe() + ": " + err;
    fprintf(stderr, "%s\n", gServerError.c_str());
    return false;
}

} // namespace

const std::string& videoGenError() { return gServerError; }

bool videoGenNetwork() { return plat::prefs().getB("videoGenLinkNet", false); }

void videoGenSetNetwork(bool on) {
    plat::prefs().setB("videoGenLinkNet", on);
    plat::prefs().flush();
    gServerTried = false;
    startServer();
}

// Tools > Digital video generator
void videoGenRequest(App& a) {
    startServer();
    if (ours() || vgLinkServer().raise(token())) {   // running: to the front (it may still be starting: then it just shows up)
        if (ours()) vgLinkServer().raise(token());
        a.videoGenOpen = true;
        return;
    }
    if (spawn(a)) a.videoGenOpen = true;
    savePrefs(a);
}

// a button without the App at hand (the Transmit panel's "Open the generator"): the next frame opens it
static std::atomic<bool> gOpenLater{false};
void videoGenRequestLater() { gOpenLater = true; }

// every frame
void videoGenTick(App& a) {
    startServer();
    if (gOpenLater.exchange(false)) videoGenRequest(a);
    if (gBye.exchange(false)) { a.videoGenOpen = false; savePrefs(a); return; }   // the user closed the generator
    if (gPid > 0 && !plat::childAlive(gPid)) {   // it ended without a BYE (crashed, or killed): not started again by itself
        gPid = 0;
        if (a.videoGenOpen) { a.videoGenOpen = false; savePrefs(a); }
        return;
    }
    if (a.videoGenOpen && gPid == 0 && ImGui::GetFrameCount() > 30) {   // saved as running: started again with OnAir
        if (!spawn(a)) savePrefs(a);
    }
}

// OnAir closes: the generator it started quits (its settings are saved by itself); a.videoGenOpen keeps its value
void videoGenShutdown() {
    if (ours()) {
        vgLinkServer().quit(token());
        for (int i = 0; i < 30 && plat::childAlive(gPid); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (plat::childAlive(gPid)) {
            plat::stopChild(gPid);
            for (int i = 0; i < 10 && plat::childAlive(gPid); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (plat::childAlive(gPid)) plat::killChild(gPid);
        }
    }
    gPid = 0;
    vgLinkServer().stop();
}
