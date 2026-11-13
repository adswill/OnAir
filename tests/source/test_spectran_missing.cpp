// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The SPECTRAN driver (source_spectran.cpp) when Aaronia's RTSA API is missing, and when it is there but does not start: nothing is listed,
// and opening the receiver or the transmitter fails with a message that says what to install (docs/SPECTRAN-V6.md).
//   test_spectran_missing <fake library>             : no library in the search folder
//   test_spectran_missing <fake library> --initfail  : the fake library, whose Init fails (run by the first form as a second process)
#include "../../core/src/source/native_common.h"
#include <filesystem>
#include <iostream>
using namespace dect2;

namespace dect2 { namespace native {
void listSpectran(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeSpectran(const DeviceInfo& d);
std::unique_ptr<TxSink> makeSpectranTx(const DeviceInfo& d);
} }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

static void setDir(const std::string& d) {
#ifdef _WIN32
    _putenv_s("DECT2_NATIVE_LIBDIR", d.c_str());
#else
    setenv("DECT2_NATIVE_LIBDIR", d.c_str(), 1);
#endif
}

static void openBoth(const char* expect) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native; d.board = "spectran"; d.nativeArgs = "spectranv6|V6-1000"; d.serial = "V6-1000";
    auto rx = native::makeSpectran(d);
    CHECK(rx != nullptr);
    IqRing ring(1 << 16);
    TuneSettings t;
    std::string err;
    if (rx) {
        CHECK(!rx->start(t, ring, err));
        CHECK(err.find(expect) != std::string::npos);
        std::cerr << "receive: " << err << '\n';
    }
    auto tx = native::makeSpectranTx(d);
    CHECK(tx != nullptr);
    if (tx) {
        TxTune tt; tt.centerHz = 433e6; tt.sampleRate = 1e6;
        err.clear();
        CHECK(!tx->open(tt, err));
        CHECK(err.find(expect) != std::string::npos || err.find("RTSA") != std::string::npos);
        std::cerr << "transmit: " << err << '\n';
    }
    d.nativeArgs = "nonsense";
    CHECK(native::makeSpectran(d) == nullptr);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("onair-spectran-miss-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + (argc > 2 ? "b" : "a"));
    fs::create_directory(dir);
    setDir(dir.string());
    if (argc > 2 && std::string(argv[2]) == "--initfail") {
#ifdef _WIN32
        const auto file = dir / "AaroniaRTSAAPI.dll";
#elif defined(__APPLE__)
        const auto file = dir / "libAaroniaRTSAAPI.dylib";
#else
        const auto file = dir / "libAaroniaRTSAAPI.so";
#endif
        fs::copy_file(argv[1], file);
        native::DynLib fake;
        void (*setv)(const char*, double) = nullptr;
        double (*num)(const char*) = nullptr;
        if (!fake.open({file.string()}) || !fake.get(setv, "fake_spectran_set") || !fake.get(num, "fake_spectran_num")) return 2;
        setv("fail_init", 1);
        std::vector<DeviceInfo> v;
        native::listSpectran(v);
        CHECK(v.empty());
        CHECK(num("init_with_path") == 1 && num("open") == 0);
        CHECK(native::spectranApiInstalled());   // the library is there; it did not start
        openBoth("did not start");
    } else {
        // the folder is empty: no RTSA API
        std::vector<DeviceInfo> v;
        native::listSpectran(v);
        CHECK(v.empty());
        CHECK(!native::spectranApiInstalled());
        openBoth("RTSA-Suite PRO is not installed");
        // the second case in a process of its own (the library is loaded once per process)
        std::string cmd = "\"" + std::string(argv[0]) + "\" \"" + std::string(argv[1]) + "\" --initfail";
#ifdef _WIN32
        cmd = "\"" + cmd + "\"";
#endif
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "spectran_missing: all checks passed" << (argc > 2 ? " (init failure)" : "") << '\n';
    return 0;
}
