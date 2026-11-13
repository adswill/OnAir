// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound-card IF/AF input (issue #137): a real tone in a WAV file goes through the sound-card source path (AudioIn File ->
// audioIfProcess) and must land at (tone - IF offset) Hz in the IQ the decoders get; sideband inversion mirrors it; the rates 8k..192k work;
// an FT8-like and an RTTY-like signal shifted by the offset are seen at the right frequency; DC removal, channel choice, gain.
#include "dect2/source/audio_if.h"
#include "dect2/common/mathconst.h"
#include <chrono>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static const double kPi = dect2::math::kPi;

// 16-bit mono PCM WAV of a synthesized real signal: sum of tones (hz, amp), with a start-stop keying option for the RTTY-like case
static std::string writeWav(const std::string& name, int rate, double seconds, const std::vector<std::pair<double, double>>& tones, double dc = 0) {
    const std::string path = (std::filesystem::temp_directory_path() / name).string();
    const int n = (int)(rate * seconds);
    std::vector<int16_t> d((size_t)n);
    for (int i = 0; i < n; i++) {
        double v = dc;
        for (const auto& t : tones) v += t.second * std::sin(2 * kPi * t.first * i / rate);
        d[(size_t)i] = (int16_t)std::lround(std::max(-1.0, std::min(1.0, v)) * 30000);
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return {};
    const uint32_t dataBytes = (uint32_t)n * 2, riff = 36 + dataBytes, fmtLen = 16, sr = (uint32_t)rate, br = sr * 2;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f); fwrite(d.data(), 2, d.size(), f);
    fclose(f);
    return path;
}

// the IQ the source gives for a WAV: through makeAudioSource ("file:" serial), exactly as a sound card would feed it
static std::vector<cf32> capture(const std::string& wav, const TuneSettings& t, double& rate, size_t want) {
    DeviceInfo d; d.kind = DeviceInfo::Audio; d.serial = "file:" + wav; d.name = "Sound card: test file";
    auto src = makeAudioSource(d);
    IqRing ring(1u << 20);
    std::string err;
    std::vector<cf32> out;
    if (!src->start(t, ring, err)) { printf("start: %s\n", err.c_str()); return out; }
    rate = src->sampleRate();
    std::vector<cf32> x(8192);
    const auto t0 = std::chrono::steady_clock::now();
    while (out.size() < want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
        const size_t g = ring.read(x.data(), x.size());
        out.insert(out.end(), x.begin(), x.begin() + (long)g);
        if (!g) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    src->stop();
    return out;
}

static double power(const std::vector<cf32>& x, size_t skip, double hz, double rate) {   // power at hz (signed) by a single-bin DFT
    cf32 acc = 0;
    for (size_t i = skip; i < x.size(); i++) {
        const double w = -2 * kPi * hz * (double)(i - skip) / rate;
        acc += x[i] * cf32((float)std::cos(w), (float)std::sin(w));
    }
    return std::norm(acc) / ((double)(x.size() - skip) * (double)(x.size() - skip));
}

// where the strongest energy sits (a scan of the bins around the expectation, 1 Hz apart)
static double peakHz(const std::vector<cf32>& x, size_t skip, double lo, double hi, double rate) {
    double best = -1, at = lo;
    for (double f = lo; f <= hi; f += 1) { const double p = power(x, skip, f, rate); if (p > best) { best = p; at = f; } }
    return at;
}

static TuneSettings cfg(int mode, double ifHz, int rate, bool invert = false) {
    TuneSettings t;
    t.radio["audio_iq"] = std::to_string(mode);
    t.radio["audio_if_hz"] = std::to_string(ifHz);
    t.radio["audio_rate"] = std::to_string(rate);
    t.radio["audio_invert"] = invert ? "1" : "0";
    return t;
}

// A 12 kHz IF carrying a tone 1000 Hz above the IF (USB): after the shift it is at +1000 Hz; inverted, at -1000 Hz.
static void testShiftAndSideband() {
    const std::string w = writeWav("onair_if_12k.wav", 48000, 3, {{13000, 0.5}});
    double rate = 0;
    auto x = capture(w, cfg(3, 12000, 48000), rate, 40000);
    CHECK(x.size() > 30000 && std::fabs(rate - 48000) < 1, "captured %zu at %g", x.size(), rate);
    const size_t skip = 2000;
    const double f = peakHz(x, skip, -3000, 3000, rate);
    CHECK(std::fabs(f - 1000) < 3, "Real (mono) with IF 12 kHz: tone at %g Hz (want +1000)", f);
    const double image = power(x, skip, -1000, rate), wanted = power(x, skip, 1000, rate);
    CHECK(wanted > 100 * image, "the mirror image is suppressed (%g vs %g)", wanted, image);

    auto y = capture(w, cfg(3, 12000, 48000, true), rate, 40000);
    const double fi = peakHz(y, skip, -3000, 3000, rate);
    CHECK(std::fabs(fi - (-1000)) < 3, "sideband inversion: tone at %g Hz (want -1000)", fi);
    auto z = capture(w, cfg(2, 12000, 48000), rate, 40000);   // the LSB mode is the same inversion
    CHECK(std::fabs(peakHz(z, skip, -3000, 3000, rate) - (-1000)) < 3, "LSB mode mirrors too");
    auto u = capture(w, cfg(1, 0, 48000), rate, 40000);       // no offset: the tone stays at its audio frequency
    CHECK(std::fabs(peakHz(u, skip, 11000, 15000, rate) - 13000) < 3, "no offset keeps the audio frequency");
}

// the dial mapping: an IF at 455 kHz in a 192 kHz card does not fit (Nyquist 96 kHz): the offset is clamped, never wraps to nonsense
static void testClamp() {
    TuneSettings t = cfg(3, 455000, 192000);
    const AudioIfConfig c = audioIfConfigOf(t);
    CHECK(std::fabs(c.ifHz) <= 96000, "IF clamped to Nyquist (%g)", c.ifHz);
    TuneSettings r; r.radio["audio_rate"] = "1000000";
    CHECK(audioIfConfigOf(r).rate == 192000, "rate clamped to 192 kHz");
    r.radio["audio_rate"] = "100";
    CHECK(audioIfConfigOf(r).rate == 8000, "rate clamped to 8 kHz");
}

// all the offered rates 8k..192k: an IF tone at a quarter of the rate comes out 500 Hz above 0 when the IF is the carrier
static void testRates() {
    int n = 0;
    const int* r = audioRateChoices(n);
    CHECK(n >= 10 && r[0] == 8000 && r[n - 1] == 192000, "rate list %d", n);
    for (int i = 0; i < n; i++) {
        const int rate = r[i];
        const double ifHz = rate / 4.0;
        const std::string w = writeWav("onair_if_rate.wav", rate, 1.5, {{ifHz + 500, 0.5}});
        double got = 0;
        auto x = capture(w, cfg(3, ifHz, rate), got, (size_t)rate / 2 + 2000);
        const double f = peakHz(x, 1500, 400, 600, got);
        CHECK(std::fabs(got - rate) < 1 && std::fabs(f - 500) < 4, "rate %d: tone at %g Hz (want 500), source rate %g", rate, f, got);
    }
}

// FT8-like (8 tones 6.25 Hz apart around 1500 Hz audio) and RTTY-like (mark/space 170 Hz apart) signals on a 12 kHz IF carry their shape
// to the decoders: the pair of tones keeps its spacing and sits around the offset-corrected centre
static void testModeSignals() {
    const double ifHz = 12000;
    // FT8: tones at ifHz + 1500 + k*6.25 (k = 0..7, all present for the test)
    std::vector<std::pair<double, double>> ft8;
    for (int k = 0; k < 8; k++) ft8.push_back({ifHz + 1500 + 6.25 * k, 0.05});
    double rate = 0;
    auto x = capture(writeWav("onair_if_ft8.wav", 48000, 4, ft8), cfg(3, ifHz, 48000), rate, 100000);
    const double c = peakHz(x, 3000, 1450, 1600, rate);
    CHECK(c >= 1499 && c <= 1545, "FT8-like tones start at %g Hz (want ~1500..1544)", c);
    CHECK(power(x, 3000, 1500, rate) > 20 * power(x, 3000, -1500, rate), "FT8-like: only the wanted sideband");
    // RTTY: mark 2125 / space 2295 on a 12 kHz IF (LSB receiver output would be mirrored: invert gives the same decode frequencies mirrored)
    auto y = capture(writeWav("onair_if_rtty.wav", 48000, 3, {{ifHz + 2125, 0.3}, {ifHz + 2295, 0.3}}), cfg(3, ifHz, 48000), rate, 80000);
    const double m = peakHz(y, 3000, 2100, 2150, rate), s = peakHz(y, 3000, 2270, 2320, rate);
    CHECK(std::fabs(m - 2125) < 3 && std::fabs(s - 2295) < 3 && std::fabs((s - m) - 170) < 4, "RTTY-like mark %g space %g", m, s);
}

// DC removal, left/right choice, gain: direct on audioIfProcess
static void testProcess() {
    const int rate = 48000, n = 24000;
    std::vector<float> st((size_t)n * 2);
    for (int i = 0; i < n; i++) {
        st[(size_t)i * 2] = (float)(0.2 + 0.3 * std::sin(2 * kPi * 1000.0 * i / rate));   // left: tone with a DC offset
        st[(size_t)i * 2 + 1] = (float)(0.3 * std::sin(2 * kPi * 2000.0 * i / rate));     // right: another tone
    }
    std::vector<cf32> iq((size_t)n);
    AudioIfConfig c; c.mode = AudioIqMode::MonoReal; c.rate = rate;
    AudioIfState s;
    audioIfProcess(st.data(), n, c, iq.data(), s);
    double dc = 0; for (int i = n / 2; i < n; i++) dc += iq[(size_t)i].real();
    CHECK(std::fabs(dc / (n / 2)) < 0.01, "DC removed (%g)", dc / (n / 2));
    CHECK(power(iq, 4000, 1000, rate) > 100 * power(iq, 4000, 2000, rate), "left channel selected");
    c.chan = 1; AudioIfState s2;
    audioIfProcess(st.data(), n, c, iq.data(), s2);
    CHECK(power(iq, 4000, 2000, rate) > 100 * power(iq, 4000, 1000, rate), "right channel selected");
    c.chan = 2; c.gainDb = 20; AudioIfState s3;
    audioIfProcess(st.data(), n, c, iq.data(), s3);
    CHECK(s3.peak > 1.5f && audioSourceLevel() > 1.5f, "gain +20 dB raises the level (%g)", s3.peak);
    c.mode = AudioIqMode::StereoIq; c.gainDb = 0; c.ifHz = 0; c.dc = false; AudioIfState s4;
    audioIfProcess(st.data(), n, c, iq.data(), s4);
    CHECK(std::fabs(iq[100].real() - st[200]) < 1e-6 && std::fabs(iq[100].imag() - st[201]) < 1e-6, "I/Q stereo untouched");
}

int main() {
    testShiftAndSideband();
    testClamp();
    testRates();
    testModeSignals();
    testProcess();
    if (fails) { printf("audio_if: %d checks FAILED\n", fails); return 1; }
    printf("audio_if: all checks passed\n");
    return 0;
}
