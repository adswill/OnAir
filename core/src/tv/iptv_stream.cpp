// Internet TV: the stream reader. Opens an http(s) MPEG-TS stream, an HLS playlist (live or VOD, several bitrates, AES-128) or a UDP / RTP
// (multicast) source, reconnects by itself and hands 188-byte transport stream packets to a sink (the player). See dect2/tv/iptv.h, docs/IPTV.md.
#include "dect2/tv/iptv.h"
#include "iptv_aes.h"
#include "dect2/common/thread_name.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>

namespace dect2::iptv {

using Clock = std::chrono::steady_clock;
static double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

struct Stream::Impl {
    std::string url;
    StreamOptions opt;
    Sink sink;
    std::thread th, pacer;
    std::atomic<bool> stop{false};
    mutable std::mutex mu;
    StreamInfo inf;
    std::atomic<int> forced{-1};

    // paced delivery of HLS segments
    struct Seg { std::vector<uint8_t> data; double dur; bool disc; };
    std::deque<Seg> q;
    std::condition_variable cv;
    double qDur = 0;
    std::atomic<bool> outBusy{false};   // a segment has been taken from the queue and is still being delivered

    void set(const char* state, const std::string& msg) { std::lock_guard<std::mutex> l(mu); inf.state = state; inf.message = msg; }
    void setKind(const std::string& k) { std::lock_guard<std::mutex> l(mu); inf.kind = k; }

    HttpOptions httpOpts() const {
        HttpOptions h; h.userAgent = opt.userAgent; h.referer = opt.referer; h.headers = opt.headers; h.insecureTls = opt.insecureTls; h.cancel = const_cast<std::atomic<bool>*>(&stop);
        return h;
    }
    bool sleepFor(double s) {   // interruptible; false when stopping
        auto end = Clock::now() + std::chrono::duration<double>(s);
        while (Clock::now() < end) { if (stop) return false; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        return !stop;
    }

    // ---- delivery of queued segments at the speed of their durations ----
    void pacerLoop() {
        double t0 = 0;       // wall time at which the current stream position was due
        bool primed = false;
        while (!stop) {
            Seg s;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait_for(l, std::chrono::milliseconds(200), [&] { return stop.load() || !q.empty(); });
                if (!stop && opt.alive) { l.unlock(); const bool keep = opt.alive(); l.lock(); if (!keep) stop = true; }
                if (stop) return;
                if (q.empty()) { primed = false; continue; }
                s = std::move(q.front()); q.pop_front();
                outBusy = true;
            }
            const double dur = s.dur > 0 ? s.dur : 6.0;
            const double now = nowSec();
            if (!primed || now > t0 + 1.0) { t0 = now; primed = true; }   // after a stall, restart the clock instead of bursting
            const size_t pk = s.data.size() / 188;
            const size_t chunk = 56;
            TsAligner al([&](const uint8_t* d, size_t n) { sink(d, n / 188); });
            for (size_t i = 0; i < pk && !stop; i += chunk) {
                const size_t k = std::min(chunk, pk - i);
                const double due = t0 + dur * (double)i / (double)std::max<size_t>(pk, 1);
                while (!stop) { const double w = due - nowSec(); if (w <= 0) break; std::this_thread::sleep_for(std::chrono::milliseconds((int)std::min(w * 1000.0, 50.0) + 1)); }
                if (stop) return;
                sink(s.data.data() + i * 188, k);
            }
            t0 += dur;
            outBusy = false;
            std::lock_guard<std::mutex> l(mu);
            qDur = std::max(0.0, qDur - s.dur);
            inf.bufferedSec = qDur;
        }
    }

    void enqueue(std::vector<uint8_t>&& data, double dur, bool disc) {
        // keep the queue from growing without bound: wait while more than ~30 s are waiting
        for (;;) {
            { std::lock_guard<std::mutex> l(mu); if (qDur < 30.0 || stop) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::lock_guard<std::mutex> l(mu);
        qDur += dur; inf.bufferedSec = qDur;
        q.push_back(Seg{std::move(data), dur, disc});
        cv.notify_all();
    }

    // ---- raw transport stream over a connection ----
    // returns the reason the stream ended (empty = stopped by the user)
    std::string pumpTs(HttpBody& body, std::string head) {
        TsAligner al([&](const uint8_t* d, size_t n) { sink(d, n / 188); });
        if (!head.empty()) al.feed((const uint8_t*)head.data(), head.size());
        std::vector<uint8_t> buf(64 * 1024);
        set("playing", "");
        for (;;) {
            if (stop) return "";
            const long k = body.read(buf.data(), buf.size());
            if (k == 0) return "the server ended the stream";
            if (k == -2 || stop) return "";
            if (k < 0) return "no data from the server for " + std::to_string(15) + " seconds";
            al.feed(buf.data(), (size_t)k);
        }
    }

    // ---- UDP / RTP ----
    std::string runUdp(const Url& u, bool rtpScheme) {
        netInit();
        sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kBadSock) return "could not create a UDP socket";
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&one, sizeof one);
#endif
        int rcv = 4 << 20; setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcv, sizeof rcv);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)u.port);
        in_addr grp{}; bool multicast = false;
        if (inet_pton(AF_INET, u.host.c_str(), &grp) == 1) multicast = (ntohl(grp.s_addr) >> 28) == 0xE;
        else if (!u.host.empty() && u.host != "@" ) {
            addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
            if (getaddrinfo(u.host.c_str(), nullptr, &hints, &res) == 0 && res) { grp = ((sockaddr_in*)res->ai_addr)->sin_addr; multicast = (ntohl(grp.s_addr) >> 28) == 0xE; freeaddrinfo(res); }
            else { sockClose(s); return "cannot find the address " + u.host; }
        }
        a.sin_addr.s_addr = multicast ? htonl(INADDR_ANY) : (u.host.empty() || u.host == "@" || ntohl(grp.s_addr) == 0 ? htonl(INADDR_ANY) : grp.s_addr);
        if (bind(s, (sockaddr*)&a, sizeof a) != 0) { sockClose(s); return "cannot listen on UDP port " + std::to_string(u.port) + " (is another program using it?)"; }
        if (multicast) {
            ip_mreq m{}; m.imr_multiaddr = grp;
            if (!opt.multicastIface.empty()) inet_pton(AF_INET, opt.multicastIface.c_str(), &m.imr_interface); else m.imr_interface.s_addr = htonl(INADDR_ANY);
            if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&m, sizeof m) != 0) { sockClose(s); return "could not join the multicast group " + u.host + " (check the network adapter, or set the interface under Advanced)"; }
        }
        sockRecvTimeout(s, 500);
        setKind(std::string(rtpScheme ? "RTP" : "UDP") + (multicast ? " multicast" : ""));
        set("buffering", "waiting for data on " + u.host + ":" + std::to_string(u.port));
        TsAligner al([&](const uint8_t* d, size_t n) { sink(d, n / 188); });
        std::vector<uint8_t> buf(65536);
        double lastData = nowSec();
        bool first = true;
        std::string err;
        for (;;) {
            if (stop) break;
            const long long k = sockRecv(s, buf.data(), buf.size());
            if (k <= 0) { if (nowSec() - lastData > 10) { err = "no data arrived for 10 seconds on " + u.host + ":" + std::to_string(u.port); break; } continue; }
            lastData = nowSec();
            const uint8_t* p = buf.data(); size_t len = (size_t)k;
            const uint8_t* pl; size_t pn;
            if (p[0] != 0x47 && rtpPayload(p, len, pl, pn)) { p = pl; len = pn; }
            if (first) { first = false; set("playing", ""); }
            al.feed(p, len);
        }
        if (multicast) { ip_mreq m{}; m.imr_multiaddr = grp; m.imr_interface.s_addr = htonl(INADDR_ANY); setsockopt(s, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&m, sizeof m); }
        sockClose(s);
        return err;
    }

    // ---- HLS ----
    bool fetchText(const std::string& u, std::string& body, std::string& err) {
        HttpResult r;
        if (!httpGetAll(u, httpOpts(), body, r, 16u << 20)) { err = r.error; return false; }
        return true;
    }

    std::string runHls(const std::string& startUrl, const std::string& firstBody) {
        setKind("HLS");
        std::string mediaUrl = startUrl;
        std::string text = firstBody, err;
        HlsPlaylist pl = parseHls(text, startUrl);
        if (!pl.ok) return pl.error;
        std::vector<HlsVariant> vars;
        size_t cur = 0;
        if (pl.master) {
            vars = pl.variants;
            std::stable_sort(vars.begin(), vars.end(), [](const HlsVariant& a, const HlsVariant& b) { return a.bandwidth < b.bandwidth; });
            if (vars.empty()) return "the playlist lists no video streams";
            std::vector<std::string> at, st;
            for (auto& m : pl.media) {
                const std::string nm = m.name + (m.lang.empty() ? "" : " (" + m.lang + ")");
                if (m.type == "AUDIO") at.push_back(nm); else if (m.type == "SUBTITLES" || m.type == "CLOSED-CAPTIONS") st.push_back(nm);
            }
            { std::lock_guard<std::mutex> l(mu); inf.variantCount = (int)vars.size(); inf.audioTracks = at; inf.subtitleTracks = st; }
            // start in the middle: quick to start, then adapt from the measured speed
            cur = forced >= 0 ? std::min((size_t)forced.load(), vars.size() - 1) : (vars.size() - 1) / 2;
            if (forced < 0) { for (size_t i = 0; i < vars.size(); i++) if (vars[i].bandwidth <= 2500000) cur = i; }
        }
        double thr = 0;       // smoothed throughput, bits/s
        int64_t lastSeq = -1;
        bool started = false;
        std::string keyUriCached; uint8_t key[16]; bool haveKey = false;
        double lastChange = nowSec();
        int consecFail = 0;
        size_t lastCount = 0; int64_t lastFirst = -1;
        for (;;) {
            if (stop) return "";
            if (pl.master) {
                const int f = forced;
                cur = pickVariant(vars, thr, f, cur);
                const HlsVariant& v = vars[cur];
                mediaUrl = v.uri;
                { std::lock_guard<std::mutex> l(mu); inf.variant = (int)cur; inf.bandwidth = v.bandwidth; inf.vw = v.w; inf.vh = v.h; }
                if (!fetchText(mediaUrl, text, err)) return "could not load the stream's playlist: " + err;
                HlsPlaylist m = parseHls(text, mediaUrl);
                if (!m.ok) return m.error;
                if (m.master) return "the playlist points to another playlist of playlists, which OnAir does not follow";
                pl = m; pl.master = true;   // keep the "master" flag for the loop; segments are in pl
                // (variants stay in `vars`)
            }
            if (!pl.keyMethod.empty() && pl.keyMethod != "NONE" && pl.keyMethod != "AES-128") {
                std::lock_guard<std::mutex> l(mu); inf.drm = true;
                return pl.keyMethod.rfind("DRM:", 0) == 0 || pl.keyMethod == "SAMPLE-AES" || pl.keyMethod == "SAMPLE-AES-CTR" || pl.keyMethod == "SAMPLE-AES-CENC"
                    ? "this stream is protected by DRM (" + pl.keyMethod + "). OnAir does not open DRM protected streams."
                    : "unknown encryption method " + pl.keyMethod;
            }
            if (pl.fmp4) return "this stream uses fragmented MP4 segments (CMAF), which OnAir cannot play yet. Streams with MPEG-TS segments work.";
            if (pl.segments.empty()) {
                if (pl.endList) return "";
            }
            // new segments
            std::vector<HlsSegment> todo;
            for (auto& s : pl.segments) if (s.seq > lastSeq) todo.push_back(s);
            if (!started && pl.live && opt.liveEdgeStart && todo.size() > 3) todo.erase(todo.begin(), todo.end() - 3);
            if (lastSeq >= 0 && !todo.empty() && todo.front().seq > lastSeq + 1 && !pl.segments.empty()) { /* we fell behind: segments are gone, carry on with what exists */ }
            if (lastSeq >= 0 && pl.segments.size() && pl.segments.back().seq < lastSeq) lastSeq = pl.segments.front().seq - 1;   // the stream restarted its numbering
            if (!todo.empty()) { lastChange = nowSec(); }
            for (auto& sg : todo) {
                if (stop) return "";
                // key
                std::vector<uint8_t> iv(16, 0);
                bool enc = pl.keyMethod == "AES-128" && !pl.keyUri.empty();
                if (enc) {
                    if (!haveKey || keyUriCached != pl.keyUri) {
                        std::string kb;
                        if (!fetchText(pl.keyUri, kb, err)) return "could not load the AES-128 key: " + err;
                        if (kb.size() != 16) return "the AES-128 key is not 16 bytes (the key server may need a login)";
                        memcpy(key, kb.data(), 16); keyUriCached = pl.keyUri; haveKey = true;
                    }
                    // explicit IV not parsed here: the sequence number is the default per RFC 8216
                    for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)((uint64_t)sg.seq >> (8 * i));
                }
                const double t0 = nowSec();
                HttpResult r;
                std::string body;
                HttpOptions ho = httpOpts();
                if (sg.byteOff >= 0) ho.headers.emplace_back("Range", "bytes=" + std::to_string(sg.byteOff) + "-" + std::to_string(sg.byteOff + sg.byteLen - 1));
                if (!httpGetAll(sg.uri, ho, body, r, 256u << 20)) {
                    if (stop) return "";
                    consecFail++;
                    lastSeq = sg.seq;   // skip it: a missing segment must not stop the picture
                    set("buffering", "segment " + std::to_string(sg.seq) + " failed: " + r.error);
                    if (consecFail >= 5) return "segments keep failing: " + r.error;
                    continue;
                }
                consecFail = 0;
                const double dt = std::max(0.001, nowSec() - t0);
                const double bps = (double)body.size() * 8.0 / dt;
                thr = thr <= 0 ? bps : 0.6 * thr + 0.4 * bps;
                { std::lock_guard<std::mutex> l(mu); inf.measuredBps = thr; }
                std::vector<uint8_t> data(body.begin(), body.end());
                body.clear(); body.shrink_to_fit();
                if (enc && !aes128CbcDecrypt(key, iv.data(), data)) return "could not decrypt a segment (wrong key or damaged data)";
                if (data.empty() || (data[0] != 0x47 && data.size() > 188 && data[188] != 0x47)) {
                    // not a transport stream segment: ID3-tagged AAC, MP4, WebVTT ...
                    if (data.size() >= 8 && memcmp(&data[4], "ftyp", 4) == 0) return "this stream uses MP4 segments, which OnAir cannot play yet. Streams with MPEG-TS segments work.";
                    if (data.size() > 188 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') return "this is an audio-only stream (AAC/MP3 segments); OnAir plays video streams with MPEG-TS segments.";
                    if (data.size() > 188) return "the segments are not MPEG-TS (the first bytes do not look like a transport stream)";
                }
                data.resize(data.size() / 188 * 188);
                lastSeq = sg.seq;
                if (!started) { started = true; set("playing", ""); }
                enqueue(std::move(data), sg.dur > 0 ? sg.dur : pl.targetDuration, sg.discontinuity);
                if (pl.master && forced < 0) {   // look again at the speed between segments
                    const size_t n = pickVariant(vars, thr, -1, cur);
                    if (n != cur) break;
                }
                if (forced.load() != -2 && pl.master && (int)cur != (forced >= 0 ? forced.load() : (int)cur)) break;
            }
            if (!pl.live) {
                // VOD: wait for the queue to drain, then end
                bool more = false;
                for (auto& s : pl.segments) if (s.seq > lastSeq) more = true;
                if (!more) {
                    for (;;) { { std::lock_guard<std::mutex> l(mu); if (q.empty() && !outBusy) break; } if (!sleepFor(0.2)) return ""; }
                    return "the stream has ended";
                }
                continue;
            }
            // live: wait, then reload
            if (!pl.master || true) {
                const size_t cnt = pl.segments.size(); const int64_t first = pl.segments.empty() ? -1 : pl.segments.front().seq;
                if (cnt == lastCount && first == lastFirst && todo.empty()) {
                    if (nowSec() - lastChange > std::max(30.0, pl.targetDuration * 5)) return "the playlist has not changed for " + std::to_string((int)(nowSec() - lastChange)) + " seconds (the stream seems to have stopped)";
                }
                lastCount = cnt; lastFirst = first;
            }
            if (!sleepFor(todo.empty() ? std::max(0.5, pl.targetDuration / 2) : 0.05)) return "";
            if (!pl.master) {
                if (!fetchText(mediaUrl, text, err)) return "could not reload the playlist: " + err;
                HlsPlaylist m = parseHls(text, mediaUrl);
                if (!m.ok) return m.error;
                pl = m;
            }
            if (pl.endList) { /* loop once more to take the last segments, then the !live branch ends */ }
        }
    }

    // ---- one attempt ----
    std::string once() {
        set("connecting", "connecting");
        const Url u = parseUrl(url);
        if (!u.ok) return "this is not a valid address: " + redactUrl(url) + " (use http://, https://, udp:// or rtp://)";
        if (u.scheme == "udp" || u.scheme == "rtp") return runUdp(u, u.scheme == "rtp");
        if (u.scheme == "rtsp" || u.scheme == "rtsps") return "RTSP streams are not supported yet in this version.";
        if (u.scheme != "http" && u.scheme != "https") return "OnAir cannot open " + u.scheme + ":// streams (http, https, udp and rtp work)";
        HttpResult r;
        auto body = httpOpen(url, httpOpts(), r);
        if (!body) return stop ? std::string() : r.error;
        std::string head;
        char buf[4096];
        long k = body->read(buf, sizeof buf);
        if (k <= 0) return stop ? std::string() : "the server sent no data";
        head.assign(buf, (size_t)k);
        // sniff
        size_t b = 0;
        if (head.size() >= 3 && (uint8_t)head[0] == 0xEF) b = 3;
        while (b < head.size() && std::isspace((unsigned char)head[b])) b++;
        const std::string ct = r.contentType;
        if (head.compare(b, 7, "#EXTM3U") == 0) {
            // read the rest of the playlist
            std::string all = head;
            for (;;) { const long n = body->read(buf, sizeof buf); if (n <= 0) break; all.append(buf, (size_t)n); if (all.size() > (16u << 20)) break; }
            return runHls(r.finalUrl.empty() ? url : r.finalUrl, all);
        }
        if (head.compare(b, 4, "<?xm") == 0 || head.compare(b, 4, "<MPD") == 0 || ct.find("dash+xml") != std::string::npos) return "this is a DASH (.mpd) stream, which OnAir cannot play yet. HLS (.m3u8) and MPEG-TS streams work.";
        if (head.size() >= 8 && memcmp(head.data() + 4, "ftyp", 4) == 0) return "this is an MP4 file or fragmented MP4 stream, which OnAir cannot play yet.";
        bool ts = false;
        for (size_t i = 0; i + 188 < head.size() && i < 400; i++) if ((uint8_t)head[i] == 0x47 && (uint8_t)head[i + 188] == 0x47) { ts = true; break; }
        if (!ts && head.size() < 377) ts = head.find((char)0x47) != std::string::npos;   // too short to tell: try
        if (!ts) {
            if (ct.find("text/html") != std::string::npos) return "the address is a web page, not a stream (the server sent HTML)";
            return "the data is not an MPEG transport stream or an HLS playlist (content type " + (ct.empty() ? std::string("unknown") : ct) + ")";
        }
        setKind(r.contentLength < 0 ? "MPEG-TS over HTTP" : "MPEG-TS file over HTTP");
        return pumpTs(*body, head);
    }

    void run() {
        double back = opt.retryBaseSec;
        int tries = 0;
        while (!stop) {
            const double started = nowSec();
            const std::string why = once();
            if (stop) break;
            // fatal reasons: no point retrying
            if (why.find("DRM") != std::string::npos || why.find("cannot play yet") != std::string::npos || why.find("not supported yet") != std::string::npos ||
                why.find("not a valid address") != std::string::npos || why.find("not a stream") != std::string::npos || why.find("is not an MPEG") != std::string::npos ||
                why.find("audio-only") != std::string::npos || why.find("cannot open ") != std::string::npos || why.find("is a web page") != std::string::npos ||
                why.find("is not an HLS") != std::string::npos || why.find("no TLS support") != std::string::npos) {
                set("error", why);
                break;
            }
            if (why == "the stream has ended") { set("ended", why); break; }
            if (nowSec() - started > 30) { back = opt.retryBaseSec; tries = 0; }   // it ran for a while: start again quickly
            tries++;
            if (opt.maxRetries >= 0 && tries > opt.maxRetries) { set("error", why); break; }
            { std::lock_guard<std::mutex> l(mu); inf.reconnects++; }
            char b[64]; snprintf(b, sizeof b, "%.0f", back);
            set("reconnecting", (why.empty() ? std::string("the connection was lost") : why) + ". Trying again in " + b + " s (attempt " + std::to_string(tries) + ")");
            if (!sleepFor(back)) break;
            back = std::min(opt.retryMaxSec, back * 1.8);
        }
        if (stop) set("stopped", "");
    }
};

Stream::Stream() : p_(new Impl) {}
Stream::~Stream() { stop(); }

void Stream::start(const std::string& url, const StreamOptions& o, Sink sink) {
    stop();
    p_.reset(new Impl);
    p_->url = url; p_->opt = o; p_->sink = std::move(sink); p_->forced = o.forcedVariant;
    p_->inf = StreamInfo();
    p_->inf.state = "connecting";
    run_ = true;
    Impl* im = p_.get();
    im->pacer = namedThread("onair-iptv-out", [im] { im->pacerLoop(); });
    im->th = namedThread("onair-iptv", [im, this] { im->run(); run_ = false; });
}

void Stream::stop() {
    if (!p_) return;
    p_->stop = true;
    p_->cv.notify_all();
    if (p_->th.joinable()) p_->th.join();
    if (p_->pacer.joinable()) p_->pacer.join();
    run_ = false;
}

StreamInfo Stream::info() const { std::lock_guard<std::mutex> l(p_->mu); return p_->inf; }
void Stream::setVariant(int idx) { p_->forced = idx; }

} // namespace dect2::iptv
