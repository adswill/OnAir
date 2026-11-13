// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Voice clip history (core/include/dect2/voice/clip_history.h): the retention rules (count, age, size, pinned), clip metadata, WAV helpers,
// folder scan and pin persistence. Pure: temp files only.
#include "dect2/voice/clip_history.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
using namespace dect2::voice;
#include "../common/test_util.h"
namespace fs = std::filesystem;

static void wav(const std::string& path, int rate, size_t n, int amp, bool zeroHeader = false) {
    FILE* f = fopen(path.c_str(), "wb");
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) fputc(int((v >> (8 * i)) & 255), f); };
    auto u16 = [&](uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); };
    const uint32_t bytes = uint32_t(n * 2);
    fwrite("RIFF", 1, 4, f); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(uint32_t(rate)); u32(uint32_t(rate) * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(zeroHeader ? 0 : bytes);
    for (size_t i = 0; i < n; i++) u16(uint16_t(int16_t(amp * ((i / 20) % 2 ? 1 : -1))));
    fclose(f);
}
static Clip mk(int id, int64_t t, uint64_t bytes, bool pin = false) {
    Clip c; c.path = "c" + std::to_string(id); c.wallStart = t; c.bytes = bytes; c.pinned = pin; return c;
}
static bool has(const std::vector<size_t>& v, size_t i) { for (size_t x : v) if (x == i) return true; return false; }

int main() {
    const int64_t now = 100 * 86400;
    // count: keep the newest 3 of 6; pinned ones are exempt and not counted
    {
        std::vector<Clip> c;
        for (int i = 0; i < 6; i++) c.push_back(mk(i, now - i * 100, 1000));
        ClipPolicy p; p.keepLast = 3;
        auto d = clipsToDelete(c, p, now);
        CHECK(d.size() == 3 && has(d, 3) && has(d, 4) && has(d, 5), "keepLast 3: deleted %zu", d.size());
        c[5].pinned = true;
        d = clipsToDelete(c, p, now);
        CHECK(d.size() == 2 && has(d, 3) && has(d, 4), "pinned not counted: deleted %zu", d.size());
        CHECK(!has(d, 5), "a pinned clip was deleted");
        p.keepLast = 0;   // keep all
        CHECK(clipsToDelete(c, p, now).empty(), "keepLast 0 must keep everything");
        // unsorted input still keeps the newest
        std::vector<Clip> u = {mk(0, now - 500, 1), mk(1, now - 10, 1), mk(2, now - 300, 1)};
        p.keepLast = 1;
        d = clipsToDelete(u, p, now);
        CHECK(d.size() == 2 && !has(d, 1), "newest kept on unsorted input");
    }
    // age
    {
        std::vector<Clip> c = {mk(0, now - 86400 * 1, 1), mk(1, now - 86400 * 5, 1), mk(2, now - 86400 * 20, 1, true), mk(3, now - 86400 * 9, 1)};
        ClipPolicy p; p.keepLast = 0; p.maxAgeDays = 7;
        auto d = clipsToDelete(c, p, now);
        CHECK(d.size() == 1 && has(d, 3), "age 7 days: deleted %zu", d.size());
    }
    // size cap: 1 MB cap, clips of 400 kB: newest two fit, the rest go; a pinned clip does not count
    {
        std::vector<Clip> c;
        for (int i = 0; i < 5; i++) c.push_back(mk(i, now - i * 10, 400 * 1024));
        ClipPolicy p; p.keepLast = 0; p.maxMB = 1;
        auto d = clipsToDelete(c, p, now);
        CHECK(d.size() == 3 && !has(d, 0) && !has(d, 1), "size cap: deleted %zu", d.size());
        c[0].pinned = true;
        d = clipsToDelete(c, p, now);
        CHECK(d.size() == 2 && !has(d, 0) && !has(d, 1) && !has(d, 2), "size cap with pin: deleted %zu", d.size());
    }
    // clamping
    {
        ClipPolicy p; p.keepLast = 5000; p.maxAgeDays = -3; p.maxMB = 999999;
        const ClipPolicy q = ClipPolicy::clamped(p);
        CHECK(q.keepLast == 1000 && q.maxAgeDays == 0 && q.maxMB == 100000, "clamp %d %d %d", q.keepLast, q.maxAgeDays, q.maxMB);
        p.keepLast = -1;
        CHECK(ClipPolicy::clamped(p).keepLast == 0, "negative keepLast is keep all");
    }
    // files: metadata, WAV helpers, prune deletes file + entry, pin, scan, persistence
    const fs::path dir = fs::temp_directory_path() / "onair-test-voice-clips";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "2026-01-01", ec);
    {
        const std::string a = (dir / "a.wav").string(), z = (dir / "zero.wav").string(), h = (dir / "hdr0.wav").string();
        wav(a, 8000, 16000, 5000); wav(z, 8000, 8000, 0); wav(h, 8000, 4000, 3000, true);
        int rate = 0; double sec = 0;
        CHECK(wavInfo(a, rate, sec) && rate == 8000 && sec > 1.99 && sec < 2.01, "wavInfo %d %.3f", rate, sec);
        CHECK(wavInfo(h, rate, sec) && sec > 0.49 && sec < 0.51, "zero data length in the header: %.3f s", sec);
        std::vector<int16_t> pcm;
        CHECK(wavReadMono16(a, pcm, rate) && pcm.size() == 16000 && pcmHasSound(pcm), "read a.wav");
        CHECK(wavReadMono16(z, pcm, rate) && !pcmHasSound(pcm), "zeros must not count as sound");
        { std::ofstream(dir / "junk.wav") << "not a wav"; }
        CHECK(!wavInfo((dir / "junk.wav").string(), rate, sec), "junk read as a WAV");
        fs::remove(dir / "junk.wav", ec); fs::remove(z, ec); fs::remove(h, ec);

        ClipHistory H("iridium");
        for (int i = 0; i < 6; i++) {
            const std::string p = (dir / "2026-01-01" / ("c" + std::to_string(i) + ".wav")).string();
            wav(p, 8000, 800, 4000);
            Clip c; c.path = p; c.label = "1621.250 MHz down"; c.wallStart = now - i * 60; c.seconds = 0.1;
            H.add(c);
        }
        H.add(Clip{(dir / "missing.wav").string(), "", "", now, 1, 0, false});
        CHECK(H.size() == 6, "entries %zu (a missing file must not be added)", H.size());
        const auto list = H.clips();
        CHECK(list.size() == 6 && list[0].wallStart > list[5].wallStart && list[0].mode == "iridium" && list[0].bytes == 44 + 1600 && list[0].label == "1621.250 MHz down", "metadata / order");
        H.setPolicy(ClipPolicy{3, 0, 0});
        CHECK(H.setPinned(list[5].path, true), "pin");
        CHECK(!H.remove(list[5].path), "a pinned clip was removed without force");
        const size_t n = H.prune(now);
        CHECK(n == 2 && H.size() == 4, "pruned %zu, left %zu", n, H.size());
        CHECK(fs::exists(list[5].path) && fs::exists(list[0].path) && !fs::exists(list[3].path) && !fs::exists(list[4].path), "files do not match the entries");
        // re-adding a deleted path (a stale call entry) does nothing
        H.add(Clip{list[3].path, "", "", now, 1, 0, false});
        CHECK(H.size() == 4, "a deleted clip came back");
        // pins persist
        const std::string pins = H.savePins();
        ClipHistory G("iridium");
        G.loadPins(pins);
        G.scanFolder(dir.string());
        size_t pinned = 0;
        for (const auto& c : G.clips()) pinned += c.pinned;
        CHECK(G.size() == 5 && pinned == 1, "scan found %zu clips, %zu pinned (a.wav + 4 left)", G.size(), pinned);
        for (const auto& c : G.clips()) CHECK(c.seconds > 0 && c.wallStart > 0 && !c.label.empty(), "scanned clip metadata");
        CHECK(G.scanFolder(dir.string()) == 0, "second scan added clips");
        CHECK(G.remove(list[0].path) && !fs::exists(list[0].path), "remove deletes the file");
        ClipHistory F("iridium");
        F.scanFolder(dir.string(), "c");   // prefix filter: only c*.wav
        CHECK(F.size() == 3, "prefix scan found %zu", F.size());
    }
    fs::remove_all(dir, ec);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
