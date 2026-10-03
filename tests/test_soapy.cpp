// The SoapySDR source with a fake radio: device list, tuning and gain, streaming, and a DVB-T2 lock through the whole engine.
#ifdef _WIN32
#include <stdlib.h>
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#include "dect2/engine.h"
#include "dect2/source.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char** argv) {
    if (argc > 1) setenv("SOAPY_SDR_PLUGIN_PATH", argv[1], 1);
    setenv("ONAIR_TEST_PACE", "0.5", 1);   // half speed: independent of how fast the machine is
    CHECK(soapySupported(), "built without SoapySDR");

    std::string err;
    DeviceInfo radio;
    bool found = false;
    for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest") { radio = d; found = true; }
    CHECK(found, "fake radio not listed (%s)", err.c_str());
    if (!found) { printf("soapy tests FAILED\n"); return 1; }
    printf("listed: %s, rates %.3f..%.3f Msps, gain %.0f..%.0f dB, args '%s'\n", radio.name.c_str(), radio.minRateHz / 1e6, radio.maxRateHz / 1e6, radio.gainMinDb, radio.gainMaxDb, radio.soapyArgs.c_str());
    CHECK(radio.isRadio() && radio.kind == DeviceInfo::Soapy, "not a radio");
    CHECK(radio.maxRateHz > 9e6 && radio.maxRateHz < 9.2e6, "max rate %.3f", radio.maxRateHz / 1e6);
    CHECK(radio.gainMinDb == 0 && radio.gainMaxDb == 60, "gain range");

    Engine e;
    e.setStandard(1);   // DVB-T2 only
    TuneSettings t;
    t.bandwidthMhz = 8;
    t.sampleRate = 10e6;   // the radio only offers the native rate: the source must pick it
    t.centerHz = 522e6;
    t.gainDb = 30;
    FileOptions fo;
    CHECK(e.start(radio, t, fo), "engine did not start");
    CHECK(std::abs(e.sampleRate() - 64e6 / 7.0) < 10, "sample rate %.1f", e.sampleRate());

    SpectrumFrame sf; uint64_t sseq = 0;
    RxTelemetry rx; uint64_t rseq = 0;
    bool locked = false, gotSpec = false, retuned = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40 && !locked) {
        if (e.latestSpectrum(sf, sseq)) { sseq = sf.seq; gotSpec = true; }
        if (e.latestRx(rx, rseq)) { rseq = rx.seq; if (rx.dataValid && rx.dataSnrDb > 15) locked = true; }
        if (gotSpec && !retuned) { t.gainDb = 35; t.centerHz = 522.5e6; e.retune(t); retuned = true; }   // live gain and frequency changes must not break the stream
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gotSpec, "no spectrum from the radio");
    CHECK(locked, "no DVB-T2 lock through the SoapySDR source");
    printf("locked: SNR %.1f dB, %llu dropped samples\n", rx.dataSnrDb, (unsigned long long)e.droppedSamples());
    e.stop();
    CHECK(!e.running(), "engine still running");
    printf(fails ? "soapy tests FAILED\n" : "soapy tests passed\n");
    return fails ? 1 : 0;
}
