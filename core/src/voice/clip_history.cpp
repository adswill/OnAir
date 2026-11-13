// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/voice/clip_history.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace dect2::voice {
namespace fs = std::filesystem;

ClipPolicy ClipPolicy::clamped(ClipPolicy p) {
    p.keepLast = p.keepLast <= 0 ? 0 : std::min(p.keepLast, 1000);
    p.maxAgeDays = std::max(0, std::min(p.maxAgeDays, 3650));
    p.maxMB = std::max(0, std::min(p.maxMB, 100000));
    return p;
}

std::vector<size_t> clipsToDelete(const std::vector<Clip>& clips, const ClipPolicy& pol, int64_t now) {
    const ClipPolicy p = ClipPolicy::clamped(pol);
    std::vector<size_t> order;
    for (size_t i = 0; i < clips.size(); i++) if (!clips[i].pinned) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return clips[a].wallStart > clips[b].wallStart; });   // newest first
    std::vector<size_t> del;
    uint64_t sum = 0;
    const uint64_t cap = uint64_t(p.maxMB) * 1024 * 1024;
    for (size_t r = 0; r < order.size(); r++) {
        const Clip& c = clips[order[r]];
        sum += c.bytes;
        const bool tooMany = p.keepLast > 0 && r >= size_t(p.keepLast);
        const bool tooOld = p.maxAgeDays > 0 && c.wallStart > 0 && now - c.wallStart > int64_t(p.maxAgeDays) * 86400;
        const bool tooBig = p.maxMB > 0 && sum > cap;
        if (tooMany || tooOld || tooBig) { del.push_back(order[r]); sum -= c.bytes; }   // a deleted clip no longer counts against the cap
    }
    return del;
}

static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// finds the fmt and data chunks; dataOff/dataLen relative to the file
static bool wavParse(FILE* f, int& rate, long& dataOff, uint32_t& dataLen) {
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
    bool fmt = false;
    rate = 0;
    for (int guard = 0; guard < 32; guard++) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) return false;
        const uint32_t len = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
            uint8_t b[16];
            if (fread(b, 1, 16, f) != 16) return false;
            if (rd16(b) != 1 || rd16(b + 2) != 1 || rd16(b + 14) != 16) return false;   // 16 bit mono PCM only
            rate = int(rd32(b + 4));
            fmt = rate > 0;
            if (len > 16) fseek(f, long(len - 16), SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!fmt) return false;
            dataOff = ftell(f);
            fseek(f, 0, SEEK_END);
            const long end = ftell(f);
            dataLen = std::min<uint32_t>(len, uint32_t(std::max(0L, end - dataOff)));   // a header written as 0 or too big is clamped to the file
            if (len == 0 && end > dataOff) dataLen = uint32_t(end - dataOff);
            return true;
        } else fseek(f, long(len) + long(len & 1), SEEK_CUR);
    }
    return false;
}

bool wavInfo(const std::string& path, int& rate, double& seconds) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    long off = 0; uint32_t n = 0;
    const bool ok = wavParse(f, rate, off, n);
    fclose(f);
    if (ok) seconds = double(n / 2) / rate;
    return ok;
}

bool wavReadMono16(const std::string& path, std::vector<int16_t>& pcm, int& rate) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    long off = 0; uint32_t n = 0;
    bool ok = wavParse(f, rate, off, n);
    if (ok) {
        std::vector<uint8_t> raw(n);
        fseek(f, off, SEEK_SET);
        ok = fread(raw.data(), 1, n, f) == n;
        if (ok) { pcm.resize(n / 2); for (size_t i = 0; i < pcm.size(); i++) pcm[i] = int16_t(rd16(&raw[2 * i])); }
    }
    fclose(f);
    return ok;
}

bool pcmHasSound(const std::vector<int16_t>& pcm, int minPeak) {
    for (int16_t s : pcm) if (std::abs(int(s)) >= minPeak) return true;
    return false;
}

void ClipHistory::add(Clip c) {
    if (c.path.empty() || removed_.count(c.path)) return;
    std::error_code ec;
    if (!fs::is_regular_file(c.path, ec)) return;
    if (c.mode.empty()) c.mode = mode_;
    if (!c.bytes) c.bytes = uint64_t(fs::file_size(c.path, ec));
    if (pins_.count(c.path)) c.pinned = true;
    for (auto& e : clips_) if (e.path == c.path) { c.pinned = c.pinned || e.pinned; e = c; return; }
    clips_.push_back(std::move(c));
}

size_t ClipHistory::scanFolder(const std::string& dir, const std::string& prefix) {
    size_t added = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".wav") continue;
        const std::string name = it->path().filename().string();
        if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string p = it->path().string();
        bool known = removed_.count(p) > 0;
        for (const auto& e : clips_) if (e.path == p) { known = true; break; }
        if (known) continue;
        int rate = 0; double sec = 0;
        if (!wavInfo(p, rate, sec)) continue;
        const auto mt = fs::last_write_time(*it, ec);
        const int64_t unixSec = ec ? 0 : int64_t(std::chrono::duration_cast<std::chrono::seconds>(mt.time_since_epoch()).count()) +
            int64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() -
                    std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now().time_since_epoch()).count());
        Clip c;
        c.path = p; c.mode = mode_; c.label = it->path().stem().string(); c.seconds = sec;
        c.wallStart = unixSec > 0 ? unixSec - int64_t(sec) : 0;
        const size_t before = clips_.size();
        add(std::move(c));
        added += clips_.size() - before;
    }
    return added;
}

std::vector<Clip> ClipHistory::clips() const {
    std::vector<Clip> v = clips_;
    std::stable_sort(v.begin(), v.end(), [](const Clip& a, const Clip& b) { return a.wallStart > b.wallStart; });
    return v;
}

bool ClipHistory::setPinned(const std::string& path, bool on) {
    if (on) pins_.insert(path); else pins_.erase(path);
    for (auto& e : clips_) if (e.path == path) { e.pinned = on; return true; }
    return false;
}

bool ClipHistory::remove(const std::string& path, bool deleteFile, bool force) {
    for (size_t i = 0; i < clips_.size(); i++) {
        if (clips_[i].path != path) continue;
        if (clips_[i].pinned && !force) return false;
        if (deleteFile) { std::error_code ec; fs::remove(path, ec); }
        clips_.erase(clips_.begin() + long(i));
        pins_.erase(path);
        removed_.insert(path);
        return true;
    }
    return false;
}

size_t ClipHistory::prune(int64_t now) {
    std::vector<size_t> del = clipsToDelete(clips_, policy_, now);
    std::vector<std::string> paths;
    for (size_t i : del) paths.push_back(clips_[i].path);
    for (const auto& p : paths) remove(p, true, false);
    return paths.size();
}

std::string ClipHistory::savePins() const {
    std::string s;
    for (const auto& p : pins_) s += p + "\n";
    return s;
}

void ClipHistory::loadPins(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) if (!line.empty()) setPinned(line, true);
}

uint64_t ClipHistory::totalBytes() const { uint64_t n = 0; for (const auto& c : clips_) n += c.bytes; return n; }

} // namespace dect2::voice
