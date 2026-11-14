// OnAir's side of the Digital video generator, which is its own program since #458 (onair-videogen, app/videogen/):
//  - the link server (vglink.h) runs from the start of OnAir: the generators send their multiplex to it over RTP and the transmitter
//    reads it as "generator:N", as before;
//  - Tools > Digital video generator starts onair-videogen from OnAir's own folder with --onair 127.0.0.1:<port> --token <t>, or
//    asks the running one to come to the front;
//  - a.videoGenOpen (saved) means "the generator runs": set when it is started, cleared when the user closes the generator window (its
//    RTCP BYE "closed") or it ends otherwise, kept when OnAir closes, so the next start of OnAir starts it again. OnAir closing asks the
//    generator it started to quit (and ends it after 3 s); a generator started by hand keeps running and links again later.
#include "../app.h"
#include "../platform/platform.h"
#include "dect2/tv/vglink.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>

using namespace dect2::vgen;

namespace {

long gPid = 0;                   // the generator OnAir started (0: none)
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
    const int port = (int)plat::prefs().getI("videoGenLinkPort", kVgLinkDefaultPort);
    const bool any = plat::prefs().getB("videoGenLinkAll", false);   // generators on other computers (off: this one only)
    vgLinkServer().onBye = [](const std::string& tok) { if (!tok.empty() && tok == gToken) gBye = true; };
    if (!vgLinkServer().start(port, any, gServerError)) fprintf(stderr, "video generator link: %s\n", gServerError.c_str());
}

bool ours() { return gPid > 0 && plat::childAlive(gPid); }

bool spawn(App& a) {
    gToken = newToken();
    std::string err;
    const std::vector<std::string> args = {"--onair", "127.0.0.1:" + std::to_string(vgLinkServer().port()), "--token", gToken};
    gPid = plat::spawnChild(generatorExe(), args, plat::cacheDir() + "/onair-videogen.log", err);
    if (gPid > 0) return true;
    a.videoGenOpen = false;
    gServerError = "could not start " + generatorExe() + ": " + err;
    fprintf(stderr, "%s\n", gServerError.c_str());
    return false;
}

} // namespace

const std::string& videoGenError() { return gServerError; }

// Tools > Digital video generator
void videoGenRequest(App& a) {
    startServer();
    if (ours() || vgLinkServer().raise(gToken)) {   // running: to the front (it may still be starting: then it just shows up)
        if (ours()) vgLinkServer().raise(gToken);
        a.videoGenOpen = true;
        return;
    }
    if (spawn(a)) a.videoGenOpen = true;
    savePrefs(a);
}

// every frame
void videoGenTick(App& a) {
    startServer();
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
        vgLinkServer().quit(gToken);
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
