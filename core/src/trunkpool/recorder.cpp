// One WAV (16 bit mono) and one JSON sidecar per call (see trunkpool.h). Windows: no gmtime_r / localtime_r, no POSIX calls.
#include "dect2/trunkpool/trunkpool.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace dect2 {
namespace tpool {

namespace {
std::string sanitize(const std::string& s) {
    std::string o;
    for (char c : s) o += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return o.empty() ? "sys" : o;
}
std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += ' ';
        else o += c;
    }
    return o + "\"";
}
std::string stamp(int64_t t) {
    const std::time_t tt = (std::time_t)t;
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &g);
    return b;
}
void le32(unsigned char* p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = (v >> 24) & 255; }
}  // namespace

std::string CallRecorder::fileStem(const CallMeta& m) {
    char b[160];
    snprintf(b, sizeof b, "%s_tg%d_%s_%llu", sanitize(m.system).c_str(), m.tg, stamp(m.wallStart).c_str(), (unsigned long long)m.id);
    return b;
}

std::string CallRecorder::metaJson(const CallMeta& m, const std::string& wav) {
    char b[600];
    snprintf(b, sizeof b,
             "{\n  \"call\": %llu,\n  \"protocol\": %s,\n  \"system\": %s,\n  \"talkgroup\": %d,\n  \"source\": %d,\n  \"slot\": %d,\n  \"frequency_hz\": %.0f,\n"
             "  \"start_unix\": %lld,\n  \"duration_sec\": %.2f,\n  \"audio_sec\": %.2f,\n  \"setup_sec\": %.3f,\n  \"group\": %s,\n  \"emergency\": %s,\n  \"encrypted\": false,\n",
             (unsigned long long)m.id, jsonStr(protoName(m.proto)).c_str(), jsonStr(m.system).c_str(), m.tg, m.src, m.slot, m.freqHz, (long long)m.wallStart, m.durationSec, m.audioSec, m.setupSec,
             m.group ? "true" : "false", m.emergency ? "true" : "false");
    std::string o = b;
    o += "  \"device\": " + jsonStr(m.device) + ",\n  \"end_reason\": " + jsonStr(m.endReason) + ",\n  \"audio_file\": " + jsonStr(std::filesystem::path(wav).filename().string()) + "\n}\n";
    return o;
}

void CallRecorder::header() {
    FILE* f = (FILE*)f_;
    unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    const uint32_t bytes = (uint32_t)std::min<uint64_t>(frames_ * 2, 0xFFFFFFF0u - 36);
    le32(h + 4, 36 + bytes);
    le32(h + 24, (uint32_t)rate_);
    le32(h + 28, (uint32_t)rate_ * 2);
    le32(h + 40, bytes);
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f);
}

bool CallRecorder::begin(const std::string& dir, const CallMeta& m, std::string* err) {
    finish(nullptr);
    if (dir.empty()) { if (err) *err = "no recording folder"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    meta_ = m;
    path_ = (std::filesystem::u8path(dir) / (fileStem(m) + ".wav")).u8string();
    f_ = std::fopen(path_.c_str(), "wb+");
    if (!f_) { if (err) *err = "cannot open " + path_; path_.clear(); return false; }
    frames_ = 0; rate_ = 0;
    return true;
}

void CallRecorder::feed(const float* mono, int frames, int rate) {
    if (!f_ || frames <= 0 || rate <= 0) return;
    if (rate_ == 0) { rate_ = rate; header(); std::fseek((FILE*)f_, 44, SEEK_SET); }
    if (rate != rate_) return;   // the rate of a call does not change; a stray block of another rate is dropped
    short buf[1024];
    for (int off = 0; off < frames;) {
        const int n = std::min(frames - off, 1024);
        for (int i = 0; i < n; i++) {
            const float v = std::max(-1.f, std::min(1.f, mono[off + i]));
            buf[i] = (short)std::lrintf(v * 32767.f);
        }
        std::fwrite(buf, 2, (size_t)n, (FILE*)f_);
        off += n;
    }
    frames_ += (uint64_t)frames;
}

void CallRecorder::finishMetaOnly(const std::string& dir, const CallMeta& m) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
    const std::string side = (std::filesystem::u8path(dir) / (fileStem(m) + ".json")).u8string();
    const std::string json = metaJson(m, "");
    if (FILE* j = std::fopen(side.c_str(), "wb")) { std::fwrite(json.data(), 1, json.size(), j); std::fclose(j); }
}

std::string CallRecorder::finish(const CallMeta* final) {
    if (!f_) return "";
    FILE* f = (FILE*)f_;
    std::string wav = path_;
    const bool empty = frames_ == 0;
    if (!empty) header();
    std::fclose(f);
    f_ = nullptr;
    std::error_code ec;
    if (empty) { std::filesystem::remove(std::filesystem::u8path(wav), ec); path_.clear(); return ""; }
    CallMeta m = final ? *final : meta_;
    m.audioSec = seconds();
    const std::string json = metaJson(m, wav);
    std::string side = wav.substr(0, wav.size() - 4) + ".json";
    if (FILE* j = std::fopen(side.c_str(), "wb")) { std::fwrite(json.data(), 1, json.size(), j); std::fclose(j); }
    path_.clear();
    return wav;
}

} // namespace tpool
} // namespace dect2
