// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AVS+ video through the user's FFmpeg add-on (avsplus_dec.h). Same loading model as core/src/audio/ac4_dec.cpp.
#include "dect2/tv/avsplus_dec.h"
#include "../source/native_common.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include "dect2/common/diag.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace dect2 {

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

std::string lowerS(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

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
};

struct LibState {
    std::mutex mu;
    bool searched = false;
    std::string dir;                // test override
    std::string decoderName;        // empty: env, else "cavs"
    AvsPlusLibInfo info;
    std::unique_ptr<Lib> lib;
    double lastPoll = -1e9;
};
LibState& S() { static LibState s; return s; }

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string wantedName(const LibState& s) {
    if (!s.decoderName.empty()) return s.decoderName;
    if (const char* e = getenv("DECT2_AVSPLUS_DECODER")) if (*e) return e;
    return "cavs";
}

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
    return true;
}

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
        if (lowerS(n).rfind(".dll") != n.size() - 4) continue;
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
    if (const char* e = getenv("DECT2_AVSPLUS_DIR")) if (*e) v.push_back(e);
    { const std::string x = dect2::codec::externalFfmpegDir(); if (!x.empty()) v.push_back(x); }   // the user's own FFmpeg folder (Settings)
    for (auto& d : native::codecFolders()) v.push_back(d);
    return v;
}

void doSearch(LibState& s) {
    s.info = AvsPlusLibInfo();
    s.lib.reset();
    std::string anyFound, abiMsg;
    const std::string name = wantedName(s);
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
                const unsigned ver = L->avcodec_version();
                if (!s.info.found) { s.info.found = true; s.info.path = cp; s.info.utilPath = up; s.info.avcodecVersion = (int)ver; }
                if (!avsPlusAbiOk(ver)) { abiMsg = cp + " is libavcodec " + std::to_string(ver >> 16) + ", a version this loader does not know"; continue; }
                const AVCodec* c = L->find_decoder_by_name(name.c_str());
                if (!c) { anyFound = cp; continue; }   // not unloaded: another FFmpeg is looked at, and unloading a FFmpeg is not worth the risk
                s.info.found = true; s.info.hasDecoder = true;
                s.info.path = cp; s.info.utilPath = up; s.info.avcodecVersion = (int)ver;
                s.info.message.clear();
                s.lib = std::move(L);
                return;
            }
        }
    }
    if (!abiMsg.empty() && s.info.found) s.info.message = abiMsg;
    else if (s.info.found)
        s.info.message = "libavcodec was found (" + (anyFound.empty() ? s.info.path : anyFound) + ") but it has no \"" + name + "\" decoder: it is not an FFmpeg with the AVS+ patch";
    else
        s.info.message = "no libavcodec / libavutil in the codecs folders";
}

} // namespace

bool avsPlusAbiOk(unsigned v) { const unsigned major = v >> 16; return major >= 59 && major <= 64; }   // FFmpeg 5.0 (59) .. 9.x (63), one more for the next

AvsPlusLibInfo avsPlusLibrary(bool rescan) {
    LibState& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.searched || rescan) { doSearch(s); s.searched = true; s.lastPoll = nowSec(); }
    return s.info;
}

bool avsPlusPoll(double minIntervalSec) {
    LibState& s = S();
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.searched && s.info.hasDecoder) return true;
    const double t = nowSec();
    if (!s.searched || t - s.lastPoll >= minIntervalSec) { doSearch(s); s.searched = true; s.lastPoll = t; }
    return s.info.hasDecoder;
}

void avsPlusSetLibraryDir(const std::string& dir) { LibState& s = S(); std::lock_guard<std::mutex> lk(s.mu); s.dir = dir; s.searched = false; }
void avsPlusSetDecoderName(const std::string& name) { LibState& s = S(); std::lock_guard<std::mutex> lk(s.mu); s.decoderName = name; s.searched = false; }
std::vector<std::string> avsPlusSearchFolders() { LibState& s = S(); std::lock_guard<std::mutex> lk(s.mu); return searchFolders(s.dir); }

std::string avsPlusNote(const AvsPlusLibInfo& info) {
    if (info.hasDecoder) return std::string();
    if (info.found && !info.message.empty()) return "AVS+ video: the AVS+ add-on cannot be used (" + info.message + ")";
    return "AVS+ video: install the AVS+ add-on to see the picture";
}

// ===================================================================================================================================
// the decoder
// ===================================================================================================================================

struct AvsPlusDecoder::Impl {
    const Lib* L = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    std::string err;
    std::multiset<std::pair<int64_t, bool>> pending;   // (pts, key) of the packets sent whose picture has not come out yet
    ~Impl() {
        if (!L) return;
        if (ctx) L->free_context(&ctx);
        if (pkt) L->packet_free(&pkt);
        if (frame) L->frame_free(&frame);
    }
};

AvsPlusDecoder::AvsPlusDecoder() : p_(new Impl) {}
AvsPlusDecoder::~AvsPlusDecoder() = default;
bool AvsPlusDecoder::isOpen() const { return p_->ctx != nullptr; }
std::string AvsPlusDecoder::error() const { return p_->err; }

bool AvsPlusDecoder::open() {
    if (p_->ctx) return true;
    const Lib* L = nullptr;
    std::string name;
    {
        LibState& s = S();
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.searched) { doSearch(s); s.searched = true; }
        if (!s.lib) { DIAG_DEBUG(475358, "The AVS+ decoder add-on library was not found or could not be loaded, so AVS+ video has no picture."); p_->err = s.info.message.empty() ? "AVS+ add-on not found" : s.info.message; return false; }
        L = s.lib.get();   // never freed: the library stays loaded
        name = wantedName(s);
    }
    p_->L = L;
    const AVCodec* c = L->find_decoder_by_name(name.c_str());
    if (!c) { DIAG(203683, "The AVS+ decoder add-on has no decoder named %s, so AVS+ video has no picture.", name.c_str()); p_->err = "the add-on has no \"" + name + "\" decoder"; return false; }
    p_->ctx = L->alloc_context3(c);
    if (!p_->ctx) { p_->err = "out of memory"; return false; }
    if (L->open2(p_->ctx, c, nullptr) < 0) { DIAG(536967, "The AVS+ decoder %s from the add-on did not open, so AVS+ video has no picture.", name.c_str()); L->free_context(&p_->ctx); p_->err = "the AVS+ decoder would not open"; return false; }
    p_->pkt = L->packet_alloc();
    p_->frame = L->frame_alloc();
    if (!p_->pkt || !p_->frame) { p_->err = "out of memory"; return false; }
    return true;
}

void AvsPlusDecoder::flush() {
    if (!p_->ctx) return;
    p_->L->flush_buffers(p_->ctx);
    p_->pending.clear();
}

bool AvsPlusDecoder::send(const uint8_t* data, size_t size, int64_t pts, bool key) {
    if (!p_->ctx || !data || size == 0 || size > (1u << 26)) return false;
    const Lib& L = *p_->L;
    L.packet_unref(p_->pkt);
    if (L.new_packet(p_->pkt, (int)size) < 0) return false;
    memcpy(p_->pkt->data, data, size);
    p_->pkt->pts = pts == INT64_MIN ? AV_NOPTS_VALUE : pts;
    p_->pkt->dts = AV_NOPTS_VALUE;
    const int r = L.send_packet(p_->ctx, p_->pkt);
    L.packet_unref(p_->pkt);
    sent++;
    if (r < 0 && r != AVERROR(EAGAIN)) { DIAG_DEBUG_EVERY(605832, 10.0, "The AVS+ decoder rejected a packet (error %d), so that picture is skipped.", r); p_->err = "the AVS+ decoder rejected the data"; return false; }
    if (pts != INT64_MIN) p_->pending.insert({pts, key});
    if (p_->pending.size() > 64) p_->pending.erase(p_->pending.begin());
    return true;
}

bool AvsPlusDecoder::receive(AvsPicture& out) {
    if (!p_->ctx) return false;
    const Lib& L = *p_->L;
    L.frame_unref(p_->frame);
    if (L.receive_frame(p_->ctx, p_->frame) < 0) return false;
    const AVFrame* f = p_->frame;
    out = AvsPicture();
    out.w = f->width; out.h = f->height; out.format = f->format;
    for (int i = 0; i < 4; i++) { out.data[i] = f->data[i]; out.linesize[i] = f->linesize[i]; }
    if (!p_->pending.empty()) { out.pts = p_->pending.begin()->first; out.key = p_->pending.begin()->second; p_->pending.erase(p_->pending.begin()); }
    pictures++;
    return out.w > 0 && out.h > 0 && out.data[0];
}

} // namespace dect2
