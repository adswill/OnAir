// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV from a WAV recording: FSK packets are generated, turned into the audio an FM receiver's discriminator gives (a baseband waveform), written
// as WAV files of several layouts (mono 16-bit 48 kHz, stereo 24-bit, 32-bit float, 8-bit, 44.1 kHz) and decoded through the audio path.
#include "dect2/ssdv/ssdv_audio.h"
#include "dect2/ssdv/ssdv_gen.h"
#include "dect2/ssdv/ssdv_proto.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/ssdv_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<std::array<uint8_t, ssdv::kPktSize>> gPk;
static std::vector<uint8_t> gGoodJpeg;

static void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(v & 255); b.push_back(v >> 8 & 255); }
static void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

// kind: 8, 16, 24 PCM, 32 = float32
static void writeWav(const std::string& path, const std::vector<float>& a, int rate, int channels, int kind) {
    std::vector<uint8_t> d;
    for (float v : a) for (int c = 0; c < channels; c++) {
        const float x = c == 0 ? v : 0.3f * v;
        if (kind == 8) d.push_back((uint8_t)std::lround(x * 127 + 128));
        else if (kind == 16) put16(d, (uint16_t)(int16_t)std::lround(x * 32000));
        else if (kind == 24) { const int32_t q = (int32_t)std::lround(x * 8000000); d.push_back(q & 255); d.push_back(q >> 8 & 255); d.push_back(q >> 16 & 255); }
        else { uint32_t u; memcpy(&u, &x, 4); put32(d, u); }
    }
    std::vector<uint8_t> h;
    const int bits = kind == 32 ? 32 : kind;
    h.insert(h.end(), {'R', 'I', 'F', 'F'}); put32(h, 36 + (uint32_t)d.size()); h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(h, 16); put16(h, kind == 32 ? 3 : 1); put16(h, channels); put32(h, rate); put32(h, rate * channels * bits / 8); put16(h, channels * bits / 8); put16(h, bits);
    h.insert(h.end(), {'d', 'a', 't', 'a'}); put32(h, (uint32_t)d.size());
    std::ofstream o(path, std::ios::binary);
    o.write((const char*)h.data(), (std::streamsize)h.size());
    o.write((const char*)d.data(), (std::streamsize)d.size());
}

// the discriminator output of the generator's signal, boxcar-decimated to `outRate` (the generator runs at a multiple of it)
static std::vector<float> discAudio(double baud, double sec, double genRate, int dec) {
    SsdvGenConfig c; c.rate = genRate; c.baud = baud; c.gaussBt = 0.5; c.packets = gPk;
    SsdvGenerator g(c);
    std::vector<cf32> x((size_t)(sec * genRate));
    g.generate(x.data(), x.size());
    std::vector<float> a;
    cf32 prev = x[0];
    double acc = 0; int k = 0;
    for (size_t i = 1; i < x.size(); i++) {
        acc += std::arg(x[i] * std::conj(prev)); prev = x[i];
        if (++k == dec) { a.push_back((float)(acc / dec)); acc = 0; k = 0; }
    }
    float pk = 1e-9f;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    for (float& v : a) v /= pk;
    return a;
}

static bool decodeWav(const std::string& wav, int fixedBaud, int& baud) {
    const std::string cf = wav + ".cf32";
    double rate = 0; std::string err;
    if (!ssdvWavToCf32File(wav, cf, rate, err)) { printf("  convert failed: %s\n", err.c_str()); return false; }
    std::ifstream f(cf, std::ios::binary);
    std::vector<cf32> x((std::filesystem::file_size(cf)) / sizeof(cf32));
    f.read((char*)x.data(), (std::streamsize)(x.size() * sizeof(cf32)));
    SsdvReceiver r;
    r.configure(rate);
    r.setSignalOffset(0);
    r.setBaud(fixedBaud);
    for (size_t i = 0; i < x.size(); i += 4096) r.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    r.flush();
    SsdvTelemetry t; r.telemetry(t, 0);
    baud = t.baud;
    std::error_code removeEc;
    std::filesystem::remove(cf, removeEc);   // Windows can still hold the file open here; the folder is removed at the end
    return !t.images.empty() && t.images[0].complete && t.images[0].jpeg && *t.images[0].jpeg == gGoodJpeg;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::vector<uint8_t> jpg = ssdv::testJpeg(48, 32);
    gPk = ssdv::encodePackets(jpg, "ONAIR", 3, 5, false);
    { ssdv::Assembler a; for (auto& p : gPk) a.add(p.data()); gGoodJpeg = a.jpeg(); }
    CHECK(gPk.size() >= 2 && !gGoodJpeg.empty(), "test picture");
    const auto dir = std::filesystem::temp_directory_path() / "onair_ssdv_audio";
    std::filesystem::create_directories(dir);
    struct Case { double baud; int rate, channels, kind; };
    const Case cases[] = {{9600, 48000, 1, 16}, {9600, 48000, 2, 24}, {9600, 48000, 1, 32}, {1200, 48000, 1, 16}, {4800, 48000, 1, 8}, {9600, 96000, 1, 16}};
    for (const Case& k : cases) {
        const double sec = (double)gPk.size() * 264 * 8 / k.baud * 1.3 + 1.0;
        const int dec = (int)(960000 / k.rate);
        const std::vector<float> a = discAudio(k.baud, sec, 960000.0, dec);
        const std::string p = (dir / "t.wav").string();
        writeWav(p, a, k.rate, k.channels, k.kind);
        int baud = 0;
        const bool ok = decodeWav(p, 0, baud);
        CHECK(ok && baud == (int)k.baud, "%g baud, %d Hz, %d ch, kind %d: picture complete %d (baud %d)", k.baud, k.rate, k.channels, k.kind, ok, baud);
    }
    {   // 44.1 kHz: resampled from the 48 kHz waveform's time base (the generator at 882 kHz, 20x)
        const std::vector<float> a = discAudio(1200, (double)gPk.size() * 264 * 8 / 1200 * 1.3 + 1.0, 882000.0, 20);
        const std::string p = (dir / "t441.wav").string();
        writeWav(p, a, 44100, 1, 16);
        int baud = 0;
        const bool ok = decodeWav(p, 0, baud);
        CHECK(ok, "1200 baud, 44.1 kHz: picture complete %d (baud %d)", ok, baud);
    }
    {   // not a WAV / silence: a clear error, never a crash
        WavAudio w; std::string err;
        const std::string p = (dir / "bad.wav").string();
        std::ofstream(p) << "not a wav";
        CHECK(!readWavAudio(p, w, err) && !err.empty(), "a file that is not a WAV is refused");
        writeWav(p, std::vector<float>(4800, 0.f), 48000, 1, 16);
        double r; CHECK(!ssdvWavToCf32File(p, p + ".cf32", r, err) && !err.empty(), "a silent recording is refused");
    }
    { std::error_code ec; std::filesystem::remove_all(dir, ec); }
    return testSummary("ssdv audio");
}
