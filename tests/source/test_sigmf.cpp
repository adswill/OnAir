// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SigMF metadata next to IQ recordings (iq_record.h): the JSON has the global datatype, sample_rate and version and a capture with the frequency and
// the time; the datatype names are the ones IQEngine reads; a real recording gets a .sigmf-meta and a .sigmf-data with the same bytes.
#include "dect2/source/iq_record.h"
#include "dect2/source/iq_file.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)
static bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
    CHECK(std::string(sigmfDatatype(FileFormat::CU8)) == "cu8", "cu8");
    CHECK(std::string(sigmfDatatype(FileFormat::CS8)) == "ci8_le", "cs8");
    CHECK(std::string(sigmfDatatype(FileFormat::CF32)) == "cf32_le", "cf32");
    SigmfInfo i;
    i.fmt = FileFormat::CF32; i.rateHz = 20e6; i.centerHz = 2437.5e6; i.datetimeUtc = "2026-10-13T09:15:30Z"; i.dataFile = "a\"b.cf32"; i.sampleCount = 1234;
    const std::string j = sigmfMetaJson(i);
    CHECK(has(j, "\"core:datatype\": \"cf32_le\""), "datatype");
    CHECK(has(j, "\"core:sample_rate\": 20000000"), "rate");
    CHECK(has(j, "\"core:version\": \"1.0.0\""), "version");
    CHECK(has(j, "\"core:frequency\": 2437500000.0"), "frequency");
    CHECK(has(j, "\"core:datetime\": \"2026-10-13T09:15:30Z\""), "datetime");
    CHECK(has(j, "\"core:sample_count\": 1234"), "count");
    CHECK(has(j, "a\\\"b.cf32"), "string escaping");
    CHECK(has(j, "\"captures\"") && has(j, "\"annotations\""), "sections");
    int depth = 0; bool ok = true;
    for (char c : j) { if (c == '{' || c == '[') depth++; if (c == '}' || c == ']') depth--; if (depth < 0) ok = false; }
    CHECK(ok && depth == 0, "balanced brackets");
    SigmfInfo e; e.rateHz = 2.4e6;
    const std::string k = sigmfMetaJson(e);
    CHECK(!has(k, "core:frequency") && !has(k, "core:datetime") && has(k, "ci8_le"), "optional fields left out");

    const fs::path dir = fs::temp_directory_path() / "onair_sigmf_rec_test";
    fs::remove_all(dir);
    IqRecorder r;
    r.setSigmf(true, 433.92e6);
    std::string err;
    const std::string path = (dir / "cap.cs8").string();
    CHECK(r.start(path, FileFormat::CS8, 2e6, err), "start");
    std::vector<cf32> x(5000, cf32(0.5f, -0.25f));
    r.push(x.data(), x.size());
    r.stop();
    CHECK(fs::exists(dir / "cap.sigmf-meta"), "meta file");
    CHECK(fs::exists(dir / "cap.sigmf-data") && fs::file_size(dir / "cap.sigmf-data") == 10000, "data file under the SigMF name");
    std::stringstream ss;
    { std::ifstream f(dir / "cap.sigmf-meta"); ss << f.rdbuf(); }   // closed before the folder is removed (Windows)
    CHECK(has(ss.str().c_str(), "\"core:frequency\": 433920000.0") && has(ss.str(), "\"core:sample_count\": 5000") && has(ss.str(), "\"core:dataset\": \"cap.cs8\""), "recorded meta");
    IqRecorder r2;   // off: no metadata
    CHECK(r2.start((dir / "plain.cs8").string(), FileFormat::CS8, 2e6, err), "start plain");
    r2.stop();
    CHECK(!fs::exists(dir / "plain.sigmf-meta"), "no meta when off");
    {   // radio name + location in the meta, and the size cap
        IqRecorder r3;
        r3.setSigmf(true, 100e6, "fm");
        r3.setSigmfExtra("OnAir \"Android\" test signal", true, 54.6872, 25.2797);
        r3.setMaxBytes(4000);
        CHECK(r3.start((dir / "extra.cs8").string(), FileFormat::CS8, 2e6, err), "start extra");
        std::vector<cf32> y(1500, cf32(0.1f, 0.1f));   // 3000 bytes each
        r3.push(y.data(), y.size());
        CHECK(!r3.capReached(), "below the cap");
        r3.push(y.data(), y.size());
        r3.push(y.data(), y.size());   // after the cap: not queued
        CHECK(r3.capReached(), "cap reached");
        r3.stop();
        CHECK(fs::file_size(dir / "extra.cs8") == 6000, "stopped at the cap block");
        std::stringstream s3;
        { std::ifstream f(dir / "extra.sigmf-meta"); s3 << f.rdbuf(); }
        CHECK(has(s3.str(), "\"core:hw\":\"OnAir \\\"Android\\\" test signal\"") && has(s3.str(), "\"core:geolocation\":{\"type\":\"Point\",\"coordinates\":[25.279700,54.687200]}"), "hw and geolocation");
        int d3 = 0; bool ok3 = true;
        for (char c : s3.str()) { if (c == '{' || c == '[') d3++; if (c == '}' || c == ']') d3--; if (d3 < 0) ok3 = false; }
        CHECK(ok3 && d3 == 0, "balanced after extra");
        CHECK(sigmfAddExtra("{\"global\":{}}", "x", false, 0, 0) == "{\"global\":{\"core:hw\":\"x\"}}", "empty global");
        CHECK(sigmfAddExtra("{\"a\":1}", "x", true, 1, 2) == "{\"a\":1}", "no global leaves it alone");
    }
    {   // legacy ri8 / ri16 aliases and core:dataset (OnAir writes the .cs8 name, not .sigmf-data)
        const fs::path d2 = dir / "legacy";
        fs::create_directories(d2);
        std::ofstream(d2 / "iq.cs8", std::ios::binary) << "abcd";
        std::ofstream meta(d2 / "cap.sigmf-meta");
        meta << "{\n  \"global\": {\n    \"core:datatype\": \"ri8\",\n    \"core:sample_rate\": 2000000,\n    \"core:dataset\": \"iq.cs8\"\n  },\n  \"captures\": [{\"core:sample_start\": 0, \"core:frequency\": 8942000.0}]\n}\n";
        meta.close();
        IqFileInfo info;
        std::string err;
        const std::string metaPath = (d2 / "cap.sigmf-meta").string();
        const std::string dataPath = (d2 / "iq.cs8").string();
        CHECK(probeIqFile(metaPath, info, err), "ri8 + dataset probe");
        CHECK(info.format == FileFormat::CS8 && info.sampleRate == 2e6 && info.centerHz == 8942000.0, "ri8 fields");
        CHECK(fs::equivalent(info.dataPath, dataPath), "dataset path beside meta");
        std::ofstream(d2 / "ri16.sigmf-meta") << "{\"global\":{\"core:datatype\":\"ri16_le\",\"core:sample_rate\":1000}}\n";
        std::ofstream(d2 / "ri16.sigmf-data", std::ios::binary) << "abcd";
        CHECK(probeIqFile((d2 / "ri16.sigmf-meta").string(), info, err) && info.format == FileFormat::CI16, "ri16_le alias");
    }
    fs::remove_all(dir);
    printf(fails ? "sigmf: %d FAILED\n" : "sigmf: all passed\n", fails);
    return fails ? 1 : 0;
}
