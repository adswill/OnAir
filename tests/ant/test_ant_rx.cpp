// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ receiver on the generated signal: the strap and the bike sensors decode with their fields, with carrier offsets, noise, a clock
// error, at 4 and 8 Msps; a damaged frame is counted and not shown; noise alone gives nothing; a foreign network's frames are found.
#include "dect2/ant/ant_gen.h"
#include "dect2/ant/ant_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static AntTelemetry run(const AntGenConfig& c, double sec, double tuneOffset = 1e6, bool anyNet = true) {
    AntGenerator g(c);
    AntReceiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(-tuneOffset);
    rx.setAnyNetwork(anyNet);
    std::vector<cf32> buf(8192);
    const size_t total = (size_t)(sec * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    AntTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static const AntDevice* find(const AntTelemetry& t, int type) { for (const auto& d : t.devices) if (d.type == type) return &d; return nullptr; }

int main() {
    {   // clean: three sensors
        AntGenConfig c; c.rate = 4e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.sensors = antTestSensors(0);
        const AntTelemetry t = run(c, 6.0);
        printf("  clean: %s, %llu good %llu bad, snr %.1f dB\n", antSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.snrDb);
        CHECK(t.devices.size() == 3, "%zu sensors", t.devices.size());
        const AntDevice* hr = find(t, ant::HeartRate);
        CHECK(hr && hr->heartRate >= 60 && hr->heartRate <= 99 && hr->deviceNumber == 0x6941, "heart rate %d", hr ? hr->heartRate : -1);
        CHECK(hr && hr->frames >= 20, "strap frames %llu (6 s at 4 a second)", hr ? (unsigned long long)hr->frames : 0ull);
        CHECK(hr && hr->history.size() >= 20, "history %zu", hr ? hr->history.size() : 0);
        CHECK(hr && std::fabs(hr->periodSec - ant::kHrmPeriodSec) < 0.01, "period %.4f", hr ? hr->periodSec : 0);
        const AntDevice* pw = find(t, ant::BikePower);
        CHECK(pw && pw->powerW > 150 && pw->powerW < 210 && pw->cadenceRpm == 85, "power %d W %d rpm", pw ? pw->powerW : -1, pw ? pw->cadenceRpm : -1);
        const AntDevice* sc = find(t, ant::BikeSpeedCadence);
        CHECK(sc && std::fabs(sc->speedKmh - 25) < 1.5 && std::abs(sc->cadenceRpm - 85) <= 3, "speed %.1f km/h cadence %d", sc ? sc->speedKmh : 0, sc ? sc->cadenceRpm : -1);
        CHECK(t.blocksBad == 0, "%llu bad frames on a clean signal", (unsigned long long)t.blocksBad);
        CHECK(std::fabs(t.cfoHz) < 5e3, "cfo %.0f", t.cfoHz);
    }
    {   // the strap alone, with the background pages: manufacturer, battery, operating time arrive within 65 messages
        AntGenConfig c; c.rate = 4e6; c.snrDb = 25; c.dialOffsetHz = -1e6; c.sensors = antTestSensors(1);
        const AntTelemetry t = run(c, 18.0);
        const AntDevice* hr = find(t, ant::HeartRate);
        CHECK(hr && hr->manufacturer == "Garmin" && hr->serial == 12345, "manufacturer page: %s serial %d", hr ? hr->manufacturer.c_str() : "", hr ? hr->serial : -1);
        CHECK(hr && hr->batteryPct == 85 && hr->batteryStatus == 2, "battery page");
        CHECK(hr && hr->rrSec > 0.5 && hr->rrSec < 1.1, "R-R %.3f", hr ? hr->rrSec : 0);
        CHECK(hr && (hr->pagesSeen & 0x8F) == 0x8F, "pages seen 0x%X", hr ? hr->pagesSeen : 0);
    }
    {   // carrier offset 60 kHz and 30 ppm clock error, 15 dB
        AntGenConfig c; c.rate = 4e6; c.snrDb = 15; c.dialOffsetHz = -1e6; c.cfoHz = 60e3; c.clockPpm = 30; c.sensors = antTestSensors(1);
        const AntTelemetry t = run(c, 5.0);
        const AntDevice* hr = find(t, ant::HeartRate);
        printf("  15 dB, +60 kHz: %llu good, snr %.1f dB, cfo %.0f Hz\n", (unsigned long long)t.blocksOk, t.snrDb, t.cfoHz);
        CHECK(hr && hr->frames >= 12, "frames %llu at 15 dB with offset", hr ? (unsigned long long)hr->frames : 0ull);
        CHECK(std::fabs(t.cfoHz - 60e3) < 8e3, "cfo %.0f", t.cfoHz);
    }
    {   // 8 Msps input (a wider radio)
        AntGenConfig c; c.rate = 8e6; c.snrDb = 25; c.dialOffsetHz = -1e6; c.sensors = antTestSensors(1);
        const AntTelemetry t = run(c, 4.0);
        const AntDevice* hr = find(t, ant::HeartRate);
        CHECK(hr && hr->frames >= 12, "8 Msps: frames %llu", hr ? (unsigned long long)hr->frames : 0ull);
    }
    {   // every fourth frame damaged: counted as bad, the rest decode
        AntGenConfig c; c.rate = 4e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.sensors = antTestSensors(1); c.sensors[0].badCrcEvery = 4;
        const AntTelemetry t = run(c, 5.0);
        printf("  damaged: %llu good %llu bad\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksBad >= 3, "bad %llu", (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= 12, "good %llu", (unsigned long long)t.blocksOk);
    }
    {   // noise only
        AntGenConfig c; c.rate = 4e6; c.snrDb = 0; c.dialOffsetHz = -1e6; c.noiseOnly = true; c.sensors = antTestSensors(1);
        const AntTelemetry t = run(c, 5.0);
        CHECK(t.blocksOk == 0 && t.devices.empty(), "noise gave %llu frames", (unsigned long long)t.blocksOk);
    }
    {   // another ANT network (marker 0x5B25): found with "any network", ignored without
        AntGenConfig c; c.rate = 4e6; c.snrDb = 30; c.dialOffsetHz = -1e6; c.sensors = antTestSensors(1); c.sensors[0].marker = 0x5B25;
        AntTelemetry t = run(c, 4.0);
        const AntDevice* hr = find(t, ant::HeartRate);
        CHECK(hr && hr->frames >= 10 && hr->marker == 0x5B25, "foreign network: %llu frames", hr ? (unsigned long long)hr->frames : 0ull);
        t = run(c, 4.0, 1e6, false);
        CHECK(t.blocksOk == 0, "foreign network decoded with any-network off: %llu", (unsigned long long)t.blocksOk);
    }
    printf(fails ? "ant rx: %d FAILED\n" : "ant rx: all passed\n", fails);
    return fails ? 1 : 0;
}
