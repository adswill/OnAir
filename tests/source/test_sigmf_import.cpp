// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SigMF and WAV IQ files (iq_file.h): the built-in FM test signal is recorded as a float file, rewritten as SigMF pairs (cf32_le, cf32_be,
// ci16_le, ci8, cu8) and a two-channel WAV, each with a different sample_start, and probeIqFile() must give back the rate, the centre frequency and
// the format; every file played as a source gives the same FM receiver state as the live signal. Bad metas fail with a message.
#include "dect2/engine/engine.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static const double kRate = 1e6, kCenter = 93.9e6;

static TuneSettings fmTune() {
    TuneSettings t;
    t.sampleRate = kRate; t.centerHz = kCenter; t.bandwidthMhz = 0.2;
    t.synth.mode = 7; t.synth.snrDb = 30;
    return t;
}

static int watchFm(Engine& e, double secs) {
    int best = 0;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        RxTelemetry t;
        if (e.latestRx(t, seq)) { seq = t.seq; if (t.standard == 6) best = std::max(best, t.fm.state); }
    }
    return best;
}

static void put(std::ofstream& f, const void* p, size_t n) { f.write((const char*)p, (std::streamsize)n); }
static void le32(std::ofstream& f, uint32_t v) { put(f, &v, 4); }
static void le16(std::ofstream& f, uint16_t v) { put(f, &v, 2); }

// the samples written in the format fmt after `pad` zero samples
static void writeSamples(const fs::path& p, const std::vector<cf32>& x, FileFormat fmt, bool be, size_t pad, bool wavHeader) {
    std::ofstream f(p, std::ios::binary);
    const size_t n = pad + x.size();
    const size_t bps = fmt == FileFormat::CF32 ? 8 : fmt == FileFormat::CI16 ? 4 : 2;
    if (wavHeader) {
        put(f, "RIFF", 4); le32(f, (uint32_t)(36 + n * bps)); put(f, "WAVEfmt ", 8); le32(f, 16);
        le16(f, fmt == FileFormat::CF32 ? 3 : 1); le16(f, 2); le32(f, (uint32_t)kRate); le32(f, (uint32_t)(kRate * bps)); le16(f, (uint16_t)bps);
        le16(f, (uint16_t)(bps * 4)); put(f, "data", 4); le32(f, (uint32_t)(n * bps));
    }
    for (size_t i = 0; i < n; i++) {
        const cf32 s = i < pad ? cf32(0, 0) : x[i - pad];
        const float v[2] = {s.real(), s.imag()};
        for (int k = 0; k < 2; k++) {
            if (fmt == FileFormat::CF32) {
                uint32_t u; memcpy(&u, &v[k], 4);
                if (be) u = (u >> 24) | ((u >> 8) & 0xFF00u) | ((u << 8) & 0xFF0000u) | (u << 24);
                put(f, &u, 4);
            } else if (fmt == FileFormat::CI16) {
                uint16_t u = (uint16_t)(int16_t)std::max(-32767.f, std::min(32767.f, std::round(v[k] * 32767.f)));
                if (be) u = (uint16_t)((u >> 8) | (u << 8));
                put(f, &u, 2);
            } else if (fmt == FileFormat::CU8) {
                const uint8_t u = (uint8_t)std::max(0.f, std::min(255.f, std::round(v[k] * 127.f + 127.5f)));
                put(f, &u, 1);
            } else {
                const int8_t u = (int8_t)std::max(-127.f, std::min(127.f, std::round(v[k] * 127.f)));
                put(f, &u, 1);
            }
        }
    }
}

static int replay(const std::string& path, const IqFileInfo& info) {
    Engine e;
    e.setStandard(7);
    e.player().setMuted(true);
    e.fm().setSilent(true);
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = path;
    FileOptions fo;
    fo.path = info.dataPath; fo.format = info.format; fo.sampleRate = info.sampleRate; fo.loop = false;
    fo.offsetBytes = info.offsetBytes; fo.endBytes = info.endBytes; fo.bigEndian = info.bigEndian;
    TuneSettings t = fmTune();
    t.centerHz = info.centerHz;
    if (!e.start(dev, t, fo)) return -1;
    const int s = watchFm(e, 3.0);
    e.stop();
    return s;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_sigmf_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // 1.5 s of the FM test signal as raw floats
    std::vector<cf32> x;
    int live = 0;
    {
        Engine e;
        e.setStandard(7);
        e.player().setMuted(true);
        e.fm().setSilent(true);
        DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
        std::string err;
        const std::string raw = (dir / "raw.cf32").string();
        CHECK(e.start(dev, fmTune(), FileOptions()), "engine started");
        CHECK(e.startRecording(raw, FileFormat::CF32, err), "recording started");
        live = watchFm(e, 3.0);
        e.stopRecording();
        e.stop();
        std::ifstream f(raw, std::ios::binary);
        const size_t n = fs::exists(raw) ? (size_t)fs::file_size(raw) / 8 : 0;
        x.resize(n);
        f.read((char*)x.data(), (std::streamsize)(n * 8));
        CHECK(n > 1000000, "the recording has samples");
    }
    CHECK(live >= 1, "the live signal is received");

    struct Case { const char* name; FileFormat fmt; bool be; size_t pad; };
    const Case cases[] = {
        {"a_cf32", FileFormat::CF32, false, 0}, {"b_cf32be", FileFormat::CF32, true, 1000}, {"c_ci16", FileFormat::CI16, false, 777},
        {"d_ci8", FileFormat::CS8, false, 0}, {"e_cu8", FileFormat::CU8, false, 5},
    };
    for (const Case& c : cases) {
        const std::string base = (dir / c.name).string(); std::string werr;
        writeSigmfMeta(base + ".sigmf-meta", c.fmt, c.be, kRate, kCenter, c.pad, "test", werr);
        writeSamples(base + ".sigmf-data", x, c.fmt, c.be, c.pad, false);
        for (const std::string& open : {base + ".sigmf-meta", base + ".sigmf-data"}) {
            IqFileInfo info; std::string err;
            const bool ok = probeIqFile(open, info, err);
            CHECK(ok, (std::string(c.name) + ": probe works: " + err).c_str());
            if (!ok) continue;
            const size_t bps = c.fmt == FileFormat::CF32 ? 8 : c.fmt == FileFormat::CI16 ? 4 : 2;
            CHECK(info.format == c.fmt && info.bigEndian == c.be, "format and byte order read back");
            CHECK(std::abs(info.sampleRate - kRate) < 1 && std::abs(info.centerHz - kCenter) < 1, "rate and centre frequency read back");
            CHECK(info.offsetBytes == c.pad * bps && info.endBytes == 0, "sample_start gives the offset");
            CHECK(info.dataPath == base + ".sigmf-data", "the data file is found");
            if (open == base + ".sigmf-meta") {
                const int s = replay(open, info);
                printf("  %-9s FM state %d (live %d)\n", c.name, s, live);
                CHECK(s == live, (std::string(c.name) + ": plays like the live signal").c_str());
            }
        }
    }

    // two-channel WAV, 16-bit, the centre frequency in the name
    {
        const std::string p = (dir / "rec_93.900MHz.wav").string();
        writeSamples(p, x, FileFormat::CI16, false, 0, true);
        IqFileInfo info; std::string err;
        CHECK(probeIqFile(p, info, err), "WAV probe works");
        CHECK(info.format == FileFormat::CI16 && std::abs(info.sampleRate - kRate) < 1 && std::abs(info.centerHz - kCenter) < 1 && info.offsetBytes == 44, "WAV format, rate, frequency and data offset");
        const int s = replay(p, info);
        printf("  wav       FM state %d (live %d)\n", s, live);
        CHECK(s == live, "the WAV plays like the live signal");
    }

    // things that must fail with a message
    {
        IqFileInfo info; std::string err;
        const std::string base = (dir / "bad").string();
        { std::ofstream f(base + ".sigmf-meta"); f << "{\"global\": {\"core:datatype\": \"rf32_le\", \"core:sample_rate\": 1000}}"; }
        { std::ofstream f(base + ".sigmf-data"); f << "abcd"; }
        CHECK(!probeIqFile(base + ".sigmf-meta", info, err) && !err.empty(), "real-valued data is refused");
        { std::ofstream f(base + ".sigmf-meta"); f << "{not json"; }
        CHECK(!probeIqFile(base + ".sigmf-meta", info, err) && !err.empty(), "broken JSON is refused");
        fs::remove(base + ".sigmf-data");
        { std::ofstream f(base + ".sigmf-meta"); f << "{\"global\": {\"core:datatype\": \"cu8\"}}"; }
        CHECK(!probeIqFile(base + ".sigmf-meta", info, err) && !err.empty(), "a meta without its data file is refused");
        CHECK(!probeIqFile((dir / "x.cs8").string(), info, err) && err.empty(), "other extensions are left alone");
    }

    fs::remove_all(dir);
    printf(fails ? "sigmf: FAILED\n" : "sigmf: ok\n");
    return fails ? 1 : 0;
}
