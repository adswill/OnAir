// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Listen-pack WAVs of a spoken phrase through every vocoder OnAir is allowed to encode with (Codec 2: M17 and FreeDV).
// DMR / NXDN / dPMR / P25 / D-STAR have no encoder here (mbelib is decode-only), so those files are not written.
// Set DECT2_VOICE_LISTEN_DIR (or writes next to the binary). Optional DECT2_VOICE_WAV, else voice_listen/00_original.wav.
#include "dect2/hfdig/hfdig_freedv.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/m17/m17_codec2.h"
#include "dect2/m17/m17_gen.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/wb/wb_export.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace dect2;
#include "../common/test_util.h"

static std::vector<float> toF(const int16_t* p, int n) {
    std::vector<float> o((size_t)n);
    for (int i = 0; i < n; i++) o[(size_t)i] = (float)p[i] / 32768.f;
    return o;
}
static bool save(const std::string& dir, const char* name, const std::vector<float>& a, int rate) {
    std::string err, path = dir + "/" + name;
    if (!wb::writeWavMono16(path, a, rate, err)) { printf("write %s: %s\n", path.c_str(), err.c_str()); fails++; return false; }
    printf("wrote %s (%zu samples @ %d Hz)\n", path.c_str(), a.size(), rate);
    return true;
}

static std::vector<float> synthPhrase(int rate, float sec) {
    const int n = (int)std::lround(sec * rate);
    std::vector<float> a((size_t)n);
    for (int i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const double f0 = 140.0 + 25.0 * std::sin(2 * 3.141592653589793 * 0.7 * t);
        double v = 0, ph = 2 * 3.141592653589793 * f0 * t;
        for (int k = 1; k <= 8; k++) v += (1.0 / k) * std::sin(k * ph);
        const double env = (t < 0.08 || t > sec - 0.08) ? t * 12 : 0.7 + 0.3 * std::sin(2 * 3.141592653589793 * 3.0 * t);
        a[(size_t)i] = (float)(std::max(-1.0, std::min(1.0, v * env * 0.12)));
    }
    return a;
}

static std::vector<float> resampleTo8k(const std::vector<float>& mono, int rate) {
    if (rate == 8000) return mono;
    const double ratio = 8000.0 / std::max(1, rate);
    std::vector<float> o((size_t)std::max(1.0, std::floor(mono.size() * ratio)));
    for (size_t i = 0; i < o.size(); i++) {
        const double x = (double)i / ratio;
        const size_t j = (size_t)x;
        const float fr = (float)(x - (double)j);
        const float a = j < mono.size() ? mono[j] : 0.f, b = j + 1 < mono.size() ? mono[j + 1] : a;
        o[i] = a + (b - a) * fr;
    }
    return o;
}

static bool readWavMono(const char* path, std::vector<float>& mono, int& rate) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) || memcmp(riff + 8, "WAVE", 4)) { fclose(f); return false; }
    int ch = 1, bps = 16;
    rate = 8000;
    for (;;) {
        unsigned char h[8];
        if (fread(h, 1, 8, f) != 8) { fclose(f); return false; }
        const unsigned sz = (unsigned)h[4] | ((unsigned)h[5] << 8) | ((unsigned)h[6] << 16) | ((unsigned)h[7] << 24);
        if (memcmp(h, "fmt ", 4) == 0) {
            unsigned char fmt[40] = {};
            const unsigned n = std::min(sz, 40u);
            if (fread(fmt, 1, n, f) != n) { fclose(f); return false; }
            if (sz > n) fseek(f, (long)(sz - n), SEEK_CUR);
            if (sz & 1) fseek(f, 1, SEEK_CUR);
            ch = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bps = fmt[14] | (fmt[15] << 8);
        } else if (memcmp(h, "data", 4) == 0) {
            std::vector<unsigned char> raw(sz);
            if (sz && fread(raw.data(), 1, sz, f) != sz) { fclose(f); return false; }
            fclose(f);
            const int step = std::max(1, ch);
            if (bps == 16) {
                const int16_t* p = (const int16_t*)raw.data();
                const size_t n = raw.size() / 2;
                for (size_t i = 0; i + (size_t)(step - 1) < n; i += (size_t)step)
                    mono.push_back((float)p[i] / 32768.f);
            } else {
                for (size_t i = 0; i + (size_t)(step - 1) < raw.size(); i += (size_t)step)
                    mono.push_back((float)raw[i] / 128.f - 1.f);
            }
            return !mono.empty();
        } else {
            fseek(f, (long)sz + (sz & 1), SEEK_CUR);
        }
    }
}

static std::vector<float> loadPhrase(const std::string& dir) {
    const char* p = getenv("DECT2_VOICE_WAV");
    std::string path = p && *p ? p : dir + "/00_original.wav";
    std::vector<float> mono;
    int rate = 8000;
    if (readWavMono(path.c_str(), mono, rate)) return resampleTo8k(mono, rate);
    return synthPhrase(8000, 3.2f);
}

static int nzCount(const std::vector<float>& a) {
    int n = 0;
    for (float v : a) n += std::fabs(v) > 1e-4f;
    return n;
}

static void codec2Roundtrip(const std::string& dir, const char* name, int rate, const std::vector<float>& phrase) {
    M17Codec2 c2(rate);
    if (!c2.ok()) { printf("codec2 missing: skip %s\n", name); return; }
    const int spf = c2.samplesPerFrame();
    std::vector<int16_t> pcm((phrase.size() / (size_t)spf) * (size_t)spf);
    for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, phrase[i])) * 32767.f);
    std::vector<int16_t> out(pcm.size());
    uint8_t bits[8];
    for (size_t i = 0; i + (size_t)spf <= pcm.size(); i += (size_t)spf) {
        c2.encode(pcm.data() + i, bits);
        c2.decode(bits, out.data() + i);
    }
    auto a = toF(out.data(), (int)out.size());
    CHECK(nzCount(a) > (int)a.size() / 10, "%s silent", name);
    save(dir, name, a, 8000);
}

static void freedvRoundtrip(const std::string& dir, const char* name, int txMode, int rxChoice, const std::vector<float>& phrase) {
    SynthConfig sc;
    sc.modeOpt[1] = txMode;
    size_t pos = 0;
    auto pull = [&](float* o, int n) {
        for (int i = 0; i < n; i++) o[i] = pos < phrase.size() ? phrase[pos++] : 0.f;
    };
    auto tx = makeFreedvTxAudio(sc, pull);
    if (!tx) { printf("FreeDV mode %d unavailable: skip %s\n", txMode, name); return; }
    hfdigFreedvSetMode(rxChoice);
    auto rx = makeFreedvDecoder();
    std::vector<float> speech;
    rx->setSpeechOut([&](const float* x, size_t n) { speech.insert(speech.end(), x, x + n); });
    std::vector<float> buf(160);
    const size_t want = phrase.size() + 8000;
    for (int i = 0; i < 2500 && speech.size() < want; i++) {
        tx->generate(buf.data(), buf.size());
        rx->feedAudio(buf.data(), buf.size());
    }
    CHECK(!speech.empty(), "%s no speech", name);
    if (!speech.empty()) {
        CHECK(nzCount(speech) > (int)speech.size() / 20, "%s silent", name);
        save(dir, name, speech, 8000);
    }
}

int main() {
    const char* d = getenv("DECT2_VOICE_LISTEN_DIR");
    std::string dir = d && *d ? d : "voice_listen";
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    const char* stale[] = { "02_freedv_700d.wav", "03_dmr_ambe_decoder.wav", "04_dmr_receiver_tap.wav" };
    for (const char* n : stale) {
        const std::string p = dir + "/" + n;
#ifdef _WIN32
        _unlink(p.c_str());
#else
        unlink(p.c_str());
#endif
    }

    const std::vector<float> phrase = loadPhrase(dir);
    CHECK(!phrase.empty(), "phrase");
    save(dir, "00_original.wav", phrase, 8000);

    codec2Roundtrip(dir, "01_m17_codec2_3200.wav", 3200, phrase);
    codec2Roundtrip(dir, "02_m17_codec2_1600.wav", 1600, phrase);

    if (!hfdigFreedvLibrary()) printf("codec2 missing: skip FreeDV WAVs\n");
    else {
        freedvRoundtrip(dir, "03_freedv_700d.wav", 0, 1, phrase);
        freedvRoundtrip(dir, "04_freedv_700e.wav", 1, 2, phrase);
        freedvRoundtrip(dir, "05_freedv_1600.wav", 2, 3, phrase);
    }

    if (m17Codec2Available()) {
        constexpr double kRate = 500000, kOff = -50000;
        M17GenConfig c;
        c.rate = kRate; c.dialOffsetHz = kOff; c.snrDb = 40; c.loop = false; c.voice = true; c.leadSec = 0.25;
        M17GenTx t;
        t.src = "N0CALL"; t.dst = "@ALL"; t.dataType = 2;
        t.durSec = (double)phrase.size() / 8000.0 + 0.2;
        t.gapSec = 0.2;
        size_t pos = 0;
        t.pullPcm = [&](int16_t* pcm, int n) {
            for (int i = 0; i < n; i++) {
                const float s = pos < phrase.size() ? phrase[pos++] : 0.f;
                pcm[i] = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, s)) * 32767.f);
            }
        };
        c.txs = { t };
        M17Generator g(c);
        CHECK(g.codedVoice(), "M17 air not using codec2");
        const double total = c.leadSec + t.durSec + 0.4 + t.gapSec;
        std::vector<cf32> x((size_t)(total * kRate));
        g.generate(x.data(), x.size());
        M17Receiver r;
        r.setSilent(true);
        std::vector<float> tap;
        r.setAudioTap([&](const float* a, size_t n) { tap.insert(tap.end(), a, a + n); });
        r.configure(kRate);
        r.setSignalOffset(kOff);
        for (size_t o = 0; o < x.size(); o += 8192) r.feed(x.data() + o, std::min<size_t>(8192, x.size() - o));
        CHECK(!tap.empty(), "M17 air tap empty");
        if (!tap.empty()) {
            CHECK(nzCount(tap) > (int)tap.size() / 20, "M17 air silent");
            save(dir, "06_m17_air.wav", tap, 8000);
        }
    } else printf("codec2 missing: skip 06_m17_air.wav\n");

    const char* note =
        "These files are the spoken phrase encoded and decoded where OnAir is allowed to encode.\n"
        "\n"
        "00  original (8 kHz)\n"
        "01  M17 Codec 2 3200 (vocoder only)\n"
        "02  M17 Codec 2 1600 (vocoder only)\n"
        "03  FreeDV 700D\n"
        "04  FreeDV 700E\n"
        "05  FreeDV 1600\n"
        "06  M17 over the air (4FSK -> receiver)\n"
        "\n"
        "Not in this pack, and they cannot contain this recording:\n"
        "DMR (4FSK), NXDN 4800/9600, dPMR, P25 Phase 1, D-STAR.\n"
        "Those vocoders are patented. OnAir loads mbelib to decode only; there is no encoder,\n"
        "so the phrase cannot be turned into AMBE/IMBE bits. Earlier 03/04 files were random\n"
        "decoder output, not speech — they were removed.\n"
        "TETRA / TETRAPOL: the speech codecs are user-installed plug-ins; OnAir has no encoder for them.\n"
        "FreeDV RADE: UI slot only, not encoded here.\n";
    std::string err;
    CHECK(wb::writeTextFile(dir + "/LISTEN.txt", note, err), "LISTEN.txt %s", err.c_str());
    printf("wrote %s/LISTEN.txt\n", dir.c_str());

    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
