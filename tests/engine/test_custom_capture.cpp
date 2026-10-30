// Custom bandwidth and sample rate in every family of modes (3.4): what the user types is saved per mode (and per radio), comes back from the
// settings store, becomes the capture plan (rx_width.h) and reaches the engine, which runs the receiver at that rate. The default (nothing saved,
// or the Custom tick off) leaves each mode's own values exactly as they were. A rate or width outside what a radio lists is not refused here.
#include "../../app/state/rx_width.h"
#include "dect2/engine/engine.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/rate_choice.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

// the settings store of the app, in memory
struct MapPrefs {
    std::map<std::string, double> d;
    double getD(const char* k, double def) { auto i = d.find(k); return i == d.end() ? def : i->second; }
    long getI(const char* k, long def) { auto i = d.find(k); return i == d.end() ? def : (long)i->second; }
    void setD(const char* k, double v) { d[k] = v; }
    void setI(const char* k, long v) { d[k] = (double)v; }
};

// what applyBandwidth() does with the mode's own values and the saved choice
static TuneSettings planFor(const ModeTuning& mt, double widthMhz, double chosenRate) {
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate; t.basebandFilterHz = mt.basebandHz;
    if (widthMhz > 0) {
        const RxWidthPlan p = rxWidthPlan(widthMhz, mt.bandwidthMhz, mt.sampleRate, mt.basebandHz, mt.minSampleRate);
        t.bandwidthMhz = p.bandwidthMhz; t.sampleRate = p.sampleRate; t.basebandFilterHz = p.basebandHz;
    }
    if (chosenRate > 0) t.sampleRate = chosenRate;
    t.rateChosen = chosenRate > 0 || widthMhz > 0;
    t.synth.mode = mt.stdMode;
    return t;
}

// a mode of each family of receivers: DMR, ADS-B, AIS, pagers, VDL2, ISM, TETRA, P25, LoRa raw, spectrum analyser, HFDL, GNSS
static const int kStd[] = {10, 13, 16, 25, 29, 30, 48, 52, 73, 44, 37, 14};

int main() {
    DeviceInfo syn;   // the test signal
    for (int std : kStd) {
        const ModeTuning* mt = modeTuning(std);
        CHECK(mt != nullptr, "mode %d has no tuning", std);
        if (!mt) continue;
        // 1. saved per mode, returned from the store; a mode that saved nothing is unchanged
        MapPrefs p;
        CHECK(rxWidthLoad(p, mt->id) == 0, "%s: nothing saved must mean the standard width", mt->id);
        rxWidthSave(p, mt->id, true, 6.0);
        rxWidthSave(p, "other_mode", true, 1.0);
        CHECK(rxWidthLoad(p, mt->id) == 6.0 && rxWidthLoad(p, "other_mode") == 1.0, "%s: width not saved per mode", mt->id);
        rxWidthSave(p, mt->id, false, 6.0);
        CHECK(rxWidthLoad(p, mt->id) == 0, "%s: tick off must give the standard width back", mt->id);
        rxWidthSave(p, mt->id, true, 6.0);
        // 2. the plan: the standard values untouched, a wide width raises the rate to carry it, no upper limit (30 MHz)
        const TuneSettings def = planFor(*mt, 0, 0);
        CHECK(def.sampleRate == mt->sampleRate && def.bandwidthMhz == mt->bandwidthMhz && def.basebandFilterHz == mt->basebandHz && !def.rateChosen, "%s: the default changed", mt->id);
        const TuneSettings wide = planFor(*mt, rxWidthLoad(p, mt->id), 0);
        CHECK(wide.bandwidthMhz == 6.0 && wide.sampleRate >= 6.6e6 && wide.rateChosen, "%s: 6 MHz gave %.2f Msps", mt->id, wide.sampleRate / 1e6);
        const TuneSettings huge = planFor(*mt, 30.0, 0);
        CHECK(huge.bandwidthMhz == 30.0 && huge.sampleRate >= 33.3e6, "%s: 30 MHz gave %.2f Msps", mt->id, huge.sampleRate / 1e6);
        // 3. it reaches the engine: the receiver runs at the rate the plan gave (a width, a typed rate, and the default)
        struct Case { const char* what; TuneSettings t; };
        const Case cases[] = {{"default", def}, {"6 MHz wide", wide}, {"typed 3.5 Msps", planFor(*mt, 0, 3.5e6)}};
        for (const auto& c : cases) {
            Engine e;
            e.setStandard(mt->stdMode);
            FileOptions fo;
            const bool ok = e.start(syn, c.t, fo);
            CHECK(ok, "%s, %s: the engine did not start (%s)", mt->id, c.what, e.startError().c_str());
            if (ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                CHECK(std::fabs(e.sampleRate() - c.t.sampleRate) < 1.0 || std::fabs(e.sampleRate() - c.t.sampleRate) < c.t.sampleRate * 1e-3,
                      "%s, %s: asked for %.3f Msps, the engine runs at %.3f", mt->id, c.what, c.t.sampleRate / 1e6, e.sampleRate() / 1e6);
                e.stop();
            }
        }
    }
    // the sample rate: saved under rate.<mode>.<radio>, any positive value, never refused by the check
    {
        MapPrefs p;
        const std::string k = rxRateKey("tetra", "radio.hackrf.0123");
        CHECK(k == "rate.tetra.radio.hackrf.0123", "rate key %s", k.c_str());
        p.setD(k.c_str(), 20e6);
        CHECK(p.getD(k.c_str(), 0) == 20e6 && p.getD(rxRateKey("tetra", "radio.hackrf.9999").c_str(), 0) == 0 && p.getD(rxRateKey("dmr", "radio.hackrf.0123").c_str(), 0) == 0, "rate not saved per mode and radio");
        DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF;
        const RateLimits L = rateLimitsOf(hackrf);
        for (double hz : {0.5e6, 1e6, 20e6, 25e6, 40e6, 61.44e6}) {
            const RateCheck c = checkManualRate(L, 2e5, hz);
            CHECK(c.ok && c.hz == (hz >= 2e6 && hz <= 20e6 ? deliveredRate(L, hz) : hz), "%.2f Msps on a HackRF was refused", hz / 1e6);
            CHECK((hz >= 2e6 && hz <= 20e6) == c.why.empty(), "%.2f Msps: warning text '%s'", hz / 1e6, c.why.c_str());
        }
    }
    printf(fails ? "custom capture: %d FAILED\n" : "custom capture: all passed\n", fails);
    return fails ? 1 : 0;
}
