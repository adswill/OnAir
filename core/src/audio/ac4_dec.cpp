// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/audio/ac4_dec.h"
#include "../source/native_common.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include "dect2/common/str.h"
}

namespace dect2 {

// ===================================================================================================================================
// signalling
// ===================================================================================================================================

namespace {

std::string lower(std::string s) { return dect2::str::lower(std::move(s)); }
std::string trim(const std::string& s) { return dect2::str::trimSpace(s); }
std::vector<std::string> splitCodecs(const std::string& s) {
    std::vector<std::string> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t c = s.find(',', pos);
        if (c == std::string::npos) c = s.size();
        std::string t = trim(s.substr(pos, c - pos));
        if (!t.empty()) v.push_back(t);
        pos = c + 1;
    }
    return v;
}
AudioCodecKind kindOfToken(const std::string& tok) {
    const std::string t = lower(tok);
    auto starts = [&](const char* p) { return t.compare(0, strlen(p), p) == 0; };
    if (starts("mp4a.40.") || starts("mp4a.67")) return AudioCodecKind::Aac;
    if (t == "ac-3" || starts("ac-3.")) return AudioCodecKind::Ac3;
    if (t == "ec-3" || starts("ec-3.")) return AudioCodecKind::Eac3;
    if (t == "ac-4" || starts("ac-4.")) return AudioCodecKind::Ac4;
    if (starts("mhm1") || starts("mhm2") || starts("mha1") || starts("mha2")) return AudioCodecKind::MpegH;
    if (t == "opus") return AudioCodecKind::Opus;
    return AudioCodecKind::Unknown;
}
// every value of attribute `name` (name="value" or name='value') in an XML text
std::vector<std::string> attrValues(const std::string& xml, const char* name) {
    std::vector<std::string> v;
    const std::string key = std::string(name) + "=";
    size_t pos = 0;
    while ((pos = xml.find(key, pos)) != std::string::npos) {
        const bool boundary = pos == 0 || std::isspace((unsigned char)xml[pos - 1]);
        pos += key.size();
        if (!boundary || pos >= xml.size() || (xml[pos] != '"' && xml[pos] != '\'')) continue;
        const char q = xml[pos];
        const size_t e = xml.find(q, pos + 1);
        if (e == std::string::npos) break;
        v.push_back(xml.substr(pos + 1, e - pos - 1));
        pos = e + 1;
    }
    return v;
}

} // namespace

AudioCodecKind audioCodecFromCodecs(const std::string& codecs) {
    for (const auto& t : splitCodecs(codecs)) { const AudioCodecKind k = kindOfToken(t); if (k != AudioCodecKind::Unknown) return k; }
    return AudioCodecKind::Unknown;
}

const char* audioCodecName(AudioCodecKind k) {
    switch (k) {
    case AudioCodecKind::Aac: return "AAC";
    case AudioCodecKind::Ac3: return "AC-3";
    case AudioCodecKind::Eac3: return "E-AC-3";
    case AudioCodecKind::Ac4: return "AC-4";
    case AudioCodecKind::MpegH: return "MPEG-H";
    case AudioCodecKind::Opus: return "Opus";
    default: return "unknown";
    }
}

Ac4Codecs parseAc4Codecs(const std::string& codecs) {
    Ac4Codecs r;
    for (const auto& tok : splitCodecs(codecs)) {
        const std::string t = lower(tok);
        if (t.compare(0, 4, "ac-4") != 0 || (t.size() > 4 && t[4] != '.')) continue;
        r.valid = true;
        int vals[3] = {-1, -1, -1};
        size_t pos = 5;
        for (int i = 0; i < 3 && pos < t.size(); i++) {
            size_t e = t.find('.', pos);
            if (e == std::string::npos) e = t.size();
            int n = 0;
            bool ok = e > pos;
            for (size_t k = pos; k < e; k++) { if (t[k] < '0' || t[k] > '9') { ok = false; break; } n = n * 10 + (t[k] - '0'); }
            vals[i] = ok ? n : -1;
            pos = e + 1;
        }
        r.bitstreamVersion = vals[0]; r.presentationVersion = vals[1]; r.mdcompat = vals[2];
        return r;
    }
    return r;
}

std::vector<std::string> mpdAc4Codecs(const std::string& mpd) {
    std::vector<std::string> out;
    for (const auto& val : attrValues(mpd, "codecs"))
        for (const auto& t : splitCodecs(val)) if (kindOfToken(t) == AudioCodecKind::Ac4) out.push_back(t);
    return out;
}

std::vector<AudioCodecKind> mpdAudioKinds(const std::string& mpd) {
    std::vector<AudioCodecKind> out;
    for (const auto& val : attrValues(mpd, "codecs"))
        for (const auto& t : splitCodecs(val)) {
            const AudioCodecKind k = kindOfToken(t);
            if (k != AudioCodecKind::Unknown && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
        }
    return out;
}

Ac4Dsi parseDac4(const uint8_t* p, size_t n) {
    Ac4Dsi d;
    if (!p || n < 3) return d;
    const uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];   // 3 + 7 + 1 + 4 + 9 = 24 bits
    d.dsiVersion = (int)(v >> 21);
    d.bitstreamVersion = (int)((v >> 14) & 0x7f);
    d.fsIndex = (int)((v >> 13) & 1);
    d.frameRateIndex = (int)((v >> 9) & 0xf);
    d.presentations = (int)(v & 0x1ff);
    d.raw.assign(p, p + n);
    d.valid = true;
    return d;
}

namespace {

struct Box { size_t start = 0, payload = 0, end = 0; char type[5] = {0}; };
bool nextBox(const uint8_t* d, size_t pos, size_t limit, Box& b) {
    if (pos + 8 > limit) return false;
    uint64_t sz = ((uint32_t)d[pos] << 24) | ((uint32_t)d[pos + 1] << 16) | ((uint32_t)d[pos + 2] << 8) | d[pos + 3];
    size_t hdr = 8;
    memcpy(b.type, d + pos + 4, 4); b.type[4] = 0;
    if (sz == 1) {
        if (pos + 16 > limit) return false;
        sz = 0;
        for (int i = 0; i < 8; i++) sz = (sz << 8) | d[pos + 8 + i];
        hdr = 16;
    } else if (sz == 0) sz = limit - pos;
    if (sz < hdr || pos + sz > limit) return false;
    b.start = pos; b.payload = pos + hdr; b.end = pos + (size_t)sz;
    return true;
}

bool walkForAc4(const uint8_t* d, size_t begin, size_t end, Ac4Dsi* dsi, int* channels, int depth) {
    if (depth > 8) return false;
    Box b;
    for (size_t pos = begin; nextBox(d, pos, end, b); pos = b.end) {
        const std::string t = b.type;
        if (t == "moov" || t == "trak" || t == "mdia" || t == "minf" || t == "stbl") {
            if (walkForAc4(d, b.payload, b.end, dsi, channels, depth + 1)) return true;
        } else if (t == "stsd") {
            // full box header (4) + entry count (4), then the sample entries
            Box e;
            for (size_t ep = b.payload + 8; nextBox(d, ep, b.end, e); ep = e.end) {
                if (memcmp(e.type, "ac-4", 4) != 0) continue;
                if (e.end - e.payload < 28) return false;
                if (channels) *channels = (d[e.payload + 16] << 8) | d[e.payload + 17];
                if (dsi) {
                    Box c;
                    for (size_t cp = e.payload + 28; nextBox(d, cp, e.end, c); cp = c.end)
                        if (memcmp(c.type, "dac4", 4) == 0) { *dsi = parseDac4(d + c.payload, c.end - c.payload); break; }
                }
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool findAc4InInit(const uint8_t* init, size_t size, Ac4Dsi* dsi, int* channels) {
    if (dsi) *dsi = Ac4Dsi();
    if (channels) *channels = 0;
    if (!init || size < 8) return false;
    return walkForAc4(init, 0, size, dsi, channels, 0);
}

// ===================================================================================================================================
// the optional library
// ===================================================================================================================================

namespace {

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle openLib(const std::string& p) { return LoadLibraryExA(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); }
void* symOf(LibHandle h, const char* n) { return (void*)GetProcAddress(h, n); }
void closeLib(LibHandle h) { FreeLibrary(h); }
#else
using LibHandle = void*;
LibHandle openLib(const std::string& p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* symOf(LibHandle h, const char* n) { return dlsym(h, n); }
void closeLib(LibHandle h) { dlclose(h); }
#endif

struct Lib {
    LibHandle codec = nullptr, util = nullptr;
    unsigned (*avcodec_version)() = nullptr;
    const AVCodec* (*find_decoder_by_name)(const char*) = nullptr;
    AVCodecContext* (*alloc_context3)(const AVCodec*) = nullptr;
    int (*open2)(AVCodecContext*, const AVCodec*, AVDictionary**) = nullptr;
    void (*free_context)(AVCodecContext**) = nullptr;
    int (*send_packet)(AVCodecContext*, const AVPacket*) = nullptr;
    int (*receive_frame)(AVCodecContext*, AVFrame*) = nullptr;
    void (*flush_buffers)(AVCodecContext*) = nullptr;
    AVPacket* (*packet_alloc)() = nullptr;
    void (*packet_free)(AVPacket**) = nullptr;
    int (*new_packet)(AVPacket*, int) = nullptr;
    void (*packet_unref)(AVPacket*) = nullptr;
    AVFrame* (*frame_alloc)() = nullptr;
    void (*frame_free)(AVFrame**) = nullptr;
    void (*frame_unref)(AVFrame*) = nullptr;
    int (*opt_get_int)(void*, const char*, int, int64_t*) = nullptr;
    int (*opt_get_chlayout)(void*, const char*, int, AVChannelLayout*) = nullptr;      // FFmpeg 5.1 and later
    void (*chlayout_uninit)(AVChannelLayout*) = nullptr;
};

// libac4dec, through its run-time entry points (its ac4dec.h, "RUN-TIME LOADING")
struct A4Lib {
    LibHandle h = nullptr;
    const char* (*version)() = nullptr;
    void* (*open_options)(const char*, char*, size_t) = nullptr;
    void (*close)(void*) = nullptr;
    int (*decode)(void*, const uint8_t*, size_t, float*, int, int*, int*, int*) = nullptr;
    int (*flush)(void*, float*, int, int*, int*, int*) = nullptr;
    int (*delay)(const void*) = nullptr;
};
constexpr int kA4MaxOut = 2050, kA4MaxCh = 16;   // AC4DEC_MAX_OUT, AC4DEC_MAX_CHANNELS
constexpr int kA4Lookahead = 8;                  // AC4DEC_MIN_LOOKAHEAD: 0.27 s at 29.97 fps
constexpr int kA4Latency = 577;                  // every channel comes out 577 samples late (the QMF bank)

struct LibState {
    std::mutex mu;
    bool searched = false;
    std::string dir;                // test override
    std::string decoderName = "ac4";
    Ac4LibInfo info;
    std::unique_ptr<Lib> lib;
    std::unique_ptr<A4Lib> a4;
    std::vector<std::unique_ptr<A4Lib>> a4Retired;   // from before a "Look again"
};
LibState& S() { static LibState s; return s; }

#define NEED(h, field, name) do { (field) = (decltype(field))symOf(h, name); if (!(field)) { miss = name; return false; } } while (0)

bool resolve(Lib& L, std::string& miss) {
    NEED(L.codec, L.avcodec_version, "avcodec_version");
    NEED(L.codec, L.find_decoder_by_name, "avcodec_find_decoder_by_name");
    NEED(L.codec, L.alloc_context3, "avcodec_alloc_context3");
    NEED(L.codec, L.open2, "avcodec_open2");
    NEED(L.codec, L.free_context, "avcodec_free_context");
    NEED(L.codec, L.send_packet, "avcodec_send_packet");
    NEED(L.codec, L.receive_frame, "avcodec_receive_frame");
    NEED(L.codec, L.flush_buffers, "avcodec_flush_buffers");
    NEED(L.codec, L.packet_alloc, "av_packet_alloc");
    NEED(L.codec, L.packet_free, "av_packet_free");
    NEED(L.codec, L.new_packet, "av_new_packet");
    NEED(L.codec, L.packet_unref, "av_packet_unref");
    NEED(L.util, L.frame_alloc, "av_frame_alloc");
    NEED(L.util, L.frame_free, "av_frame_free");
    NEED(L.util, L.frame_unref, "av_frame_unref");
    NEED(L.util, L.opt_get_int, "av_opt_get_int");
    L.opt_get_chlayout = (decltype(L.opt_get_chlayout))symOf(L.util, "av_opt_get_chlayout");
    L.chlayout_uninit = (decltype(L.chlayout_uninit))symOf(L.util, "av_channel_layout_uninit");
    return true;
}

bool resolveA4(A4Lib& L, std::string& miss) {
    NEED(L.h, L.version, "ac4dec_version");
    NEED(L.h, L.open_options, "ac4dec_open_options");
    NEED(L.h, L.close, "ac4dec_close");
    NEED(L.h, L.decode, "ac4dec_decode_interleaved");
    NEED(L.h, L.flush, "ac4dec_flush_interleaved");
    NEED(L.h, L.delay, "ac4dec_delay");
    return true;
}

// the candidate files of one stem ("avcodec" / "avutil") in a folder, best (highest version in the name) first
std::vector<std::string> candidates(const std::string& dir, const char* stem) {
    std::vector<std::string> v;
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        const std::string n = e.path().filename().string();
        const std::string a = std::string("lib") + stem, b = stem;
        size_t at = std::string::npos;
        if (n.compare(0, a.size(), a) == 0) at = a.size();
        else if (n.compare(0, b.size(), b) == 0) at = b.size();
        if (at == std::string::npos || at >= n.size() || (n[at] != '.' && n[at] != '-')) continue;
#if defined(_WIN32)
        if (lower(n).rfind(".dll") != n.size() - 4) continue;
#else
        if (n.find(".dylib") == std::string::npos && n.find(".so") == std::string::npos) continue;
#endif
        if (!std::filesystem::is_regular_file(e.path(), ec) && !std::filesystem::is_symlink(e.path(), ec)) continue;
        v.push_back(e.path().string());
    }
    std::sort(v.begin(), v.end(), [](const std::string& x, const std::string& y) { return native::newerVersion(x, y); });
    return v;
}

std::vector<std::string> searchFolders(const std::string& override) {
    std::vector<std::string> v;
    if (!override.empty()) { v.push_back(override); return v; }
    if (const char* e = getenv("DECT2_AC4_DIR")) if (*e) v.push_back(e);
    { const std::string x = dect2::codec::externalFfmpegDir(); if (!x.empty()) v.push_back(x); }   // the user's own FFmpeg folder (Settings)
    for (auto& d : native::codecFolders()) v.push_back(d);
    return v;
}

// libac4dec in the folders, first found wins. False when there is none (or none with the run-time entry points: an old version).
bool searchA4(LibState& s, std::string& note) {
    for (const std::string& rawDir : searchFolders(s.dir)) {
        std::string dir = rawDir;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        for (const std::string& p : candidates(dir, "ac4dec")) {
            auto L = std::make_unique<A4Lib>();
            L->h = openLib(p);
            if (!L->h) continue;
            std::string miss;
            if (!resolveA4(*L, miss)) { closeLib(L->h); note = p + " has no " + miss + ": it is too old (it needs the run-time entry points)"; continue; }
            s.info.found = true; s.info.hasDecoder = true;
            s.info.backend = "libac4dec";
            s.info.version = L->version() ? L->version() : "";
            s.info.path = p;
            s.info.message.clear();
            s.a4 = std::move(L);
            return true;
        }
    }
    return false;
}

void doSearch(LibState& s) {
    s.info = Ac4LibInfo();
    s.lib.reset();
    if (s.a4) s.a4Retired.push_back(std::move(s.a4));   // decoders still open keep using it: never freed, never unloaded
    std::string anyFound, a4note;
    const char* be = getenv("DECT2_AC4_BACKEND");
    const std::string backend = be ? lower(be) : std::string();
    if (s.decoderName == "ac4" && backend != "ffmpeg" && searchA4(s, a4note)) return;
    if (backend == "libac4dec") { s.info.message = a4note.empty() ? "no libac4dec in the codecs folders" : a4note; return; }
    for (const std::string& rawDir : searchFolders(s.dir)) {
        std::string dir = rawDir;
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        for (const std::string& cp : candidates(dir, "avcodec")) {
            for (const std::string& up : candidates(dir, "avutil")) {
                auto L = std::make_unique<Lib>();
                L->util = openLib(up);
                if (!L->util) continue;
                L->codec = openLib(cp);
                if (!L->codec) { closeLib(L->util); continue; }
                std::string miss;
                if (!resolve(*L, miss)) { closeLib(L->codec); closeLib(L->util); continue; }
                if (!s.info.found) { s.info.found = true; s.info.path = cp; s.info.utilPath = up; s.info.avcodecVersion = (int)L->avcodec_version(); }
                const AVCodec* c = L->find_decoder_by_name(s.decoderName.c_str());
                if (!c) { anyFound = cp; continue; }   // not unloaded: another FFmpeg is looked at, and unloading a FFmpeg is not worth the risk
                s.info.found = true; s.info.hasDecoder = true;
                s.info.backend = "FFmpeg";
                s.info.path = cp; s.info.utilPath = up; s.info.avcodecVersion = (int)L->avcodec_version();
                s.info.message.clear();
                s.lib = std::move(L);
                return;
            }
        }
    }
    if (s.info.found)
        s.info.message = "libavcodec was found (" + (anyFound.empty() ? s.info.path : anyFound) + ") but it has no \"" + s.decoderName + "\" decoder: it is not an FFmpeg with the AC-4 patch";
    else
        s.info.message = "no libac4dec and no libavcodec / libavutil in the codecs folders";
    if (!a4note.empty()) s.info.message += "; " + a4note;
}

} // namespace

Ac4LibInfo ac4Library(bool rescan) {
    LibState& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.searched || rescan) { doSearch(s); s.searched = true; }
    return s.info;
}

void ac4SetLibraryDir(const std::string& dir) {
    LibState& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.dir = dir;
    s.searched = false;
}

void ac4SetDecoderName(const std::string& name) {
    LibState& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    s.decoderName = name.empty() ? "ac4" : name;
    s.searched = false;
}

std::vector<std::string> ac4SearchFolders() { LibState& s = S(); std::lock_guard<std::mutex> lk(s.mu); return searchFolders(s.dir); }

// ===================================================================================================================================
// the decoder
// ===================================================================================================================================

struct Ac4Decoder::Impl {
    const Lib* L = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    const A4Lib* A = nullptr;   // libac4dec instead of FFmpeg
    void* a4 = nullptr;
    std::vector<float> a4buf;
    int wantCh = 0;
    std::string err;
    int channels = 0, rate = 0;

    bool openA4() {
        std::string opts = "lookahead=" + std::to_string(kA4Lookahead) + " loudness=1 target_level=-24 drc=auto";
        if (wantCh == 6) opts += " layout=5.1";
        else if (wantCh == 2) opts += " layout=2.0";
        char e[160] = {0};
        a4 = A->open_options(opts.c_str(), e, sizeof e);
        if (!a4) { err = std::string("libac4dec would not open: ") + e; return false; }
        a4buf.assign((size_t)kA4MaxOut * kA4MaxCh, 0.f);
        return true;
    }
    // appends one frame of libac4dec output (n samples, ch channels) to pcm, clipped to full scale
    int takeA4(int n, int ch, int r, std::vector<float>& pcm, int& chOut, int& rateOut) {
        if (n <= 0 || ch <= 0 || ch > kA4MaxCh || r <= 0) return n < 0 ? -1 : 0;
        const size_t base = pcm.size();
        pcm.resize(base + (size_t)n * ch);
        for (size_t i = 0; i < (size_t)n * ch; i++) pcm[base + i] = std::clamp(a4buf[i], -1.f, 1.f);
        chOut = ch; rateOut = r;
        channels = ch; rate = r;
        return n;
    }

    ~Impl() {
        if (A && a4) A->close(a4);
        if (!L) return;
        if (ctx) L->free_context(&ctx);
        if (pkt) L->packet_free(&pkt);
        if (frame) L->frame_free(&frame);
    }
};

Ac4Decoder::Ac4Decoder() : p_(new Impl) {}
Ac4Decoder::~Ac4Decoder() = default;

bool Ac4Decoder::isOpen() const { return p_->ctx != nullptr || p_->a4 != nullptr; }
void Ac4Decoder::setOutputChannels(int channels) { p_->wantCh = channels; }
int Ac4Decoder::delayFrames() const { return p_->a4 ? p_->A->delay(p_->a4) : 0; }
int Ac4Decoder::latencySamples() const { return p_->a4 ? kA4Latency : 0; }
std::string Ac4Decoder::error() const { return p_->err; }

bool Ac4Decoder::open() {
    if (p_->ctx || p_->a4) return true;
    const Lib* L = nullptr;
    std::string name;
    {
        LibState& s = S();
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.searched) { doSearch(s); s.searched = true; }
        if (s.a4) { p_->A = s.a4.get(); return p_->openA4(); }
        if (!s.lib) { p_->err = s.info.message.empty() ? "AC-4 decoder library not found" : s.info.message; return false; }
        L = s.lib.get();   // never freed: the library stays loaded
        name = s.decoderName;
    }
    p_->L = L;
    const AVCodec* c = L->find_decoder_by_name(name.c_str());
    if (!c) { p_->err = "the library has no AC-4 decoder"; return false; }
    p_->ctx = L->alloc_context3(c);
    if (!p_->ctx) { p_->err = "out of memory"; return false; }
    const int r = L->open2(p_->ctx, c, nullptr);
    if (r < 0) { L->free_context(&p_->ctx); p_->err = "the AC-4 decoder would not open"; return false; }
    p_->pkt = L->packet_alloc();
    p_->frame = L->frame_alloc();
    if (!p_->pkt || !p_->frame) { p_->err = "out of memory"; return false; }
    return true;
}

void Ac4Decoder::flush() {
    if (p_->ctx) p_->L->flush_buffers(p_->ctx);
    if (p_->a4) { p_->A->close(p_->a4); p_->a4 = nullptr; p_->openA4(); }   // a new stream: the frames inside are dropped
}

int Ac4Decoder::decode(const uint8_t* data, size_t size, std::vector<float>& pcm, int& channels, int& sampleRate) {
    if (p_->a4) {
        if (!data || size == 0 || size > (1u << 24)) return -1;
        int ch = 0, r = 0, st = 0;
        const int n = p_->A->decode(p_->a4, data, size, p_->a4buf.data(), kA4MaxOut, &ch, &r, &st);
        return p_->takeA4(n, ch, r, pcm, channels, sampleRate);
    }
    if (!p_->ctx || !data || size == 0 || size > (1u << 24)) return -1;
    const Lib& L = *p_->L;
    L.packet_unref(p_->pkt);
    if (L.new_packet(p_->pkt, (int)size) < 0) return -1;
    memcpy(p_->pkt->data, data, size);
    int r = L.send_packet(p_->ctx, p_->pkt);
    L.packet_unref(p_->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) { p_->err = "the AC-4 decoder rejected the data"; return -1; }
    int total = 0;
    for (;;) {
        r = L.receive_frame(p_->ctx, p_->frame);
        if (r < 0) break;   // EAGAIN: nothing more
        AVFrame* f = p_->frame;
        int64_t ar = 0;
        L.opt_get_int(p_->ctx, "ar", 0, &ar);
        int ch = 0;
        if (L.opt_get_chlayout && L.chlayout_uninit) {
            AVChannelLayout cl = {};
            if (L.opt_get_chlayout(p_->ctx, "ch_layout", 0, &cl) >= 0) { ch = cl.nb_channels; L.chlayout_uninit(&cl); }
        }
        if (ch <= 0) { int64_t v = 0; if (L.opt_get_int(p_->ctx, "ac", 0, &v) >= 0) ch = (int)v; }
        const int n = f->nb_samples;
        const int fmt = f->format;
        if (ch <= 0 || ch > 16 || n <= 0 || ar <= 0) { L.frame_unref(f); continue; }
        const size_t base = pcm.size();
        pcm.resize(base + (size_t)n * ch);
        float* o = pcm.data() + base;
        bool ok = true;
        const bool planar = fmt >= AV_SAMPLE_FMT_U8P;
        auto src = [&](int c) -> const uint8_t* { return planar ? f->extended_data[c] : f->data[0]; };
        for (int c = 0; c < ch && ok; c++) {
            const uint8_t* s = src(c);
            if (!s) { ok = false; break; }
            const int stride = planar ? 1 : ch;
            const int off = planar ? 0 : c;
            for (int i = 0; i < n; i++) {
                const size_t k = (size_t)i * stride + off;
                float v;
                switch (fmt) {
                case AV_SAMPLE_FMT_FLTP: case AV_SAMPLE_FMT_FLT: v = ((const float*)s)[k]; break;
                case AV_SAMPLE_FMT_DBLP: case AV_SAMPLE_FMT_DBL: v = (float)((const double*)s)[k]; break;
                case AV_SAMPLE_FMT_S16P: case AV_SAMPLE_FMT_S16: v = ((const int16_t*)s)[k] / 32768.f; break;
                case AV_SAMPLE_FMT_S32P: case AV_SAMPLE_FMT_S32: v = (float)(((const int32_t*)s)[k] / 2147483648.0); break;
                case AV_SAMPLE_FMT_U8P: case AV_SAMPLE_FMT_U8: v = (s[k] - 128) / 128.f; break;
                default: v = 0; ok = false; break;
                }
                o[(size_t)i * ch + c] = v;
            }
        }
        L.frame_unref(f);
        if (!ok) { pcm.resize(base); p_->err = "the AC-4 decoder gave an unsupported sample format"; return -1; }
        channels = ch; sampleRate = (int)ar;
        p_->channels = ch; p_->rate = (int)ar;
        total += n;
    }
    return total;
}

// ===================================================================================================================================
// status
// ===================================================================================================================================

namespace {
struct StatusState { std::mutex mu; Ac4Status st; };
StatusState& SS() { static StatusState s; return s; }
}

Ac4Status ac4Status() {
    Ac4Status r;
    { StatusState& s = SS(); std::lock_guard<std::mutex> lk(s.mu); r = s.st; }
    const Ac4LibInfo li = ac4Library();
    r.libFound = li.found; r.hasDecoder = li.hasDecoder; r.libPath = li.path; r.libMessage = li.message;
    return r;
}
void ac4NoteSeen(const std::string& detail) { StatusState& s = SS(); std::lock_guard<std::mutex> lk(s.mu); s.st.seen = true; if (!detail.empty()) s.st.detail = detail; }
void ac4NoteDropped() { StatusState& s = SS(); std::lock_guard<std::mutex> lk(s.mu); s.st.seen = true; s.st.dropped = true; }
void ac4NoteDecode(bool ok, int channels, int rate) {
    StatusState& s = SS(); std::lock_guard<std::mutex> lk(s.mu);
    s.st.accessUnits++;
    if (ok) { s.st.decoded++; s.st.channels = channels; s.st.sampleRate = rate; } else s.st.failed++;
}
void ac4StatusReset() { StatusState& s = SS(); std::lock_guard<std::mutex> lk(s.mu); s.st = Ac4Status(); }

} // namespace dect2
