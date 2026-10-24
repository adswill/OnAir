// onair-server: OnAir without a window. The radio is attached to this computer (a Raspberry Pi, a Linux box, a Windows PC with no screen); it is
// controlled from a web browser (the page this program serves) or by any program that speaks the remote-control API (docs/headless-server.md).
// No cloud and no relay: the browser connects straight to this program. Access from other computers needs a token; TLS is optional.
#include "dect2/common/platform.h"
#include "dect2/engine/crash_report.h"
#include "dect2/net/remote_api.h"
#include "dect2/net/share.h"
#include "server_config.h"
#include "server_host.h"
#include "web/web_config.h"
#include "web/web_decoders.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

using namespace onair_server;

static std::atomic<bool> gQuit{false};
static void onSignal(int) { gQuit = true; }

// OnAir Web's "host" decoder: the server's own receiver (the mode chosen in the page) as events for web listeners who subscribe to it. Its
// policy kind is the running mode, so a voice or pager mode stays private unless the host enabled that kind (web_policy.h).
class HostDecoder : public onair_web::WebDecoder {
public:
    explicit HostDecoder(ServerHost& h) : h_(h) {}
    void poll(std::vector<std::string>& ev) override {
        uint64_t seq = 0;
        std::string j = h_.telemetryJson(seq);
        if (seq == seq_ || j.empty()) return;
        seq_ = seq;
        ev.push_back("{\"mode\":\"" + onair_web::jsonEsc(h_.modeId()) + "\",\"telemetry\":" + j + "}");
    }
    std::string policyKind() const override { return h_.modeId(); }
private:
    ServerHost& h_;
    uint64_t seq_ = 0;
};

static void usage() {
    printf("onair-server %s: OnAir with no window, controlled from a web browser or the remote-control API\n\n"
           "  onair-server [--config FILE] [options]\n\n"
           "  --config FILE      settings file (default %s)\n"
           "  --port N           web / API port (default 8765)\n"
           "  --lan              accept other computers (needs a token; made and printed when there is none)\n"
           "  --token TEXT       use this access token (16 characters or more) for this run\n"
           "  --new-token        make a new token, save it in the settings file and print it\n"
           "  --tls-cert FILE --tls-key FILE   serve HTTPS (PEM files; needs a build with OpenSSL, DECT2_WITH_TLS)\n"
           "  --device NAME|N    radio to use (see the device list in the page); 'test' is the built-in test signal\n"
           "  --mode ID          receiver mode, e.g. fm, dab, t2, adsb, dvbs (see /api/v1/modes)\n"
           "  --freq MHZ         tuning frequency\n"
           "  --autostart        start the receiver at once and keep it running (comes back when the radio is replugged)\n"
           "  --share-rtltcp [PORT]   also share the raw samples as an rtl_tcp server (default port 1234) for SDR++, SDR# or another OnAir; no access control: trusted networks only\n"
           "  --web              OnAir Web: many listeners, each with an own receiver in this radio's capture (docs/ONAIR-WEB.md)\n"
           "  --web-port N       OnAir Web port (default 8073)\n"
           "  --web-lan          OnAir Web on every interface (without TLS only private addresses are let in)\n"
           "  --directory-remove remove this server from the public OnAir Web directory now (signed with its key), then exit\n"
           "  --version, --help\n\n"
           "Settings file: plain key=value lines (see docs/headless-server.md).\n",
           ONAIR_SERVER_VERSION, defaultConfigPath().c_str());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 4096);   // size 0 is invalid with _IOLBF on the Windows CRT (#291); a service log (journald, a file) shows lines as they happen
    dect2::crash::install("crash-server");
    if (!dect2::cpuSupportsBuild()) { fprintf(stderr, "this build needs a processor with AVX2 and FMA\n"); return 1; }
    std::string path = defaultConfigPath();
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--version") { printf("onair-server %s\n", ONAIR_SERVER_VERSION); return 0; }
        if (a == "--config" && i + 1 < argc) path = argv[++i];
    }
    ConfigFile cfg;
    const bool haveFile = cfg.load(path);

    dect2::RemoteApiConfig api;
    api.on = true;
    api.port = (int)cfg.num("port", 8765);
    api.lan = cfg.flag("lan", false);
    api.token = cfg.get("token");
    api.tlsCert = cfg.get("tls_cert");
    api.tlsKey = cfg.get("tls_key");
    bool dirRemove = false;
    bool newToken = false, share = cfg.flag("share_rtltcp", false);
    int sharePort = (int)cfg.num("share_port", 1234);
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--config") next();
        else if (a == "--port") api.port = atoi(next().c_str());
        else if (a == "--lan") api.lan = true;
        else if (a == "--token") api.token = next();
        else if (a == "--new-token") newToken = true;
        else if (a == "--tls-cert") api.tlsCert = next();
        else if (a == "--tls-key") api.tlsKey = next();
        else if (a == "--device") cfg.set("device", next());
        else if (a == "--mode") cfg.set("mode", next());
        else if (a == "--freq") cfg.set("freq_mhz", next());
        else if (a == "--autostart") cfg.set("autostart", "1");
        else if (a == "--web") cfg.set("web", "1");
        else if (a == "--web-port") cfg.set("web_port", next());
        else if (a == "--web-lan") cfg.set("web_lan", "1");
        else if (a == "--directory-remove") dirRemove = true;
        else if (a == "--share-rtltcp") { share = true; if (i + 1 < argc && argv[i + 1][0] != '-') sharePort = atoi(argv[++i]); }
        else { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if (api.port < 1 || api.port > 65535) { fprintf(stderr, "the port must be 1 to 65535\n"); return 2; }
    if (api.tlsCert.empty() != api.tlsKey.empty()) { fprintf(stderr, "TLS needs both tls_cert and tls_key\n"); return 2; }

    bool saveCfg = !haveFile;   // a first run leaves a file to edit
    if (newToken || (api.lan && api.token.size() < 16)) {
        api.token = dect2::remoteApiNewToken();
        cfg.set("token", api.token);
        saveCfg = true;
        fprintf(stderr, "onair-server: new access token %s (saved in %s)\n", api.token.c_str(), path.c_str());
    }
    if (api.token.size() > 0 && api.token.size() < 16) { fprintf(stderr, "onair-server: the token must have 16 characters or more\n"); return 2; }
    bool madeAdmin = false;
    onair_web::WebSettings webCfg = onair_web::loadWebSettings(cfg, madeAdmin);
    if (madeAdmin) { saveCfg = true; fprintf(stderr, "onair-server: OnAir Web admin token made (web_admin_token in %s)\n", path.c_str()); }
    if (webCfg.directory.keyFile.empty()) webCfg.directory.keyFile = (std::filesystem::path(path).parent_path() / "web_directory.key").string();
    webCfg.directory.version = ONAIR_SERVER_VERSION;
    if (dirRemove) {
        std::unique_ptr<onair_web::DirectoryHeartbeat> d(onair_web::makeDirectoryHeartbeat());
        std::string err;
        if (!d->removeNow(webCfg.directory, err)) { fprintf(stderr, "onair-server: directory removal failed: %s\n", err.c_str()); return 1; }
        printf("onair-server: removed from the directory %s. It stays removed until web_directory is on and a heartbeat is sent again.\n", webCfg.directory.url.c_str());
        return 0;
    }
    if (saveCfg) {
        std::string err;
        if (!cfg.save(path, err)) fprintf(stderr, "onair-server: %s (the settings will not be saved)\n", err.c_str());
    }

    ServerHost host(cfg);
    host.init();
    dect2::RemoteApiServer server(host);
    if (!server.configure(api)) {
        fprintf(stderr, "onair-server: %s\n", server.status().lastError.c_str());
        return 1;
    }
    const dect2::RemoteApiConfig live = server.config();
    const char* scheme = server.status().tls ? "https" : "http";
    if (api.lan) {
        printf("onair-server: listening on all interfaces, port %d. Open %s://<this computer>:%d/?token=<token from %s>\n", live.port, scheme, live.port, path.c_str());
        if (!server.status().tls) fprintf(stderr, "onair-server: no TLS: the token and the page travel unencrypted. Use it on a network you trust, or set tls_cert / tls_key (or a reverse proxy).\n");
    } else {
        printf("onair-server: listening on this computer only: %s://127.0.0.1:%d/ (use --lan to let other computers in)\n", scheme, live.port);
    }
    if (share) {
        dect2::ShareConfig sc;
        sc.on = true; sc.port = sharePort; sc.lan = api.lan; sc.allowControl = false;
        dect2::ShareServer::get().configure(sc);
        const dect2::ShareStatus ss = dect2::ShareServer::get().status();
        if (!ss.listening) fprintf(stderr, "onair-server: rtl_tcp share: %s\n", ss.lastError.c_str());
        else printf("onair-server: sharing the samples as rtl_tcp on port %d (%s; no password)\n", sharePort, api.lan ? "whole network" : "this computer only");
    }
    std::unique_ptr<onair_web::WebHub> hub;
    std::unique_ptr<onair_web::WebServer> web;
    std::unique_ptr<onair_web::DirectoryHeartbeat> directory;   // OnAir Web directory (off unless web_directory=1 and confirmed)
    if (webCfg.on && webCfg.server.webRoot.empty()) {
        // the browser UI's files: beside the program (web/onair-web), or where a package puts them (../share/onair/web/onair-web)
        std::error_code ec;
        const std::filesystem::path exe = std::filesystem::absolute(argv[0], ec).parent_path();
        for (const auto& p : {exe / "web" / "onair-web", exe / ".." / "share" / "onair" / "web" / "onair-web", exe / ".." / "web" / "onair-web"})
            if (std::filesystem::is_regular_file(p / "index.html", ec)) { webCfg.server.webRoot = p.string(); break; }
    }
    if (webCfg.on) {
        hub.reset(new onair_web::WebHub);
        hub->setLimits(webCfg.limits);
        hub->policy().load(cfg);
        { std::vector<std::string> rw; hub->ranges().load(cfg, &rw); webCfg.warnings.insert(webCfg.warnings.end(), rw.begin(), rw.end()); }
        hub->setLogIp(webCfg.logIp);
        hub->setServerInfo(webCfg.name, webCfg.location, webCfg.antenna);
        hub->setLog([](const std::string& l) { printf("onair-server: %s\n", l.c_str()); });
        hub->registerDecoder(onair_web::DecoderFactory{"host", "This server's receiver (its mode's results)", 0, false,
                                                       [&host](double) { return std::unique_ptr<onair_web::WebDecoder>(new HostDecoder(host)); }});
        onair_web::registerWebDecoders(*hub);   // ADS-B, AIS, APRS, FT8 / FT4 / WSPR, SSTV, fax, Meteor, RDS, CW, RTTY (server/web/web_decoders.h)
        hub->start();
        host.setWebHub(hub.get());
        web.reset(new onair_web::WebServer(*hub));
        web->setOnChange([&host, &hub](const onair_web::WebPolicy& p, const std::vector<onair_web::InviteToken>& inv) {
            host.editConfig([&](ConfigFile& c) { onair_web::saveWebState(c, p, inv); hub->ranges().save(c); });   // called with the policy mutex held: the ranges are safe to read
        });
        for (const auto& w : webCfg.warnings) fprintf(stderr, "onair-server: %s\n", w.c_str());
        if (!web->start(webCfg.server)) {
            fprintf(stderr, "onair-server: OnAir Web: %s\n", web->status().lastError.c_str());
            host.setWebHub(nullptr);
            web.reset();
            hub->stop();
            hub.reset();
        } else {
            const auto st = web->status();
            printf("onair-server: OnAir Web on %s://%s:%d/ (%s access, at most %d listeners)\n", st.tls ? "https" : "http", webCfg.server.bind.c_str(), st.port,
                   onair_web::authModeName(webCfg.server.auth.mode), webCfg.limits.maxUsers);
            if (webCfg.directory.on) {
                directory.reset(onair_web::makeDirectoryHeartbeat());
                std::string derr;
                onair_web::WebHub* h = hub.get();
                if (!directory->start(webCfg.directory, [h] { return h->infoJson(); }, derr)) {
                    fprintf(stderr, "onair-server: %s\n", derr.c_str());
                    directory.reset();
                }
            }
        }
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    dect2::crash::startupDone();
    while (!gQuit) {
        host.supervise(path);
        if (hub) hub->tick();   // OnAir Web: time limits, idle listeners, CPU and bandwidth budgets
        for (int i = 0; i < 10 && !gQuit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("onair-server: stopping\n");
    if (directory) directory->stop();
    if (web) web->stop();
    host.setWebHub(nullptr);
    if (hub) hub->stop();
    host.supervise(path);
    host.shutdown();   // the receiver stops; the saved "autostart" keeps what the user last chose, so a restart brings it back
    dect2::ShareServer::get().configure(dect2::ShareConfig{});
    return 0;
}
