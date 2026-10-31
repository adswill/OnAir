// OnAir Web session manager on synthetic IQ (no sockets): admission limits and the busy refusal, per-session tuning and audio (the tone of
// each user's own channel comes out of that user's audio only), spectrum rows, the policy defaults (voice / pager decoders off until the host
// accepts the legal notice), shared decoders (identical subscriptions share one instance), the host decoder is muted when the host's mode is
// a restricted one, kick / idle, bounded queues for a listener who never reads, and the WebPolicy / settings round trip.
#include "../../server/web/web_config.h"
#include "../../server/web/web_hub.h"
#include "server_config.h"
#include <cmath>
#include <cstdio>
#include <thread>
using namespace onair_web;
#include "../common/test_util.h"

static const double kRate = 2.4e6, kCentre = 100e6;

static void pushTones(WebHub& h, const std::vector<double>& offs, double seconds, size_t& pos) {
    const size_t n = (size_t)(kRate * seconds), chunk = 24000;
    std::vector<cf32> x(chunk);
    for (size_t i = 0; i < n; i += chunk) {
        for (size_t k = 0; k < chunk; k++, pos++) {
            cf32 v(0, 0);
            for (double o : offs) { const double a = 2 * M_PI * o * (double)pos / kRate; v += cf32((float)std::cos(a), (float)std::sin(a)) * 0.05f; }
            x[k] = v;
        }
        h.pushIq(x.data(), chunk, kCentre, kRate);
        h.processPending();
    }
}

static ClientMsg msg(const std::string& j) { ClientMsg m; std::string c, w; if (!parseClientMsg(j, m, c, w)) printf("bad test message %s: %s\n", j.c_str(), w.c_str()); return m; }

struct Drained { std::vector<std::string> text; std::vector<std::string> bin; std::string kick; bool alive = true; };
static Drained drainAll(WebHub& h, int id) {
    Drained d;
    std::vector<OutMsg> out;
    d.alive = h.drain(id, out, d.kick);
    for (auto& m : out) (m.binary ? d.bin : d.text).push_back(m.data);
    return d;
}
static bool hasText(const Drained& d, const std::string& sub) { for (auto& t : d.text) if (t.find(sub) != std::string::npos) return true; return false; }

static double tonePower(const std::vector<int16_t>& x, double rate, double f) {
    double re = 0, im = 0, tot = 0;
    for (size_t i = 0; i < x.size(); i++) { const double a = 2 * M_PI * f * (double)i / rate; re += x[i] * std::cos(a); im -= x[i] * std::sin(a); tot += (double)x[i] * x[i]; }
    return tot > 0 ? 2 * (re * re + im * im) / ((double)x.size() * tot) : 0;
}

static void testAdmission() {
    WebHub h;
    HubLimits L; L.maxUsers = 3; L.maxPerIp = 2;
    h.setLimits(L);
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    const int s1 = h.admit(a, c, w), s2 = h.admit(a, c, w);
    CHECK(s1 > 0 && s2 > 0 && s1 != s2, "two from one address");
    CHECK(h.admit(a, c, w) == 0 && c == err::Busy, "third from the same address refused: %s", w.c_str());
    a.ip = "10.0.0.2";
    CHECK(h.admit(a, c, w) > 0, "another address");
    a.ip = "10.0.0.3";
    CHECK(h.admit(a, c, w) == 0 && c == err::Busy && w.find("full") != std::string::npos, "server full: %s", w.c_str());
    h.leave(s1);
    CHECK(h.admit(a, c, w) > 0, "room again after one left");
    CHECK(h.statsJson().find("\"rejected\":2") != std::string::npos, "rejections counted: %s", h.statsJson().c_str());
    // addresses are not shown unless the host enables it
    CHECK(h.usersJson().find("10.0.0.") == std::string::npos, "no addresses in the user list by default");
    h.setLogIp(true);
    CHECK(h.usersJson().find("10.0.0.") != std::string::npos, "addresses with web_log_ip");
}

static void testTwoListeners() {
    WebHub h;
    std::string c, w;
    AdmitRequest a; a.ip = "10.0.0.1";
    size_t pos = 0;
    pushTones(h, {}, 0.01, pos);   // the capture is known before the first listener
    const int u1 = h.admit(a, c, w);
    a.ip = "10.0.0.2";
    const int u2 = h.admit(a, c, w);
    CHECK(h.welcomeJson(u1).find("\"centre_hz\":100000000") != std::string::npos, "welcome carries the capture: %s", h.welcomeJson(u1).c_str());
    // user 1: USB 1 kHz below a carrier at +300 kHz (hears 1000 Hz); user 2: USB 1.5 kHz below a carrier at -500 kHz (hears 1500 Hz)
    h.handle(u1, msg("{\"type\":\"tune\",\"freq_hz\":100299000,\"mode\":\"usb\"}"));
    h.handle(u2, msg("{\"type\":\"tune\",\"freq_hz\":99498500,\"mode\":\"usb\"}"));
    h.handle(u1, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"pcm16\",\"rate\":12000}"));
    h.handle(u2, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"adpcm\",\"rate\":8000}"));
    h.handle(u2, msg("{\"type\":\"spectrum\",\"on\":true,\"bins\":512,\"fps\":10}"));
    Drained d1 = drainAll(h, u1);
    CHECK(hasText(d1, "\"type\":\"state\"") && hasText(d1, "\"mode\":\"usb\""), "state after tune");
    Drained d2;
    d1 = Drained();
    for (int step = 0; step < 8; step++) {   // a listener that reads as it goes
        pushTones(h, {300000, -500000}, 0.1, pos);
        Drained x1 = drainAll(h, u1), x2 = drainAll(h, u2);
        d1.bin.insert(d1.bin.end(), x1.bin.begin(), x1.bin.end());
        d2.bin.insert(d2.bin.end(), x2.bin.begin(), x2.bin.end());
    }
    std::vector<int16_t> a1, a2, tmp;
    AudioCodec cc; int rate; bool sq;
    int specRows = 0;
    for (auto& b : d1.bin) if (parseAudioFrame(b, cc, rate, sq, tmp)) a1.insert(a1.end(), tmp.begin(), tmp.end());
    for (auto& b : d2.bin) {
        if ((uint8_t)b[0] == kFrameSpectrum) { specRows++; continue; }
        if (parseAudioFrame(b, cc, rate, sq, tmp)) a2.insert(a2.end(), tmp.begin(), tmp.end());
    }
    CHECK(a1.size() > 6000 && a2.size() > 4000, "audio: %zu / %zu samples", a1.size(), a2.size());
    if (a1.size() > 3000 && a2.size() > 3000) {
        std::vector<int16_t> t1(a1.begin() + 2400, a1.end()), t2(a2.begin() + 1600, a2.end());
        CHECK(tonePower(t1, 12000, 1000) > 0.7, "user 1 hears 1000 Hz (%.2f)", tonePower(t1, 12000, 1000));
        CHECK(tonePower(t2, 8000, 1500) > 0.7, "user 2 hears 1500 Hz (%.2f)", tonePower(t2, 8000, 1500));
        CHECK(tonePower(t1, 12000, 1500) < 0.05, "user 1 does not hear user 2's channel");
    }
    CHECK(specRows >= 4 && specRows <= 10, "about 8 spectrum rows in 0.8 s at 10 fps: %d", specRows);
    // out of the capture: refused, the old frequency kept
    h.handle(u1, msg("{\"type\":\"tune\",\"freq_hz\":105000000}"));
    d1 = drainAll(h, u1);
    CHECK(hasText(d1, "\"code\":\"out_of_range\""), "tuning outside the capture is refused");
    // a passband wider than the mode allows
    h.handle(u1, msg("{\"type\":\"tune\",\"mode\":\"nfm\",\"low_hz\":-50000,\"high_hz\":50000}"));
    CHECK(hasText(drainAll(h, u1), "out_of_range"), "NFM 100 kHz wide refused");
    // too much bandwidth for one listener
    HubLimits L = h.limits(); L.maxKbpsUser = 300; h.setLimits(L);
    h.handle(u1, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"pcm16\",\"rate\":48000}"));
    CHECK(hasText(drainAll(h, u1), "out_of_range"), "PCM 48 kHz over a 300 kbit/s limit is refused");
    h.handle(u1, msg("{\"type\":\"ping\",\"t\":42}"));
    CHECK(hasText(drainAll(h, u1), "{\"type\":\"pong\",\"t\":42}"), "pong");
    h.handle(u1, msg("{\"type\":\"bye\"}"));
    d1 = drainAll(h, u1);
    CHECK(!d1.alive && d1.kick == "bye", "bye ends the session");
}

// a decoder that counts its instances and the samples it got
static int gInstances = 0;
struct FakeDec : WebDecoder {
    size_t got = 0;
    std::string pk;
    FakeDec() { gInstances++; }
    ~FakeDec() override { gInstances--; }
    void feed(const cf32*, size_t n) override { got += n; }
    void poll(std::vector<std::string>& ev) override { if (got) ev.push_back("{\"samples\":" + std::to_string(got) + "}"); got = 0; }
    std::string policyKind() const override { return pk; }
};

static void testPolicyAndDecoders() {
    WebPolicy p;
    CHECK(p.decoderAllowed("adsb") && p.decoderAllowed("aprs"), "data decoders on by default");
    for (const char* k : {"dmr", "p25", "nxdn", "tetra", "dpmr", "dstar", "ysf", "fusion", "pocsag", "flex", "pager"}) CHECK(!p.decoderAllowed(k), "%s off by default", k);
    std::string e;
    CHECK(!p.setDecoder("dmr", true, "", e) && !p.decoderAllowed("dmr"), "voice needs the legal notice");
    CHECK(!p.setDecoder("pocsag", true, "yes", e), "a wrong acceptance text is refused");
    CHECK(p.setDecoder("dmr", true, kLegalNoticeVersion, e) && p.decoderAllowed("dmr") && !p.decoderAllowed("p25"), "accepted: only the one enabled");
    onair_server::ConfigFile cf;
    p.save(cf);
    WebPolicy q;
    q.load(cf);
    CHECK(q.decoderAllowed("dmr") && !q.decoderAllowed("pocsag"), "saved and loaded");
    cf.set("web_legal_ack", "onair-web-legal-0");   // an old acceptance does not count
    q.load(cf);
    CHECK(!q.decoderAllowed("dmr"), "an old notice version does not enable anything");
    cf.set("web_decoders_on", "dmr,adsb");          // data kinds cannot sneak in through the restricted list
    cf.set("web_decoders_off", "pocsag,ais");       // nor restricted ones through the data list
    q.load(cf);
    CHECK(!q.decoderAllowed("ais") && q.decoderAllowed("adsb") && !q.decoderAllowed("pocsag"), "lists keep to their class");

    WebHub h;
    FakeDec* hostDec = nullptr;
    h.registerDecoder(DecoderFactory{"aprs", "APRS", 24000, true, [](double) { return std::unique_ptr<WebDecoder>(new FakeDec); }});
    h.registerDecoder(DecoderFactory{"host", "Host", 0, false, [&](double) { auto d = new FakeDec; d->got = 1; hostDec = d; return std::unique_ptr<WebDecoder>(d); }});
    h.registerDecoder(DecoderFactory{"pocsag", "POCSAG", 24000, true, [](double) { return std::unique_ptr<WebDecoder>(new FakeDec); }});
    std::string c, w;
    size_t pos = 0;
    pushTones(h, {}, 0.01, pos);
    AdmitRequest a; a.ip = "10.0.0.1";
    const int u1 = h.admit(a, c, w);
    a.ip = "10.0.0.2";
    const int u2 = h.admit(a, c, w);
    CHECK(h.welcomeJson(u1).find("pocsag") == std::string::npos, "a disabled decoder is not offered");
    h.handle(u1, msg("{\"type\":\"subscribe\",\"decoder\":\"pocsag\",\"freq_hz\":100100000}"));
    CHECK(hasText(drainAll(h, u1), "not_allowed"), "pager decoder refused by default");
    h.handle(u1, msg("{\"type\":\"subscribe\",\"decoder\":\"aprs\",\"freq_hz\":100100200}"));
    h.handle(u2, msg("{\"type\":\"subscribe\",\"decoder\":\"aprs\",\"freq_hz\":100099800}"));   // the same kHz: shared
    CHECK(gInstances == 1, "one shared instance for two listeners (%d)", gInstances);
    CHECK(h.statsJson().find("\"listeners\":2") != std::string::npos, "stats show it shared: %s", h.statsJson().c_str());
    h.handle(u2, msg("{\"type\":\"subscribe\",\"decoder\":\"aprs\",\"freq_hz\":100300000}"));
    CHECK(gInstances == 2, "another frequency: its own instance");
    h.handle(u1, msg("{\"type\":\"subscribe\",\"decoder\":\"aprs\"}"));
    CHECK(hasText(drainAll(h, u1), "needs freq_hz"), "a frequency is required");
    // events reach both subscribers (the DSP thread polls; here: start it briefly)
    drainAll(h, u2);
    h.start();
    pushTones(h, {100000}, 0.3, pos);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    h.stop();
    Drained d1 = drainAll(h, u1), d2 = drainAll(h, u2);
    CHECK(hasText(d1, "\"decoder\":\"aprs@100100k\"") && hasText(d2, "\"decoder\":\"aprs@100100k\""), "events to both listeners");
    CHECK(hasText(d2, "\"decoder\":\"aprs@100300k\"") && !hasText(d1, "aprs@100300k"), "events only to the subscribers");
    h.handle(u2, msg("{\"type\":\"unsubscribe\",\"decoder\":\"aprs\",\"freq_hz\":100300000}"));
    CHECK(gInstances == 1, "the last listener gone: the instance goes");
    h.leave(u2);
    h.leave(u1);
    CHECK(gInstances == 0, "all gone with the sessions (%d)", gInstances);
    // the host decoder: muted while the host's mode is a restricted one
    a.ip = "10.0.0.5";
    const int u3 = h.admit(a, c, w);
    h.handle(u3, msg("{\"type\":\"subscribe\",\"decoder\":\"host\"}"));
    CHECK(hostDec != nullptr, "host decoder made");
    if (hostDec) hostDec->pk = "dmr";
    drainAll(h, u3);
    h.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    h.stop();
    CHECK(!hasText(drainAll(h, u3), "\"type\":\"event\""), "host telemetry of a DMR mode is not passed to web listeners");
    {
        std::lock_guard<std::mutex> g(h.policyMutex());
        CHECK(h.policy().setDecoder("dmr", true, kLegalNoticeVersion, e), "host enables DMR");
    }
    if (hostDec) hostDec->got = 1;
    h.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    h.stop();
    CHECK(hasText(drainAll(h, u3), "\"type\":\"event\""), "after the host's acceptance it is");
}

static void testKickIdleQueue() {
    WebHub h;
    HubLimits L; L.idleMin = 0; L.queueBytes = 64 * 1024;
    h.setLimits(L);
    std::string c, w;
    size_t pos = 0;
    pushTones(h, {}, 0.01, pos);
    AdmitRequest a; a.ip = "10.0.0.1";
    const int u = h.admit(a, c, w);
    h.handle(u, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"pcm16\",\"rate\":16000}"));
    h.handle(u, msg("{\"type\":\"spectrum\",\"on\":true,\"bins\":2048,\"fps\":15}"));
    // never drained for 3 s of signal: the queue stays bounded (old audio and rows dropped)
    pushTones(h, {1000}, 3.0, pos);
    std::vector<OutMsg> out;
    std::string k;
    h.drain(u, out, k);
    size_t bytes = 0;
    for (auto& m : out) bytes += m.data.size();
    CHECK(bytes <= 64 * 1024 + 8192, "queue bounded: %zu bytes", bytes);
    CHECK(h.usersJson().find("\"dropped\":0") == std::string::npos, "drops counted");
    CHECK(h.kick(u, "test"), "kick");
    Drained d = drainAll(h, u);
    CHECK(!d.alive && d.kick == "test", "kicked: %s", d.kick.c_str());
    CHECK(!h.kick(9999, ""), "no such session");
    // idle
    L.idleMin = 1;
    h.setLimits(L);
    const int u2 = h.admit(a, c, w);
    h.tick();
    CHECK(drainAll(h, u2).alive, "not idle yet");
}

static void testSettings() {
    onair_server::ConfigFile cf;
    cf.parse("web=1\nweb_lan=1\nweb_auth=password\nweb_password=hunter22\nweb_max_users=20\nweb_invite=0123456789abcdef0123,Ana,30\nweb_invite=short\n");
    bool made = false;
    WebSettings s = loadWebSettings(cf, made);
    CHECK(s.on && s.server.bind == "0.0.0.0" && s.server.auth.mode == AuthMode::Password && s.limits.maxUsers == 20, "settings");
    CHECK(made && cf.get("web_admin_token").size() == 32, "an admin token is made");
    CHECK(s.server.auth.invites.size() == 1 && s.server.auth.invites[0].label == "Ana" && s.server.auth.invites[0].maxMinutes == 30, "invites");
    CHECK(!s.warnings.empty(), "plain HTTP on the network warns");
    AuthResult r = authenticate(s.server.auth, "hunter22", "");
    CHECK(r.ok && r.cls == UserClass::Member, "password");
    CHECK(!authenticate(s.server.auth, "hunter2", "").ok && !authenticate(s.server.auth, "", "").ok, "wrong password");
    r = authenticate(s.server.auth, "", "0123456789abcdef0123");
    CHECK(r.ok && r.cls == UserClass::Invited && r.maxMinutes == 30, "invite");
    CHECK(!authenticate(s.server.auth, "", "0123456789abcdef0124").ok, "wrong invite");
    std::vector<InviteToken> inv = s.server.auth.invites;
    inv.push_back(InviteToken{"fedcba9876543210fedc", "Bo", 0});
    WebPolicy p;
    saveWebState(cf, p, inv);
    CHECK(cf.getAll("web_invite").size() == 2, "two invites saved: %zu", cf.getAll("web_invite").size());
    FailLimiter fl;
    for (int i = 0; i < 5; i++) fl.fail("1.2.3.4", 100);
    CHECK(fl.blocked("1.2.3.4", 101) && !fl.blocked("1.2.3.5", 101) && !fl.blocked("1.2.3.4", 161), "failed logins block for 60 s");
}

// web_spectrum=0 (the setup window's "audio only"): the spectrum request is refused with a sentence and no rows are ever sent, audio still works
static void testAudioOnly() {
    WebHub h;
    HubLimits L; L.spectrum = false;
    h.setLimits(L);
    std::string c, w;
    size_t pos = 0;
    pushTones(h, {}, 0.01, pos);
    AdmitRequest a; a.ip = "10.0.0.1";
    const int u = h.admit(a, c, w);
    h.handle(u, msg("{\"type\":\"audio\",\"on\":true,\"codec\":\"pcm16\",\"rate\":12000}"));
    h.handle(u, msg("{\"type\":\"spectrum\",\"on\":true,\"bins\":512,\"fps\":10}"));
    pushTones(h, {1000}, 0.5, pos);
    Drained d = drainAll(h, u);
    int rows = 0, frames = 0;
    for (auto& b : d.bin) { if ((uint8_t)b[0] == kFrameSpectrum) rows++; else frames++; }
    CHECK(rows == 0 && frames > 0, "audio only: %d spectrum rows, %d audio frames", rows, frames);
    CHECK(hasText(d, "audio only"), "the listener is told why");
}

int main() {
    testAudioOnly();
    testAdmission();
    testTwoListeners();
    testPolicyAndDecoders();
    testKickIdleQueue();
    testSettings();
    return testSummary("web_hub");
}
