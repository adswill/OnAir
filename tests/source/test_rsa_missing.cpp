// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Tektronix RSA driver (source_rsa.cpp) when Tektronix's RSA API is missing: nothing is listed, and opening the receiver fails with
// a message that says what to install (docs/TEKTRONIX-RSA.md).
#include "../../core/src/source/native_common.h"
#include <filesystem>
#include <iostream>
using namespace dect2;

namespace dect2 { namespace native {
void listRsa(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeRsa(const DeviceInfo& d);
} }

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("onair-rsa-miss-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    // the folder is empty: no RSA API
#ifdef _WIN32
    _putenv_s("DECT2_NATIVE_LIBDIR", dir.string().c_str());
#else
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#endif
    std::vector<DeviceInfo> v;
    native::listRsa(v);
    CHECK(v.empty());
    CHECK(!native::rsaApiInstalled());
    DeviceInfo d;
    d.kind = DeviceInfo::Native; d.board = "rsa"; d.nativeArgs = "RSA306|B010000"; d.serial = "B010000";
    auto rx = native::makeRsa(d);
    CHECK(rx != nullptr);
    IqRing ring(1 << 16);
    TuneSettings t;
    std::string err;
    if (rx) {
        CHECK(!rx->start(t, ring, err));
        CHECK(err.find("RSA API is not installed") != std::string::npos);
        std::cerr << "receive: " << err << '\n';
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "rsa_missing: all checks passed\n";
    return 0;
}
