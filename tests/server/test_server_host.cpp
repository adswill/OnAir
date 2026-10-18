// The headless server: the settings file, and the real engine on the built-in test signal behind the remote-control API with the server's host
// (page, modes, start / stop, tuning, the saved state, the binary spectrum, audio for the browser). No radio is needed.
#include "../../core/src/net/tcp_client.h"
#include "../../server/server_config.h"
#include "../../server/server_host.h"
#include "dect2/net/remote_api.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
using namespace dect2;
using namespace onair_server;

#include "../common/test_util.h"

// ---------------------------------------------------------------- client helpers
static std::string readAll(sock_t s, int ms) {
    std::string r;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < ms) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[4096];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        r.append(b, (size_t)k);
    }
    return r;
}
struct Resp { int status = 0; std::string head, body; };
static Resp request(int port, const std::string& method, const std::string& path, const std::string& body = "") {
    std::string e;
    sock_t s = tcpConnect("127.0.0.1", port, 2000, e);
    Resp o;
    if (s == kBadSock) return o;
    std::string rq = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
    if (method == "POST") rq += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    rq += "\r\n" + body;
    sockSendAll(s, rq.data(), rq.size());
    const std::string r = readAll(s, 8000);
    sockClose(s);
    if (r.compare(0, 9, "HTTP/1.1 ") == 0) o.status = atoi(r.c_str() + 9);
    const size_t h = r.find("\r\n\r\n");
    o.head = r.substr(0, h);
    if (h != std::string::npos) o.body = r.substr(h + 4);
    return o;
}
static std::string cframe(const std::string& pl) {
    std::string f;
    f.push_back((char)0x81);
    if (pl.size() < 126) f.push_back((char)(0x80 | pl.size()));
    else { f.push_back((char)(0x80 | 126)); f.push_back((char)(pl.size() >> 8)); f.push_back((char)(pl.size() & 0xFF)); }
    const char key[4] = {0x12, 0x34, 0x56, 0x78};
    f.append(key, 4);
    for (size_t i = 0; i < pl.size(); i++) f.push_back((char)(pl[i] ^ key[i & 3]));
    return f;
}
static bool sframe(sock_t s, std::string& buf, int& op, std::string& pl, int ms = 4000) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (buf.size() >= 2) {
            size_t len = (uint8_t)buf[1] & 0x7F, hl = 2;
            const bool need = len == 126 && buf.size() < 4;
            if (len == 126 && buf.size() >= 4) { len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3]; hl = 4; }
            if (!need && buf.size() >= hl + len) { op = (uint8_t)buf[0] & 0x0F; pl = buf.substr(hl, len); buf.erase(0, hl + len); return true; }
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[8192];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) return false;
        buf.append(b, (size_t)k);
    }
}
static sock_t wsOpen(int port, std::string& buf) {
    std::string e;
    sock_t s = tcpConnect("127.0.0.1", port, 2000, e);
    if (s == kBadSock) return s;
    const std::string rq = "GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    sockSendAll(s, rq.data(), rq.size());
    buf.clear();
    size_t end;
    const auto t0 = std::chrono::steady_clock::now();
    while ((end = buf.find("\r\n\r\n")) == std::string::npos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
        fd_set rd; FD_ZERO(&rd); FD_SET(s, &rd);
        timeval tv{0, 50000};
        if (select((int)s + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char b[1024];
        const long long k = sockRecv(s, b, sizeof b);
        if (k <= 0) break;
        buf.append(b, (size_t)k);
    }
    if (end == std::string::npos) { sockClose(s); return kBadSock; }
    buf.erase(0, end + 4);
    return s;
}

// ---------------------------------------------------------------- the settings file
static void testConfig() {
    ConfigFile c;
    c.parse("# my server\nport = 9000\n\ntoken=abc\nnet_radio=rtltcp,1.2.3.4,1234\nnet_radio=pluto,5.6.7.8,0\nflag=yes\nbroken line\nnum=2.5\n");
    CHECK(c.num("port", 0) == 9000, "port");
    CHECK(c.get("token") == "abc" && c.get("missing", "d") == "d", "get");
    CHECK(c.getAll("net_radio").size() == 2 && c.getAll("net_radio")[1] == "pluto,5.6.7.8,0", "repeated keys");
    CHECK(c.flag("flag", false) && !c.flag("nothere", false) && c.flag("nothere", true), "flags");
    CHECK(c.num("num", 0) == 2.5 && c.num("token", 7) == 7, "numbers; text is not a number");
    c.set("port", "9100");
    c.set("new_key", "v");
    c.set("net_radio", "rtltcp,9.9.9.9,1");
    const std::string t = c.text();
    CHECK(t.find("# my server") == 0 && t.find("broken line") != std::string::npos, "comments and unknown lines survive");
    CHECK(t.find("port=9100") != std::string::npos && t.find("port = 9000") == std::string::npos, "a key is replaced in place");
    CHECK(c.getAll("net_radio").size() == 1, "set() collapses repeated keys");
    CHECK(t.rfind("new_key=v") != std::string::npos && t.size() - t.rfind("new_key=v") <= 10, "a new key is appended");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("onair_srv_cfg_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string p = (dir / "sub" / "server.conf").string();
    std::string err;
    CHECK(c.save(p, err), "save: %s", err.c_str());
    ConfigFile d;
    CHECK(d.load(p) && d.get("token") == "abc" && d.get("port") == "9100", "round trip");
    CHECK(!std::filesystem::exists(p + ".tmp"), "no temporary file is left");
#ifndef _WIN32
    CHECK((std::filesystem::status(p).permissions() & (std::filesystem::perms::group_read | std::filesystem::perms::others_read)) == std::filesystem::perms::none, "the file holding the token is private");
#endif
    CHECK(!ConfigFile().load((dir / "nothing").string()), "a missing file does not load");
    std::filesystem::remove_all(dir);
}

// ---------------------------------------------------------------- the host on the test signal
static void testHost() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("onair_srv_host_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string cfgPath = (dir / "server.conf").string();
    ConfigFile cfg;
    cfg.set("device", "test");
    cfg.set("mode", "fm");
    ServerHost host(cfg);
    host.init();
    RemoteApiServer srv(host);
    RemoteApiConfig rc; rc.on = true; rc.port = 0;
    CHECK(srv.configure(rc), "the API starts: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;

    Resp r = request(port, "GET", "/");
    CHECK(r.status == 200 && r.body.find("<title>OnAir server</title>") != std::string::npos, "the page is served: %d", r.status);
    CHECK(r.body.find("<script src") == std::string::npos && r.body.find("<link") == std::string::npos && r.body.find("http://") == std::string::npos && r.body.find("https://") == std::string::npos && r.body.find("@import") == std::string::npos, "the page loads nothing from outside");
    r = request(port, "GET", "/api/v1/status");
    CHECK(r.status == 200 && r.body.find("\"running\":false") != std::string::npos && r.body.find("\"modeId\":\"fm\"") != std::string::npos && r.body.find("Test signal") != std::string::npos, "status before start: %s", r.body.c_str());
    r = request(port, "GET", "/api/v1/modes");
    CHECK(r.body.find("\"id\":\"fm\"") != std::string::npos && r.body.find("\"id\":\"dvbs\"") != std::string::npos && r.body.find("\"id\":\"adsb\"") != std::string::npos && r.body.find("\"sub\":\"ft8\"") != std::string::npos, "the mode list has the standards, the other modes and the sub modes");
    r = request(port, "GET", "/api/v1/devices");
    CHECK(r.body.find("\"kind\":\"synthetic\"") != std::string::npos, "the test signal is a device");
    CHECK(host.findMode("fm") && host.findMode("7") && host.findMode("7") == host.findMode("fm") && !host.findMode("nonsense"), "mode lookup by id and number");

    r = request(port, "POST", "/api/v1/start", "{}");
    CHECK(r.status == 200, "start: %d %s", r.status, r.body.c_str());
    // spectrum and telemetry arrive from the snapshot thread
    bool spec = false, tel = false;
    for (int i = 0; i < 100 && !(spec && tel); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        spec = request(port, "GET", "/api/v1/spectrum?bins=128").body.find("\"running\":true") != std::string::npos;
        tel = request(port, "GET", "/api/v1/telemetry").body.find("\"running\":true") != std::string::npos;
    }
    CHECK(spec, "a spectrum line is published");
    CHECK(tel, "telemetry is published");
    r = request(port, "GET", "/api/v1/status");
    CHECK(r.body.find("\"running\":true") != std::string::npos && r.body.find("\"wanted\":true") != std::string::npos, "status while running: %s", r.body.c_str());

    // WebSocket: bytes for the spectrum, audio for the browser
    std::string buf, pl;
    int op = 0;
    sock_t s = wsOpen(port, buf);
    CHECK(s != kBadSock, "websocket");
    if (s != kBadSock) {
        const std::string sub = cframe("{\"telemetry\":false,\"spectrum\":true,\"bins\":200,\"fps\":10,\"fmt\":\"u8\",\"audio\":true,\"arate\":16000}");
        sockSendAll(s, sub.data(), sub.size());
        bool bin = false, audio = false;
        size_t audioBytes = 0;
        for (int i = 0; i < 400 && !(bin && audioBytes >= 3200); i++) {
            if (!sframe(s, buf, op, pl, 1500)) break;
            if (op == 2 && !pl.empty() && pl[0] == 1) {
                bin = pl.size() == 1 + 4 + 16 + 200;
                if (bin) { float c, rate; memcpy(&c, pl.data() + 5, 4); memcpy(&rate, pl.data() + 9, 4); bin = rate > 1e5f && c > 1e6f; }
            }
            if (op == 2 && !pl.empty() && pl[0] == 2) { audio = true; audioBytes += pl.size() - 1; }
        }
        CHECK(bin, "a binary spectrum message with the right size, centre and rate");
        CHECK(audio && audioBytes >= 3200, "audio reaches the browser: %zu bytes (16 kHz, 16 bit mono: 3200 bytes = 0.1 s)", audioBytes);
        sockClose(s);
    }

    // tuning and the saved state
    r = request(port, "POST", "/api/v1/frequency", "{\"mhz\":93.9}"); CHECK(r.status == 200, "frequency %d %s", r.status, r.body.c_str());
    r = request(port, "POST", "/api/v1/x/demod", "{\"demod\":\"usb\"}"); CHECK(r.status == 200, "demod %d", r.status);
    r = request(port, "POST", "/api/v1/x/demod", "{\"demod\":\"wrong\"}"); CHECK(r.status == 400, "a bad demodulator is refused %d", r.status);
    r = request(port, "POST", "/api/v1/mode", "{\"family\":13}");   // ADS-B: the receiver restarts by itself
    CHECK(r.status == 200, "mode change while running %d %s", r.status, r.body.c_str());
    r = request(port, "GET", "/api/v1/status");
    CHECK(r.body.find("\"modeId\":\"adsb\"") != std::string::npos && r.body.find("\"running\":true") != std::string::npos && r.body.find("\"frequencyHz\":1090000000") != std::string::npos, "the new mode runs at its own frequency: %s", r.body.c_str());
    r = request(port, "POST", "/api/v1/mode", "{\"family\":99999}"); CHECK(r.status == 400, "an unknown mode is refused %d", r.status);
    host.supervise(cfgPath);
    ConfigFile saved;
    CHECK(saved.load(cfgPath) && saved.get("mode") == "adsb" && saved.get("autostart") == "1" && saved.get("audio_demod") == "usb", "the changes are saved: mode=%s autostart=%s", saved.get("mode").c_str(), saved.get("autostart").c_str());
    r = request(port, "POST", "/api/v1/gain", "{\"db\":20}"); CHECK(r.status == 400, "the test signal has no gain %d", r.status);

    r = request(port, "POST", "/api/v1/stop", "{}");
    CHECK(r.status == 200, "stop");
    r = request(port, "GET", "/api/v1/status");
    CHECK(r.body.find("\"running\":false") != std::string::npos && r.body.find("\"wanted\":false") != std::string::npos, "stopped: %s", r.body.c_str());
    host.supervise(cfgPath);
    CHECK(saved.load(cfgPath) && saved.get("autostart") == "0", "stop is saved as autostart=0");

    // a restart with autostart: comes up running by itself
    srv.configure(RemoteApiConfig());
    host.shutdown();
    std::filesystem::remove_all(dir);
}

static void testAutostartAndSupervision() {
    ConfigFile cfg;
    cfg.set("device", "test");
    cfg.set("mode", "fm");
    cfg.set("autostart", "1");
    ServerHost host(cfg);
    host.init();
    CHECK(host.wantRun() && !host.engine().running(), "autostart: wanted, not yet running");
    host.supervise("");
    CHECK(host.engine().running(), "supervise starts a wanted receiver");
    host.engine().stop();   // as if the radio had been unplugged
    CHECK(!host.engine().running(), "stopped from outside");
    host.supervise("");   // inside the 10 s retry window: nothing yet
    CHECK(!host.engine().running(), "no retry before 10 s have passed");
    host.shutdown();
    CHECK(host.wantRun(), "shutdown keeps what is wanted (so the next start brings it back)");
}

// A HackRF's gain from the browser: the amp only with amp=1 in the settings file
static void testHackrfAmp() {
    DeviceInfo d; d.kind = DeviceInfo::HackRF;
    GainSetting g = hackrfGain(d, 70, false);
    CHECK(!g.amp && g.lna + g.vga == 70, "70 dB without the amp: amp %d lna %d vga %d", g.amp, g.lna, g.vga);
    g = hackrfGain(d, 110, false);
    CHECK(!g.amp && g.lna + g.vga == 102, "110 dB without the amp stops at 102: amp %d lna %d vga %d", g.amp, g.lna, g.vga);
    g = hackrfGain(d, 110, true);
    CHECK(g.amp && g.lna + g.vga + 14 == 110, "110 dB with the amp: amp %d lna %d vga %d", g.amp, g.lna, g.vga);
}

int main() {
    testConfig();
    testHackrfAmp();
    testHost();
    testAutostartAndSupervision();
    return testSummary("server_host");
}
