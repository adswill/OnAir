// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transmit into an IQ file (tx_iqfile.h): a short generated IQ written in every format and read back with the project's own reader (probeIqFile
// for the SigMF and WAV sidecar / header, then the samples from the data file). The sample rate, the centre frequency, the sample count and the samples
// are compared. The sink also runs inside a TxSession with a generated tone, unpaced, and stops cleanly.
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include "dect2/source/iq_file.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/test_util.h"

static std::vector<cf32> signal(size_t n) {
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) {
        const double ph = 2 * 3.141592653589793 * 0.0371 * (double)i;
        x[i] = cf32((float)(0.7 * std::cos(ph)), (float)(0.4 * std::sin(ph * 1.3)));
    }
    return x;
}

static std::vector<unsigned char> readAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_test_tx_iqfile";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const double rate = 96000, centre = 433.5e6;
    const std::vector<cf32> x = signal(40000);
    const char* names[] = {"sigmf_cf32", "sigmf_ci16", "sigmf_ci8", "raw_cf32", "wav", "blue_cf32", "blue_ci16", "blue_ci8"};

    for (int f = 0; f < kIqFileFormats; f++) {
        const IqFileFormat fmt = (IqFileFormat)f;
        IqFileTxConfig c;
        c.format = fmt;
        c.centerHz = centre;
        c.paced = false;
        c.description = "test";
        c.counters = std::make_shared<IqFileTxCounters>();
        c.path = iqFilePath((dir / names[f]).string(), "ignored", fmt);
        IqFileTxSink sink(c);
        TxTune t;
        t.sampleRate = rate; t.centerHz = centre;
        std::string err;
        CHECK(sink.open(t, err), "%s: open: %s", names[f], err.c_str());
        CHECK(!sink.realtime() && sink.sampleRate() == rate, "%s: unpaced, rate", names[f]);
        for (size_t o = 0; o < x.size(); o += 4096) CHECK(sink.write(x.data() + o, std::min<size_t>(4096, x.size() - o), err), "%s: write %s", names[f], err.c_str());
        sink.close();
        CHECK(c.counters->samples.load() == x.size() && std::fabs(c.counters->seconds() - (double)x.size() / rate) < 1e-9, "%s: counters", names[f]);

        // read it back
        std::vector<cf32> y;
        if (fmt == IqFileFormat::RawCf32) {
            const auto b = readAll(c.path);
            CHECK(b.size() == x.size() * 8, "%s: size %zu", names[f], b.size());
            y.resize(b.size() / 8);
            memcpy(y.data(), b.data(), y.size() * 8);
        } else {
            IqFileInfo info;
            err.clear();
            CHECK(probeIqFile(c.path, info, err), "%s: probe: %s", names[f], err.c_str());
            CHECK(std::fabs(info.sampleRate - rate) < 1, "%s: sample rate %.1f", names[f], info.sampleRate);
            if (fmt != IqFileFormat::WavIq16) CHECK(std::fabs(info.centerHz - centre) < 1, "%s: centre %.1f", names[f], info.centerHz);
            const auto b = readAll(info.dataPath);
            const uint64_t end = info.endBytes ? info.endBytes : b.size();
            const bool isF = fmt == IqFileFormat::SigmfCf32 || fmt == IqFileFormat::BlueCf32, isB = fmt == IqFileFormat::SigmfCs8 || fmt == IqFileFormat::BlueCs8;
            const size_t bps = isF ? 8 : isB ? 2 : 4;
            CHECK(end - info.offsetBytes == x.size() * bps, "%s: data bytes %llu", names[f], (unsigned long long)(end - info.offsetBytes));
            CHECK(info.format == (isF ? FileFormat::CF32 : isB ? FileFormat::CS8 : FileFormat::CI16), "%s: format", names[f]);
            y.resize(x.size());
            for (size_t i = 0; i < x.size() && info.offsetBytes + (i + 1) * bps <= b.size(); i++) {
                const unsigned char* p = b.data() + info.offsetBytes + i * bps;
                if (isF) memcpy(&y[i], p, 8);
                else if (isB) y[i] = cf32((float)(int8_t)p[0] / 127.f, (float)(int8_t)p[1] / 127.f);
                else y[i] = cf32((float)(int16_t)(p[0] | (p[1] << 8)) / 32767.f, (float)(int16_t)(p[2] | (p[3] << 8)) / 32767.f);
            }
        }
        const float tol = fmt == IqFileFormat::SigmfCs8 || fmt == IqFileFormat::BlueCs8 ? 0.01f : fmt == IqFileFormat::SigmfCi16 || fmt == IqFileFormat::BlueCi16 || fmt == IqFileFormat::WavIq16 ? 0.0001f : 0.f;
        float worst = 0;
        for (size_t i = 0; i < x.size() && i < y.size(); i++) worst = std::max(worst, std::max(std::fabs(x[i].real() - y[i].real()), std::fabs(x[i].imag() - y[i].imag())));
        CHECK(y.size() == x.size() && worst <= tol, "%s: samples differ by %.6f (tolerance %.6f)", names[f], (double)worst, (double)tol);
        printf("%-11s ok, worst difference %.6f\n", names[f], (double)worst);
    }

    {   // inside a TxSession: the KAAL mode with the test tone as speech, a one-pass transmission of 1 s at 48 kHz written as fast as it is made
        IqFileTxConfig c;
        c.format = IqFileFormat::SigmfCf32;
        c.paced = false;
        c.path = iqFilePath((dir / "session").string(), "x", c.format);
        c.counters = std::make_shared<IqFileTxCounters>();
        TxRequest r;
        r.modeId = "kaal";   // 48 kHz is its own rate; the speech is the 1 kHz test tone
        r.audioSrc = 2;
        r.loop = false;
        r.onceSeconds = 1.0;
        r.tune.centerHz = 433.0e6;
        r.tune.sampleRate = 48000;
        TxSession s;
        std::string err;
        const TxModeSpec* m = txModeById("kaal");
        CHECK(m != nullptr, "kaal mode");
        if (m) {
            CHECK(s.start(makeIqFileTxSink(c), r, {}, err), "session: %s", err.c_str());
            const auto t0 = std::chrono::steady_clock::now();
            while (s.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            s.stop();
            CHECK(c.counters->samples.load() >= 47000, "session wrote %llu samples", (unsigned long long)c.counters->samples.load());
            CHECK(took < 5.0, "an unpaced file transmission took %.1f s for 1 s of signal", took);
            IqFileInfo info;
            err.clear();
            CHECK(probeIqFile(c.path, info, err) && std::fabs(info.centerHz - 433.0e6) < 1 && std::fabs(info.sampleRate - 48000) < 1, "session file: probe %s", err.c_str());
        }
    }
    fs::remove_all(dir);
    return testSummary("tx_iqfile");
}
