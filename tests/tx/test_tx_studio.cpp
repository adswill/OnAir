// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TX studio (no GUI): generate a mode's signal into memory, measure it, write it in every format (read back with the project's reader), batch sweep, list parsing.
// NOTHING is transmitted and no radio is opened.
#include "dect2/tx/tx_studio.h"
#include "dect2/source/iq_file.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint64_t fileSize(const std::string& p) { std::error_code ec; const auto s = std::filesystem::file_size(p, ec); return ec ? 0 : (uint64_t)s; }
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main() {
    namespace fs = std::filesystem;
    const auto v1 = studioParseList("10, 20,30");
    CHECK(v1.size() == 3 && v1[1] == 20, "comma list");
    const auto v2 = studioParseList("5:25:10");
    CHECK(v2.size() == 3 && v2[2] == 25, "range list");
    CHECK(studioParseList("1,x").empty() && studioParseList("5:1:1").empty() && studioParseList("1:1000:1").empty(), "bad lists are refused");
    TxRequest r;
    studioSetParam(r, "wpm", 25);
    studioSetParam(r, "am.depth", 0.5);
    CHECK(r.wpm == 25 && r.o("am.depth", 0) == 0.5, "set param");
    CHECK(studioFileBytes(StudioFormat::Cu8, 100) == 200 && studioFileBytes(StudioFormat::Cs16, 100) == 400 && studioFileBytes(StudioFormat::Cf32, 100) == 800 && studioFileBytes(StudioFormat::WavAudio, 100) == 244, "file sizes");

    const TxModeSpec* cw = txModeById("cw");
    if (!cw) { printf("no cw mode\n"); return 1; }
    TxRequest q;
    q.text = "CQ TEST";
    q.callsign = "N0CALL";
    q.wpm = 20;
    StudioSignal s;
    std::string err;
    // CW declares 0.25 Msps as its lowest rate (cwTuning().minSampleRate): makeTxGenerator refuses less, so the bench works at that rate
    CHECK(studioGenerate(*cw, q, 250000, 2.0, s, err), "generate cw: %s", err.c_str());
    CHECK(s.rateHz > 0 && s.iq.size() > 1000 && s.seconds > 0.5, "signal length %zu at %.0f", s.iq.size(), s.rateHz);
    for (const auto& v : s.iq) if (!std::isfinite(v.real()) || !std::isfinite(v.imag()) || std::abs(v) > 1.0001f) { CHECK(false, "a sample is not finite or above full scale"); break; }
    const StudioMeasure m = studioMeasure(s, "cw");
    CHECK(m.valid && m.obw99Hz > 0 && m.obw99Hz < s.rateHz && m.paprDb >= 0 && m.peakDb <= 0.01, "measure: obw %.0f papr %.1f peak %.2f", m.obw99Hz, m.paprDb, m.peakDb);
    StudioSignal bad;
    CHECK(!studioGenerate(*cw, q, 0, 0, bad, err) || bad.iq.size() > 0, "zero length still behaves");

    const fs::path dir = fs::temp_directory_path() / ("onair_txstudio_test_" + std::to_string((long long)fs::file_time_type::clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    for (int fi = 0; fi < kStudioFormats; fi++) {
        const StudioFormat f = (StudioFormat)fi;
        StudioWriteInfo wi;
        wi.mode = cw; wi.req = &q; wi.measure = &m; wi.centerHz = 14.07e6;
        std::string data, meta;
        const std::string base = (dir / ("t" + std::to_string(fi))).string();
        StudioSignal fsig = s;
        if (f == StudioFormat::WavAudio) fsig.rateHz = 48000;   // WAV audio only takes audio rates (refused above 100 kHz, checked below): the same samples labelled 48 kHz
        CHECK(studioWrite(base, fsig, f, wi, data, meta, err), "write %s: %s", studioFormatName(f), err.c_str());
        CHECK(fileSize(data) == studioFileBytes(f, s.iq.size()), "%s: %llu bytes, expected %llu", studioFormatName(f), (unsigned long long)fileSize(data), (unsigned long long)studioFileBytes(f, s.iq.size()));
        const std::string mj = slurp(meta);
        CHECK(mj.find("\"onair:mode\": \"cw\"") != std::string::npos && mj.find("N0CALL") != std::string::npos && mj.find("\"onair:transmitted\": false") != std::string::npos, "%s: metadata content", studioFormatName(f));
        if (f == StudioFormat::SigMF) {
            IqFileInfo info; std::string e2;
            CHECK(probeIqFile(meta, info, e2) && std::fabs(info.sampleRate - s.rateHz) < 1 && info.format == FileFormat::CF32 && std::fabs(info.centerHz - 14.07e6) < 1, "SigMF read back: %s", e2.c_str());
        }
    }
    // WAV audio refuses a high sample rate
    StudioSignal hi = s;
    hi.rateHz = 2e6;
    std::string d2, m2;
    CHECK(!studioWrite((dir / "hi").string(), hi, StudioFormat::WavAudio, StudioWriteInfo(), d2, m2, err), "WAV audio is refused above 100 kHz");

    // batch: three speeds, three files, each with its own name
    const auto items = studioBatch(*cw, q, "wpm", {10, 20, 30}, 250000, 1.0, txm::Impair(), StudioFormat::Cs8, dir.string(), "sweep", 0, nullptr, nullptr);
    CHECK(items.size() == 3, "batch made %zu items", items.size());
    for (const auto& it : items) CHECK(it.ok && fileSize(it.dataPath) > 1000, "batch item %g: %s", it.value, it.err.c_str());
    CHECK(items.size() == 3 && items[0].dataPath != items[1].dataPath, "batch files differ");
    const auto sn = studioBatch(*cw, q, "impair.snr", {5, 15}, 250000, 0.5, txm::Impair(), StudioFormat::Cu8, dir.string(), "snr", 0, nullptr, nullptr);
    CHECK(sn.size() == 2 && sn[0].ok && sn[1].ok, "impairment sweep");
    std::error_code ec;
    fs::remove_all(dir, ec);
    printf(fails ? "tx_studio: %d FAILED\n" : "tx_studio: all passed\n", fails);
    return fails ? 1 : 0;
}
