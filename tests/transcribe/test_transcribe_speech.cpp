// Real English speech through the same path the app uses (Transcriber::feed at the sound card rate -> VoiceSegmenter -> whisper -> filters -> store).
// Needs DECT2_WHISPER_MODEL (ggml model) and DECT2_SPEECH_WAV (mono 16-bit wav of clear English speech); skipped (passes) without them.
#include "dect2/transcribe/transcriber.h"
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool readWav(const char* p, std::vector<float>& x, int& rate) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    size_t i = 12;
    rate = 0;
    while (i + 8 <= b.size()) {
        uint32_t sz; memcpy(&sz, &b[i + 4], 4);
        if (!memcmp(&b[i], "fmt ", 4)) { uint32_t r; memcpy(&r, &b[i + 12], 4); rate = (int)r; }
        if (!memcmp(&b[i], "data", 4)) {
            const size_t n = std::min<size_t>(sz, b.size() - i - 8) / 2;
            x.resize(n);
            for (size_t k = 0; k < n; k++) { int16_t s; memcpy(&s, &b[i + 8 + 2 * k], 2); x[k] = s / 32768.f; }
            return rate > 0;
        }
        i += 8 + sz + (sz & 1);
    }
    return false;
}

int main() {
    const char* mp = getenv("DECT2_WHISPER_MODEL");
    const char* wp = getenv("DECT2_SPEECH_WAV");
    if (!mp || !*mp || !wp || !*wp) { printf("transcribe_speech: skipped (set DECT2_WHISPER_MODEL and DECT2_SPEECH_WAV)\n"); return 0; }
    if (!whisperAvailable()) { printf("transcribe_speech: skipped (built without whisper)\n"); return 0; }
    std::string err;
    auto be = makeWhisperBackend(mp, 1, &err);
    CHECK(be != nullptr, "load: %s", err.c_str());
    if (!be) return 1;
    std::vector<float> sp;
    int rate = 0;
    if (!readWav(wp, sp, rate)) { printf("cannot read %s\n", wp); return 1; }
    std::mt19937 g(7);
    dect2::normal_distribution<float> nd(0.f, 1.f);
    struct Case { const char* name; float gain; float noise; bool tail; int outRate; };
    // gain: how loud the station is; noise: background hiss (a real FM receiver has one); outRate: the sound card rate the tap sees
    const Case cases[] = {
        {"clean, loud", 0.5f, 0.0f, true, 48000},
        {"quiet station", 0.05f, 0.0f, true, 48000},
        {"with FM hiss", 0.3f, 0.01f, true, 48000},
        {"with louder hiss", 0.5f, 0.03f, true, 48000},
        {"speech runs to the end, no pause after", 0.3f, 0.01f, false, 48000},
    };
    for (const Case& c : cases) {
        TranscriptStore store;
        Transcriber tr(store);
        TranscribeConfig cfg;
        cfg.enabled = true; cfg.threads = 4;
        tr.configure(cfg);
        tr.setBackend(be);
        tr.setSynchronous(true);
        SourceInfo src;
        src.key = 1; src.freqHz = 93.9e6; src.mode = "audio"; src.channel = "Audio"; src.domain = "general";
        std::vector<float> a;
        a.reserve(sp.size() + 4 * (size_t)c.outRate);
        for (size_t i = 0; i < (size_t)c.outRate; i++) a.push_back(c.noise * nd(g));   // 1 s of hiss first
        for (float v : sp) a.push_back(c.gain * v + c.noise * nd(g));
        if (c.tail) for (size_t i = 0; i < 3 * (size_t)c.outRate; i++) a.push_back(c.noise * nd(g));
        int64_t now = 1800000000000LL;
        for (size_t i = 0; i < a.size(); i += 512) { now += 10; tr.feed(src, a.data() + i, std::min<size_t>(512, a.size() - i), c.outRate, now); }
        if (!c.tail) tr.endSource(src.key, now);
        const TranscriberStats st = tr.stats();
        printf("[%s] segments %llu kept %llu filtered %llu (last: %s) errors %llu\n", c.name, (unsigned long long)st.segments, (unsigned long long)st.kept,
               (unsigned long long)st.filtered, st.lastFilter.c_str(), (unsigned long long)st.errors);
        for (const auto& e : store.find({})) printf("   text: \"%s\" (conf %.2f)\n", e.text.c_str(), e.confidence);
        CHECK(store.size() >= 1, "[%s] no transcript for clear English speech", c.name);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("transcribe_speech: ok\n");
    return 0;
}
