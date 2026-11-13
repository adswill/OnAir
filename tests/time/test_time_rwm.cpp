// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RWM (Moscow): the keyed carrier through the time receiver. The 1 pulse minutes give the seconds, the minute pulse and DUT1 / dUT1 from
// the doubled pulses; the 10 pulse minutes give the seconds and the minute pulse only (their 20 ms pulses must not be taken for the second);
// the plain carrier minutes give nothing. RWM sends no time of day, so the minute comes from the clock and the check is the clock offset.
// Also: the frame encoder and decoder, a weak signal with a carrier and sample clock error, and the station from the frequency.
#include "dect2/dsp/gen_util.h"
#include "dect2/time/time_gen.h"
#include "dect2/time/time_proto.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
using namespace dect2::timesig;
#include "../common/test_util.h"

static const double kLat = 54.687, kLon = 25.280;   // Vilnius

static TimeTelemetry run(TimeGenConfig c, double secs) {
    c.station = Rwm; c.rate = 64000;
    c.dialOffsetHz = -timeTuning().tuneOffsetHz;
    double la, lo;
    stationSite(Rwm, la, lo);
    c.delayMs = propagationMs(Rwm, distanceKm(kLat, kLon, la, lo));
    TimeGenerator g(c);
    TimeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(-timeTuning().tuneOffsetHz);
    r.setDialHz(9.996e6);                           // the station from the frequency
    r.setClockMode(2, c.startUtc);
    r.setReceiverLocation(kLat, kLon, true);
    std::vector<cf32> buf(8192);
    TimeTelemetry last;
    uint64_t seq = 0;
    for (size_t done = 0; done < (size_t)(secs * c.rate); done += buf.size()) {
        g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        TimeTelemetry t;
        if (r.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    return last;
}

static double at(int hour, int minute, int sec) { return (double)(daysFromCivil(2026, 10, 22) * 86400 + hour * 3600 + minute * 60 + sec); }

static void live(const char* what, double start, double secs, double snr, double cfo, double ppm, int dut1, int dUt1, bool expectTime, int expDut1, int expDUt1) {
    TimeGenConfig c;
    c.startUtc = start; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm;
    c.opts.dut1 = dut1; c.opts.dUt1 = dUt1;
    const TimeTelemetry t = run(c, secs);
    printf("RWM %-34s %s, ok %llu bad %llu, offset %+.2f ms (avg %+.2f), SNR %.1f dB, depth %.2f, DUT1 %d dUT1 %d\n", what, timeSummary(t).c_str(),
           (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.clockOffsetMs, t.clockOffsetAvgMs, t.snrDb, t.depth, t.fields.dut1, t.fields.dUt1);
    CHECK(t.station == Rwm, "%s: station %d from 9.996 MHz", what, t.station);
    if (!expectTime) { CHECK(!t.timeValid && t.blocksOk == 0, "%s: a time from the plain carrier", what); return; }
    CHECK(t.timeValid, "%s: no time", what);
    CHECK(t.blocksBad == 0, "%s: %llu minutes rejected", what, (unsigned long long)t.blocksBad);
    const double drift = ppm * 1e-6 * (secs - 30) * 1000;
    CHECK(std::fabs(t.clockOffsetAvgMs - drift) < 2.0, "%s: clock offset %.2f ms, drift %.2f", what, t.clockOffsetAvgMs, drift);
    const double expect = start + secs - c.delayMs * 1e-3;
    CHECK(std::fabs(t.utcNow - expect) < 1.5, "%s: time %.1f s off", what, t.utcNow - expect);
    CHECK(t.fields.dut1 == expDut1 && t.fields.dUt1 == expDUt1, "%s: DUT1 %d dUT1 %d, want %d %d", what, t.fields.dut1, t.fields.dUt1, expDut1, expDUt1);
}

int main() {
    // the code: every DUT1 / dUT1 back from its symbols, a doubled pulse out of place is refused
    for (int d = -8; d <= 8; d++)
        for (int e = -4; e <= 4; e++) {
            EncodeOpts o; o.dut1 = d; o.dUt1 = e;
            const std::vector<uint8_t> s = encodeMinute(Rwm, 0, o);
            Fields f;
            CHECK(decodeMinute(Rwm, s.data(), f) && f.dut1 == d && f.dUt1 == e, "RWM code %d %d", d, e);
        }
    {
        EncodeOpts o; o.dut1 = 3;
        std::vector<uint8_t> s = encodeMinute(Rwm, 0, o);
        Fields f;
        s[2] = S0; CHECK(!decodeMinute(Rwm, s.data(), f), "RWM: a gap in the DUT1 run");
        s = encodeMinute(Rwm, 0, o); s[10] = S1; CHECK(!decodeMinute(Rwm, s.data(), f), "RWM: both signs");
        s = encodeMinute(Rwm, 0, o); s[45] = S1; CHECK(!decodeMinute(Rwm, s.data(), f), "RWM: a doubled pulse out of place");
        s = encodeMinute(Rwm, 0, o); s[0] = S0; CHECK(!decodeMinute(Rwm, s.data(), f), "RWM: no minute pulse");
    }
    CHECK(stationForKhz(4996, Msf) == Rwm && stationForKhz(14996, Msf) == Rwm && stationForKhz(10000, Msf) == Wwv, "RWM frequencies");
    CHECK(stationIsHf(Rwm) && stationKeysCarrier(Rwm) && !stationKeysCarrier(Wwv) && stationKeysCarrier(Dcf77), "RWM station class");

    // 1 pulse minutes (hh:10 - hh:19 and hh:40 - hh:49), started part way into a minute
    live("1 pulse, DUT1 -0.2 s, dUT1 +0.06 s", at(14, 11, 37), 175, 40, 0, 0, -2, 3, true, -2, 3);
    live("1 pulse, DUT1 +0.5 s, half hour 2", at(14, 41, 20), 175, 40, 0, 0, 5, -1, true, 5, -1);
    live("1 pulse, 15 dB, 8 Hz, 20 ppm", at(14, 12, 5), 175, 15, 8, 20, -2, 0, true, -2, 0);
    // 10 pulse minutes (hh:20 - hh:29): the seconds and the minute, no DUT1
    live("10 pulse", at(14, 21, 37), 175, 40, 0, 0, -2, 3, true, 99, 99);
    live("10 pulse, 20 dB, -5 Hz, -15 ppm", at(14, 51, 10), 175, 20, -5, -15, 0, 0, true, 99, 99);
    // plain carrier (hh:00 - hh:07): no seconds
    live("plain carrier", at(14, 1, 0), 150, 40, 0, 0, 0, 0, false, 0, 0);
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
