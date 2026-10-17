// RSPduo regression through the public IqSource interface and a strict vendor-library fake.
// Runs on Windows, Linux and macOS; never opens a real radio.
#include "../core/src/native_common.h"
#include <filesystem>
#include <iostream>
using namespace dect2;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

static bool samples(IqRing& ring) {
    std::vector<cf32> b(65536);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end) {
        const auto n = ring.read(b.data(), b.size());
        if (n) {
            for (size_t i = 0; i < n; ++i)
                if (std::abs(b[i] - cf32(0.5f, -0.25f)) > 1e-6f) return false;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("onair-sdrplay-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
#ifdef _WIN32
    const auto file = dir / "sdrplay_api.dll";
    _putenv_s("DECT2_NATIVE_LIBDIR", dir.string().c_str());
#else
    const auto file = dir / "libsdrplay_api.so";
    setenv("DECT2_NATIVE_LIBDIR", dir.string().c_str(), 1);
#endif
    fs::copy_file(argv[1], file);
    native::DynLib fake;
    double (*state)(const char*) = nullptr;
    void (*set)(const char*, double) = nullptr;
    if (!fake.open({file.string()}) || !fake.get(state, "fake_state") || !fake.get(set, "fake_set")) return 2;
    set("sdrplay.model", 3);
    std::vector<DeviceInfo> devices;
    native::listSdrplay(devices);
    CHECK(devices.size() == 3);
    if (devices.size() != 3) return 1;
    CHECK(!devices[0].hasBiasTee && devices[1].hasBiasTee && !devices[2].hasBiasTee);
    CHECK(devices[2].maxFreqHz == 30e6 && devices[2].gainMaxDb == 76);
    CHECK(devices[0].minRateHz == 62500 && devices[0].maxRateHz == 10000000);
    IqRing ring(1 << 20);
    TuneSettings t;
    t.centerHz = 7.1e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2;
    std::string err;
    for (int port : {0, 1, 2}) {
        auto source = native::makeSdrplay(devices[port]);
        t.gainDb = 0; t.biasTee = true; // stale power-on setting must be harmless on tuner 1 / Hi-Z
        ring.clear(); err.clear();
        const bool ok = source->start(t, ring, err);
        CHECK(ok);
        if (!ok) { std::cerr << err << '\n'; continue; }
        CHECK(state("sdrplay.tuner") == (port == 1 ? 2 : 1));
        CHECK(state("sdrplay.mode") == 1);
        CHECK(state("sdrplay.biasDuo") == (port == 1 ? 1 : 0));
        CHECK(state("sdrplay.amPortDuo") == (port == 2 ? 1 : 0));
        CHECK(samples(ring)); // B has only rxChannelB and emits only StreamBCbFn
        const auto inits = state("sdrplay.inits");
        for (double gain : {0.0, 20.0, 40.0, 60.0, 76.0, 103.0}) {
            t.gainDb = gain;
            CHECK(source->retune(t, err));
            CHECK(state("sdrplay.lna") >= 0 && state("sdrplay.lna") < (port == 2 ? 5 : 7));
            CHECK(state("sdrplay.grdb") >= 20 && state("sdrplay.grdb") <= 59);
        }
        t.centerHz = 14.2e6; t.radio = {{"ppm", "1.25"}, {"rfnotch", "1"}, {"dabnotch", "1"}};
        CHECK(source->retune(t, err));
        CHECK(state("sdrplay.rf") == t.centerHz && state("sdrplay.ppm") == 1.25);
        CHECK(state(port == 2 ? "sdrplay.amNotchDuo" : "sdrplay.rfNotchDuo") == 1);
        CHECK(state("sdrplay.dabNotchDuo") == 1 && state("sdrplay.inits") == inits);
        for (double rate : {62500.0, 250000.0, 1000000.0, 2000000.0, 8000000.0, 10660000.0}) {
            t.sampleRate = rate;
            CHECK(source->retune(t, err));
            CHECK(source->sampleRate() == std::min(rate, 10e6));
            if (rate > 10e6) CHECK(err.find("only offers") != std::string::npos);
            ring.clear();
            CHECK(samples(ring));
        }
        source->stop(); source->stop();
        CHECK(state("sdrplay.selected") == 0 && state("sdrplay.init") == 0);
        CHECK(state("sdrplay.biasDuo") == 0);
        t.radio.clear(); t.biasTee = false; t.sampleRate = 2e6; t.centerHz = 7.1e6;
    }
    // Retuning while unplugged must preserve automatic reconnection, without waiting for an Init that was not started.
    auto source = native::makeSdrplay(devices[0]);
    CHECK(source->start(t, ring, err));
    set("sdrplay.absent", 1); set("sdrplay.remove", 1);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (state("sdrplay.selected") == 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(state("sdrplay.selected") == 0);
    t.sampleRate = 250e3; t.centerHz = 14.2e6;
    const auto before = std::chrono::steady_clock::now();
    CHECK(source->retune(t, err));
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(200));
    ring.clear(); set("sdrplay.absent", 0);
    CHECK(samples(ring));
    CHECK(source->sampleRate() == 250e3 && state("sdrplay.rf") == 14.2e6);
    source->stop();
    // A failed asynchronous Init must reach start(), and release the selection.
    set("sdrplay.initFail", 1);
    for (int detail : {0, 1, 2}) {   // empty details, a vendor message, and a null error-info pointer
        set("sdrplay.errorDetail", detail);
        CHECK(!source->start(t, ring, err));
        CHECK(err.find("did not start") != std::string::npos && state("sdrplay.selected") == 0);
        CHECK((err.find("vendor detail: sample rate rejected") != std::string::npos) == (detail == 1));
    }
    set("sdrplay.initFail", 0);
    CHECK(source->start(t, ring, err));
    set("sdrplay.initFail", 1);
    set("sdrplay.errorDetail", 1);
    t.sampleRate = 4e6;
    CHECK(!source->retune(t, err));
    CHECK(err.find("did not start") != std::string::npos && state("sdrplay.selected") == 0);
    CHECK(err.find("vendor detail: sample rate rejected") != std::string::npos);
    set("sdrplay.initFail", 0);
    set("sdrplay.errorDetail", 0);
    // An Init that returns success after the five-second deadline must still fail and release its stream/selection.
    for (bool restart : {false, true}) {
        if (restart) CHECK(source->start(t, ring, err));
        set("sdrplay.initDelayMs", 6000);
        const auto began = std::chrono::steady_clock::now();
        if (restart) { t.sampleRate = 2e6; CHECK(!source->retune(t, err)); }
        else CHECK(!source->start(t, ring, err));
        CHECK(std::chrono::steady_clock::now() - began >= std::chrono::seconds(5));
        CHECK(err.find("timed out waiting") != std::string::npos);
        CHECK(state("sdrplay.selected") == 0 && state("sdrplay.init") == 0);
        set("sdrplay.initDelayMs", 0);
    }
    CHECK(source->start(t, ring, err));
    ring.clear(); CHECK(samples(ring));
    source->stop();
    // Do not silently configure A if the API has no settings for selected tuner B.
    set("sdrplay.missingB", 1);
    source = native::makeSdrplay(devices[1]);
    CHECK(!source->start(t, ring, err));
    CHECK(err.find("no settings for tuner 2") != std::string::npos && state("sdrplay.selected") == 0);
    set("sdrplay.missingB", 0);
    set("sdrplay.slaveOnly", 1);
    devices.clear(); native::listSdrplay(devices);
    CHECK(devices.empty());
    std::cout << "sdrplay: " << failures << " failure(s)\n";
    // DLL is held by the driver until process exit on Windows; do not unload it under its static API object.
#ifndef _WIN32
    fs::remove(file); fs::remove(dir);
#endif
    return failures ? 1 : 0;
}
