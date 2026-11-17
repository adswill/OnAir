// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Tektronix RSA driver (source_rsa.cpp) against the fake RSA API (rsa_fake.cpp): a device in use elsewhere, the list, the rate
// steps, the reference level and the sample scale, the order of I and Q, retunes and a new rate (the device stopped first), a stop
// that keeps the connection, the preamplifier of a model without one and with one, and an unplug. Never touches a real device.
// The fake behaves as the RSA306 that was measured (docs/TEKTRONIX-RSA.md).
#include "../../core/src/source/native_common.h"
#include <filesystem>
#include <iostream>
using namespace dect2;

namespace dect2 { namespace native {
void listRsa(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeRsa(const DeviceInfo& d);
} }

static int failures = 0;
static constexpr double kPi = 3.14159265358979323846;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

static double (*num)(const char*) = nullptr;
static void (*setv)(const char*, double) = nullptr;

static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// waits for fresh samples (the ring emptied first, then 150 ms), returns the mean magnitude and the mean phase step of the last 4096
static bool tone(IqRing& ring, double& mag, double& step) {
    std::vector<cf32> b(1 << 20);
    while (ring.read(b.data(), b.size())) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    size_t got = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (got < 8192 && std::chrono::steady_clock::now() < end) {
        got += ring.read(b.data() + got, b.size() - got);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (got < 8192) return false;
    const cf32* x = b.data() + got - 4096;
    double m = 0; std::complex<double> s = 0;
    for (int k = 0; k < 4096; k++) {
        m += std::abs(x[k]);
        if (k) s += std::complex<double>(x[k]) * std::conj(std::complex<double>(x[k - 1]));
    }
    mag = m / 4096;
    step = std::arg(s);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("onair-rsa-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
#ifdef _WIN32
    const auto file = dir / "RSA_API.dll";
    _putenv_s("DECT2_NATIVE_LIBDIR", dir.string().c_str());
#elif defined(__APPLE__)
    const auto file = dir / "libRSA_API.dylib";
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#else
    const auto file = dir / "libRSA_API.so";
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#endif
    fs::copy_file(argv[1], file);
    native::DynLib fake;
    if (!fake.open({file.string()}) || !fake.get(num, "fake_rsa_num") || !fake.get(setv, "fake_rsa_set")) return 2;

    // ---- the list: one RSA306, its rates and frequency range, nothing connected for it
    std::vector<DeviceInfo> devs;
    native::listRsa(devs);
    CHECK(devs.size() == 1);
    if (devs.size() != 1) { std::cerr << "no device\n"; return 1; }
    const DeviceInfo dev = devs[0];
    CHECK(dev.board == "rsa" && dev.serial == "B010000" && dev.nativeArgs == "RSA306|B010000");
    CHECK(dev.name.find("Tektronix RSA306 B010000") != std::string::npos);
    CHECK(dev.maxRateHz == 56e6 && dev.minRateHz == 56e6 / 4096 && dev.minFreqHz == 9e3 && dev.maxFreqHz == 6.2e9 && !dev.canTransmit);
    CHECK(num("search") == 1 && num("connect") == 0);
    DeviceInfo bad = dev;
    bad.nativeArgs = "nonsense";
    CHECK(native::makeRsa(bad) == nullptr);

    IqRing ring(1 << 22);
    std::string err;
    TuneSettings t;
    double mag = 0, step = 0;
    {
        // ---- a device another program has: the start says so
        setv("busy", 1);
        auto rx = native::makeRsa(dev);
        t.centerHz = 100e6; t.sampleRate = 2e6; t.gainDb = 60;
        CHECK(!rx->start(t, ring, err));
        CHECK(err.find("did not connect") != std::string::npos);
        setv("busy", 0);
    }
    auto rx = native::makeRsa(dev);
    CHECK(rx != nullptr);
    {
        // ---- a start: 2 Msps is asked for, the slowest rate at or above it is 3.5 Msps (a bandwidth of 2.5 MHz)
        err.clear();
        CHECK(rx->start(t, ring, err));
        if (!err.empty()) std::cerr << "start: " << err << '\n';
        CHECK(num("connect") == 1 && num("run") == 1 && num("stream_start") == 1);
        CHECK(num("bandwidth") == 2.5e6 && rx->sampleRate() == 3.5e6 && num("dest") == 0 && num("dtype") == 0);
        CHECK(num("centre") == 100e6 && num("ref") == -30);   // gain 60 dB: 30 dBm - 60
        CHECK(tone(ring, mag, step));
        CHECK(approx(mag, 0.5, 0.01));            // half the reference level arrives as 0.5
        CHECK(approx(step, kPi / 4, 1e-3));      // +rate/8: I and Q in their places
        CHECK(num("run_rate") == 3.5e6);
        // a retune: the frequency with a correction, the reference level (the scale follows it). The device takes nothing while it
        // runs, so it is stopped, set and run again; the stream goes on
        t.centerHz = 7.1e6; t.radio = {{"ppm", "10"}}; t.gainDb = 70;
        CHECK(rx->retune(t, err));
        CHECK(approx(num("centre"), 7.1e6 / (1 + 10e-6), 1e-3) && num("ref") == -40);
        CHECK(dev.gainMaxDb == 75);   // a gain beyond the top is taken as the top: -45 dBm
        CHECK(approx(num("centre_run"), 7.1e6 / (1 + 10e-6), 1e-3));
        CHECK(num("run") == 2 && num("stop") == 1 && num("centre_while_running") == 0 && num("bandwidth_while_running") == 0);
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01) && approx(step, kPi / 4, 1e-3));
        // the same settings again: nothing is stopped
        CHECK(rx->retune(t, err));
        CHECK(num("run") == 2 && num("stop") == 1);
        // another rate: the device is stopped, the bandwidth set, and run again (never while it runs)
        t.sampleRate = 10e6; t.radio.clear();
        CHECK(rx->retune(t, err));
        CHECK(num("bandwidth") == 10e6 && rx->sampleRate() == 14e6 && num("run") == 3 && num("stop") == 2 && num("bandwidth_while_running") == 0);
        CHECK(tone(ring, mag, step) && approx(step, kPi / 4, 1e-3) && num("run_rate") == 14e6);
        // every step: the exact rate, the widest and the narrowest at the ends
        const struct { double want, rate; } steps[] = {{56e6, 56e6}, {40e6, 56e6}, {28e6, 28e6}, {20e6, 28e6}, {7e6, 7e6}, {3.5e6, 3.5e6}, {1e6, 1.75e6},
                                                       {250e3, 437.5e3}, {48e3, 54687.5}, {12e3, 13671.875}, {100.0, 13671.875}, {100e6, 56e6}};
        for (const auto& s : steps) {
            t.sampleRate = s.want;
            err.clear();
            CHECK(rx->retune(t, err));
            if (rx->sampleRate() != s.rate) std::cerr << "rate for " << s.want << ": " << rx->sampleRate() << '\n';
            CHECK(rx->sampleRate() == s.rate);
            if (s.want == 100e6) CHECK(err.find("only offers") != std::string::npos);   // beyond the device: the message says so
        }
        CHECK(num("bandwidth_while_running") == 0 && num("centre_while_running") == 0);
        // a stop ends the stream and keeps the connection (a disconnect takes 4 s on the device); the next start does not connect again
        rx->stop();
        CHECK(num("stream_stop") >= 1 && num("disconnect") == 0);
        t.sampleRate = 2e6;
        err.clear();
        CHECK(rx->start(t, ring, err));
        CHECK(num("connect") == 1 && num("connect_twice") == 0 && rx->sampleRate() == 3.5e6);
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01));
        // the list while the device is connected: the last one stands, the API is not asked to search
        const double searches = num("search");
        std::vector<DeviceInfo> again;
        native::listRsa(again);
        CHECK(again.size() == 1 && again[0].serial == dev.serial && num("search") == searches && num("search_while_connected") == 0);
        rx->stop();
    }
    {
        // ---- the preamplifier: an RSA306 has none (the call succeeds and it stays off: the start says so); a model with one takes it
        t.radio = {{"preamp", "on"}};
        err.clear();
        CHECK(rx->start(t, ring, err));
        CHECK(err.find("no preamplifier") != std::string::npos && num("preamp") == 0);
        rx->stop();
        setv("has_preamp", 1);
        err.clear();
        CHECK(rx->start(t, ring, err));
        CHECK(err.find("no preamplifier") == std::string::npos && num("preamp") == 1);
        rx->stop();
        t.radio = {{"preamp", "off"}};
        CHECK(rx->start(t, ring, err));
        CHECK(num("preamp") == 0);
        // "keep": the device is left as it is
        const double sets = num("set_preamp");
        rx->stop();
        t.radio.clear();
        CHECK(rx->start(t, ring, err));
        CHECK(num("set_preamp") == sets);
    }
    {
        // ---- unplugged while it streams: the connection is given up and the device is looked for and connected again
        const double connects = num("connect");
        CHECK(tone(ring, mag, step));
        setv("fail_after", num("blocks") + 5);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (std::chrono::steady_clock::now() < end && num("connect") == connects) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(num("failed_stream") == 1 && num("disconnect") >= 1 && num("connect") == connects + 1);
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01) && approx(step, kPi / 4, 1e-3));
        rx->stop();
    }
    {
        // ---- a second source while the first one streams: refused (the API has one device at a time)
        CHECK(rx->start(t, ring, err));
        auto rx2 = native::makeRsa(dev);
        IqRing ring2(1 << 16);
        err.clear();
        CHECK(!rx2->start(t, ring2, err));
        CHECK(err.find("already in use") != std::string::npos);
        CHECK(tone(ring, mag, step));   // the first one is not disturbed
        rx->stop();
    }
    rx.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "rsa: all checks passed\n";
    return 0;
}
