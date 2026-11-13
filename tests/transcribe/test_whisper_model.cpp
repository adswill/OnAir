// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The real speech engine on generated audio (needs a model: DECT2_WHISPER_MODEL=/path/ggml-tiny.en.bin; without it the test is skipped and passes).
// 1. Silence and noise must not come out as text (no hallucination reaches the store). 2. The real-time factor on a generated speech-like signal.
#include "dect2/transcribe/transcriber.h"
#include "dect2/common/stable_random.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void wav16(const std::string& p, const std::vector<float>& x, int rate) {
    std::ofstream f(p, std::ios::binary);
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); w32(36 + (uint32_t)x.size() * 2); f.write("WAVEfmt ", 8); w32(16); w16(1); w16(1); w32((uint32_t)rate); w32((uint32_t)rate * 2); w16(2); w16(16);
    f.write("data", 4); w32((uint32_t)x.size() * 2);
    for (float v : x) { const int16_t s = (int16_t)std::lround(std::max(-1.f, std::min(1.f, v)) * 32767); f.write((const char*)&s, 2); }
}

// A synthetic "voice": a buzz of changing pitch through moving formants, syllables at 4 Hz with pauses: speech-like in rhythm and spectrum, not words.
static std::vector<float> synth(double sec, int rate) {
    std::vector<float> v((size_t)(sec * rate));
    double ph = 0;
    for (size_t i = 0; i < v.size(); i++) {
        const double t = (double)i / rate;
        const double f0 = 110 + 25 * std::sin(2 * M_PI * 0.6 * t);
        ph += 2 * M_PI * f0 / rate;
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.5 * t)) * (std::sin(2 * M_PI * 0.3 * t) > -0.6 ? 1.0 : 0.0);
        double s = 0;
        const double f1 = 500 + 250 * std::sin(2 * M_PI * 1.3 * t), f2 = 1500 + 600 * std::sin(2 * M_PI * 0.9 * t);
        for (int h = 1; h < 30; h++) {
            const double fh = f0 * h;
            if (fh > rate / 2.2) break;
            s += std::sin(h * ph) / h * (std::exp(-std::pow((fh - f1) / 150, 2)) + 0.6 * std::exp(-std::pow((fh - f2) / 250, 2)) + 0.05);
        }
        v[i] = (float)(0.25 * syl * s);
    }
    return v;
}

int main() {
    const char* mp = getenv("DECT2_WHISPER_MODEL");
    if (!mp || !*mp) { printf("whisper_model: skipped (set DECT2_WHISPER_MODEL to a ggml model file)\n"); return 0; }
    if (!whisperAvailable()) { printf("whisper_model: skipped (built without whisper)\n"); return 0; }
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    auto be = makeWhisperBackend(mp, 1, &err);
    const double loadSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(be != nullptr, "load: %s", err.c_str());
    if (!be) return 1;
    printf("model loaded in %.2f s (%s)\n", loadSec, be->englishOnly() ? "English only" : "multilingual");

    TranscriptStore store;
    Transcriber tr(store);
    TranscribeConfig cfg;
    cfg.enabled = true;
    cfg.threads = 4;
    tr.configure(cfg);
    tr.setBackend(be);
    tr.setSynchronous(true);
    SourceInfo src;
    src.key = 1; src.freqHz = 118.7e6; src.mode = "airband"; src.channel = "118.700";
    int64_t now = 1800000000000LL;
    auto feed = [&](const std::vector<float>& a) {
        for (size_t i = 0; i < a.size(); i += 160) { now += 20; tr.feed(src, a.data() + i, std::min<size_t>(160, a.size() - i), 8000, now); }
    };
    std::mt19937 g(5);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    // silence with a little noise: never opens
    { std::vector<float> a(8000 * 20); for (auto& x : a) x = 0.001f * nd(g); feed(a); }
    // loud broadband noise (a squelch that is open on static): opens, goes to the engine, must not come out as text
    for (float amp : {0.05f, 0.2f}) {
        std::vector<float> a(8000 * 8), z(8000 * 2, 0.f);
        for (auto& x : a) x = amp * nd(g);
        feed(a); feed(z);
    }
    // a tone (a data burst or a whistle)
    { std::vector<float> a(8000 * 5), z(8000 * 2, 0.f); for (size_t i = 0; i < a.size(); i++) a[i] = 0.3f * (float)std::sin(2 * M_PI * 1000 * i / 8000.0); feed(a); feed(z); }
    const TranscriberStats st = tr.stats();
    printf("noise and tone: %llu transmissions sent to the engine, %llu kept, %llu filtered (last: %s)\n", (unsigned long long)st.segments, (unsigned long long)st.kept,
           (unsigned long long)st.filtered, st.lastFilter.c_str());
    for (const auto& e : store.find({})) printf("  kept text: \"%s\" (p %.2f)\n", e.text.c_str(), e.confidence);
    CHECK(store.size() == 0, "no text from silence, noise or a tone (%zu entries)", store.size());

    // real-time factor on 20 s of generated speech-like audio, 16 kHz, as the segmenter would hand it over (the file is written for the record)
    const std::vector<float> sp = synth(20.0, 16000);
    const std::string wavPath = (std::filesystem::temp_directory_path() / "onair_whisper_synth.wav").string();
    wav16(wavPath, sp, 16000);
    SpeechOptions so; so.threads = 4; so.language = "en";
    double best = 1e9;
    std::string text;
    for (int k = 0; k < 3; k++) {
        const auto a = std::chrono::steady_clock::now();
        const SpeechResult r = be->transcribe(sp.data(), sp.size(), so);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        best = std::min(best, el);
        text = r.text;
        CHECK(r.ok, "transcribe: %s", r.error.c_str());
    }
    printf("real-time factor (4 threads, 20 s of generated audio %s): %.3f (%.2f s for 20 s)\n", wavPath.c_str(), best / 20.0, best);
    printf("engine text on the synthetic voice: \"%s\"\n", text.c_str());
    so.threads = 1;
    {
        const auto a = std::chrono::steady_clock::now();
        be->transcribe(sp.data(), sp.size(), so);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
        printf("real-time factor (1 thread): %.3f\n", el / 20.0);
    }
    std::error_code ec;
    std::filesystem::remove(wavPath, ec);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("whisper_model: ok\n");
    return 0;
}
