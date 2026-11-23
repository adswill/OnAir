// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SynthMediaAudio streams a long sound file (the sound of the analog TV transmitter, FM and general test signals) instead of holding it whole
// and instead of cutting it at 900 s. The test files are generated here: a mono 16-bit WAV with a saw-tooth of 7 s period, so the value at any
// instant says where in the file the sound is.
//   long_audio_streams_without_cap       a 2 h file is carried to its end and in step
//   memory_stays_flat_over_long_file     30 minutes of it played: the window stays a few hundred kB, the process does not grow
//   seek_and_loop_keep_sound_in_sync     a seek, a loop, a stop and start, two source sample rates
#include "dect2/source/synth_media.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif
using namespace dect2;
#include "../common/test_util.h"

static std::string g_dir;

static double saw(double t) { return 0.8 * (std::fmod(t, 7.0) / 7.0 - 0.5); }
// what mono() and read() give for the saw at time t: FFmpeg spreads a mono file at -3 dB over both channels
static double expectAt(double t) { return 0.70710678 * saw(t); }
// away from the jump of the saw-tooth (the resampler rings there) and from the 30 ms around the loop point
static bool checkable(double t, double length) {
    const double f = std::fmod(t, 7.0);
    if (f < 0.05 || f > 6.95) return false;
    const double l = std::fmod(t, length);
    return l > 0.03 && l < length - 0.03;
}

static std::string writeSaw(const char* name, int rate, double seconds) {
    const std::string p = g_dir + "/" + name;
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return "";
    const uint32_t n = (uint32_t)std::llround(seconds * rate), ds = n * 2, rs = 36 + ds, fl = 16, sr = (uint32_t)rate, br = (uint32_t)rate * 2;
    const uint16_t ch = 1, bps = 16, fmt = 1, ba = 2;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    std::vector<int16_t> blk(1 << 16);
    for (uint32_t k = 0; k < n;) {
        const uint32_t m = std::min<uint32_t>((uint32_t)blk.size(), n - k);
        for (uint32_t i = 0; i < m; i++) blk[i] = (int16_t)std::lround(saw((double)(k + i) / rate) * 32768.0);
        fwrite(blk.data(), 2, m, f);
        k += m;
    }
    fclose(f);
    return p;
}

static SynthMedia media(const std::string& wav) { SynthMedia m; m.audio = wav; return m; }

// reads `seconds` from the current position and counts the frames that are not what the saw says at startT + i / 48000 (looped at `length`)
static int wrongFrames(SynthMediaAudio& a, double startT, double seconds, double length, int* checked = nullptr) {
    int bad = 0, seen = 0;
    std::vector<float> buf(2 * 4096);
    const long total = (long)(seconds * 48000), blk = 4096;
    for (long k = 0; k < total; k += blk) {
        const int m = (int)std::min<long>(blk, total - k);
        a.read(buf.data(), m);
        for (int i = 0; i < m; i++) {
            const double t = std::fmod(startT + (double)(k + i) / 48000.0, length);
            if (!checkable(t, length)) continue;
            seen++;
            if (std::fabs(buf[2 * i] - expectAt(t)) > 0.003 || std::fabs(buf[2 * i + 1] - expectAt(t)) > 0.003) bad++;
        }
    }
    if (checked) *checked = seen;
    return bad;
}

static long peakRssKb() {
#ifndef _WIN32
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return (long)(u.ru_maxrss / 1024);
#else
    return (long)u.ru_maxrss;
#endif
#else
    return 0;
#endif
}

static void testLongAndFlat() {
    const std::string wav = writeSaw("long2h.wav", 4000, 7200);
    CHECK(!wav.empty(), "could not write the 2 h file");
    if (wav.empty()) return;
    const long rss0 = peakRssKb();
    SynthMediaAudio a;
    std::string err;
    CHECK(a.loadStreaming(media(wav), kSynthMediaMaxSoundSeconds, err), "load: %s", err.c_str());   // long_audio_streams_without_cap
    CHECK(a.ok() && a.streaming(), "a 2 h file is not streamed");
    CHECK(std::fabs(a.seconds() - 7200) < 1.0, "length %.1f s", a.seconds());
    int seen = 0;
    CHECK(wrongFrames(a, 0, 5, 7200, &seen) == 0 && seen > 100000, "the start is not in step (%d checked)", seen);
    a.seek(6100.25);   // past 900 s and past 1 h: the old cap would have looped long before
    CHECK(wrongFrames(a, 6100.25, 4, 7200, &seen) == 0 && seen > 100000, "6100 s is not in step (%d checked)", seen);
    // memory_stays_flat_over_long_file: 30 minutes played from 1 h
    a.seek(3600);
    size_t worst = 0;
    std::vector<float> buf(2 * 4096);
    for (long k = 0; k < 1800L * 48000; k += 4096) { a.read(buf.data(), 4096); worst = std::max(worst, a.bufferedBytes()); }
    printf("window at most %zu kB, process grew %ld kB\n", worst / 1024, peakRssKb() - rss0);
    CHECK(worst < 4u * 1024 * 1024, "the window grew to %zu bytes", worst);
    CHECK(peakRssKb() - rss0 < 150L * 1024 || peakRssKb() == 0, "the process grew by %ld kB (a whole 2 h would be 2.7 GB)", peakRssKb() - rss0);
    CHECK(wrongFrames(a, 5400, 3, 7200, &seen) == 0 && seen > 50000, "after 30 min the sound is not in step (%d checked)", seen);
    std::filesystem::remove(wav);
}

static void testSeekLoopStop(int rate, const char* name) {
    const std::string wav = writeSaw(name, rate, 300);
    CHECK(!wav.empty(), "could not write %s", name);
    if (wav.empty()) return;
    std::string err;
    {
        SynthMediaAudio a;
        CHECK(a.loadStreaming(media(wav), 60, err), "load %d Hz: %s", rate, err.c_str());
        CHECK(a.streaming(), "%d Hz: a 300 s file over a 60 s limit was not streamed", rate);
        int seen = 0;
        CHECK(wrongFrames(a, 0, 3, 300, &seen) == 0 && seen > 50000, "%d Hz: start (%d checked)", rate, seen);
        // seek: forwards, then backwards, by time (mono) and by position (read)
        const double ts[] = {150.3, 10.1, 299.5, 75.2, 0.5};
        for (double t : ts) CHECK(std::fabs(a.mono(t) - expectAt(t)) < 0.003 || !checkable(t, 300), "%d Hz: mono(%.1f) = %.4f, want %.4f", rate, t, a.mono(t), expectAt(t));
        a.seek(200.2);
        CHECK(wrongFrames(a, 200.2, 2, 300, &seen) == 0 && seen > 30000, "%d Hz: after seek(200.2) (%d checked)", rate, seen);
        // loop: 2 s before the end, 5 s on: the sound goes on from the start, in step with the saw
        a.seek(298);
        CHECK(wrongFrames(a, 298, 5, 300, &seen) == 0 && seen > 100000, "%d Hz: across the loop (%d checked)", rate, seen);
        // a second loop, after reading on
        a.seek(299.9);
        CHECK(wrongFrames(a, 299.9, 1, 300, &seen) == 0, "%d Hz: second loop", rate);
        // mono() after read() and read() after mono(): the two cursors do not disturb each other
        a.seek(40.1);
        const float m = a.mono(250.4);
        CHECK(std::fabs(m - expectAt(250.4)) < 0.003, "%d Hz: mono after seek", rate);
        CHECK(wrongFrames(a, 40.1, 1, 300, &seen) == 0 && seen > 20000, "%d Hz: read after mono (%d checked)", rate, seen);
    }
    {   // stop / start: a new object (the generator is rebuilt) plays from the start again
        SynthMediaAudio b;
        CHECK(b.loadStreaming(media(wav), 60, err), "reload: %s", err.c_str());
        int seen = 0;
        CHECK(wrongFrames(b, 0, 2, 300, &seen) == 0 && seen > 30000, "%d Hz: restart is not at the start (%d checked)", rate, seen);
    }
    {   // a file within the limit is decoded whole, as before
        SynthMediaAudio w;
        CHECK(w.loadStreaming(media(wav), 900, err) && !w.streaming() && w.ok(), "%d Hz: a short file was streamed", rate);
        int seen = 0;
        w.seek(298);
        CHECK(wrongFrames(w, 298, 5, 300, &seen) == 0 && seen > 100000, "%d Hz: whole file across the loop (%d checked)", rate, seen);
    }
    std::filesystem::remove(wav);
}

int main() {
    g_dir = (std::filesystem::temp_directory_path() / ("onair_synth_audio_stream_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
    std::filesystem::create_directories(g_dir);
    testSeekLoopStop(4000, "s4000.wav");
    testSeekLoopStop(11025, "s11025.wav");   // sample-rate change: another source rate gives the same sound positions
    testLongAndFlat();
    std::error_code ec; std::filesystem::remove_all(g_dir, ec);
    return testSummary("synth_audio_stream");
}
