// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// COSPAS-SARSAT baseband export (issue #214), simulated signal: with the export on, every decoded burst is saved as a 2 channel float WAV at 32 kHz
// with its identification in the name, the telemetry counts the files, a saved burst decodes again to the same identification through the stand-alone
// demodulator, the whole-capture search exports too, and with the export off or on noise alone nothing is written.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/sarsat/sarsat_rx.h"
#include "dect2/sarsat/sarsat_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/test_util.h"

static SarsatTelemetry run(const std::string& dir, bool wide, bool noiseOnly, double seconds) {
    const double rate = 256000;
    SarsatReceiver rx;
    rx.configure(rate);
    rx.setWideSearch(wide);
    rx.setBurstExport(dir);
    rx.setSignalOffset(-sarsatTuning().tuneOffsetHz);
    SynthConfig cfg; cfg.snrDb = 35;
    auto syn = makeSarsatSynth(cfg, rate);
    std::vector<cf32> buf(32768);
    SarsatTelemetry t;
    uint64_t seq = 0;
    unsigned s = 99;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        if (noiseOnly) for (auto& v : buf) { s = s * 1664525u + 1013904223u; const float a = ((s >> 8) & 0xFFFF) / 65536.f - 0.5f; s = s * 1664525u + 1013904223u; v = cf32(a * 0.3f, (((s >> 8) & 0xFFFF) / 65536.f - 0.5f) * 0.3f); }
        else syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        SarsatTelemetry n;
        if (rx.telemetry(n, seq)) { seq = n.seq; t = n; }
    }
    return t;
}

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool readWav(const std::string& path, std::vector<cf32>& y, uint32_t& rate) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t tmp[4096]; size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    std::fclose(f);
    if (b.size() < 58 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVEfmt ", 8)) return false;
    if (le16(&b[20]) != 3 || le16(&b[22]) != 2 || le16(&b[34]) != 32 || std::memcmp(&b[38], "fact", 4) || std::memcmp(&b[50], "data", 4)) return false;
    rate = le32(&b[24]);
    const uint32_t bytes = le32(&b[54]);
    if (58 + (size_t)bytes != b.size() || le32(&b[4]) + 8 != b.size()) return false;
    for (size_t i = 0; i + 8 <= bytes; i += 8) { float a, q; std::memcpy(&a, &b[58 + i], 4); std::memcpy(&q, &b[62 + i], 4); y.push_back(cf32(a, q)); }
    return true;
}

static std::string freshDir(const char* tag) {
    const fs::path p = fs::temp_directory_path() / (std::string("onair-sarsat-export-") + tag + "-" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(p);
    return p.string();
}

int main() {
    const double seconds = 5 * 3.6 + 3;
    for (int wide = 0; wide < 2; wide++) {
        const std::string dir = freshDir(wide ? "wide" : "one");
        const SarsatTelemetry t = run(dir, wide != 0, false, seconds);
        int files = 0, decoded = 0;
        if (fs::exists(dir)) for (auto& e : fs::directory_iterator(dir)) {
            files++;
            const std::string name = e.path().filename().string();
            std::vector<cf32> y; uint32_t rate = 0;
            CHECK(readWav(e.path().string(), y, rate), "%s: not a valid float WAV", name.c_str());
            CHECK(rate == 32000 && y.size() >= (size_t)(0.4 * 32000), "%s: rate %u, %zu samples", name.c_str(), rate, y.size());
            if (name.find("undecoded") != std::string::npos) continue;
            // the saved burst decodes again, and the name carries its identification
            sarsat::Beacon b;
            CHECK(sarsat::demodBurst(y, 0.0, b), "%s: the exported burst does not decode", name.c_str());
            CHECK(name.find(b.hexId) != std::string::npos, "%s: name does not carry %s", name.c_str(), b.hexId.c_str());
            decoded++;
        }
        printf("%s: %zu entries, %d files (%d decoded), telemetry says %llu\n", wide ? "whole capture" : "single", t.log.size(), files, decoded, (unsigned long long)t.exportedBursts);
        CHECK(t.blocksOk >= 4, "only %llu bursts decoded", (unsigned long long)t.blocksOk);
        CHECK(decoded >= 4, "only %d decoded bursts saved", decoded);
        CHECK((int)t.exportedBursts == files && !t.lastExport.empty(), "telemetry counts %llu files, %d on disk", (unsigned long long)t.exportedBursts, files);
        fs::remove_all(dir);
    }
    {   // off: nothing; noise alone: nothing
        const std::string d1 = freshDir("off"), d2 = freshDir("noise");
        const SarsatTelemetry a = run("", false, false, seconds);
        CHECK(a.exportedBursts == 0, "export off wrote %llu files", (unsigned long long)a.exportedBursts);
        const SarsatTelemetry b = run(d2, false, true, 10);
        CHECK(b.exportedBursts == 0 && !fs::exists(d2), "noise alone exported %llu files", (unsigned long long)b.exportedBursts);
        (void)d1;
    }
    return testSummary("sarsat_export");
}
