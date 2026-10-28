// Internet TV (IPTV): OnAir only plays; it ships no channel lists. This header holds
//  - the playlist and guide parsers (M3U / M3U8 with #EXTINF attributes, XMLTV) and URL handling,
//  - the HLS playlist parser,
//  - the HTTP(S) client and the stream reader that turns an internet stream into MPEG transport stream packets for the player.
// The bundled FFmpeg has no network code (and must not get any: see docs/IPTV.md); the bytes are fetched here and the player decodes
// the transport stream exactly as it does for an antenna.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2::iptv {

// ---- URLs ----
struct Url {
    std::string scheme, host, path = "/", user;   // path includes the query; host without brackets
    int port = 0;
    bool ok = false;
    bool ipv6 = false;
};
Url parseUrl(const std::string& s);
std::string resolveUrl(const std::string& base, const std::string& ref);   // RFC 3986-ish: absolute, //host, /path, relative, ../
std::string trim(const std::string& s);
bool looksLikeStreamUrl(const std::string& s);   // http(s), udp, rtp, rtsp, mms, file-less schemes the player knows
std::string redactUrl(const std::string& s);      // user:password@ and token-like query values removed, for logs and the screen

// ---- playlist ----
struct Channel {
    std::string name, url, logo, group, tvgId, tvgName, userAgent, referer;
    std::vector<std::pair<std::string, std::string>> headers;   // from #EXTVLCOPT / #EXTHTTP
    bool drm = false;        // #KODIPROP license_type / license_key: the stream is DRM protected, OnAir will not open it
    std::string country, language;
};
struct Playlist {
    std::vector<Channel> channels;
    std::string guideUrl;     // url-tvg / x-tvg-url of the #EXTM3U line
    bool isHlsStream = false; // the file is an HLS playlist itself (one stream), channels holds that one entry
    size_t duplicatesDropped = 0;
};
// base resolves relative stream URLs (the playlist's own URL or folder). Tolerates BOM, CR LF, CR, odd quoting, missing #EXTM3U.
Playlist parseM3u(const std::string& text, const std::string& base = std::string());
std::string channelKey(const Channel& c);   // stable id for favourites: tvg-id if present, else the URL without query

// ---- programme guide (XMLTV) ----
struct Programme { int64_t start = 0, stop = 0; std::string title, desc, category; };   // unix seconds UTC
struct Guide {
    std::map<std::string, std::vector<Programme>> byChannel;   // key: XMLTV channel id, programmes sorted by start
    std::map<std::string, std::string> names;                   // channel id -> display name
    size_t programmes = 0;
};
Guide parseXmltv(const std::string& xml);
int64_t parseXmltvTime(const std::string& s);   // "20240131203000 +0200" -> unix seconds; 0 when invalid
// the programme on at `now` for a channel (matched by tvg-id, then by name), and the next; nullptr when unknown
const Programme* guideNow(const Guide& g, const Channel& c, int64_t now, const Programme** next = nullptr);

// ---- HLS ----
struct HlsVariant { std::string uri; int64_t bandwidth = 0; int w = 0, h = 0; std::string codecs, audioGroup, subGroup; double fps = 0; };
struct HlsMedia { std::string type, group, name, lang, uri; bool isDefault = false; };
struct HlsSegment { std::string uri; double dur = 0; int64_t seq = 0; bool discontinuity = false; int64_t byteOff = -1, byteLen = 0; };
struct HlsPlaylist {
    bool ok = false, master = false, endList = false, live = true;
    std::vector<HlsVariant> variants;
    std::vector<HlsMedia> media;
    std::vector<HlsSegment> segments;
    int64_t mediaSequence = 0;
    double targetDuration = 6;
    std::string keyMethod, keyUri;     // #EXT-X-KEY: NONE, AES-128 (not supported), SAMPLE-AES / DRM (refused)
    bool fmp4 = false;                 // #EXT-X-MAP: fragmented MP4 segments (not supported yet)
    std::string error;
};
HlsPlaylist parseHls(const std::string& text, const std::string& base);
// the variant the adaptive logic picks for a measured throughput (bits/s); idx < 0 = automatic
size_t pickVariant(const std::vector<HlsVariant>& v, double throughputBps, int forcedIdx, size_t current);

// ---- HTTP(S) ----
struct HttpOptions {
    std::string userAgent = "OnAir";
    std::string referer;
    std::vector<std::pair<std::string, std::string>> headers;
    int connectTimeoutMs = 10000, readTimeoutMs = 15000, maxRedirects = 8;
    bool insecureTls = false;   // Advanced: accept a certificate that does not verify (self-signed, expired); off by default, never silent
    std::atomic<bool>* cancel = nullptr;
};
struct HttpResult {
    int status = 0;            // 0 = no response (see error)
    std::string error;         // a human sentence: "could not resolve host", "HTTPS is not available in this build", "403 Forbidden"
    std::string contentType, finalUrl;
    int64_t contentLength = -1;
    std::map<std::string, std::string> headers;   // lower-case names
};
class HttpBody {
public:
    virtual ~HttpBody() {}
    virtual long read(void* buf, size_t n) = 0;    // bytes, 0 at the end, < 0 on error/timeout/cancel
};
std::unique_ptr<HttpBody> httpOpen(const std::string& url, const HttpOptions& o, HttpResult& r);
bool httpGetAll(const std::string& url, const HttpOptions& o, std::string& body, HttpResult& r, size_t maxBytes = 64u << 20);
bool httpsAvailable();    // false when this build can not do TLS (the message to show is in httpsMissingText())
const char* httpsMissingText();
// platform TLS back-ends (cocoa / WinHTTP); nullptr when not built
std::unique_ptr<HttpBody> httpOpenPlatform(const std::string& url, const HttpOptions& o, HttpResult& r, bool& handled);

// ---- the stream ----
struct StreamInfo {
    std::string state;            // "connecting", "buffering", "playing", "reconnecting", "error", "ended", "stopped"
    std::string message;          // what to tell the user, real reason included
    std::string kind;             // "HLS", "MPEG-TS over HTTP", "UDP", "RTP", ...
    int variantCount = 0, variant = -1;
    int64_t bandwidth = 0;        // selected variant (HLS) bits/s
    int vw = 0, vh = 0;           // from the playlist, 0 unknown
    double measuredBps = 0;       // download throughput
    double bufferedSec = 0;
    int reconnects = 0;
    bool drm = false;
    std::vector<std::string> audioTracks, subtitleTracks;
};
struct StreamOptions {
    std::string userAgent = "OnAir", referer;
    std::vector<std::pair<std::string, std::string>> headers;
    int forcedVariant = -1;                 // -1 automatic, otherwise the index in the master playlist (sorted by bandwidth)
    int maxRetries = -1;                    // -1: keep trying until stopped
    double retryBaseSec = 1.0, retryMaxSec = 15.0;
    std::string multicastIface;             // optional local address for the multicast join
    bool liveEdgeStart = true;
    bool insecureTls = false;               // Advanced > Ignore certificate errors
    std::function<bool()> alive;            // polled about 5 times a second from the output thread; false stops the stream (the screen was left). May be empty
};
class Stream {
public:
    using Sink = std::function<void(const uint8_t* pkts, size_t nPackets)>;   // 188-byte aligned
    Stream();
    ~Stream();
    void start(const std::string& url, const StreamOptions& o, Sink sink);
    void stop();
    StreamInfo info() const;
    bool running() const { return run_; }
    void setVariant(int idx);     // -1 automatic
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::atomic<bool> run_{false};
};

// Keeps 188-byte alignment of a byte stream: finds the sync byte, drops junk, holds the tail. Also strips an RTP header when asked.
class TsAligner {
public:
    explicit TsAligner(std::function<void(const uint8_t*, size_t)> out) : out_(std::move(out)) {}
    void feed(const uint8_t* d, size_t n);
    size_t resyncs() const { return resyncs_; }
    void reset() { buf_.clear(); }
private:
    std::function<void(const uint8_t*, size_t)> out_;
    std::vector<uint8_t> buf_;
    size_t resyncs_ = 0;
};

// An RTP packet (RFC 3550) -> its payload (MPEG-TS, payload type 33, or any); false when it is not RTP.
bool rtpPayload(const uint8_t* d, size_t n, const uint8_t*& payload, size_t& len, uint16_t* seq = nullptr);

} // namespace dect2::iptv
