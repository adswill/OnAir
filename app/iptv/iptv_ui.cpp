// Internet TV (IPTV) screen: playlists (M3U / M3U8, a URL or a file), a single stream address, the channel list with logos, groups, search and
// favourites, the programme guide (XMLTV), and the player with its usual controls. OnAir only plays: it ships no channels (docs/IPTV.md).
// The bytes are fetched by dect2::iptv::Stream (HTTP(S), HLS, UDP / RTP) and go to the Player as transport stream packets, the same way a
// recording does (tvrec_ui.cpp).
#include "app.h"
#include "dect2/common/png.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/tv/iptv.h"
#include "dect2/tv/ts.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

using namespace dect2;

namespace {

struct ListRef { std::string name, src; };
struct Logo { gfx::Image* img = nullptr; int w = 0, h = 0; int state = 0; };   // state: 0 not asked, 1 loading, 2 ready, 3 none

struct State {
    bool loaded = false;
    std::vector<ListRef> lists;
    int cur = -1;
    std::vector<iptv::Channel> channels;
    std::vector<int> shown;                        // indices into channels after search / group / favourites
    std::set<std::string> favs;
    std::deque<std::string> recent;
    std::string search, group;
    bool favOnly = false, dirty = true;
    char urlBuf[1024] = "", searchBuf[128] = "", guideBuf[1024] = "", uaBuf[256] = "", refBuf[256] = "", hdrBuf[1024] = "", ifaceBuf[64] = "";
    bool insecure = false;
    int quality = -1;                               // -1 automatic
    iptv::Guide guide;
    std::mutex mu;                                  // guards the fields the loader thread sets
    std::thread loader;
    std::atomic<bool> busy{false};
    std::string msg;                                // result of the last load, shown under the address
    // results from the loader thread
    bool newList = false, playNow = false, newGuide = false;
    ListRef resList; std::vector<iptv::Channel> resChannels; std::string resGuideUrl;
    iptv::Guide resGuide; std::string playUrl, playName;
    // playing
    iptv::Stream stream;
    std::mutex pmu;
    TsDemux demux;
    TsSnapshot snap;
    double snapT = 0;
    bool selected = false;
    std::atomic<double> lastTick{0};
    std::string playing, playingKey, playingName;
    iptv::Channel playingCh;
    bool havePlaying = false;
    // logos
    std::map<std::string, Logo> logos;
    std::mutex lmu;
    std::deque<std::pair<std::string, std::string>> logoQ;   // url, cache path
    std::vector<std::pair<std::string, std::vector<uint8_t>>> logoDone;
    std::thread logoTh;
    std::atomic<bool> logoRun{false};
    std::atomic<int> logoActive{0};
};
State S;

std::string dir() {
    std::error_code ec;
    const std::string d = plat::dataDir() + "/iptv";
    std::filesystem::create_directories(u8file(d), ec);
    return d;
}
std::string hashHex(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}
std::string readAll(const std::string& p) {
    std::ifstream f(u8file(p), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
void writeAll(const std::string& p, const std::string& d) { std::ofstream f(u8file(p), std::ios::binary); f << d; }
int64_t nowUtc() { return (int64_t)std::time(nullptr); }

void saveState() {
    std::string o;
    for (auto& l : S.lists) o += "list\t" + l.name + "\t" + l.src + "\n";
    for (auto& f : S.favs) o += "fav\t" + f + "\n";
    for (auto& r : S.recent) o += "recent\t" + r + "\n";
    o += "cur\t" + std::to_string(S.cur) + "\nguide\t" + S.guideBuf + "\nua\t" + S.uaBuf + "\nref\t" + S.refBuf + "\niface\t" + S.ifaceBuf + "\n";
    std::string h = S.hdrBuf; std::replace(h.begin(), h.end(), '\n', '\x1f');
    o += "hdr\t" + h + "\ninsecure\t" + (S.insecure ? "1" : "0") + "\nurl\t" + S.urlBuf + "\n";
    writeAll(dir() + "/state.tsv", o);
}
void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    std::istringstream in(readAll(dir() + "/state.tsv"));
    std::string line;
    while (std::getline(in, line)) {
        const size_t t = line.find('\t'); if (t == std::string::npos) continue;
        const std::string k = line.substr(0, t), v = line.substr(t + 1);
        if (k == "list") { const size_t t2 = v.find('\t'); if (t2 != std::string::npos) S.lists.push_back({v.substr(0, t2), v.substr(t2 + 1)}); }
        else if (k == "fav") S.favs.insert(v);
        else if (k == "recent") S.recent.push_back(v);
        else if (k == "cur") S.cur = atoi(v.c_str());
        else if (k == "guide") snprintf(S.guideBuf, sizeof S.guideBuf, "%s", v.c_str());
        else if (k == "ua") snprintf(S.uaBuf, sizeof S.uaBuf, "%s", v.c_str());
        else if (k == "ref") snprintf(S.refBuf, sizeof S.refBuf, "%s", v.c_str());
        else if (k == "iface") snprintf(S.ifaceBuf, sizeof S.ifaceBuf, "%s", v.c_str());
        else if (k == "hdr") { std::string h = v; std::replace(h.begin(), h.end(), '\x1f', '\n'); snprintf(S.hdrBuf, sizeof S.hdrBuf, "%s", h.c_str()); }
        else if (k == "insecure") S.insecure = v == "1";
        else if (k == "url") snprintf(S.urlBuf, sizeof S.urlBuf, "%s", v.c_str());
    }
    // the last playlist from its cached copy
    if (S.cur >= 0 && S.cur < (int)S.lists.size()) {
        const std::string c = readAll(dir() + "/pl_" + hashHex(S.lists[S.cur].src) + ".m3u");
        if (!c.empty()) S.channels = iptv::parseM3u(c, S.lists[S.cur].src).channels;
        S.dirty = true;
    }
}

std::vector<std::pair<std::string, std::string>> userHeaders(const char* buf) {
    std::vector<std::pair<std::string, std::string>> h;
    std::istringstream in(buf);
    std::string line;
    while (std::getline(in, line)) {
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = iptv::trim(line.substr(0, c)), v = iptv::trim(line.substr(c + 1));
        if (!k.empty()) h.emplace_back(k, v);
    }
    return h;
}

iptv::HttpOptions httpOpts() {
    iptv::HttpOptions o;
    if (S.uaBuf[0]) o.userAgent = S.uaBuf;
    o.referer = S.refBuf;
    o.headers = userHeaders(S.hdrBuf);
    o.insecureTls = S.insecure;
    return o;
}

// ---- the player side ----
void sink(const uint8_t* p, size_t n, App* a) {
    std::lock_guard<std::mutex> lk(S.pmu);
    for (size_t i = 0; i < n; i++) S.demux.feed(p + i * 188);
    a->engine.feedPlaybackSubtitles(p, n);
    const double t = glfwGetTime();
    bool haveSvc = false;
    for (auto& sv : S.snap.services) if (sv.havePmt) haveSvc = true;
    if (!haveSvc || t - S.snapT > 1.0) { S.snap = S.demux.snapshot(); S.snapT = t; }
    Player& pl = a->engine.player();
    if (!S.selected) {
        int pick = -1;
        for (auto& sv : S.snap.services) if (sv.havePmt) { if (pick < 0) pick = sv.id; }
        if (pick >= 0) { pl.select(pick); pl.setVolume(a->volume); S.selected = true; }
    }
    if (pl.selected() >= 0) pl.push(p, n, S.snap);
}

void stopPlaying(App& a) {
    S.stream.stop();
    std::lock_guard<std::mutex> lk(S.pmu);
    a.engine.player().select(-1);
    a.engine.setPlayerExternal(false);
    a.engine.teletext().reset();
    S.selected = false;
    S.havePlaying = false;
}

void play(App& a, const std::string& url, const iptv::Channel* ch, const std::string& name) {
    iptv::StreamOptions o;
    o.userAgent = S.uaBuf[0] ? S.uaBuf : "OnAir";
    o.referer = S.refBuf;
    o.headers = userHeaders(S.hdrBuf);
    o.insecureTls = S.insecure;
    o.forcedVariant = S.quality;
    o.multicastIface = S.ifaceBuf;
    if (ch) {   // a channel's own options win over the global ones
        if (!ch->userAgent.empty()) o.userAgent = ch->userAgent;
        if (!ch->referer.empty()) o.referer = ch->referer;
        for (auto& h : ch->headers) o.headers.push_back(h);
    }
    if (ch && ch->drm) {
        S.msg = "This channel is marked as DRM protected in the playlist. OnAir does not open DRM protected streams.";
        return;
    }
    stopPlaying(a);
    { std::lock_guard<std::mutex> lk(S.pmu); S.demux.reset(); S.snap = TsSnapshot(); S.snapT = 0; S.selected = false; }
    a.engine.setPlayerExternal(true);
    a.engine.teletext().reset();
    App* ap = &a;
    o.alive = [ap] {
        if (glfwGetTime() - S.lastTick.load() < 2.0) return true;
        ap->engine.player().select(-1);   // the screen was left: give the player back to the antenna
        ap->engine.setPlayerExternal(false);
        return false;
    };
    S.playing = url; S.playingName = name.empty() ? iptv::redactUrl(url) : name;
    S.playingKey = ch ? iptv::channelKey(*ch) : "url:" + url;
    S.playingCh = ch ? *ch : iptv::Channel();
    S.havePlaying = true;
    S.msg.clear();
    if (ch) {
        const std::string k = iptv::channelKey(*ch);
        S.recent.erase(std::remove(S.recent.begin(), S.recent.end(), k), S.recent.end());
        S.recent.push_front(k);
        while (S.recent.size() > 20) S.recent.pop_back();
        saveState();
    }
    S.stream.start(url, o, [ap](const uint8_t* p, size_t n) { sink(p, n, ap); });
}

// ---- loading playlists and guides (off the UI thread) ----
void loadPlaylistAsync(const std::string& srcIn, bool fromFile) {
    if (S.busy) return;
    S.busy = true;
    if (S.loader.joinable()) S.loader.join();
    const iptv::HttpOptions ho = httpOpts();
    const std::string src = srcIn;
    S.loader = namedThread("onair-iptvld", [src, fromFile, ho] {
        std::string body, err, base = src;
        bool directStream = false;
        if (fromFile) {
            body = readAll(src);
            if (body.empty()) err = "cannot read " + src;
            base.clear();
        } else {
            const iptv::Url u = iptv::parseUrl(src);
            if (!u.ok) err = "this is not a valid address";
            else if (u.scheme == "udp" || u.scheme == "rtp" || u.scheme == "rtsp" || u.scheme == "rtsps") directStream = true;
            else {
                iptv::HttpResult r;
                auto b = iptv::httpOpen(src, ho, r);
                if (!b) err = r.error;
                else {
                    char buf[8192];
                    long k = b->read(buf, sizeof buf);
                    if (k <= 0) err = "the server sent no data";
                    else {
                        body.assign(buf, (size_t)k);
                        size_t i = 0; if (body.size() >= 3 && (uint8_t)body[0] == 0xEF) i = 3;
                        while (i < body.size() && std::isspace((unsigned char)body[i])) i++;
                        if (body.compare(i, 7, "#EXTM3U") == 0 || body.compare(i, 5, "<?xml") == 0) {
                            for (;;) { k = b->read(buf, sizeof buf); if (k <= 0) break; body.append(buf, (size_t)k); if (body.size() > (96u << 20)) break; }
                            base = r.finalUrl.empty() ? src : r.finalUrl;
                        } else directStream = true;
                    }
                }
            }
        }
        std::lock_guard<std::mutex> l(S.mu);
        if (!err.empty()) { S.msg = err; S.busy = false; return; }
        if (directStream) { S.playUrl = src; S.playName.clear(); S.playNow = true; S.busy = false; return; }
        iptv::Playlist pl = iptv::parseM3u(body, base);
        if (pl.isHlsStream) { S.playUrl = base.empty() ? src : base; S.playName.clear(); S.playNow = true; S.busy = false; return; }
        if (pl.channels.empty()) { S.msg = "No channels found in this file. It should be an M3U / M3U8 playlist (#EXTM3U ... #EXTINF ... stream address)."; S.busy = false; return; }
        std::string nm = src;
        const size_t sl = nm.find_last_of("/\\"); if (sl != std::string::npos && sl + 1 < nm.size()) nm = nm.substr(sl + 1);
        const size_t q = nm.find('?'); if (q != std::string::npos) nm.resize(q);
        if (nm.empty()) nm = src;
        writeAll(dir() + "/pl_" + hashHex(src) + ".m3u", body);
        S.resList = {nm, src}; S.resChannels = std::move(pl.channels); S.resGuideUrl = pl.guideUrl;
        char b[160]; snprintf(b, sizeof b, "%zu channels loaded%s", S.resChannels.size(), pl.duplicatesDropped ? (", " + std::to_string(pl.duplicatesDropped) + " duplicates left out").c_str() : "");
        S.msg = b;
        S.newList = true; S.busy = false;
    });
}

void loadGuideAsync(const std::string& srcIn) {
    if (S.busy || srcIn.empty()) return;
    S.busy = true;
    if (S.loader.joinable()) S.loader.join();
    const iptv::HttpOptions ho = httpOpts();
    const std::string src = srcIn;
    S.loader = namedThread("onair-iptvgd", [src, ho] {
        std::string body, err;
        const iptv::Url u = iptv::parseUrl(src);
        if (u.ok) { iptv::HttpResult r; if (!iptv::httpGetAll(src, ho, body, r, 256u << 20)) err = r.error; }
        else { body = readAll(src); if (body.empty()) err = "cannot read " + src; }
        if (err.empty() && body.size() > 2 && (uint8_t)body[0] == 0x1f && (uint8_t)body[1] == 0x8b) err = "the guide is compressed (.gz); OnAir cannot unpack it yet, use the uncompressed .xml address";
        iptv::Guide g;
        if (err.empty()) { g = iptv::parseXmltv(body); if (g.programmes == 0) err = "no programmes found in this XMLTV file"; }
        std::lock_guard<std::mutex> l(S.mu);
        if (!err.empty()) S.msg = "Guide: " + err;
        else { S.resGuide = std::move(g); S.newGuide = true; char b[96]; snprintf(b, sizeof b, "Guide: %zu programmes for %zu channels", S.resGuide.programmes, S.resGuide.byChannel.size()); S.msg = b; }
        S.busy = false;
    });
}

void rebuildShown() {
    S.shown.clear();
    std::string q = S.search;
    for (auto& c : q) c = (char)std::tolower((unsigned char)c);
    for (size_t i = 0; i < S.channels.size(); i++) {
        const iptv::Channel& c = S.channels[i];
        if (S.favOnly && !S.favs.count(iptv::channelKey(c))) continue;
        if (!S.group.empty() && c.group != S.group) continue;
        if (!q.empty()) {
            std::string n = c.name + " " + c.group + " " + c.tvgId;
            for (auto& ch : n) ch = (char)std::tolower((unsigned char)ch);
            if (n.find(q) == std::string::npos) continue;
        }
        S.shown.push_back((int)i);
    }
    S.dirty = false;
}

// ---- logos ----
void logoWorker() {
    while (S.logoRun) {
        std::pair<std::string, std::string> job;
        {
            std::lock_guard<std::mutex> l(S.lmu);
            if (S.logoQ.empty()) { S.logoRun = false; return; }
            job = S.logoQ.front(); S.logoQ.pop_front();
        }
        std::string body = readAll(job.second);
        if (body.empty()) {
            iptv::HttpOptions o; o.connectTimeoutMs = 5000; o.readTimeoutMs = 6000;
            iptv::HttpResult r;
            if (iptv::httpGetAll(job.first, o, body, r, 2u << 20) && !body.empty()) writeAll(job.second, body); else body.clear();
        }
        std::lock_guard<std::mutex> l(S.lmu);
        S.logoDone.emplace_back(job.first, std::vector<uint8_t>(body.begin(), body.end()));
    }
}
const Logo* logoFor(const std::string& url) {
    if (url.empty() || !iptv::looksLikeStreamUrl(url)) return nullptr;
    Logo& l = S.logos[url];
    if (l.state == 2) return &l;
    if (l.state == 0 && S.logoQ.size() < 200) {
        l.state = 1;
        std::lock_guard<std::mutex> lk(S.lmu);
        S.logoQ.emplace_back(url, dir() + "/logo_" + hashHex(url) + ".png");
        if (!S.logoRun) { S.logoRun = true; if (S.logoTh.joinable()) S.logoTh.join(); S.logoTh = namedThread("onair-iptvlg", logoWorker); }
    }
    return nullptr;
}
void pumpLogos() {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> done;
    { std::lock_guard<std::mutex> l(S.lmu); done.swap(S.logoDone); }
    for (auto& d : done) {
        Logo& l = S.logos[d.first];
        int w = 0, h = 0; std::vector<uint8_t> rgba;
        if (!d.second.empty() && decodePng(d.second.data(), d.second.size(), w, h, rgba) && w > 0 && h > 0 && w <= 4096 && h <= 4096 && rgba.size() >= (size_t)w * h * 4) {
            l.img = gGfx->createImage(w, h, 0x00000000u);
            l.img->update(0, 0, w, h, reinterpret_cast<const uint32_t*>(rgba.data()));
            l.w = w; l.h = h; l.state = 2;
        } else l.state = 3;   // JPEG / SVG / broken: no logo, the name is enough
    }
}

// ---- screens ----
void takeResults(App& a) {
    std::string pu, pn; bool doPlay = false;
    {
        std::lock_guard<std::mutex> l(S.mu);
        if (S.newList) {
            S.newList = false;
            S.channels = std::move(S.resChannels);
            int idx = -1;
            for (size_t i = 0; i < S.lists.size(); i++) if (S.lists[i].src == S.resList.src) idx = (int)i;
            if (idx < 0) { S.lists.push_back(S.resList); idx = (int)S.lists.size() - 1; }
            S.cur = idx; S.group.clear(); S.dirty = true;
            if (!S.resGuideUrl.empty() && !S.guideBuf[0]) snprintf(S.guideBuf, sizeof S.guideBuf, "%s", S.resGuideUrl.c_str());
            saveState();
        }
        if (S.newGuide) { S.newGuide = false; S.guide = std::move(S.resGuide); saveState(); }
        if (S.playNow) { S.playNow = false; pu = S.playUrl; pn = S.playName; doPlay = true; }
    }
    if (doPlay) play(a, pu, nullptr, pn);
}

void drawNowNext() {
    if (!S.havePlaying) return;
    if (S.guide.programmes == 0) return;
    const iptv::Programme* nx = nullptr;
    const int64_t now = nowUtc();
    const iptv::Programme* cur = iptv::guideNow(S.guide, S.playingCh, now, &nx);
    auto hm = [](int64_t t) { char b[16]; std::time_t tt = (std::time_t)t; std::strftime(b, sizeof b, "%H:%M", std::localtime(&tt)); return std::string(b); };
    if (cur) {
        const float f = std::min(1.f, std::max(0.f, (float)(now - cur->start) / (float)std::max<int64_t>(1, cur->stop - cur->start)));
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1), "%s", tr("NOW"));
        ImGui::SameLine();
        ImGui::Text("%s - %s  %s", hm(cur->start).c_str(), hm(cur->stop).c_str(), cur->title.c_str());
        char left[64]; snprintf(left, sizeof left, tr("%d min left"), (int)std::max<int64_t>(0, (cur->stop - now + 30) / 60));
        ImGui::ProgressBar(f, ImVec2(-1, 12 * gUi), left);
        if (!cur->desc.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", cur->desc.substr(0, 300).c_str()); ImGui::PopTextWrapPos(); }
    }
    if (nx) { ImGui::TextDisabled("%s", tr("NEXT")); ImGui::SameLine(); ImGui::Text("%s  %s", hm(nx->start).c_str(), nx->title.c_str()); }
    if (!cur && !nx) ImGui::TextDisabled("%s", tr("no programme information for this channel in the guide"));
}

void drawStatus(App& a) {
    const iptv::StreamInfo i = S.stream.info();
    const PlayerStats ps = a.engine.player().stats();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::heading(), "%s", S.playingName.c_str());
    ImVec4 col = i.state == "playing" ? ImVec4(0.45f, 0.85f, 0.5f, 1) : i.state == "error" ? ImVec4(0.95f, 0.45f, 0.4f, 1) : ImVec4(0.95f, 0.8f, 0.35f, 1);
    const char* label = i.state == "playing" ? "playing" : i.state == "buffering" ? "buffering" : i.state == "reconnecting" ? "reconnecting" : i.state == "error" ? "cannot play" : i.state == "ended" ? "ended" : i.state == "stopped" ? "stopped" : "connecting";
    ImGui::TextColored(col, "%s", label);
    if (!i.message.empty()) { ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(i.message.c_str()); ImGui::PopTextWrapPos(); }
    std::string line;
    if (!i.kind.empty()) line += i.kind;
    if (ps.width > 0) line += (line.empty() ? "" : "  |  ") + std::to_string(ps.width) + "x" + std::to_string(ps.height) + (ps.fps > 0 ? " " + std::to_string((int)(ps.fps + 0.5)) + " fps" : "");
    if (!ps.videoCodec.empty()) line += "  |  " + ps.videoCodec + (ps.audioCodec.empty() ? "" : " + " + ps.audioCodec);
    if (i.measuredBps > 0) { char b[48]; snprintf(b, sizeof b, "  |  %.1f Mbit/s download", i.measuredBps / 1e6); line += b; }
    if (i.bufferedSec > 0.05) { char b[48]; snprintf(b, sizeof b, "  |  %.0f s buffered", i.bufferedSec); line += b; }
    if (i.reconnects) line += "  |  reconnected " + std::to_string(i.reconnects) + "x";
    if (!line.empty()) ImGui::TextDisabled("%s", line.c_str());
    if (i.variantCount > 1) {
        // quality: automatic, or one of the stream's own variants (Advanced)
        ImGui::SetNextItemWidth(220 * gUi);
        char cur[96];
        if (S.quality < 0) snprintf(cur, sizeof cur, "Automatic%s", i.vh > 0 ? (" (" + std::to_string(i.vh) + "p)").c_str() : "");
        else snprintf(cur, sizeof cur, "Fixed: variant %d", S.quality + 1);
        if (ImGui::BeginCombo("Quality##iptvq", cur)) {
            if (ImGui::Selectable("Automatic (follows your connection)", S.quality < 0)) { S.quality = -1; S.stream.setVariant(-1); }
            for (int v = 0; v < i.variantCount; v++) { char l[48]; snprintf(l, sizeof l, "Variant %d of %d (lowest first)", v + 1, i.variantCount); if (ImGui::Selectable(l, S.quality == v)) { S.quality = v; S.stream.setVariant(v); } }
            ImGui::EndCombo();
        }
    }
    if (!i.audioTracks.empty() || !i.subtitleTracks.empty()) {
        std::string t = "Stream lists";
        if (!i.audioTracks.empty()) { t += " audio: "; for (size_t k = 0; k < i.audioTracks.size() && k < 6; k++) t += (k ? ", " : "") + i.audioTracks[k]; }
        if (!i.subtitleTracks.empty()) { t += "  subtitles: "; for (size_t k = 0; k < i.subtitleTracks.size() && k < 6; k++) t += (k ? ", " : "") + i.subtitleTracks[k]; }
        ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", t.c_str()); ImGui::PopTextWrapPos();
    }
    if (i.state == "error" || i.state == "ended") {
        if (ImGui::Button("Try again##iptvretry")) { const std::string u = S.playing; iptv::Channel c = S.playingCh; const bool have = !c.url.empty(); play(a, u, have ? &c : nullptr, S.playingName); }
        ImGui::SameLine();
    }
    if (ImGui::Button("Stop##iptvstop")) stopPlaying(a);
    ImGui::SameLine();
    if (!S.playingCh.url.empty()) {
        const bool fav = S.favs.count(S.playingKey) > 0;
        if (ImGui::Button(fav ? "Remove favourite##iptvfav" : "Add to favourites##iptvfav")) { if (fav) S.favs.erase(S.playingKey); else S.favs.insert(S.playingKey); S.dirty = true; saveState(); }
    }
}

void tab(App& a) {
    loadState();
    takeResults(a);
    pumpLogos();
    if (S.dirty) rebuildShown();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("OnAir plays television that is streamed over the internet. It comes with no channels: add a playlist (M3U / M3U8) from your provider or another source you are allowed to use, or paste the address of one stream. You are responsible for what you open."));
    ImGui::PopTextWrapPos();
    if (ImGui::SmallButton("?##iptvhelp")) ImGui::OpenPopup("iptvhelp");
    if (ImGui::BeginPopup("iptvhelp")) {
        ImGui::PushTextWrapPos(520 * gUi);
        ImGui::TextColored(pal::heading(), "%s", tr("Internet TV"));
        ImGui::TextUnformatted(tr("Opens: playlists (M3U / M3U8, an address or a file); HLS streams (.m3u8, live or on demand, several bitrates, AES-128); MPEG-TS over http or https; udp:// and rtp:// (also multicast)."));
        ImGui::TextUnformatted(tr("Not yet: RTSP, DASH, fragmented MP4 (CMAF) HLS. Never: DRM protected streams (Widevine, PlayReady, FairPlay); OnAir tells you and stops."));
        ImGui::TextUnformatted(tr("OnAir comes with no channels. You are responsible for what you open; use sources you may watch."));
        ImGui::TextUnformatted(tr("Free public channels: the community project iptv-org (github.com/iptv-org/iptv) keeps playlists of publicly available streams. This is a text pointer, nothing is fetched from it."));
        ImGui::TextUnformatted(tr("If a stream says 403 or refuses to play, Advanced has User-Agent, Referer and headers. Details: docs/IPTV.md."));
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(160.f * gUi, ImGui::GetContentRegionAvail().x - 330 * gUi));
    const bool enter = ImGui::InputTextWithHint("##iptvurl", "Playlist or stream address: https://..., http://..., udp://@239.1.1.1:1234, rtp://...", S.urlBuf, sizeof S.urlBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(S.busy || !S.urlBuf[0]);
    if (ImGui::Button(S.busy ? "Loading...##iptvgo" : "Add / Play##iptvgo") || (enter && S.urlBuf[0])) { loadPlaylistAsync(iptv::trim(S.urlBuf), false); saveState(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("A playlist address is added to your lists and its channels shown on the right. A single stream (HLS .m3u8, MPEG-TS over http, udp:// or rtp://, also multicast) starts playing."));
    ImGui::SameLine();
    if (ImGui::Button("Open file...##iptvfile")) { const std::string p = openFileDialog(); if (!p.empty()) loadPlaylistAsync(p, true); }
    if (!S.msg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(S.msg.find("annot") != std::string::npos || S.msg.find("No channels") != std::string::npos || S.msg.find("DRM") != std::string::npos ? ImVec4(0.95f, 0.6f, 0.4f, 1) : ImVec4(0.7f, 0.85f, 0.7f, 1), "%s", S.msg.c_str()); ImGui::PopTextWrapPos(); }
    // playlists
    if (!S.lists.empty()) {
        ImGui::SetNextItemWidth(260 * gUi);
        const std::string curName = S.cur >= 0 && S.cur < (int)S.lists.size() ? S.lists[S.cur].name : "choose a playlist";
        if (ImGui::BeginCombo("Playlist##iptvpl", curName.c_str())) {
            for (int i = 0; i < (int)S.lists.size(); i++) {
                if (ImGui::Selectable((S.lists[i].name + "##pl" + std::to_string(i)).c_str(), i == S.cur)) {
                    S.cur = i;
                    const std::string c = readAll(dir() + "/pl_" + hashHex(S.lists[i].src) + ".m3u");
                    S.channels = iptv::parseM3u(c, S.lists[i].src).channels; S.group.clear(); S.dirty = true; saveState();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", iptv::redactUrl(S.lists[i].src).c_str());
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (S.cur >= 0 && S.cur < (int)S.lists.size()) {
            if (ImGui::Button("Refresh##iptvrf")) { const std::string s = S.lists[S.cur].src; loadPlaylistAsync(s, !iptv::parseUrl(s).ok); }
            ImGui::SameLine();
            if (ImGui::Button("Remove##iptvrm")) {
                std::error_code ec; std::filesystem::remove(u8file(dir() + "/pl_" + hashHex(S.lists[S.cur].src) + ".m3u"), ec);
                S.lists.erase(S.lists.begin() + S.cur); S.cur = -1; S.channels.clear(); S.dirty = true; saveState();
            }
        }
    }
    // guide
    ImGui::SetNextItemWidth(std::max(160.f * gUi, ImGui::GetContentRegionAvail().x - 150 * gUi));
    ImGui::InputTextWithHint("##iptvguide", "Programme guide (XMLTV) address or file - optional", S.guideBuf, sizeof S.guideBuf);
    ImGui::SameLine();
    ImGui::BeginDisabled(S.busy || !S.guideBuf[0]);
    if (ImGui::Button("Load guide##iptvguidego")) { loadGuideAsync(iptv::trim(S.guideBuf)); saveState(); }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Matched to the channels by their tvg-id. The playlist often names its own guide; it is filled in for you."));
    // advanced
    if (ImGui::CollapsingHeader("Advanced##iptvadv")) {
        ImGui::SetNextItemWidth(320 * gUi); ImGui::InputTextWithHint("User-Agent##iptvua", "OnAir (some providers want a player name, e.g. VLC/3.0)", S.uaBuf, sizeof S.uaBuf);
        ImGui::SetNextItemWidth(320 * gUi); ImGui::InputTextWithHint("Referer##iptvref", "optional", S.refBuf, sizeof S.refBuf);
        ImGui::InputTextMultiline("Headers##iptvhdr", S.hdrBuf, sizeof S.hdrBuf, ImVec2(320 * gUi, 56 * gUi));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One per line, Name: value. For example Authorization or Cookie, if your provider gives you one."));
        ImGui::SetNextItemWidth(160 * gUi); ImGui::InputTextWithHint("Multicast interface##iptvif", "this computer's address, optional", S.ifaceBuf, sizeof S.ifaceBuf);
        ImGui::Checkbox("Ignore certificate errors (only for a stream you trust)##iptvins", &S.insecure);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("By default OnAir refuses an https server whose certificate does not verify (self-signed, expired, wrong name) and shows the reason. Tick this to open it anyway."));
        if (ImGui::Button("Save these settings##iptvsave")) saveState();
    }
    ImGui::Separator();
    if (S.havePlaying) {
        drawStatus(a);
        drawNowNext();
        playerTab(a);
    } else {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("Pick a channel in the list on the right, or add a playlist or a stream address above."));
        ImGui::TextDisabled("%s", tr("Press the ? button for what is supported and where to find free public channels."));
        ImGui::PopTextWrapPos();
    }
}

void list(App& a) {
    loadState();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##iptvsearch", "search channels", S.searchBuf, sizeof S.searchBuf)) { S.search = S.searchBuf; S.dirty = true; }
    // groups
    std::vector<std::string> groups;
    { std::set<std::string> g; for (auto& c : S.channels) if (!c.group.empty()) g.insert(c.group); groups.assign(g.begin(), g.end()); }
    ImGui::SetNextItemWidth(std::max(80.f * gUi, ImGui::GetContentRegionAvail().x - 90 * gUi));
    if (ImGui::BeginCombo("##iptvgrp", S.group.empty() ? "All groups" : S.group.c_str())) {
        if (ImGui::Selectable("All groups", S.group.empty())) { S.group.clear(); S.dirty = true; }
        for (auto& g : groups) if (ImGui::Selectable(g.c_str(), g == S.group)) { S.group = g; S.dirty = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Favs##iptvfo", &S.favOnly)) S.dirty = true;
    // last watched first when nothing is searched
    if (S.channels.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("No channels yet. Add a playlist on the left.")); ImGui::PopTextWrapPos(); return; }
    ImGui::TextDisabled("%zu of %zu channels", S.shown.size(), S.channels.size());
    ImGui::BeginChild("iptvlist", ImVec2(-1, -1));
    ImGuiListClipper clip;
    std::vector<int> order = S.shown;
    if (S.search.empty() && S.group.empty() && !S.favOnly && !S.recent.empty()) {   // last watched on top
        std::vector<int> head;
        for (auto& k : S.recent) for (int i : order) if (iptv::channelKey(S.channels[i]) == k) { head.push_back(i); break; }
        if (!head.empty()) { std::set<int> hs(head.begin(), head.end()); for (int i : order) if (!hs.count(i)) head.push_back(i); order = head; }
    }
    clip.Begin((int)order.size(), 34 * gUi);
    while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
        const iptv::Channel& c = S.channels[order[r]];
        const std::string key = iptv::channelKey(c);
        const bool sel = S.havePlaying && S.playingKey == key;
        ImGui::PushID(order[r]);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##row", sel, 0, ImVec2(w, 32 * gUi))) play(a, c.url, &c, c.name);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float x = p.x + 4 * gUi;
        const Logo* lg = logoFor(c.logo);
        if (lg) {
            const float bh = 26 * gUi, bw = 44 * gUi, sc = std::min(bh / (float)lg->h, bw / (float)lg->w);
            const float iw = lg->w * sc, ih = lg->h * sc;
            dl->AddImage(lg->img->texture(), ImVec2(x + (bw - iw) / 2, p.y + 3 * gUi + (bh - ih) / 2), ImVec2(x + (bw - iw) / 2 + iw, p.y + 3 * gUi + (bh - ih) / 2 + ih));
        }
        x += 50 * gUi;
        const bool fav = S.favs.count(key) > 0;
        const float nameW = w - 50 * gUi - 30 * gUi;
        dl->AddText(a.ui, 15.f * gUi, ImVec2(x, p.y + 2 * gUi), c.drm ? IM_COL32(150, 150, 150, 255) : IM_COL32(240, 242, 246, 255), ellipsize(c.name, nameW, 15.f * gUi).c_str());
        if (!c.group.empty()) dl->AddText(ImVec2(x, p.y + 18 * gUi), IM_COL32(140, 148, 158, 255), ellipsize(c.drm ? c.group + "  (DRM, not played)" : c.group, nameW, 12.f * gUi).c_str());
        if (fav) dl->AddText(ImVec2(p.x + w - 20 * gUi, p.y + 6 * gUi), IM_COL32(250, 210, 90, 255), "*");
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem(fav ? "Remove from favourites" : "Add to favourites")) { if (fav) S.favs.erase(key); else S.favs.insert(key); S.dirty = true; saveState(); }
            if (ImGui::MenuItem("Copy address")) ImGui::SetClipboardText(c.url.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void status(App& a) {
    const iptv::StreamInfo i = S.stream.info();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Internet TV"); ImGui::SameLine(0, 8 * gUi);
    ImGui::PushFont(a.mono, 0);
    ImGui::TextUnformatted(S.havePlaying ? (i.state + (i.kind.empty() ? "" : "  " + i.kind)).c_str() : "idle");
    ImGui::PopFont();
}

void summary(const App&, std::string& l1, std::string& l2) {
    l1 = "Internet TV";
    l2 = S.havePlaying ? S.playingName : "no channel";
}

void tick(App& a) {
    loadState();
    S.lastTick = glfwGetTime();
    a.engine.player().setVolume(a.volume);
}

} // namespace

extern const ModeUi kIptvUi;
const ModeUi kIptvUi = {
    .sideTitle = "CHANNELS",
    .tabName = "Internet TV",
    .tabIcon = Ic::Tv,
    .tab = tab,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
