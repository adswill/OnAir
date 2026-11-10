// Native Pluto RX/TX against fake libiio, including the shared clock and handle.
// This test never opens a real radio. It also runs on Windows without SoapySDR.
#include "../../core/src/source/native_common.h"
#include "dect2/tx/tx.h"
#include <filesystem>
#include <functional>
#include <iostream>
using namespace dect2;
namespace fs = std::filesystem;
static int failures = 0;
#define CHECK(c, message) do { if (!(c)) { std::cerr << "FAIL: " << message << '\n'; ++failures; } } while (0)
static double (*state)(const char*) = nullptr;
static void (*set)(const char*, double) = nullptr;
static double st(const char* key) { return std::max(0.0, state(key)); }
static bool waitFor(const std::function<bool()>& ready, int ms = 500) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        if (ready()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < until);
    return ready();
}
static void env(const char* key, const std::string& value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}
static void receive(IqRing& ring) {
    std::vector<cf32> x(1 << 16);
    while (ring.read(x.data(), x.size())) {}
    CHECK(waitFor([&] { return ring.read(x.data(), x.size()) > 0; }), "RX still delivers after the other direction changes");
    CHECK(std::abs(x[0] - cf32(0.5f, -0.25f)) < 0.001f, "RX sample conversion");
}
int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "usage: test_pluto_duplex <native_fakes library>\n"); return 2; }
    // Keep test artifacts next to the supplied library (also for standalone builds).
    const fs::path dir = fs::absolute(argv[1]).parent_path() /
        ("pluto-duplex-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
#ifdef _WIN32
    const auto lib = dir / "libiio.dll";
#elif defined(__APPLE__)
    const auto lib = dir / "libiio.dylib";
#else
    const auto lib = dir / "libiio.so";
#endif
    fs::copy_file(argv[1], lib);
    env("DECT2_NATIVE_LIBDIR", dir.string()); env("DECT2_NO_NATIVE", "0");
    native::DynLib fake;
    if (!fake.open({lib.string()}) || !fake.get(state, "fake_state") || !fake.get(set, "fake_set")) return 2;
    for (const char* key : {"pluto.contextOpens", "pluto.contextCloses", "pluto.rateWrites", "pluto.rxBufferOpens",
                           "pluto.txBufferOpens", "pluto.xoWrites", "pluto.firWrites"}) set(key, 0);
    // Attribute reads must respect even empty and truncated caller buffers.
    long long (*readAttr)(const void*, const char*, char*, size_t) = nullptr;
    if (!fake.get(readAttr, "iio_channel_attr_read")) return 2;
    set("pluto.sharedRate", 4000000);
    char small[3] = {'x', 'x', '!'};
    CHECK(readAttr(nullptr, "sampling_frequency", nullptr, 0) < 0, "empty attribute buffer refused");
    CHECK(readAttr(nullptr, "sampling_frequency", nullptr, 2) < 0, "null attribute buffer refused");
    CHECK(readAttr(nullptr, "sampling_frequency", small, 0) < 0 && small[0] == 'x', "zero capacity leaves buffer untouched");
    CHECK(readAttr(nullptr, "sampling_frequency", small, 1) == 0 && small[0] == '\0' && small[1] == 'x', "one-byte buffer only receives NUL");
    CHECK(readAttr(nullptr, "sampling_frequency", small, 2) == 1 && small[0] == '4' && small[1] == '\0' && small[2] == '!', "truncated attribute stays within capacity");
    set("pluto.singleContext", 1);   // a second independent USB context must fail
    const auto d = native::plutoDevice("ip:10.0.0.7");
    CHECK(d.canTransmit && d.txFullDuplex, "native Pluto advertises simultaneous RX/TX");
    const auto firOption = std::find_if(d.settings.begin(), d.settings.end(), [](const RadioSetting& v) { return v.key == "lowratefir"; });
    CHECK(firOption != d.settings.end() && firOption->def == "0" && firOption->restart, "RX-only FIR is opt-in and restarts RX");
    auto rx = native::makePluto(d);
    auto tx = native::makePlutoTx(d);
    CHECK(tx->fullDuplex(), "sink advertises simultaneous RX/TX");
    IqRing ring(1 << 18);
    TuneSettings r; r.centerHz = 435e6; r.sampleRate = 4e6; r.radio["ppm"] = "10";
    TxTune t; t.centerHz = 145e6; t.sampleRate = 2.1e6; t.gainDb = -60; t.radio["ppm"] = "10";
    const std::vector<cf32> samples(1 << 15, cf32(0.25f, -0.125f));
    std::string err;
    auto send = [&] { err.clear(); CHECK(tx->write(samples.data(), samples.size(), err), "TX writes: " << err); };
    auto noLeaks = [&] {
        CHECK(st("pluto.contextOpens") == st("pluto.contextCloses"), "last direction closes context");
        CHECK(st("pluto.rxBufferOpens") == st("pluto.rxBufferCloses"), "RX buffers released");
        CHECK(st("pluto.txBufferOpens") == st("pluto.txBufferCloses"), "TX buffers released");
        CHECK(st("pluto.closeWithBuffers") == 0 && st("pluto.duplicateBuffers") == 0, "no live buffer loses its context");
    };
    // RX first: use its faster clock, do not program the sample rate a second time.
    CHECK(rx->start(r, ring, err), "RX first: " << err);
    receive(ring);
    const double opens = st("pluto.contextOpens"), rates = st("pluto.rateWrites");
    CHECK(tx->open(t, err), "TX joins RX: " << err);
    CHECK(tx->sampleRate() == 4e6 && st("pluto.contextOpens") == opens && st("pluto.rateWrites") == rates, "TX adopts RX rate and context");
    CHECK(st("pluto.phy:altvoltage1.frequency") == t.centerHz, "TX does not apply RX's shared ppm twice");
    send(); receive(ring);
    {   // TX retune while sending (plutoTxPlan, docs/TX-CONTROL-3.8.md): the LO, the attenuation and the filter; the shared rate is not touched
        TxTune t2 = t; t2.centerHz = 146e6; t2.gainDb = -20; t2.bandwidthHz = 1e6;
        const double rw = st("pluto.rateWrites");
        CHECK(tx->retune(t2, err), "TX retunes while sending: " << err);
        CHECK(st("pluto.phy:altvoltage1.frequency") == 146e6 && st("pluto.rateWrites") == rw, "TX retune moves the LO, not the shared rate");
        CHECK(st("pluto.phy:voltage0.hardwaregain=-20.00") == 1, "TX retune sets the attenuation");
        CHECK(st("pluto.phy:voltage0.rf_bandwidth") == 1e6, "TX retune sets the filter");
        CHECK(tx->retune(t, err) && st("pluto.phy:altvoltage1.frequency") == t.centerHz, "TX retunes back: " << err);
        send(); receive(ring);
    }
    // Live retune can change LO/gain/ppm without moving the shared reference clock.
    r.centerHz += 1e6; r.gainDb = 25; r.radio["ppm"] = "20";
    const double xoWrites = st("pluto.xoWrites");
    CHECK(rx->retune(r, err), "RX tunes during TX: " << err);
    CHECK(st("pluto.xoWrites") == xoWrites, "live ppm does not change TX's clock");
    CHECK(std::abs(st("pluto.phy:altvoltage0.frequency") - ppmCorrectedHz(r.centerHz, 20) * (1.0 + 10e-6)) < 1, "RX residual ppm uses its LO");
    send(); receive(ring);
    // Large differing corrections expose subtraction approximations in shared-XO tuning.
    r.radio["ppm"] = "-900";
    CHECK(rx->retune(r, err), "RX changes residual ppm");
    CHECK(std::abs(st("pluto.phy:altvoltage0.frequency") - ppmCorrectedHz(r.centerHz, -900) * (1.0 + 10e-6)) < 1, "exact shared-XO frequency ratio");
    r.radio["ppm"] = "20"; CHECK(rx->retune(r, err), "restore RX correction");
    TuneSettings bad = r; bad.sampleRate = 8e6;
    const double buffers = st("pluto.rxBufferOpens");
    CHECK(!rx->retune(bad, err) && err.find("shares one sample rate") != std::string::npos, "conflicting RX rate is refused");
    CHECK(st("pluto.rxBufferOpens") == buffers && st("pluto.rateWrites") == rates, "refusal leaves RX buffer and shared rate alone");
    receive(ring); send();
    bad = r; bad.sampleRate = 2.048e6;
    const double firWrites = st("pluto.firWrites");
    CHECK(rx->retune(bad, err), "low-rate RX uses TX's clock: " << err);
    CHECK(rx->sampleRate() == 4e6 && st("pluto.firWrites") == firWrites && st("pluto.rateWrites") == rates,
          "low-rate request never loads a FIR or changes TX's clock");
    bad.radio["lowratefir"] = "1";
    CHECK(!rx->retune(bad, err) && err.find("disable Low-rate FIR") != std::string::npos,
          "explicit RX-only FIR is refused with an actionable error");
    receive(ring); send();
    // A transient RX error must reconnect with the last accepted settings while TX stays open.
    const double rxCloses = st("pluto.rxBufferCloses"), rxOpens = st("pluto.rxBufferOpens");
    set("pluto.rxFail", 1);
    CHECK(waitFor([&] { return st("pluto.rxBufferCloses") > rxCloses; }), "RX releases failed buffer");
    send(); set("pluto.rxFail", 0);
    CHECK(waitFor([&] { return st("pluto.rxBufferOpens") > rxOpens; }, 3000), "RX reconnects during TX after rejected retunes");
    CHECK(st("pluto.contextOpens") == opens && st("pluto.rateWrites") == rates, "RX reconnect preserves TX context and clock");
    receive(ring); send();
    // A duplicate direction must not close or disable the original's resources.
    auto extraRx = native::makePluto(d); auto extraTx = native::makePlutoTx(d);
    CHECK(!extraRx->start(r, ring, err), "second RX refused");
    CHECK(!extraTx->open(t, err), "second TX refused");
    extraRx->stop(); extraTx->close(); receive(ring); send();
    const double closes = st("pluto.contextCloses");
    tx->close(); tx->close();
    CHECK(st("pluto.contextCloses") == closes && st("pluto.rx:voltage0.enabled") == 1, "TX stop retains RX context and channels");
    receive(ring);
    CHECK(tx->open(t, err), "TX restarts alongside RX: " << err);
    rx->stop(); rx->stop();
    CHECK(st("pluto.contextCloses") == closes && st("pluto.tx:voltage0.enabled") == 1, "RX stop retains TX context and channels");
    CHECK(st("pluto.phy.xo_correction") == 40000400, "RX stop leaves shared clock unchanged while TX runs");
    send(); tx->close();
    CHECK(st("pluto.phy.xo_correction") == 40000000, "last close restores clock"); noLeaks();
    // TX first: RX can adopt its higher clock without writing shared configuration.
    t.sampleRate = 4e6; CHECK(tx->open(t, err), "TX first: " << err);
    r.sampleRate = 2.048e6; r.radio["ppm"] = "10";
    const double txRates = st("pluto.rateWrites"), txXo = st("pluto.xoWrites");
    CHECK(rx->start(r, ring, err), "RX joins TX: " << err);
    CHECK(rx->sampleRate() == 4e6 && st("pluto.rateWrites") == txRates && st("pluto.xoWrites") == txXo, "RX adopts TX rate and preserves clock");
    receive(ring); send(); rx->stop(); send(); tx->close(); noLeaks();
    // The default low-rate RX must also allow TX to join, without a FIR or a clock change.
    t.sampleRate = 2.1e6; r.sampleRate = 2.048e6;
    const double beforeFir = st("pluto.firWrites");
    CHECK(rx->start(r, ring, err), "default low-rate RX starts: " << err);
    CHECK(rx->sampleRate() == 2.1e6 && st("pluto.firWrites") == beforeFir, "RX defaults to a duplex-compatible clock");
    const double beforeJoin = st("pluto.rateWrites");
    CHECK(tx->open(t, err), "TX joins default 2.048 Msps RX: " << err);
    CHECK(tx->sampleRate() == 2.1e6 && st("pluto.rateWrites") == beforeJoin, "TX preserves the RX clock");
    receive(ring); send(); rx->stop(); tx->close(); noLeaks();
    // Reproduce the reported startup failure with TX already running at the minimum rate.
    CHECK(tx->open(t, err), "minimum-rate TX starts");
    r.radio["lowratefir"] = "1";
    CHECK(!rx->start(r, ring, err) && err.find("disable Low-rate FIR") != std::string::npos,
          "explicit RX-only FIR cannot join TX");
    send(); r.radio.erase("lowratefir");
    CHECK(rx->start(r, ring, err), "2.048 Msps RX joins 2.1 Msps TX: " << err);
    CHECK(rx->sampleRate() == 2.1e6 && st("pluto.firWrites") == beforeFir, "RX uses the minimum TX clock without a FIR");
    receive(ring); send(); rx->stop(); tx->close(); noLeaks();
    t.sampleRate = 4e6;
    // Startup failures release only the failing direction, and a subsequent start works.
    r.sampleRate = 4e6; CHECK(rx->start(r, ring, err), "RX for failed TX");
    for (const char* fault : {"pluto.failTxBuffer", "pluto.failTxFrequency", "pluto.tdd", "pluto.ensmUnreadable"}) {
        set(fault, 1); CHECK(!tx->open(t, err), "TX failure injected: " << fault);
        tx->close(); receive(ring); set(fault, 0);
    }
    t.sampleRate = 8e6; CHECK(!tx->open(t, err), "TX cannot raise active RX rate"); receive(ring);
    t.sampleRate = 4e6; CHECK(tx->open(t, err), "TX recovers after startup failures"); send();
    set("pluto.txFail", 1); CHECK(!tx->write(samples.data(), samples.size(), err), "TX stream failure reported");
    tx->close(); set("pluto.txFail", 0); receive(ring); rx->stop(); noLeaks();
    CHECK(tx->open(t, err), "TX for failed RX");
    set("pluto.failRxBuffer", 1); CHECK(!rx->start(r, ring, err), "RX buffer failure injected");
    rx->stop(); send(); set("pluto.failRxBuffer", 0);
    bad = r; bad.sampleRate = 8e6; CHECK(!rx->start(bad, ring, err), "RX cannot raise active TX rate"); send();
    CHECK(rx->start(r, ring, err), "RX recovers after startup failures"); receive(ring); rx->stop(); tx->close(); noLeaks();
    // Low-rate FIR and Tezuka CS8 remain usable alone, and refuse a TX join.
    r.sampleRate = 2.048e6; r.radio["lowratefir"] = "1";
    CHECK(rx->start(r, ring, err), "explicit RX-only FIR still works");
    CHECK(rx->sampleRate() == 2.048e6 && st("pluto.phy:voltage0.filter_fir_en") == 1, "FIR actually enabled");
    CHECK(!tx->open(t, err) && err.find("low-rate FIR") != std::string::npos, "TX rejects active FIR");
    receive(ring); rx->stop(); noLeaks();
    r.radio.erase("lowratefir");
    set("pluto.tezuka", 1); r.sampleRate = 4e6; r.radio["iqformat"] = "cs8";
    CHECK(rx->start(r, ring, err), "RX-only CS8 still opens");
    CHECK(!tx->open(t, err) && err.find("CS16") != std::string::npos, "TX rejects active CS8");
    rx->stop(); noLeaks();
    CHECK(tx->open(t, err), "TX before Tezuka CS8 RX");
    CHECK(!rx->start(r, ring, err) && err.find("select CS16") != std::string::npos, "CS8 join explains the setting to change");
    send(); r.radio["iqformat"] = "cs16";
    CHECK(rx->start(r, ring, err), "CS16 RX joins after correcting IQ format");
    receive(ring); send(); rx->stop(); tx->close();
    set("pluto.tezuka", 0); r.radio.erase("iqformat"); noLeaks();
    // Force TX to start between RX retune's preflight and its stopped-stream configure.
    r.sampleRate = t.sampleRate = 4e6;
    CHECK(rx->start(r, ring, err), "RX for retune/start race");
    set("pluto.cancelDelayMs", 100); set("pluto.cancelWaiting", 0);
    bad = r; bad.sampleRate = 8e6;
    bool changed = true; std::string changeErr;
    std::thread changer([&] { changed = rx->retune(bad, changeErr); });
    CHECK(waitFor([&] { return st("pluto.cancelWaiting") == 1; }), "RX paused between preflight and configure");
    CHECK(tx->open(t, err), "TX starts during RX retune: " << err);
    const double raceRates = st("pluto.rateWrites");
    changer.join(); set("pluto.cancelDelayMs", 0);
    CHECK(!changed && st("pluto.rateWrites") == raceRates, "racing retune refuses to change TX clock");
    receive(ring); send(); rx->stop(); tx->close(); noLeaks();
    // Simultaneous opens serialize configuration but still allow both DMA paths.
    r.sampleRate = t.sampleRate = 4e6;
    for (int iteration = 0; iteration < 12; ++iteration) {
        bool rxOk = false, txOk = false;
        std::string rxErr, txErr;
        std::atomic<int> ready{0};
        auto rendezvous = [&] { ++ready; while (ready < 2) std::this_thread::yield(); };
        std::thread rt([&] { rendezvous(); rxOk = rx->start(r, ring, rxErr); });
        std::thread tt([&] { rendezvous(); txOk = tx->open(t, txErr); });
        rt.join(); tt.join();
        CHECK(rxOk && txOk, "concurrent starts: RX " << rxErr << ", TX " << txErr);
        receive(ring); send();
        std::thread rs([&] { rx->stop(); });
        std::thread ts([&] { tx->close(); });
        rs.join(); ts.join(); noLeaks();
    }
    // Exercise the same manual-Fs session path used by the TX panel.
    {
        r.sampleRate = 4e6;
        CHECK(rx->start(r, ring, err), "RX for matching manual TX Fs");
        TxRequest request; request.modeId = "dab"; request.tune = t; request.tune.sampleRate = 4e6;
        request.loop = false; request.onceSeconds = 0.1;
        TxSession session;
        CHECK(session.start(native::makePlutoTx(d), request, {2.1e6, 61.44e6}, err), "manual 4 Msps TX joins 4 Msps RX: " << err);
        CHECK(waitFor([&] { return !session.running(); }, 5000), "manual TX session completes");
        CHECK(session.stats().finished && session.stats().rateHz == 4e6, "manual Fs drives the generator and sink");
        session.stop(); receive(ring); rx->stop(); noLeaks();
        r.sampleRate = 8e6;
        CHECK(rx->start(r, ring, err), "RX for conflicting manual TX Fs");
        const double pushed = st("pluto.txPushes"), written = st("pluto.rateWrites");
        CHECK(!session.start(native::makePlutoTx(d), request, {2.1e6, 61.44e6}, err) && err.find("not the requested TX rate") != std::string::npos,
              "manual 4 Msps cannot silently become RX's 8 Msps: " << err);
        CHECK(st("pluto.txPushes") == pushed && st("pluto.rateWrites") == written, "manual conflict sends no IQ and preserves RX clock");
        receive(ring); session.stop(); rx->stop(); noLeaks();
    }
    CHECK(st("pluto.rateWhileDuplex") == 0, "shared rate never written during simultaneous streaming");
    printf("Pluto duplex: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
