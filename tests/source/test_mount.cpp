// Antenna mount control: Hamlib rotctld and ASCOM Alpaca drivers against fake servers, the Rotator worker on top of them
// (offsets, goto, rate loop with its 3 s watchdog), Alpaca discovery, and the pass-driven mount plan on a real orbit.
#include "dect2/geo/passes.h"
#include "dect2/source/mount_driver.h"
#include "dect2/source/mount_track.h"
#include "dect2/source/rotator.h"
#include "../../core/src/net/netcompat.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static bool waitFor(const std::function<bool()>& f, int ms = 4000) {
    for (int t = 0; t < ms; t += 20) { if (f()) return true; sleepMs(20); }
    return f();
}

// ---- a tiny TCP server on 127.0.0.1 with an ephemeral port; one thread per connection
struct Fake {
    sock_t ls = kBadSock;
    int port = 0;
    std::atomic<bool> quit{false};
    std::thread acceptor;
    std::vector<std::thread> conns;
    std::mutex mu;
    double az = 100, el = 20;
    bool slewing = false, canMove = true, failGoto = false;
    std::vector<std::string> log;
    std::function<void(sock_t)> serve;

    void note(const std::string& s) { std::lock_guard<std::mutex> lk(mu); log.push_back(s); }
    int count(const std::string& sub) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (auto& l : log) if (l.find(sub) != std::string::npos) n++;
        return n;
    }
    std::string last(const std::string& sub) {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = log.size(); i-- > 0;) if (log[i].find(sub) != std::string::npos) return log[i];
        return "";
    }
    void start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof a);
        listen(ls, 8);
        socklen_t l = sizeof a;
        getsockname(ls, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        acceptor = std::thread([this] {
            while (!quit) {
                fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd);
                timeval tv{0, 100000};
                if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
                sock_t c = accept(ls, nullptr, nullptr);
                if (c == kBadSock) continue;
                std::lock_guard<std::mutex> lk(mu);
                conns.emplace_back([this, c] { serve(c); sockClose(c); });
            }
        });
    }
    ~Fake() {
        quit = true;
        if (acceptor.joinable()) acceptor.join();
        for (auto& t : conns) if (t.joinable()) t.join();
        if (ls != kBadSock) sockClose(ls);
    }
};

static bool recvLine(sock_t c, std::string& line, std::atomic<bool>& quit) {
    line.clear();
    while (!quit) {
        fd_set rd; FD_ZERO(&rd); FD_SET(c, &rd);
        timeval tv{0, 100000};
        if (select((int)c + 1, &rd, nullptr, nullptr, &tv) <= 0) continue;
        char ch;
        if (sockRecv(c, &ch, 1) <= 0) return false;
        if (ch == '\n') return true;
        line += ch;
    }
    return false;
}
static void sendStr(sock_t c, const std::string& s) { sockSend(c, s.data(), s.size()); }

// rotctld: P az el -> RPRT 0 (RPRT -1 for el out of range), p -> az\nel\n, S -> RPRT 0, anything else RPRT -4
static void serveRotctld(Fake* f, sock_t c) {
    std::string l;
    while (recvLine(c, l, f->quit)) {
        f->note(l);
        std::lock_guard<std::mutex> lk(f->mu);
        if (l[0] == 'P') {
            double a = 0, e = 0;
            std::sscanf(l.c_str(), "P %lf %lf", &a, &e);
            if (e > 90 || e < 0) { sendStr(c, "RPRT -1\n"); continue; }
            f->az = a; f->el = e;
            sendStr(c, "RPRT 0\n");
        } else if (l[0] == 'p') {
            char b[64];
            std::snprintf(b, sizeof b, "%.6f\n%.6f\n", f->az, f->el);
            sendStr(c, b);
        } else if (l[0] == 'S') sendStr(c, "RPRT 0\n");
        else sendStr(c, "RPRT -4\n");
    }
}

static std::string formValue(const std::string& body, const std::string& key) {
    const size_t k = body.find(key + "=");
    if (k == std::string::npos) return "";
    const size_t e = body.find('&', k);
    return body.substr(k + key.size() + 1, e == std::string::npos ? std::string::npos : e - k - key.size() - 1);
}

// ASCOM Alpaca telescope: just enough of the REST API. Logs "METHOD /endpoint body".
static void serveAlpaca(Fake* f, sock_t c) {
    std::string req;
    while (!f->quit) {
        fd_set rd; FD_ZERO(&rd); FD_SET(c, &rd);
        timeval tv{0, 100000};
        if (select((int)c + 1, &rd, nullptr, nullptr, &tv) <= 0) { if (req.empty()) continue; }
        char buf[1024];
        const long long r = sockRecv(c, buf, sizeof buf);
        if (r <= 0) return;
        req.append(buf, (size_t)r);
        const size_t he = req.find("\r\n\r\n");
        if (he == std::string::npos) continue;
        size_t cl = 0;
        const size_t clp = req.find("Content-Length: ");
        if (clp != std::string::npos) cl = (size_t)std::atoi(req.c_str() + clp + 16);
        if (req.size() < he + 4 + cl) continue;
        break;
    }
    const size_t he = req.find("\r\n\r\n");
    const std::string head = req.substr(0, req.find("\r\n"));
    const std::string body = he == std::string::npos ? "" : req.substr(he + 4);
    const bool isPut = head.compare(0, 3, "PUT") == 0;
    size_t p0 = head.find(' ') + 1, p1 = head.find(' ', p0);
    std::string path = head.substr(p0, p1 - p0);
    std::string query;
    if (const size_t q = path.find('?'); q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    const std::string ep = path.substr(path.rfind('/') + 1);
    f->note(std::string(isPut ? "PUT " : "GET ") + ep + " " + (isPut ? body : query));
    std::string value = "null";
    int err = 0;
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(f->mu);
        if (path.find("/api/v1/telescope/0/") != 0) { err = 1; msg = "wrong device"; }
        else if (ep == "azimuth") { char b[32]; std::snprintf(b, sizeof b, "%.4f", f->az); value = b; }
        else if (ep == "altitude") { char b[32]; std::snprintf(b, sizeof b, "%.4f", f->el); value = b; }
        else if (ep == "slewing") value = f->slewing ? "true" : "false";
        else if (ep == "canslewaltaz") value = "true";
        else if (ep == "canmoveaxis") value = f->canMove ? "true" : "false";
        else if (ep == "connected") {}
        else if (ep == "slewtoaltazasync") {
            if (f->failGoto) { err = 0x500; msg = "Slew limit reached"; }
            else { f->az = std::atof(formValue(body, "Azimuth").c_str()); f->el = std::atof(formValue(body, "Altitude").c_str()); }
        } else if (ep == "abortslew" || ep == "moveaxis") {}
        else { err = 0x400; msg = "not implemented"; }
    }
    char j[256];
    std::snprintf(j, sizeof j, "{\"Value\":%s,\"ClientTransactionID\":1,\"ServerTransactionID\":9,\"ErrorNumber\":%d,\"ErrorMessage\":\"%s\"}", value.c_str(), err, msg.c_str());
    const std::string js = j;
    sendStr(c, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(js.size()) + "\r\nConnection: close\r\n\r\n" + js);
}

static RotatorConfig cfgFor(RotatorProtocol p, int port) {
    RotatorConfig c;
    c.link = RotatorLink::Tcp;
    c.protocol = p;
    c.host = "127.0.0.1";
    c.tcpPort = port;
    return c;
}

int main() {
    // ---- pure helpers
    {
        CHECK(rotctldSetPosCmd(-10, 45.5) == "P 350.000 45.500\n", "set cmd '%s'", rotctldSetPosCmd(-10, 45.5).c_str());
        int code = 99;
        CHECK(parseRprt("RPRT 0", code) && code == 0, "rprt 0");
        CHECK(parseRprt("RPRT -1", code) && code == -1, "rprt -1");
        CHECK(!parseRprt("123.4", code), "a position is not RPRT");
        double a = 0, e = 0;
        CHECK(parseRotctldPos("181.250000", "12.5", a, e) && a == 181.25 && e == 12.5, "pos parse");
        CHECK(!parseRotctldPos("abc", "1", a, e), "garbage position");
        std::string v, err;
        CHECK(alpacaParseResponse("{\"Value\":123.5,\"ErrorNumber\":0,\"ErrorMessage\":\"\"}", v, err) && v == "123.5", "alpaca number '%s'", v.c_str());
        CHECK(alpacaParseResponse("{\"Value\":true,\"ErrorNumber\":0}", v, err) && v == "true", "alpaca bool");
        CHECK(!alpacaParseResponse("{\"Value\":null,\"ErrorNumber\":1035,\"ErrorMessage\":\"Slew limit\"}", v, err) && err.find("1035") != std::string::npos &&
              err.find("Slew limit") != std::string::npos, "alpaca error '%s'", err.c_str());
        CHECK(!alpacaParseResponse("<html>404</html>", v, err), "not json");
        CHECK(std::fabs(mountRateForError(0.0, 0.3, 6) - 0.3) < 1e-12, "inside deadband: target rate only");
        CHECK(std::fabs(mountRateForError(2.0, 0.3, 6) - 1.9) < 1e-9, "rate with error %f", mountRateForError(2.0, 0.3, 6));
        CHECK(mountRateForError(-100, 0, 6) == -6 && mountRateForError(100, 1, 6) == 6, "rate clamp");
        CHECK(std::fabs(azDelta(350, 10) - 20) < 1e-9 && std::fabs(azDelta(10, 350) + 20) < 1e-9, "azimuth wrap");
    }

    // ---- rotctld driver
    {
        Fake f;
        f.serve = [&f](sock_t c) { serveRotctld(&f, c); };
        f.start();
        auto d = makeRotctldDriver("127.0.0.1", f.port);
        std::string err;
        CHECK(d->connect(err), "rotctld connect: %s", err.c_str());
        MountPos p;
        CHECK(d->readPosition(p, err) && std::fabs(p.azDeg - 100) < 1e-3 && std::fabs(p.elDeg - 20) < 1e-3, "rotctld read %s", err.c_str());
        CHECK(d->gotoAzEl(271.5, 33.25, err), "rotctld goto %s", err.c_str());
        CHECK(d->readPosition(p, err) && std::fabs(p.azDeg - 271.5) < 1e-3 && std::fabs(p.elDeg - 33.25) < 1e-3, "rotctld readback %.3f %.3f", p.azDeg, p.elDeg);
        CHECK(!d->gotoAzEl(10, 120, err) && err.find("-1") != std::string::npos, "rotctld error code surfaced: '%s'", err.c_str());
        CHECK(d->stop(err), "rotctld stop");
        CHECK(!d->supportsRates(), "rotctld takes no speeds");
        CHECK(!d->moveAxes(1, 1, err), "rotctld refuses moveAxes");
        d->close();
        CHECK(!d->readPosition(p, err), "closed driver fails cleanly");
        // a server that is not there
        auto bad = makeRotctldDriver("127.0.0.1", 1);
        CHECK(!bad->connect(err) && !err.empty(), "connect refused is an error");
        // probe
        MountPos pp; std::string msg;
        CHECK(probeMount(cfgFor(RotatorProtocol::Rotctld, f.port), pp, msg) && msg.find("rotctld") != std::string::npos, "probe '%s'", msg.c_str());
        RotatorConfig g = cfgFor(RotatorProtocol::Gs232, f.port);
        CHECK(!probeMount(g, pp, msg), "probe refuses GS-232 here");
    }

    // ---- Alpaca driver
    {
        Fake f;
        f.serve = [&f](sock_t c) { serveAlpaca(&f, c); };
        f.start();
        auto d = makeAlpacaDriver("127.0.0.1", f.port, 0);
        std::string err;
        CHECK(d->connect(err), "alpaca connect: %s", err.c_str());
        CHECK(f.count("PUT connected Connected=true") == 1, "connected set");
        CHECK(d->supportsRates(), "alpaca reports MoveAxis");
        MountPos p;
        CHECK(d->readPosition(p, err) && std::fabs(p.azDeg - 100) < 1e-3 && std::fabs(p.elDeg - 20) < 1e-3 && p.slewingValid && !p.slewing, "alpaca read: %s", err.c_str());
        CHECK(d->gotoAzEl(-30, 45, err), "alpaca goto %s", err.c_str());
        const std::string g = f.last("slewtoaltazasync");
        CHECK(g.find("Azimuth=330.0000") != std::string::npos && g.find("Altitude=45.0000") != std::string::npos && g.find("ClientID=") != std::string::npos, "goto body '%s'", g.c_str());
        CHECK(d->moveAxes(0.25, -0.125, err), "moveaxis");
        CHECK(f.count("PUT moveaxis Axis=0&Rate=0.25000") == 1 && f.count("PUT moveaxis Axis=1&Rate=-0.12500") == 1, "moveaxis bodies");
        CHECK(d->gotoAzEl(10, 10, err), "goto after axis motion");
        CHECK(f.count("Axis=0&Rate=0.00000") >= 1, "axes were zeroed before the slew");
        CHECK(d->stop(err) && f.count("PUT abortslew") == 1, "abortslew");
        { std::lock_guard<std::mutex> lk(f.mu); f.failGoto = true; }
        CHECK(!d->gotoAzEl(10, 10, err) && err.find("Slew limit") != std::string::npos, "alpaca error message surfaced: '%s'", err.c_str());
        d->close();
        CHECK(f.count("Connected=false") == 1, "disconnected on close");
        MountPos pp; std::string msg;
        CHECK(probeMount(cfgFor(RotatorProtocol::Alpaca, f.port), pp, msg) && msg.find("Alpaca") != std::string::npos && msg.find("speeds") != std::string::npos, "probe '%s'", msg.c_str());
        // a goto-only mount
        { std::lock_guard<std::mutex> lk(f.mu); f.canMove = false; f.failGoto = false; }
        auto d2 = makeAlpacaDriver("127.0.0.1", f.port, 0);
        CHECK(d2->connect(err) && !d2->supportsRates(), "goto-only mount");
        CHECK(!d2->moveAxes(1, 0, err), "no speeds on a goto-only mount");
        d2->close();
        // wrong device number gets the server's error
        auto d3 = makeAlpacaDriver("127.0.0.1", f.port, 7);
        CHECK(!d3->connect(err) && err.find("wrong device") != std::string::npos, "wrong device: '%s'", err.c_str());
    }

    // ---- Rotator worker on rotctld: corrected az goes through the north offset
    {
        Fake f;
        f.serve = [&f](sock_t c) { serveRotctld(&f, c); };
        f.start();
        Rotator& r = Rotator::get();
        RotatorCalibration cal;
        cal.azOffsetDeg = 10;   // corrected = mechanical + 10
        cal.shortestPathAz = false;
        r.setCalibration(cal);
        r.configure(cfgFor(RotatorProtocol::Rotctld, f.port));
        CHECK(waitFor([&] { return r.status().connected && r.status().azValid; }), "rotator over rotctld connects: %s", r.status().lastError.c_str());
        r.gotoAzEl(200, 30);
        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(f.mu); return std::fabs(f.az - 190) < 0.01 && std::fabs(f.el - 30) < 0.01; }), "mechanical az = 190 for corrected 200 (offset 10): %.2f", f.az);
        CHECK(waitFor([&] { RotatorStatus s = r.status(); return std::fabs(s.azCorrectedDeg - 200) < 0.1; }), "status shows corrected 200");
        r.trackAzEl(210, 35, 0.2, 0.1);   // rotctld takes no speeds: becomes a goto
        CHECK(waitFor([&] { std::lock_guard<std::mutex> lk(f.mu); return std::fabs(f.az - 200) < 0.01 && std::fabs(f.el - 35) < 0.01; }), "track on a goto mount goes to the position: %.2f", f.az);
        CHECK(!r.supportsRates(), "rotctld: no rates");
        r.stop();
        CHECK(waitFor([&] { return f.count("S") > 0 && f.last("S") == "S"; }), "stop sent");
        CHECK(r.apiStatusJson().find("\"link\":\"rotctld\"") != std::string::npos, "api json names the protocol");
        r.disconnect();
        cal = RotatorCalibration{};
        r.setCalibration(cal);
    }

    // ---- Rotator worker on Alpaca: rate loop, then the watchdog stops the axes when the target is not refreshed
    {
        Fake f;
        f.serve = [&f](sock_t c) { serveAlpaca(&f, c); };
        f.start();
        Rotator& r = Rotator::get();
        r.configure(cfgFor(RotatorProtocol::Alpaca, f.port));
        CHECK(waitFor([&] { return r.status().connected && r.status().azValid; }), "rotator over Alpaca connects: %s", r.status().lastError.c_str());
        CHECK(r.supportsRates(), "Alpaca MoveAxis seen");
        r.trackAzEl(110, 20, 0.3, 0.0);   // mount is at az 100, el 20: 10 degrees to go, target moving at 0.3 deg/s
        CHECK(waitFor([&] { return f.count("PUT moveaxis Axis=0&Rate=") > 0; }), "rate loop issues MoveAxis");
        const std::string m = f.last("PUT moveaxis Axis=0");
        const double rate = std::atof(m.substr(m.find("Rate=") + 5).c_str());
        CHECK(rate > 5.5 && rate <= 6.0001, "rate clamped to the mount limit, got %.3f", rate);
        const std::string m1 = f.last("PUT moveaxis Axis=1");
        CHECK(std::fabs(std::atof(m1.substr(m1.find("Rate=") + 5).c_str())) < 1e-6, "no elevation motion needed");
        // refresh it while on target: speed settles to the target's own
        { std::lock_guard<std::mutex> lk(f.mu); f.az = 110; }
        r.trackAzEl(110, 20, 0.3, 0.0);
        sleepMs(700);
        const std::string m2 = f.last("PUT moveaxis Axis=0");
        const double rate2 = std::atof(m2.substr(m2.find("Rate=") + 5).c_str());
        CHECK(std::fabs(rate2 - 0.3) < 0.01, "on target: feed-forward only, got %.3f", rate2);
        // stop refreshing: after 3 s the axes are zeroed
        const bool zeroed = waitFor([&] {
            const std::string z = f.last("PUT moveaxis Axis=0");
            return std::fabs(std::atof(z.substr(z.find("Rate=") + 5).c_str())) < 1e-9;
        }, 6000);
        CHECK(zeroed, "watchdog zeroes the axes when the target goes stale");
        // goto after tracking
        r.gotoAzEl(50, 10);
        CHECK(waitFor([&] { return f.count("PUT slewtoaltazasync") > 0; }), "goto via Alpaca");
        r.stop();
        CHECK(waitFor([&] { return f.count("PUT abortslew") > 0; }), "stop via Alpaca");
        r.disconnect();
    }

    // ---- Rotator: a refused connection is reported, not hung
    {
        Rotator& r = Rotator::get();
        r.configure(cfgFor(RotatorProtocol::Rotctld, 1));
        CHECK(waitFor([&] { return !r.status().lastError.empty(); }, 3000), "connect failure reported");
        r.disconnect();
    }

    // ---- Alpaca discovery against a fake responder
    {
        netInit();
        sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(u, (sockaddr*)&a, sizeof a);
        socklen_t l = sizeof a;
        getsockname(u, (sockaddr*)&a, &l);
        const int port = ntohs(a.sin_port);
        std::thread t([&] {
            char buf[64];
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            fd_set rd; FD_ZERO(&rd); FD_SET(u, &rd);
            timeval tv{3, 0};
            if (select((int)u + 1, &rd, nullptr, nullptr, &tv) <= 0) return;
            const int n = (int)recvfrom(u, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
            if (n == 16 && std::string(buf, 16) == "alpacadiscovery1") {
                const std::string r = "{\"AlpacaPort\":32323}";
                sockSendTo(u, r.data(), r.size(), from);
            }
        });
        const std::vector<std::string> found = alpacaDiscover(1200, "127.0.0.1", port);
        t.join();
        sockClose(u);
        CHECK(found.size() == 1 && found[0] == "127.0.0.1:32323", "discovery found %zu: %s", found.size(), found.empty() ? "" : found[0].c_str());
        CHECK(alpacaDiscover(200, "127.0.0.1", port).empty(), "silence = nothing found");
    }

    // ---- pass-driven plan on a real orbit (ISS-like TLE, observer in Lithuania)
    {
        auto fix = [](std::string l) {
            l.resize(68, ' ');
            int s = 0;
            for (char c : l) { if (c >= '0' && c <= '9') s += c - '0'; else if (c == '-') s++; }
            return l + char('0' + s % 10);
        };
        Tle t;
        const bool ok = parseTle(fix("1 25544U 98067A   24001.50000000  .00016717  00000-0  10270-3 0  900"),
                                 fix("2 25544  51.6400 208.9163 0006317  69.9862  25.2906 15.49560000 00019"), t, "ISS");
        Sgp4 s;
        CHECK(ok && s.init(t), "tle");
        Observer o; o.latDeg = 54.7; o.lonDeg = 25.3; o.altKm = 0.1;
        const double t0 = jdToUnix(t.epochJd);
        const std::vector<Pass> ps = findPasses(s, o, t0, t0 + 86400, 10);
        CHECK(!ps.empty(), "passes found");
        if (!ps.empty()) {
            const Pass& p = ps[ps.size() > 1 ? 1 : 0];   // a whole pass: the first may be under way at the epoch
            MountPlan far = planMountForPass(s, o, p, p.aos - 3600);
            CHECK(far.phase == MountPhase::Idle && far.minutesToAos > 59, "idle an hour before");
            MountPlan pre = planMountForPass(s, o, p, p.aos - 30);
            CHECK(pre.phase == MountPhase::Preposition && std::fabs(pre.azDeg - p.aosAzDeg) < 1e-9 && pre.elDeg == 0, "preposition at rise az");
            MountPlan mid = planMountForPass(s, o, p, p.tca);
            Look lk;
            lookAt(s, o, p.tca, lk);
            CHECK(mid.phase == MountPhase::Track && std::fabs(mid.azDeg - lk.azDeg) < 1e-6 && std::fabs(mid.elDeg - lk.elDeg) < 1e-6, "track follows the satellite");
            CHECK(mid.elDeg >= 10 && std::fabs(mid.elDeg - p.maxElDeg) < 0.5, "culmination el %.1f vs %.1f", mid.elDeg, p.maxElDeg);
            // speeds match a finite difference over a smaller step
            Look a1, a2;
            lookAt(s, o, p.tca - 0.5, a1);
            lookAt(s, o, p.tca + 0.5, a2);
            CHECK(std::fabs(mid.azRateDegS - azDelta(a1.azDeg, a2.azDeg)) < 0.05 && std::fabs(mid.elRateDegS - (a2.elDeg - a1.elDeg)) < 0.05,
                  "rates az %.3f el %.3f", mid.azRateDegS, mid.elRateDegS);
            // before TCA the satellite climbs, after it descends
            MountPlan early = planMountForPass(s, o, p, p.tca - 20);
            MountPlan late = planMountForPass(s, o, p, p.tca + 20);
            CHECK(early.elRateDegS > 0 && late.elRateDegS < 0, "el speed sign %.3f %.3f", early.elRateDegS, late.elRateDegS);
            // the azimuth across a pass that crosses north must not jump: speeds stay small
            CHECK(std::fabs(early.azRateDegS) < 3 && std::fabs(late.azRateDegS) < 3, "az speed sane (wrap safe) %.2f %.2f", early.azRateDegS, late.azRateDegS);
            MountPlan after = planMountForPass(s, o, p, p.los + 10);
            CHECK(after.phase == MountPhase::Park, "park after LOS");
            CHECK(planMountForPass(s, o, p, p.los + 10, 90, 0).phase == MountPhase::Idle, "parkAfter 0 = never park");
            CHECK(planMountForPass(s, o, p, p.los + 1000).phase == MountPhase::Idle, "idle long after");
        }
    }

    printf(fails ? "mount: %d FAILED\n" : "mount: all passed\n", fails);
    return fails ? 1 : 0;
}
