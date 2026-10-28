// Internet TV: parsers, URL handling, AES-128, and the stream reader against a local HTTP server that serves a self-made HLS stream.
#include "dect2/tv/iptv.h"
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/tv/iptv_aes.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

using namespace dect2;
using namespace dect2::iptv;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// ---------- a tiny HTTP server ----------
struct Req { std::string path, ua, referer, range; };
struct Resp { int status = 200; std::string type = "application/octet-stream", body, location; bool chunked = false, drop = false; };
struct Server {
    sock_t ls = kBadSock; int port = 0;
    std::atomic<bool> run{true};
    std::thread th;
    std::function<Resp(const Req&)> handler;
    std::atomic<int> hits{0};
    Server() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 16);
        socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
        sockRecvTimeout(ls, 200);
        th = std::thread([this] {
            while (run) {
                fd_set rd; FD_ZERO(&rd); FD_SET(ls, &rd); timeval tv{0, 100000};
                sock_t c = kBadSock;
                if (select((int)ls + 1, &rd, nullptr, nullptr, &tv) > 0) c = accept(ls, nullptr, nullptr);
                if (c == kBadSock) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                std::thread([this, c] { serve(c); }).detach();
            }
        });
    }
    ~Server() { run = false; if (th.joinable()) th.join(); sockClose(ls); std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
    void serve(sock_t c) {
        sockRecvTimeout(c, 2000);
        std::string rq; char b[2048];
        while (rq.find("\r\n\r\n") == std::string::npos) { long long k = sockRecv(c, b, sizeof b); if (k <= 0) { sockClose(c); return; } rq.append(b, (size_t)k); }
        Req r;
        r.path = rq.substr(4, rq.find(' ', 4) - 4);
        auto hdr = [&](const char* n) { size_t p = rq.find(std::string("\r\n") + n + ": "); if (p == std::string::npos) return std::string(); p += strlen(n) + 4; return rq.substr(p, rq.find("\r\n", p) - p); };
        r.ua = hdr("User-Agent"); r.referer = hdr("Referer"); r.range = hdr("Range");
        hits++;
        Resp s = handler(r);
        if (s.drop) { sockClose(c); return; }
        std::string h = "HTTP/1.1 " + std::to_string(s.status) + " X\r\nContent-Type: " + s.type + "\r\nConnection: close\r\n";
        if (!s.location.empty()) h += "Location: " + s.location + "\r\n";
        if (s.chunked) h += "Transfer-Encoding: chunked\r\n\r\n"; else h += "Content-Length: " + std::to_string(s.body.size()) + "\r\n\r\n";
        sockSendAll(c, h.data(), h.size());
        if (s.chunked) {
            for (size_t i = 0; i < s.body.size(); i += 700) {
                const size_t n = std::min<size_t>(700, s.body.size() - i);
                char sz[16]; snprintf(sz, sizeof sz, "%zx\r\n", n);
                sockSendAll(c, sz, strlen(sz)); sockSendAll(c, s.body.data() + i, n); sockSendAll(c, "\r\n", 2);
            }
            sockSendAll(c, "0\r\n\r\n", 5);
        } else sockSendAll(c, s.body.data(), s.body.size());
        sockClose(c);
    }
    std::string url(const std::string& p) const { return "http://127.0.0.1:" + std::to_string(port) + p; }
};

static std::string makeTs(int packets, int pid = 0x100, int startCc = 0) {   // synthetic packets: sync, PID, counter, filler
    std::string s;
    for (int i = 0; i < packets; i++) {
        std::string p(188, (char)0xFF);
        p[0] = 0x47; p[1] = (char)(pid >> 8); p[2] = (char)(pid & 0xFF); p[3] = (char)(0x10 | ((startCc + i) & 15));
        s += p;
    }
    return s;
}

static bool waitFor(const std::function<bool()>& f, double sec) {
    auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(sec);
    while (std::chrono::steady_clock::now() < end) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    return f();
}

static void testUrls() {
    Url u = parseUrl("https://user:pw@Example.com:8443/a/b.m3u8?x=1#frag");
    CHECK(u.ok && u.scheme == "https" && u.host == "Example.com" && u.port == 8443 && u.path == "/a/b.m3u8?x=1" && u.user == "user:pw", "parse %s %s %d", u.host.c_str(), u.path.c_str(), u.port);
    u = parseUrl("udp://@239.1.1.1:1234"); CHECK(u.ok && u.host == "@239.1.1.1" == false || u.ok, "udp");
    u = parseUrl("http://[::1]:8080/x"); CHECK(u.ok && u.ipv6 && u.host == "::1" && u.port == 8080, "ipv6");
    CHECK(!parseUrl("not a url").ok && !parseUrl("http://").ok && !parseUrl("http://h:99999/").ok, "invalid urls accepted");
    CHECK(parseUrl("http://h/").port == 80 && parseUrl("https://h/").port == 443, "default ports");
    const std::string b = "http://h.example/live/x/index.m3u8?t=1";
    CHECK(resolveUrl(b, "seg1.ts") == "http://h.example/live/x/seg1.ts", "rel: %s", resolveUrl(b, "seg1.ts").c_str());
    CHECK(resolveUrl(b, "../y/seg.ts") == "http://h.example/live/y/seg.ts", "dotdot: %s", resolveUrl(b, "../y/seg.ts").c_str());
    CHECK(resolveUrl(b, "/root.ts") == "http://h.example/root.ts", "abs path");
    CHECK(resolveUrl(b, "//cdn.example/z.ts") == "http://cdn.example/z.ts", "scheme relative");
    CHECK(resolveUrl(b, "https://o.example/q") == "https://o.example/q", "absolute");
    CHECK(resolveUrl("http://h:81/a", "b") == "http://h:81/b", "port kept: %s", resolveUrl("http://h:81/a", "b").c_str());
    CHECK(redactUrl("http://u:p@h/x?token=abc&a=1") == "http://h/x?token=***&a=1", "redact: %s", redactUrl("http://u:p@h/x?token=abc&a=1").c_str());
}

static void testM3u() {
    std::string t = "\xEF\xBB\xBF#EXTM3U url-tvg=\"http://g.example/epg.xml,http://x\"\r\n"
        "#EXTINF:-1 tvg-id=\"bbc.uk\" tvg-name=\"BBC One\" tvg-logo=\"http://l/x.png\" group-title=\"News, UK\",BBC One HD\r\n"
        "#EXTVLCOPT:http-user-agent=Mozilla/5.0\r\n#EXTVLCOPT:http-referrer=http://ref/\r\n"
        "http://s.example/live/bbc.m3u8\r\n"
        "#EXTINF:-1 group-title='Kids' tvg-id=cn,Cartoon, the channel\n/rel/cn.ts\n"
        "#EXTINF:-1,Dup\nhttp://d/1\n#EXTINF:-1,Dup\nhttp://d/1\n"
        "#EXTINF:-1 tvg-id=\"drm\",Protected\r#KODIPROP:inputstream.adaptive.license_type=com.widevine.alpha\rhttp://p/x.mpd\r"
        "#EXTGRP:Music\n#EXTINF:0,Pipe\nhttp://k/v.m3u8|User-Agent=KodiUA&Referer=http://kr/\n"
        "http://bare.example/plain.ts\nftp://ignored/x\n";
    Playlist p = parseM3u(t, "http://pl.example/lists/main.m3u");
    CHECK(p.guideUrl == "http://g.example/epg.xml", "guide '%s'", p.guideUrl.c_str());
    CHECK(p.channels.size() == 6, "channels %zu", p.channels.size());
    if (p.channels.size() >= 6) {
        auto& c = p.channels[0];
        CHECK(c.name == "BBC One HD" && c.tvgId == "bbc.uk" && c.group == "News, UK" && c.logo == "http://l/x.png" && c.userAgent == "Mozilla/5.0" && c.referer == "http://ref/", "ch0 %s|%s|%s", c.name.c_str(), c.group.c_str(), c.userAgent.c_str());
        CHECK(p.channels[1].name == "Cartoon, the channel" && p.channels[1].group == "Kids" && p.channels[1].url == "http://pl.example/rel/cn.ts", "ch1 '%s' '%s'", p.channels[1].name.c_str(), p.channels[1].url.c_str());
        CHECK(p.channels[3].drm, "drm flag");
        CHECK(p.channels[4].group == "Music" && p.channels[4].userAgent == "KodiUA" && p.channels[4].referer == "http://kr/" && p.channels[4].url == "http://k/v.m3u8", "pipe '%s' '%s'", p.channels[4].group.c_str(), p.channels[4].url.c_str());
        CHECK(p.channels[5].name == "plain.ts", "plain name %s", p.channels[5].name.c_str());
    }
    CHECK(p.duplicatesDropped == 1, "duplicates %zu", p.duplicatesDropped);
    // an HLS playlist is one stream
    Playlist h = parseM3u("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow.m3u8\n", "http://h/m.m3u8");
    CHECK(h.isHlsStream && h.channels.size() == 1 && h.channels[0].url == "http://h/m.m3u8", "hls playlist as stream");
    // a plain list of URLs without #EXTM3U
    Playlist q = parseM3u("http://a/1.ts\nhttp://a/2.ts\n");
    CHECK(q.channels.size() == 2, "plain url list %zu", q.channels.size());
    CHECK(parseM3u("").channels.empty() && parseM3u("#EXTM3U\n").channels.empty(), "empty");
    // 50 000 channels
    std::string big = "#EXTM3U\n";
    for (int i = 0; i < 50000; i++) big += "#EXTINF:-1 tvg-id=\"c" + std::to_string(i) + "\" group-title=\"G" + std::to_string(i % 40) + "\",Channel " + std::to_string(i) + "\nhttp://h/" + std::to_string(i) + ".m3u8\n";
    auto t0 = std::chrono::steady_clock::now();
    Playlist bp = parseM3u(big, "http://h/");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(bp.channels.size() == 50000 && bp.channels[49999].tvgId == "c49999", "big list %zu", bp.channels.size());
    CHECK(ms < 2000, "50k channels took %.0f ms", ms);
    std::printf("  50000-channel playlist parsed in %.0f ms\n", ms);
}

static void testXmltv() {
    CHECK(parseXmltvTime("20240131203000 +0200") == 1706725800, "time %lld", (long long)parseXmltvTime("20240131203000 +0200"));
    CHECK(parseXmltvTime("20240131183000 +0000") == 1706725800 && parseXmltvTime("20240131183000") == 1706725800, "utc");
    CHECK(parseXmltvTime("20240131103000 -0800") == 1706725800, "negative zone");
    CHECK(parseXmltvTime("junk") == 0 && parseXmltvTime("") == 0, "invalid time");
    std::string x = "<?xml version=\"1.0\"?><tv><channel id=\"bbc.uk\"><display-name>BBC One</display-name></channel>"
        "<programme start=\"20240131180000 +0000\" stop=\"20240131190000 +0000\" channel=\"bbc.uk\"><title lang=\"en\">News &amp; Weather</title><desc><![CDATA[Top <stories>]]></desc><category>News</category></programme>"
        "<programme start=\"20240131190000 +0000\" stop=\"20240131200000 +0000\" channel=\"bbc.uk\"><title>Next &#233;</title></programme></tv>";
    Guide g = parseXmltv(x);
    CHECK(g.programmes == 2 && g.names["bbc.uk"] == "BBC One", "guide %zu", g.programmes);
    Channel c; c.tvgId = "bbc.uk";
    const Programme* nx = nullptr;
    const Programme* now = guideNow(g, c, 1706725800 - 600 + 1800 * 0, &nx);   // 18:20 UTC
    CHECK(now && now->title == "News & Weather" && now->desc == "Top <stories>" && nx && nx->title == "Next \xC3\xA9", "now/next");
    Channel byName; byName.name = "bbc one";
    CHECK(guideNow(g, byName, 1706725800 - 600) != nullptr, "match by display name");
    CHECK(guideNow(g, c, 1706725800 + 100000) == nullptr, "after the last programme");
    std::string big = "<tv>";
    for (int i = 0; i < 30000; i++) big += "<programme start=\"2024013118" + std::string(i % 2 ? "00" : "30") + "00 +0100\" stop=\"20240131200000 +0100\" channel=\"c" + std::to_string(i % 500) + "\"><title>T" + std::to_string(i) + "</title></programme>";
    big += "</tv>";
    auto t0 = std::chrono::steady_clock::now();
    Guide bg = parseXmltv(big);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(bg.programmes == 30000 && bg.byChannel.size() == 500, "big guide %zu", bg.programmes);
    std::printf("  30000-programme guide parsed in %.0f ms\n", ms);
}

static void testHls() {
    HlsPlaylist m = parseHls("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"Deutsch\",LANGUAGE=\"de\",URI=\"audio_de.m3u8\"\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"a\"\nv/low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2800000,RESOLUTION=1280x720\nv/hi.m3u8\n", "http://h/x/master.m3u8");
    CHECK(m.ok && m.master && m.variants.size() == 2 && m.variants[1].h == 720 && m.variants[0].uri == "http://h/x/v/low.m3u8" && m.media.size() == 1 && m.media[0].lang == "de", "master");
    HlsPlaylist v = parseHls("#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:4.0,\ns100.ts\n#EXT-X-DISCONTINUITY\n#EXTINF:3.5,\ns101.ts\n", "http://h/p/i.m3u8");
    CHECK(v.ok && !v.master && v.live && v.segments.size() == 2 && v.segments[1].seq == 101 && v.segments[1].discontinuity && v.mediaSequence == 100 && v.targetDuration == 4, "media");
    HlsPlaylist e = parseHls("#EXTM3U\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n", "http://h/");
    CHECK(e.endList && !e.live, "vod");
    HlsPlaylist d = parseHls("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n#EXTINF:4,\na.ts\n", "http://h/");
    CHECK(d.keyMethod.rfind("DRM:", 0) == 0, "fairplay flagged: %s", d.keyMethod.c_str());
    CHECK(!parseHls("<html>", "http://h/").ok, "html rejected");
    std::vector<HlsVariant> vs(3); vs[0].bandwidth = 500000; vs[1].bandwidth = 1500000; vs[2].bandwidth = 5000000;
    CHECK(pickVariant(vs, 10e6, -1, 0) == 2 && pickVariant(vs, 1.2e6, -1, 2) == 0 && pickVariant(vs, 2.1e6, -1, 0) == 1 && pickVariant(vs, 1e6, 2, 0) == 2, "pickVariant");
}

static void testAes() {
    const uint8_t key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
    const uint8_t pt[16] = {0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34};
    const uint8_t ct[16] = {0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32};   // FIPS-197 appendix B
    Aes128 a(key);
    uint8_t o[16]; a.encryptBlock(pt, o); CHECK(memcmp(o, ct, 16) == 0, "AES encrypt vector");
    a.decryptBlock(ct, o); CHECK(memcmp(o, pt, 16) == 0, "AES decrypt vector");
    // CBC round trip with padding
    uint8_t iv[16]; for (int i = 0; i < 16; i++) iv[i] = (uint8_t)(i * 7);
    std::vector<uint8_t> plain(1000); for (size_t i = 0; i < plain.size(); i++) plain[i] = (uint8_t)(i * 31);
    std::vector<uint8_t> enc = plain; const uint8_t pad = (uint8_t)(16 - enc.size() % 16); enc.insert(enc.end(), pad, pad);
    uint8_t prev[16]; memcpy(prev, iv, 16);
    for (size_t i = 0; i < enc.size(); i += 16) { for (int k = 0; k < 16; k++) enc[i + k] ^= prev[k]; a.encryptBlock(&enc[i], &enc[i]); memcpy(prev, &enc[i], 16); }
    std::vector<uint8_t> dec = enc;
    CHECK(aes128CbcDecrypt(key, iv, dec) && dec == plain, "CBC round trip");
    dec = enc; dec[5] ^= 1; dec.back() ^= 1;
    CHECK(!aes128CbcDecrypt(key, iv, dec) || dec != plain, "damaged data detected or changed");
}

static void testAligner() {
    std::string got;
    TsAligner al([&](const uint8_t* d, size_t n) { got.append((const char*)d, n); });
    std::string ts = makeTs(20), junk(77, 'x');
    std::string in = junk + ts.substr(0, 500) + std::string(5, 'y') + ts.substr(500);   // junk at the start and inside
    for (size_t i = 0; i < in.size(); i += 333) al.feed((const uint8_t*)in.data() + i, std::min<size_t>(333, in.size() - i));
    CHECK(got.size() % 188 == 0 && got.size() >= 188 * 17 && got[0] == 0x47, "aligned output %zu bytes", got.size());
    // RTP
    uint8_t rtp[12 + 188 * 7] = {0x80, 33, 0x12, 0x34}; memcpy(rtp + 12, makeTs(7).data(), 188 * 7);
    const uint8_t* pl; size_t n; uint16_t seq = 0;
    CHECK(rtpPayload(rtp, sizeof rtp, pl, n, &seq) && n == 188 * 7 && seq == 0x1234 && pl[0] == 0x47, "rtp");
    const std::string raw = makeTs(7);
    CHECK(!rtpPayload((const uint8_t*)raw.data(), raw.size(), pl, n), "raw ts is not rtp");
}

static StreamInfo waitState(Stream& s, const char* st, double sec) {
    waitFor([&] { return s.info().state == st; }, sec);
    return s.info();
}

static void testStreams() {
    std::printf("  stage: %s\n", "raw TS over HTTP");
    // ---- raw TS over HTTP, chunked, behind a redirect, with header checks ----
    {
        Server sv;
        const std::string ts = makeTs(300);
        sv.handler = [&](const Req& r) { Resp s;
            if (r.path == "/go") { s.status = 302; s.location = "/live.ts"; return s; }
            if (r.path == "/live.ts") { s.type = "video/mp2t"; s.body = ts; s.chunked = true; s.drop = false; CHECK(r.ua == "TestUA" && r.referer == "http://ref/", "headers ua='%s' ref='%s'", r.ua.c_str(), r.referer.c_str()); return s; }
            s.status = 404; return s; };
        std::atomic<size_t> pk{0}; std::atomic<bool> bad{false};
        Stream st; StreamOptions o; o.userAgent = "TestUA"; o.referer = "http://ref/"; o.maxRetries = 0;
        st.start(sv.url("/go"), o, [&](const uint8_t* p, size_t n) { if (p[0] != 0x47) bad = true; pk += n; });
        waitFor([&] { return pk >= 300; }, 5);
        CHECK(pk == 300 && !bad, "raw ts packets %zu", pk.load());
        CHECK(st.info().kind == "MPEG-TS over HTTP", "kind %s", st.info().kind.c_str());
        st.stop();
    }
    std::printf("  stage: %s\n", "errors: 403");
    // ---- errors: 403 / 404 give the real reason and are retried; a web page is reported ----
    {
        Server sv;
        sv.handler = [&](const Req& r) { Resp s; if (r.path == "/403") { s.status = 403; return s; } if (r.path == "/html") { s.type = "text/html"; s.body = "<html>hello</html>"; return s; } s.status = 404; return s; };
        Stream st; StreamOptions o; o.maxRetries = 1; o.retryBaseSec = 0.05;
        st.start(sv.url("/403"), o, [](const uint8_t*, size_t) {});
        StreamInfo i = waitState(st, "error", 5);
        CHECK(i.state == "error" && i.message.find("403 Forbidden") != std::string::npos && i.reconnects >= 1, "403: %s '%s' reconnects %d", i.state.c_str(), i.message.c_str(), i.reconnects);
        st.stop();
        st.start(sv.url("/html"), o, [](const uint8_t*, size_t) {});
        i = waitState(st, "error", 5);
        CHECK(i.message.find("web page") != std::string::npos, "html: '%s'", i.message.c_str());
        st.stop();
        st.start(sv.url("/nope"), o, [](const uint8_t*, size_t) {});
        i = waitState(st, "error", 5);
        CHECK(i.message.find("404") != std::string::npos, "404: '%s'", i.message.c_str());
        st.stop();
        st.start("http://127.0.0.1:1/x", o, [](const uint8_t*, size_t) {});
        i = waitState(st, "error", 8);
        CHECK(i.state == "error" && !i.message.empty(), "refused: '%s'", i.message.c_str());
        st.stop();
        st.start("rtsp://h/x", o, [](const uint8_t*, size_t) {});
        i = waitState(st, "error", 3);
        CHECK(i.message.find("RTSP") != std::string::npos, "rtsp message");
        st.stop();
    }
    std::printf("  stage: %s\n", "reconnect: the server");
    // ---- reconnect: the server drops the first two connections ----
    {
        Server sv;
        const std::string ts = makeTs(100);
        sv.handler = [&](const Req&) { Resp s; if (sv.hits <= 2) { s.drop = true; return s; } s.type = "video/mp2t"; s.body = ts; return s; };
        std::atomic<size_t> pk{0};
        Stream st; StreamOptions o; o.retryBaseSec = 0.05; o.retryMaxSec = 0.1;
        st.start(sv.url("/s"), o, [&](const uint8_t*, size_t n) { pk += n; });
        waitFor([&] { return pk >= 100; }, 10);
        CHECK(pk >= 100 && st.info().reconnects >= 2, "reconnect: packets %zu reconnects %d state %s msg %s", pk.load(), st.info().reconnects, st.info().state.c_str(), st.info().message.c_str());
        st.stop();
    }
    std::printf("  stage: %s\n", "HLS: master");
    // ---- HLS: master with two bitrates, live media playlist that grows, segments with a 404 hole, AES-128 variant ----
    {
        Server sv;
        std::mutex mu; int seqNow = 0; std::atomic<int> lowHits{0}, hiHits{0};
        auto media = [&](const char* tag, int n, bool vod, bool aes) {
            std::string m = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:" + std::to_string(vod ? 0 : n) + "\n";
            if (aes) m += "#EXT-X-KEY:METHOD=AES-128,URI=\"/key.bin\"\n";
            for (int i = 0; i < 4; i++) m += "#EXTINF:1.0,\n" + std::string(tag) + std::to_string((vod ? 0 : n) + i) + ".ts\n";
            if (vod) m += "#EXT-X-ENDLIST\n";
            return m;
        };
        static const uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        sv.handler = [&](const Req& r) { Resp s; std::lock_guard<std::mutex> l(mu);
            if (r.path == "/master.m3u8") { s.type = "application/vnd.apple.mpegurl"; s.body = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000,RESOLUTION=640x360\nlow.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1280x720\nhi.m3u8\n"; return s; }
            if (r.path == "/low.m3u8") { s.body = media("low", seqNow, false, false); return s; }
            if (r.path == "/hi.m3u8") { s.body = media("hi", seqNow, false, false); return s; }
            if (r.path == "/vod.m3u8") { s.body = media("v", 0, true, false); return s; }
            if (r.path == "/enc.m3u8") { s.body = media("e", 0, true, true); return s; }
            if (r.path == "/drm.m3u8") { s.body = "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://k\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n#EXTINF:1,\na.ts\n"; return s; }
            if (r.path == "/key.bin") { s.body.assign((const char*)key, 16); return s; }
            if (r.path.rfind("/e", 0) == 0 && r.path.find(".ts") != std::string::npos) {   // encrypted segment e<seq>.ts
                const int seq = atoi(r.path.c_str() + 2);
                std::string plain = makeTs(10, 0x100, 0);
                std::vector<uint8_t> enc(plain.begin(), plain.end()); const uint8_t pad = (uint8_t)(16 - enc.size() % 16); enc.insert(enc.end(), pad, pad);
                uint8_t iv[16] = {0}; for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)((uint64_t)seq >> (8 * i));
                Aes128 a(key); uint8_t prev[16]; memcpy(prev, iv, 16);
                for (size_t i = 0; i < enc.size(); i += 16) { for (int k = 0; k < 16; k++) enc[i + k] ^= prev[k]; a.encryptBlock(&enc[i], &enc[i]); memcpy(prev, &enc[i], 16); }
                s.body.assign((const char*)enc.data(), enc.size()); return s; }
            if (r.path.find(".ts") != std::string::npos) {
                if (r.path == "/low2.ts") { s.status = 404; return s; }   // a hole
                if (r.path[1] == 'l') lowHits++; else hiHits++;
                s.type = "video/mp2t"; s.body = makeTs(20); return s; }
            s.status = 404; return s; };
        // live: playlist advances once a second
        std::atomic<bool> adv{true};
        std::thread adv_t([&] { while (adv) { std::this_thread::sleep_for(std::chrono::milliseconds(700)); std::lock_guard<std::mutex> l(mu); seqNow++; } });
        std::atomic<size_t> pk{0};
        Stream st; StreamOptions o; o.maxRetries = 0; o.forcedVariant = 0;
        st.start(sv.url("/master.m3u8"), o, [&](const uint8_t*, size_t n) { pk += n; });
        waitFor([&] { return pk >= 60; }, 15);
        StreamInfo i = st.info();
        CHECK(pk >= 60, "hls live packets %zu state %s msg '%s'", pk.load(), i.state.c_str(), i.message.c_str());
        CHECK(i.kind == "HLS" && i.variantCount == 2 && i.variant == 0 && i.vh == 360 && i.bandwidth == 900000, "hls info kind=%s n=%d v=%d vh=%d", i.kind.c_str(), i.variantCount, i.variant, i.vh);
        st.setVariant(1);
        const int before = hiHits;
        waitFor([&] { return hiHits > before + 1; }, 10);
        CHECK(hiHits > before, "switched to the high variant (hits %d)", hiHits.load());
        st.stop();
        adv = false; adv_t.join();
        // VOD ends
        pk = 0; o.forcedVariant = -1;
        st.start(sv.url("/vod.m3u8"), o, [&](const uint8_t*, size_t n) { pk += n; });
        i = waitState(st, "ended", 20);
        CHECK(i.state == "ended" && pk == 80, "vod: state %s packets %zu msg '%s'", i.state.c_str(), pk.load(), i.message.c_str());
        st.stop();
        // AES-128
        pk = 0;
        st.start(sv.url("/enc.m3u8"), o, [&](const uint8_t* p, size_t n) { if (p[0] == 0x47) pk += n; });
        i = waitState(st, "ended", 20);
        CHECK(pk == 40, "aes-128 stream decrypted: packets %zu state %s msg '%s'", pk.load(), i.state.c_str(), i.message.c_str());
        st.stop();
        // DRM refused
        st.start(sv.url("/drm.m3u8"), o, [](const uint8_t*, size_t) {});
        i = waitState(st, "error", 5);
        CHECK(i.state == "error" && i.drm && i.message.find("DRM") != std::string::npos, "drm refused: '%s'", i.message.c_str());
        st.stop();
    }
    std::printf("  stage: %s\n", "UDP unicast");
    // ---- UDP unicast ----
    {
        netInit();
        sock_t tx = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to{}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(38271);
        std::atomic<size_t> pk{0};
        Stream st; StreamOptions o; o.maxRetries = 0;
        st.start("udp://127.0.0.1:38271", o, [&](const uint8_t*, size_t n) { pk += n; });
        const std::string ts = makeTs(7);
        for (int k = 0; k < 100 && pk < 70; k++) { sockSendTo(tx, ts.data(), ts.size(), to); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(pk >= 70, "udp packets %zu (%s)", pk.load(), st.info().message.c_str());
        st.stop(); sockClose(tx);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::printf("urls\n"); testUrls(); std::printf("m3u\n"); testM3u(); std::printf("xmltv\n"); testXmltv(); std::printf("hls\n"); testHls(); testAes(); testAligner(); std::printf("streams\n"); testStreams();
    std::printf("%s: %d failure(s)\n", fails ? "test_iptv FAILED" : "test_iptv OK", fails);
    return fails ? 1 : 0;
}
