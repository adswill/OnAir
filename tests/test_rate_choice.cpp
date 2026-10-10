// The sample-rate choices of the app (rate_choice.h): the list per radio and mode, and the check of a typed rate.
#include "dect2/rate_choice.h"
#include "dect2/mode_tuning.h"
#include <cmath>
#include <cstdio>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool has(const std::vector<RateEntry>& v, double hz) { for (auto& e : v) if (std::fabs(e.getHz - hz) < 1) return true; return false; }

int main() {
    {   // a HackRF: 2 to 20 Msps from the ladder; DVB-T2 8 MHz needs 7.9
        DeviceInfo d; d.kind = DeviceInfo::HackRF;
        const RateLimits L = rateLimitsOf(d);
        CHECK(L.minHz == 2e6 && L.maxHz == 20e6);
        auto e = rateEntries(L, 0);
        CHECK(!has(e, 1e6) && has(e, 2e6) && has(e, 20e6) && !e.empty() && e.front().getHz == 2e6);
        e = rateEntries(L, minSampleRateFor(1, 8));
        CHECK(!has(e, 6e6) && has(e, 8e6) && has(e, 10e6));
        CHECK(checkManualRate(L, 0, 12.345e6).ok && checkManualRate(L, 0, 12.345e6).hz == 12.345e6);
        CHECK(!checkManualRate(L, 0, 25e6).ok && checkManualRate(L, 0, 25e6).why == "this radio runs at 2 to 20 Msps");
        RateCheck c = checkManualRate(L, 7.9e6, 6e6);
        CHECK(!c.ok && c.why == "this mode needs at least 7.9 Msps");
        CHECK(!checkManualRate(L, 0, 0).ok && !checkManualRate(L, 0, NAN).ok);
    }
    {   // an RTL-SDR through SoapySDR: 0.225-0.3 and 0.9-3.2 Msps, nothing between
        DeviceInfo d; d.kind = DeviceInfo::Soapy; d.minRateHz = 0.225e6; d.maxRateHz = 3.2e6;
        d.rateRanges = {{0.9e6, 3.2e6}, {0.225e6, 0.3e6}};
        const RateLimits L = rateLimitsOf(d);
        auto e = rateEntries(L, 0);
        CHECK(has(e, 0.25e6) && !has(e, 0.5e6) && has(e, 1e6) && has(e, 3.2e6) && !has(e, 4e6));
        RateCheck c = checkManualRate(L, 0, 0.5e6);
        CHECK(!c.ok && c.why == "this radio cannot run between 0.3 and 0.9 Msps");
        CHECK(checkManualRate(L, 0, 0.28e6).ok && checkManualRate(L, 0, 2.4e6).ok);
        CHECK(!checkManualRate(L, 0.5e6, 0.28e6).ok);   // FM needs 0.5
        CHECK(deliveredRate(L, 0.5e6) == 0.9e6);
    }
    {   // the native RTL-SDR: 0.9 to 2.56 Msps
        DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "rtlsdr"; d.minRateHz = 0.9e6; d.maxRateHz = 2.56e6;
        d.rateRanges = {{0.225001e6, 0.3e6}, {0.900001e6, 2.56e6}};
        const RateLimits L = rateLimitsOf(d);
        auto e = rateEntries(L, 0);
        CHECK(!has(e, 0.25e6) && has(e, 1e6) && has(e, 2.56e6) && !has(e, 3.2e6));
        CHECK(!checkManualRate(L, 0, 0.5e6).ok && !checkManualRate(L, 0, 3e6).ok);
    }
    {   // an Airspy R2: only 2.5 and 10 Msps, a typed rate rounds up to the next one
        DeviceInfo d; d.kind = DeviceInfo::Native; d.minRateHz = 2.5e6; d.maxRateHz = 10e6; d.rateRanges = {{10e6, 10e6}, {2.5e6, 2.5e6}};
        const RateLimits L = rateLimitsOf(d);
        auto e = rateEntries(L, 0);
        CHECK(e.size() == 2 && e[0].getHz == 2.5e6 && e[1].getHz == 10e6);
        CHECK(rateEntries(L, 4e6).size() == 1);
        RateCheck c = checkManualRate(L, 0, 3e6);
        CHECK(c.ok && c.hz == 10e6);
    }
    {   // unknown limits (a SoapySDR radio not opened yet): the whole ladder, any positive rate
        DeviceInfo d; d.kind = DeviceInfo::Soapy;
        const RateLimits L = rateLimitsOf(d);
        CHECK(rateEntries(L, 0).size() == 17 && has(rateEntries(L, 0), 0.25e6));
        CHECK(checkManualRate(L, 0, 33.3e6).ok);
    }
    {   // a file / the test signal: no limits from a radio
        DeviceInfo d; d.kind = DeviceInfo::File;
        const RateLimits L = rateLimitsOf(d);
        CHECK(L.minHz == 0 && L.maxHz == 0);
    }
    {   // a PlutoSDR on its USB cable: the cable carries 4 Msps (8 with Tezuka CS8); a mode that needs more runs at its own rate anyway
        DeviceInfo d; d.kind = DeviceInfo::Native; d.board = "pluto"; d.maxRateHz = 61.44e6; d.minRateHz = 2.1e6; d.steadyRateHz = 4e6;
        TuneSettings t;
        CHECK(std::fabs(dvbNativeRate(8) - 64e6 / 7) < 1 && std::fabs(dvbNativeRate(7) - 8e6) < 1 && std::fabs(dvbNativeRate(6) - 48e6 / 7) < 1);
        for (double bw : {6.0, 7.0, 8.0}) CHECK(std::fabs(linkRateFor(d, t, dvbNativeRate(bw), minSampleRateFor(0, bw)) - dvbNativeRate(bw)) < 1);
        CHECK(!linkNote(d, t, 64e6 / 7).empty() && linkNote(d, t, 4e6).empty());
        CHECK(linkNote(d, t, 64e6 / 7).find("install the Tezuka firmware") != std::string::npos);   // stock firmware: no CS8 to offer here
        CHECK(linkRateFor(d, t, 2.048e6, minSampleRateFor(4, 1.7)) == 2.048e6);   // DAB fits
        CHECK(linkRateFor(d, t, 4e6, minSampleRateFor(7, 0.25)) == 4e6);          // FM at 4 Msps fits exactly
        CHECK(linkRateFor(d, t, 10e6, 1.6e6) == 4e6);                              // a mode that fits is held to the cable
        CHECK(linkRateFor(d, t, 8e6, minSampleRateFor(3, 6)) == 8e6);             // ATSC needs 6.5: its 8 Msps
        for (int sm = 8; sm < 40; sm++)   // every mode with its own tuning: its rate when it needs more than the cable, else at most 4 Msps
            if (const ModeTuning* m = modeTuning(sm)) {
                const double got = linkRateFor(d, t, m->sampleRate, m->minSampleRate);
                CHECK(got == (m->minSampleRate > 4e6 ? m->sampleRate : std::min(m->sampleRate, 4e6)));
                printf("  pluto usb: %-10s %6.3f Msps (asks %6.3f, needs %6.3f)\n", m->id, got / 1e6, m->sampleRate / 1e6, m->minSampleRate / 1e6);
            }
        // Tezuka firmware: CS16 suggests CS8; with CS8 the cable carries 8 Msps
        d.settings.push_back(RadioSetting{}); d.settings.back().key = "iqformat";
        CHECK(linkNote(d, t, 64e6 / 7).find("Set the radio's IQ format to CS8") != std::string::npos);
        t.radio["iqformat"] = "cs8";
        CHECK(linkCapHz(d, t) == 8e6);
        CHECK(linkRateFor(d, t, dvbNativeRate(8), minSampleRateFor(0, 8)) == 8e6);       // 8 MHz DVB fits 8 Msps (needs 7.9)
        CHECK(std::fabs(linkRateFor(d, t, dvbNativeRate(7), minSampleRateFor(0, 7)) - 8e6) < 1);
        CHECK(linkNote(d, t, 8e6).empty() && linkNote(d, t, 10e6).find("Ethernet") != std::string::npos);
        // on the network: no cap at all
        d.steadyRateHz = 0; t.radio.clear();
        CHECK(linkRateFor(d, t, 10e6, 7.9e6) == 10e6 && linkNote(d, t, 20e6).empty());
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("rate choice ok\n");
    return 0;
}
