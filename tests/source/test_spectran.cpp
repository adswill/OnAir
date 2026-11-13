// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Aaronia SPECTRAN V6 / V6 ECO driver (source_spectran.cpp) against the fake RTSA API (spectran_fake.cpp): listing, the IQ receiver and
// raw modes, the sample-rate plan, the reference level and the sample scale, live retunes, rate changes (the device stopped first), an
// unplug and reopen, a busy device, the transmitter's timed packets and its end. Never touches a real device.
// Only simulated: the fake follows Aaronia's public documentation, not a measured device (docs/SPECTRAN-V6.md).
#include "../../core/src/source/native_common.h"
#include <filesystem>
#include <iostream>
using namespace dect2;

namespace dect2 { namespace native {
void listSpectran(std::vector<DeviceInfo>& out);
std::unique_ptr<IqSource> makeSpectran(const DeviceInfo& d);
std::unique_ptr<TxSink> makeSpectranTx(const DeviceInfo& d);
double spectranRawRateForTest(double want, bool eco, int* clockIdx, int* decimLog2);
} }

static int failures = 0;
static constexpr double kPi = 3.14159265358979323846;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

static double (*num)(const char*) = nullptr;
static void (*setv)(const char*, double) = nullptr;
static const char* (*str)(const char*) = nullptr;

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
    const auto dir = fs::temp_directory_path() / ("onair-spectran-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
#ifdef _WIN32
    const auto file = dir / "AaroniaRTSAAPI.dll";
    _putenv_s("DECT2_NATIVE_LIBDIR", dir.string().c_str());
#elif defined(__APPLE__)
    const auto file = dir / "libAaroniaRTSAAPI.dylib";
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#else
    const auto file = dir / "libAaroniaRTSAAPI.so";
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#endif
    fs::copy_file(argv[1], file);
    native::DynLib fake;
    if (!fake.open({file.string()}) || !fake.get(num, "fake_spectran_num") || !fake.get(setv, "fake_spectran_set") || !fake.get(str, "fake_spectran_str")) return 2;
    setv("v6count", 1);
    setv("ecocount", 1);

    // ---- the rate plan of the raw mode
    int c = -2, k = -2;
    CHECK(native::spectranRawRateForTest(2e6, true, &c, &k) == 3.84e6 && c == -1 && k == 4);          // 61.44 / 16
    CHECK(native::spectranRawRateForTest(61.44e6, true, &c, &k) == 61.44e6 && k == 0);
    CHECK(native::spectranRawRateForTest(100e6, true, &c, &k) == 61.44e6);                               // the fastest there is
    CHECK(native::spectranRawRateForTest(2e6, false, &c, &k) == 2.4e6 && c == 2 && k == 5);              // 76.8 / 32
    CHECK(native::spectranRawRateForTest(23e6, false, &c, &k) == 23.04e6 && c == 0 && k == 1);           // 46.08 / 2 (= 92.16 / 4 of Aaronia's README: the slower clock)
    CHECK(native::spectranRawRateForTest(46.08e6, false, &c, &k) == 46.08e6 && c == 0 && k == 0);        // equal rates: the slower clock
    CHECK(native::spectranRawRateForTest(1e9, false, &c, &k) == 245.76e6 && c == 6 && k == 0);
    CHECK(native::spectranRawRateForTest(1e3, false, &c, &k) == 46.08e6 / 512);

    // ---- the list
    std::vector<DeviceInfo> devs;
    native::listSpectran(devs);
    CHECK(devs.size() == 2);
    if (devs.size() != 2) { std::cerr << "no devices\n"; return 1; }
    CHECK(num("init_with_path") == 1 && num("init") == 0 && num("memory") == 1 && num("rescan") >= 1);
    CHECK(fs::path(str("initpath")) == dir);
    const DeviceInfo& v6 = devs[0];
    const DeviceInfo& eco = devs[1];
    CHECK(v6.board == "spectran" && v6.nativeArgs == "spectranv6|V6-1000" && v6.serial == "V6-1000");
    CHECK(eco.nativeArgs == "spectranv6eco|ECO-1000" && eco.name.find("V6 ECO") != std::string::npos);
    CHECK(v6.maxRateHz == 245.76e6 && eco.maxRateHz == 61.44e6 && v6.canTransmit && eco.canTransmit);
    auto hasKey = [](const DeviceInfo& d, const char* key) { for (const auto& s : d.settings) if (s.key == key) return true; return false; };
    CHECK(hasKey(v6, "clock") && hasKey(v6, "rxchan") && !hasKey(eco, "clock") && !hasKey(eco, "rxchan") && hasKey(eco, "preamp") && hasKey(eco, "mode"));
    CHECK(num("bad_cbsize") == 0);

    IqRing ring(1 << 22);
    std::string err;
    TuneSettings t;
    t.centerHz = 433.92e6; t.sampleRate = 2e6; t.gainDb = 30;
    double mag = 0, step = 0;
    {
        // ---- V6, IQ receiver mode
        auto rx = native::makeSpectran(v6);
        CHECK(rx != nullptr);
        CHECK(rx->start(t, ring, err));
        if (!err.empty()) std::cerr << "start: " << err << '\n';
        CHECK(std::string(str("opentype")) == "spectranv6/iqreceiver");
        CHECK(approx(num("cfg:main/spanfreq"), 2e6 / 1.5, 1e-3));
        CHECK(std::string(str("cfg:device/receiverclock")) == "46MHz");
        CHECK(std::string(str("cfg:device/outputformat")) == "iq" && std::string(str("cfg:device/receiverchannel")) == "Rx1");
        CHECK(num("cfg:main/centerfreq") == 433.92e6 && num("cfg:main/reflevel") == -20);
        CHECK(rx->sampleRate() == 2e6);
        CHECK(tone(ring, mag, step));
        CHECK(approx(mag, 0.5, 0.01));                  // half the reference level arrives as 0.5
        CHECK(approx(step, kPi / 4, 1e-3));            // +rate/8: I and Q in their places
        // live: frequency with a correction, the reference level (the scale follows it), the amplifier
        const double starts = num("start");
        t.centerHz = 1296.1e6; t.radio = {{"ppm", "10"}, {"preamp", "Both"}}; t.gainDb = 0;
        CHECK(rx->retune(t, err));
        CHECK(approx(num("cfg:main/centerfreq"), 1296.1e6 / (1 + 10e-6), 1e-3));
        CHECK(num("cfg:main/reflevel") == 10 && std::string(str("cfg:calibration/preamp")) == "Both");
        CHECK(num("start") == starts);                // no restart for those
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01));
        // the device adjusts a reference level it does not have (V6 fake: -20 at the lowest): the scale follows what it took
        t.gainDb = 60; err.clear();
        CHECK(rx->retune(t, err));
        CHECK(num("cfg:main/reflevel") == -20);
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01));
        // a new rate: the device is stopped, set up and started again; nothing that sets the rate is sent while it streams
        t.gainDb = 30;
        for (double rate : {20e6, 100e6, 250e3, 300e6}) {
            t.sampleRate = rate; err.clear();
            const double before = num("stop");
            CHECK(rx->retune(t, err));
            CHECK(num("stop") == before + 1);
            const double span = std::min(rate, 245.76e6) / 1.5;
            CHECK(approx(num("cfg:main/spanfreq"), span, 1e-3));
            CHECK(approx(rx->sampleRate(), std::min(rate, 245.76e6), 1e-3));   // what the device delivers (300 Msps: its fastest)
            CHECK(tone(ring, mag, step) && approx(step, kPi / 4, 1e-3));
        }
        CHECK(std::string(str("cfg:device/receiverclock")) == "245MHz");
        CHECK(num("rate_change_while_streaming") == 0);
        // an unplug: the stream breaks, the driver opens the device again and the samples go on
        t.sampleRate = 2e6;
        CHECK(rx->retune(t, err));
        const double opens = num("opendevice");
        setv("fail_after", 5);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (num("opendevice") == opens && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(num("failed_stream") == 1 && num("opendevice") == opens + 1);
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01));
        CHECK(std::string(str("opentype")) == "spectranv6/iqreceiver" && approx(num("cfg:main/spanfreq"), 2e6 / 1.5, 1e-3));
        // the receiver has the device: the transmitter cannot have it too, and says why
        auto tx = native::makeSpectranTx(v6);
        TxTune tt; tt.centerHz = 433e6; tt.sampleRate = 1e6;
        err.clear();
        CHECK(!tx->open(tt, err) && err.find("busy") != std::string::npos);
        // a rescan while it is open keeps it listed
        std::vector<DeviceInfo> again;
        native::listSpectran(again);
        CHECK(again.size() == 2);
        rx->stop();
        CHECK(num("close_while_started") == 0 && num("bad_consume") == 0);
    }
    {
        // ---- V6 ECO, raw mode: 61.44 MHz / a power of two; the ECO has no receiver clock to set
        auto rx = native::makeSpectran(eco);
        t = TuneSettings();
        t.centerHz = 98.5e6; t.sampleRate = 2e6; t.gainDb = 50; t.radio = {{"mode", "raw"}};
        ring.clear(); err.clear();
        CHECK(rx->start(t, ring, err));
        CHECK(std::string(str("opentype")) == "spectranv6eco/raw");
        CHECK(std::string(str("cfg:main/decimation")) == "1 / 16");
        CHECK(rx->sampleRate() == 3.84e6);
        CHECK(num("cfg:main/reflevel") == -40);   // -40 dBm: in the ECO fake's range
        CHECK(tone(ring, mag, step) && approx(mag, 0.5, 0.01) && approx(step, kPi / 4, 1e-3));
        t.sampleRate = 61.44e6;
        CHECK(rx->retune(t, err));
        CHECK(std::string(str("cfg:main/decimation")) == "Full" && rx->sampleRate() == 61.44e6);
        rx->stop();
    }
    {
        // ---- V6 raw at a clock the device refuses: a clear error
        setv("no245", 1);
        auto rx = native::makeSpectran(v6);
        t = TuneSettings();
        t.centerHz = 2.4e9; t.sampleRate = 245.76e6; t.radio = {{"mode", "raw"}};
        err.clear();
        CHECK(!rx->start(t, ring, err));
        CHECK(err.find("receiverclock") != std::string::npos);
        setv("no245", 0);
        // a device that is not connected: the start says so
        setv("fail_connect", 1);
        t.sampleRate = 2e6; err.clear();
        auto rx2 = native::makeSpectran(v6);
        CHECK(!rx2->start(t, ring, err));
        CHECK(err.find("did not connect") != std::string::npos);
        setv("fail_connect", 0);
    }
    {
        // ---- transmit on the V6 ECO: timed packets, kept ahead of the device clock, no gaps, 1.0 = 0 dBm, the end flags at close
        auto tx = native::makeSpectranTx(eco);
        TxTune tt;
        tt.centerHz = 145.5e6; tt.offsetHz = 1000; tt.sampleRate = 1e6; tt.gainDb = -10;   // the default lead of the ECO (0.5 s): a 50 ms lead failed on loaded macOS runners with one late packet
        err.clear();
        CHECK(tx->open(tt, err));
        if (!err.empty()) std::cerr << "tx open: " << err << '\n';
        CHECK(std::string(str("opentype")) == "spectranv6eco/iqtransmitter");
        CHECK(num("cfg:main/centerfreq") == 145.501e6 && num("cfg:main/transgain") == -10 && num("cfg:main/spanfreq") == 1e6);
        CHECK(tx->sampleRate() == 1e6);
        std::vector<cf32> x(10000, cf32(1.f, 0.f));
        x[5] = cf32(3.f, 0.f);   // beyond full scale: clipped
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 30; i++) CHECK(tx->write(x.data(), x.size(), err));   // 0.3 s of samples
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(took > 0.15);                       // paced by the device clock, not taken all at once
        CHECK(num("tx_samples") == 300000);
        std::cerr << "tx counters: gaps " << num("tx_gaps") << " late " << num("tx_late") << " bad_end " << num("tx_bad_end") << " bad_packet " << num("bad_packet") << " took " << took << " s\n";
        CHECK(num("tx_gaps") == 0 && num("tx_late") == 0 && num("tx_bad_end") == 0 && num("bad_packet") == 0);
        CHECK(num("tx_starts") == 1 && num("tx_ends") == 0);
        CHECK(approx(num("tx_peak"), std::sqrt(0.05), 1e-5));
        CHECK(num("tx_startfreq") == 145.501e6 - 0.5e6 && num("tx_rate") == 1e6);
        CHECK(num("tx_ahead") <= 0.51);
        const double closes = num("closedevice");
        tx->close();
        CHECK(num("tx_ends") == 1 && num("closedevice") == closes + 1);
        // a device without the TX option: the open fails with the reason
        setv("no_tx", 1);
        err.clear();
        auto tx2 = native::makeSpectranTx(v6);
        CHECK(!tx2->open(tt, err));
        CHECK(err.find("iqtransmitter") != std::string::npos);
        setv("no_tx", 0);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
    std::cout << "spectran: all checks passed\n";
    return 0;
}
