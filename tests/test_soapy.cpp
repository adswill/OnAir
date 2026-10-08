// The SoapySDR source with a fake radio: device list, tuning and gain, streaming, and a DVB-T2 lock through the whole engine.
#ifdef _WIN32
#include <stdlib.h>
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#include "dect2/engine.h"
#include "dect2/ring.h"
#include "dect2/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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
    // the listing does not open the radio (that could take it from another program): its ranges are learned at the first start
    CHECK(radio.maxRateHz == 0 || (radio.maxRateHz > 9e6 && radio.maxRateHz < 9.2e6), "max rate %.3f", radio.maxRateHz / 1e6);

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
    {   // after a start the listing knows the ranges, without opening the radio again
        DeviceInfo again;
        for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest") again = d;
        CHECK(again.maxRateHz > 9e6 && again.maxRateHz < 9.2e6, "max rate after start %.3f", again.maxRateHz / 1e6);
        CHECK(again.gainMinDb == 0 && again.gainMaxDb == 60, "gain range after start %.0f..%.0f", again.gainMinDb, again.gainMaxDb);
    }
    CHECK(!getenv("ONAIR_TEST_ANTENNA"), "the default entry picked an antenna (%s)", getenv("ONAIR_TEST_ANTENNA"));
    DeviceInfo rx2;
    {   // ... and its antenna inputs: one more entry per input after the default one, whose name is unchanged
        std::vector<DeviceInfo> l;
        for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest") l.push_back(d);
        CHECK(l.size() == 3, "%zu entries after start (default, RX1, RX2 expected)", l.size());
        if (l.size() == 3) {
            CHECK(l[0].name == radio.name && l[0].soapyArgs == radio.soapyArgs, "default entry changed: %s", l[0].name.c_str());
            CHECK(l[1].name == "OnAir synthetic radio antenna RX1" && l[2].name == "OnAir synthetic radio antenna RX2", "antenna entries: %s / %s", l[1].name.c_str(), l[2].name.c_str());
            CHECK(l[2].maxRateHz == l[0].maxRateHz && l[2].maxRateHz > 0 && l[2].serial == radio.serial, "antenna entry lost the ranges");
            rx2 = l[2];
        }
    }

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
    if (rx2.isRadio()) {   // the RX2 entry opens the same radio and picks that input
        IqRing ring(1 << 20);
        auto src = makeSource(rx2);
        std::string e2;
        CHECK(src && src->start(t, ring, e2), "RX2 entry did not start: %s", e2.c_str());
        const char* a = getenv("ONAIR_TEST_ANTENNA");
        CHECK(a && std::string(a) == "RX2", "RX2 entry picked antenna %s", a ? a : "(none)");
        if (src) src->stop();
    }
    // ---- radio settings: the driver's own settings (learned at the first start), the frequency correction, a gain-reduction driver
    {
        DeviceInfo d;
        for (auto& x : listSoapyDevices(err)) if (x.board == "onairtest" && x.soapyArgs == radio.soapyArgs) d = x;
        auto find = [&](const char* key) -> const RadioSetting* { for (const auto& r : d.settings) if (r.key == key) return &r; return nullptr; };
        const RadioSetting* ds = find("soapy.direct_samp");
        const RadioSetting* tm = find("soapy.testmode");
        CHECK(!d.settings.empty() && d.settings[0].key == "ppm", "the frequency correction is not offered first");
        CHECK(ds && ds->type == RadioSetting::Choice && ds->def == "0" && ds->values.size() == 3 && ds->names.size() == 3 && ds->names[2] == "Q-ADC", "direct_samp setting: %s", ds ? ds->def.c_str() : "missing");
        CHECK(tm && tm->type == RadioSetting::Bool && tm->def == "0", "testmode setting: default %s (the radio's value, not the info's)", tm ? tm->def.c_str() : "missing");
        CHECK(!find("soapy.biastee") && d.hasBiasTee, "the bias-tee is a radio setting (%d) or not offered (%d)", find("soapy.biastee") != nullptr, d.hasBiasTee);
        CHECK(!find("soapy.label"), "a free-text setting is offered");
        IqRing ring(1 << 20);
        TuneSettings t2 = t;
        auto src = makeSource(d);
        std::string e2;
        bool ok = src && src->start(t2, ring, e2);
        CHECK(ok && !getenv("ONAIR_TEST_SET_direct_samp") && !getenv("ONAIR_TEST_SET_testmode") && !getenv("ONAIR_TEST_PPM"), "defaults: a setting or the correction was written (%s)", e2.c_str());
        if (src) src->stop();
        t2.radio = {{"soapy.direct_samp", "2"}, {"soapy.testmode", "1"}, {"ppm", "3"}};
        auto s2 = makeSource(d);
        ok = s2 && s2->start(t2, ring, e2);
        const char* a = getenv("ONAIR_TEST_SET_direct_samp");
        const char* b = getenv("ONAIR_TEST_SET_testmode");
        const char* p = getenv("ONAIR_TEST_PPM");
        CHECK(ok && a && std::string(a) == "2" && b && std::string(b) == "true" && p && std::fabs(atof(p) - 3) < 1e-9, "settings: direct_samp %s testmode %s ppm %s", a ? a : "-", b ? b : "-", p ? p : "-");
        if (s2) s2->stop();
    }
    {   // a driver whose gains are reductions: the top of the gain slider is the least reduction
        DeviceInfo g;
        for (auto& x : listSoapyDevices(err)) if (x.board == "onairtestgr") g = x;
        CHECK(g.isRadio(), "gain-reduction radio not listed");
        IqRing ring(1 << 20);
        TuneSettings t3 = t;
        for (double gain : {48.0, 0.0}) {
            t3.gainDb = gain;
            auto src = makeSource(g);
            std::string e3;
            const bool ok = src && src->start(t3, ring, e3);
            const char* ifgr = getenv("ONAIR_TEST_IFGR");
            const char* rfgr = getenv("ONAIR_TEST_RFGR");
            const bool full = gain > 0;
            CHECK(ok && ifgr && rfgr && atoi(ifgr) == (full ? 20 : 59) && atoi(rfgr) == (full ? 0 : 9), "gain %.0f of 48: IFGR %s RFGR %s (%s)", gain, ifgr ? ifgr : "-", rfgr ? rfgr : "-", e3.c_str());
            if (src) src->stop();
        }
    }
    {   // offset tuning on a radio: an 8 MHz channel needs more than this radio's 9.14 Msps, so it falls back to the DC removal and says so;
        // a 1.7 MHz channel fits, and the radio is tuned that far below the channel
        DeviceInfo r;
        for (auto& d : listSoapyDevices(err)) if (d.board == "onairtest" && d.name == radio.name) r = d;
        Engine eo;
        eo.setStandard(1);
        eo.setAutoOffset(true);
        TuneSettings to = t;
        to.sampleRate = 64e6 / 7.0;
        CHECK(eo.start(r, to, fo), "offset: engine did not start");
        CHECK(eo.offsetHz() == 0 && eo.sourceNote().find("offset tuning not possible") != std::string::npos, "8 MHz: offset %.0f, note '%s'", eo.offsetHz(), eo.sourceNote().c_str());
        eo.stop();
        to.bandwidthMhz = 1.7;
        CHECK(eo.start(r, to, fo), "offset 1.7 MHz: engine did not start");
        const OffsetPlan p = planOffset(1.7e6, to.sampleRate, r.maxRateHz, r.minRateHz);
        const char* f = getenv("ONAIR_TEST_FREQ");
        printf("offset tuning: %.0f Hz, radio at %s for a channel at %.0f\n", eo.offsetHz(), f ? f : "-", to.centerHz);
        CHECK(p.ok && std::fabs(eo.offsetHz() - p.offsetHz) < 1 && f && std::fabs(atof(f) - (to.centerHz - p.offsetHz)) < 2, "1.7 MHz: offset %.0f (plan %.0f), radio at %s", eo.offsetHz(), p.offsetHz, f ? f : "-");
        CHECK(std::fabs(eo.sampleRate() - to.sampleRate) < 1, "receiver rate %.0f, %.0f expected", eo.sampleRate(), to.sampleRate);
        to.centerHz += 8e6;   // a retune keeps the radio beside the channel
        CHECK(eo.retune(to), "offset retune failed");
        f = getenv("ONAIR_TEST_FREQ");
        CHECK(f && std::fabs(atof(f) - (to.centerHz - p.offsetHz)) < 2, "after a retune the radio is at %s, %.0f expected", f ? f : "-", to.centerHz - p.offsetHz);
        eo.stop();
        {   // a mode that asks for 4 Msps: the radio runs at its 9.14 Msps, and after the shift the receivers get 4 Msps again
            TuneSettings t4 = to; t4.sampleRate = 4e6;
            CHECK(eo.start(r, t4, fo) && eo.offsetHz() > 0 && std::fabs(eo.sampleRate() - 4e6) < 1, "4 Msps mode: offset %.0f, receiver rate %.0f", eo.offsetHz(), eo.sampleRate());
            eo.stop();
        }
        eo.setAutoOffset(false);
        CHECK(eo.start(r, to, fo) && eo.offsetHz() == 0, "offset still in use after switching it off");
        f = getenv("ONAIR_TEST_FREQ");
        CHECK(f && std::fabs(atof(f) - to.centerHz) < 2, "without offset the radio is at %s, %.0f expected", f ? f : "-", to.centerHz);
        eo.stop();
    }
    printf(fails ? "soapy tests FAILED\n" : "soapy tests passed\n");
    return fails ? 1 : 0;
}
