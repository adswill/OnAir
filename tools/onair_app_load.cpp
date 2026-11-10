// onair-app-load: the cost of OnAir Web 3.9 viewers (docs/WEB-3.9.md, "CPU"). Runs the real thing in one process without a radio or a
// network beyond 127.0.0.1: ServerHost on the built-in test signal, the web server with the app, and N fake viewers that each open the
// WebSocket, subscribe to the panel, the status, the spectrum (1024 bins, 10 rows a second) and audio (16 kHz PCM), and read everything.
// Prints, for each viewer count, the process CPU (% of one core; the fake viewers' reading is inside it) and the bytes each viewer gets.
//   onair-app-load [--viewers 0,1,5,20] [--seconds 10] [--mode fm] [--bins 1024] [--fps 10] [--no-audio]
#include "../server/app/webapp.h"
#include "../server/server_config.h"
#include "../server/server_host.h"
#include "../server/web/web_config.h"
#include "../server/web/web_hub.h"
#include "../server/web/web_server.h"
#include "../core/src/net/tcp_client.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace dect2;

static double cpuSeconds() {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6;
}
static std::string cframe(const std::string& pl) {
    std::string f;
    f.push_back((char)0x81);
    if (pl.size() < 126) f.push_back((char)(0x80 | pl.size()));
    else { f.push_back((char)(0x80 | 126)); f.push_back((char)(pl.size() >> 8)); f.push_back((char)(pl.size() & 0xFF)); }
    const char key[4] = {1, 2, 3, 4};
    f.append(key, 4);
    for (size_t i = 0; i < pl.size(); i++) f.push_back((char)(pl[i] ^ key[i & 3]));
    return f;
}

struct Viewer {
    std::thread th;
    std::atomic<long long> bytes{0}, frames{0};
    std::atomic<bool> stop{false};
};

static void viewerLoop(Viewer& v, int port, const std::string& sub) {
    std::string e;
    sock_t s = tcpConnect("127.0.0.1", port, 2000, e);
    if (s == kBadSock) return;
    const std::string rq = "GET /app/v1/ws?csrf=lan HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    sockSendAll(s, rq.data(), rq.size());
    const std::string f = cframe(sub);
    bool sent = false;
    std::string buf;
    while (!v.stop) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[16384];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        if (!sent) { sockSendAll(s, f.data(), f.size()); sent = true; }
        v.bytes += k;
        // count frames roughly: server frames start with 0x81 / 0x82 after the handshake; a ping (0x89) is answered
        for (long long i = 0; i + 1 < k; i++) if ((unsigned char)b[i] == 0x89 && b[i + 1] == 1) { const std::string p = cframe("k"); sockSendAll(s, p.data(), p.size()); }
        v.frames++;
    }
    sockClose(s);
}

int main(int argc, char** argv) {
    std::string viewers = "0,1,5,20", mode = "fm";
    double seconds = 10;
    int bins = 1024, fps = 10;
    bool audio = true;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--viewers") viewers = next();
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--mode") mode = next();
        else if (a == "--bins") bins = atoi(next().c_str());
        else if (a == "--fps") fps = atoi(next().c_str());
        else if (a == "--no-audio") audio = false;
    }
    const auto dir = std::filesystem::temp_directory_path() / "onair-app-load";
    std::filesystem::create_directories(dir);
    const std::string cfgPath = (dir / "server.conf").string();
    onair_server::ConfigFile cfg;
    cfg.set("device", "test"); cfg.set("mode", mode); cfg.set("web", "1"); cfg.set("web_port", "0");
    cfg.set("web_admin_token", "0123456789abcdef0123456789abcdef"); cfg.set("web_max_users", "64");
    std::string err;
    cfg.save(cfgPath, err);
    onair_server::ServerHost host(cfg);
    host.init();
    bool made = false;
    onair_web::WebSettings webCfg = onair_web::loadWebSettings(cfg, made);
    webCfg.server.maxConnections = 128; webCfg.server.maxConnPerIp = 128;
    onair_web::WebHub hub;
    hub.setLimits(webCfg.limits); hub.policy().load(cfg); hub.start();
    host.setWebHub(&hub);
    onair_app::AppOptions ao;
    ao.name = "load"; ao.version = "load";
    onair_app::WebApp app(host, cfg, ao);
    webCfg.server.app = &app;
    onair_web::WebServer web(hub);
    if (!web.start(webCfg.server)) { fprintf(stderr, "web: %s\n", web.status().lastError.c_str()); return 1; }
    const int port = web.status().port;
    std::atomic<bool> quit{false};
    std::thread sup([&] { while (!quit) { host.supervise(cfgPath); std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
    if (!host.start(-1, err)) { fprintf(stderr, "start: %s\n", err.c_str()); return 1; }
    for (int i = 0; i < 100 && !host.engine().running(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const std::string sub = "{\"type\":\"sub\",\"panel\":true,\"status\":true,\"spectrum\":true,\"bins\":" + std::to_string(bins) + ",\"fps\":" + std::to_string(fps) + ",\"audio\":" + (audio ? "true" : "false") + ",\"rate\":16000}";
    printf("mode %s, %d bins at %d rows/s, audio %s, %.0f s per step (process CPU, %% of one core, the fake viewers included)\n", mode.c_str(), bins, fps, audio ? "16 kHz PCM" : "off", seconds);
    printf("%8s %10s %14s %12s\n", "viewers", "CPU %", "kbit/s each", "frames/s");
    std::stringstream ss(viewers);
    std::string item;
    while (std::getline(ss, item, ',')) {
        const int n = atoi(item.c_str());
        std::vector<std::unique_ptr<Viewer>> vs;
        for (int i = 0; i < n; i++) { vs.emplace_back(new Viewer); Viewer& v = *vs.back(); v.th = std::thread([&v, port, sub] { viewerLoop(v, port, sub); }); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        for (auto& v : vs) { v->bytes = 0; v->frames = 0; }
        const double c0 = cpuSeconds();
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds((long long)(seconds * 1000)));
        const double c1 = cpuSeconds();
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        long long bytes = 0, frames = 0;
        for (auto& v : vs) { bytes += v->bytes; frames += v->frames; }
        printf("%8d %10.1f %14.0f %12.1f\n", n, (c1 - c0) / el * 100, n ? bytes * 8.0 / 1000 / el / n : 0.0, n ? (double)frames / el / n : 0.0);
        fflush(stdout);
        for (auto& v : vs) v->stop = true;
        for (auto& v : vs) if (v->th.joinable()) v->th.join();
    }
    quit = true;
    sup.join();
    web.stop();
    host.setWebHub(nullptr);
    hub.stop();
    host.shutdown();
    return 0;
}
