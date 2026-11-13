// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "Record audio": the recorder fed by a fake audio source (a fake AudioOut writer): duration, size, WAV/FLAC format, gating, split, stop and start again, stereo.
#include "dect2/audio/audio_rec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const int kRate = 48000;
// feeds `sec` seconds of a 1 kHz tone (amp 0 = silence), left only or L/R different when stereo, in 20 ms chunks, at about real time x10
static void feed(const void* src, double sec, float amp, bool stereo, int rate = kRate) {
    const int chunk = rate / 50;
    std::vector<float> b(chunk * 2);
    static long n = 0;
    for (int c = 0; c < (int)(sec * 50); c++) {
        for (int i = 0; i < chunk; i++, n++) {
            const float l = amp * (float)std::sin(2 * M_PI * 1000 * n / rate), r = stereo ? amp * (float)std::sin(2 * M_PI * 1500 * n / rate) : l;
            b[2 * i] = l; b[2 * i + 1] = r;
        }
        AudioRecorder::offer(src, b.data(), chunk, rate);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static uint32_t rd32(const std::vector<char>& v, size_t o) { return (uint8_t)v[o] | ((uint8_t)v[o + 1] << 8) | ((uint8_t)v[o + 2] << 16) | ((uint32_t)(uint8_t)v[o + 3] << 24); }
static std::vector<char> slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<char>((std::istreambuf_iterator<char>(f)), {}); }

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "onair_audio_rec_test";
    fs::remove_all(dir);
    AudioRecorder& rec = AudioRecorder::instance();
    int srcA = 0, srcB = 0;
    std::string err;
    AudioRecOptions o;

    // mono WAV, 2 s: the header, the size and the duration
    CHECK(rec.start((dir / "a.wav").string(), o, err), "start: %s", err.c_str());
    CHECK(rec.active(), "active");
    feed(&srcA, 0.5, 0.5f, false);
    feed(&srcB, 0.5, 0.5f, true, 44100);   // another output while A owns it: ignored
    feed(&srcA, 1.5, 0.5f, false);
    AudioRecStats st = rec.stats();
    CHECK(st.seconds > 1.5, "elapsed %.2f", st.seconds);
    rec.stop();
    CHECK(!rec.active(), "stopped");
    auto w = slurp((dir / "a.wav").string());
    CHECK(w.size() > 44 && !memcmp(w.data(), "RIFF", 4) && !memcmp(w.data() + 8, "WAVE", 4), "wav header");
    CHECK(rd32(w, 24) == (uint32_t)kRate && (uint8_t)w[22] == 1, "mono at 48 kHz (rate %u ch %d)", rd32(w, 24), (uint8_t)w[22]);
    CHECK(rd32(w, 40) == w.size() - 44 && rd32(w, 4) == w.size() - 8, "sizes patched: data %u of %zu", rd32(w, 40), w.size());
    const double secs = (w.size() - 44) / 2.0 / kRate;
    CHECK(secs > 1.9 && secs < 2.1, "duration %.3f s", secs);
    CHECK(st.rate == kRate, "rate in stats");

    // stereo WAV: kept as two channels
    CHECK(rec.start((dir / "s.wav").string(), o, err), "start stereo");
    feed(&srcA, 1.0, 0.4f, true);
    rec.stop();
    w = slurp((dir / "s.wav").string());
    CHECK((uint8_t)w[22] == 2 && rd32(w, 28) == (uint32_t)kRate * 4, "stereo header (ch %d)", (uint8_t)w[22]);
    CHECK((w.size() - 44) / 4.0 / kRate > 0.9, "stereo duration %.3f", (w.size() - 44) / 4.0 / kRate);

    // FLAC, mono and stereo: the magic, the channel count and the sample total in STREAMINFO
    for (int stereo = 0; stereo < 2; stereo++) {
        o.format = AudioRecFormat::Flac;
        const std::string p = (dir / (stereo ? "s.flac" : "m.flac")).string();
        CHECK(rec.start(p, o, err), "start flac");
        feed(&srcA, 1.0, 0.3f, stereo);
        rec.stop();
        auto fl = slurp(p);
        CHECK(fl.size() > 42 && !memcmp(fl.data(), "fLaC", 4), "flac magic");
        const uint64_t v = ((uint64_t)(uint8_t)fl[18] << 56) | ((uint64_t)(uint8_t)fl[19] << 48) | ((uint64_t)(uint8_t)fl[20] << 40) | ((uint64_t)(uint8_t)fl[21] << 32) | ((uint64_t)(uint8_t)fl[22] << 24) | ((uint64_t)(uint8_t)fl[23] << 16) | ((uint64_t)(uint8_t)fl[24] << 8) | (uint8_t)fl[25];
        CHECK(((v >> 41) & 7) == (uint64_t)stereo, "flac channels-1 = %llu", (unsigned long long)((v >> 41) & 7));
        CHECK(((v >> 44) == (uint64_t)kRate) && (v & 0xFFFFFFFFFULL) > 0.9 * kRate, "flac rate %llu samples %llu", (unsigned long long)(v >> 44), (unsigned long long)(v & 0xFFFFFFFFFULL));
    }

    // gating: tone, 4 s of silence, tone -> the silence is cut out (one file)
    o = AudioRecOptions(); o.gated = true;
    CHECK(rec.start((dir / "g.wav").string(), o, err), "start gated");
    feed(&srcA, 1.0, 0.5f, false);
    feed(&srcA, 4.0, 0.f, false);
    feed(&srcA, 1.0, 0.5f, false);
    rec.stop();
    w = slurp((dir / "g.wav").string());
    const double gs = (w.size() - 44) / 2.0 / kRate;
    CHECK(gs > 2.5 && gs < 4.6, "gated duration %.2f s (6 s in, silence cut)", gs);

    // split: two transmissions 4 s apart -> two files
    o.split = true;
    CHECK(rec.start((dir / "t.wav").string(), o, err), "start split");
    feed(&srcA, 1.0, 0.5f, false);
    feed(&srcA, 5.0, 0.f, false);
    feed(&srcA, 1.0, 0.5f, false);
    rec.stop();
    CHECK(rec.stats().files == 2 && fs::exists(dir / "t.wav") && fs::exists(dir / "t_2.wav"), "split into %d files", rec.stats().files);

    // a second start after a stop works; a rate change (another mode) continues in a new file
    o = AudioRecOptions();
    CHECK(rec.start((dir / "r.wav").string(), o, err), "start again");
    feed(&srcA, 0.6, 0.5f, false);
    feed(&srcA, 0.6, 0.5f, false, 44100);
    rec.stop();
    CHECK(rec.stats().files == 2 && fs::exists(dir / "r_2.wav"), "rate change -> %d files", rec.stats().files);
    w = slurp((dir / "r_2.wav").string());
    CHECK(w.size() > 44 && rd32(w, 24) == 44100, "second file at 44.1 kHz");

    // a bad folder is reported
    CHECK(!rec.start((dir / "r_2.wav" / "x" / "a.wav").string(), o, err)   /* below a plain file */ && !err.empty(), "bad folder reported");
    CHECK(AudioRecorder::fileName("fm", 93.9e6, AudioRecFormat::Flac).find("onair_audio_fm_93.900MHz_") == 0, "file name");

    fs::remove_all(dir);
    printf(fails ? "FAILED\n" : "audio_rec OK\n");
    return fails ? 1 : 0;
}
