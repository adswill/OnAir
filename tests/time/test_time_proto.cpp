// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal frames (time_proto.h): every station's minute encoded and decoded back over a year of minutes (DST changes, leap years,
// year ends), single symbol errors caught, the calendar and the path delay.
#include "dect2/time/time_proto.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::timesig;
#include "../common/test_util.h"

int main() {
    // calendar
    CHECK(daysFromCivil(1970, 1, 1) == 0, "epoch");
    CHECK(daysFromCivil(2026, 10, 12) == 20738, "2026-10-12 = %lld", (long long)daysFromCivil(2026, 10, 12));
    CHECK(weekdayOf(daysFromCivil(2026, 10, 12)) == 1, "2026-10-12 is a Monday");
    int y, m, d;
    civilFromDays(daysFromCivil(2024, 2, 29), y, m, d);
    CHECK(y == 2024 && m == 2 && d == 29, "leap day");
    // EU summer time 2026: 29 March 01:00 UTC to 25 October 01:00 UTC
    CHECK(!euSummerTime(daysFromCivil(2026, 3, 29) * 86400 + 3599) && euSummerTime(daysFromCivil(2026, 3, 29) * 86400 + 3600), "EU start");
    CHECK(euSummerTime(daysFromCivil(2026, 10, 25) * 86400 + 3599) && !euSummerTime(daysFromCivil(2026, 10, 25) * 86400 + 3600), "EU end");
    // US DST 2026: 8 March to 1 November
    CHECK(!usDstOnDate(2026, 3, 7) && usDstOnDate(2026, 3, 8) && usDstOnDate(2026, 10, 31) && !usDstOnDate(2026, 11, 1), "US DST dates");

    // every station, a minute every 97 minutes over 2026 - 2028, and the minutes around the changes
    for (int st = Dcf77; st <= Wwv; st++) {
        int n = 0, bad = 0;
        auto one = [&](int64_t M) {
            EncodeOpts o;
            o.dut1 = (int)(M / 60 % 7) - 3;
            o.leapAnnounce = M / 60 % 5 == 0;
            const std::vector<uint8_t> s = encodeMinute(st, M, o);
            Fields f;
            int hy, hm, hd;
            civilFromDays((M + (st == Jjy40 || st == Jjy60 ? 9 * 3600 : 0)) / 86400, hy, hm, hd);
            const bool ok = decodeMinute(st, s.data(), f, hy);   // JJY names no year at minutes 15 and 45
            n++;
            if (!ok || f.utcMinute != M) { if (bad++ < 3) printf("  %s minute %lld: ok %d got %lld\n", stationName(st), (long long)M, ok, (long long)f.utcMinute); return; }
            if (st == Dcf77 || st == Msf) CHECK(f.dst == (euSummerTime(M + 60) ? 1 : 0), "%s dst", stationName(st));
            if (st == Wwvb || st == Wwv) CHECK(f.dut1 == o.dut1, "%s DUT1 %d %d", stationName(st), f.dut1, o.dut1);
            if (st == Msf) CHECK(f.dut1 == o.dut1, "MSF DUT1");
            if (st != Msf) CHECK(f.leapAnnounce == o.leapAnnounce, "%s leap", stationName(st));
        };
        const int64_t t0 = daysFromCivil(2026, 1, 1) * 86400;
        for (int64_t M = t0; M < t0 + 3LL * 365 * 86400; M += 97 * 60) one(M);
        for (int64_t c : {daysFromCivil(2026, 3, 29), daysFromCivil(2026, 10, 25), daysFromCivil(2026, 3, 8), daysFromCivil(2026, 11, 1), daysFromCivil(2027, 1, 1), daysFromCivil(2028, 12, 31)})
            for (int64_t M = c * 86400 - 7200; M < c * 86400 + 7200; M += 60) one(M);
        CHECK(bad == 0, "%s: %d of %d minutes not decoded back", stationName(st), bad, n);
        // a single wrong symbol: the checks catch most (DCF77 and JJY carry parity bits; MSF parity; WWVB / WWV only fixed bits and ranges)
        const int64_t M = daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 3 * 60;
        const std::vector<uint8_t> s = encodeMinute(st, M);
        int caught = 0, tried = 0;
        for (int k = 0; k < 60; k++) {
            if (s[(size_t)k] >= SMark) continue;
            std::vector<uint8_t> e = s;
            e[(size_t)k] ^= 1;
            Fields f;
            tried++;
            if (!decodeMinute(st, e.data(), f, 2026) || f.utcMinute == M) caught++;
            // a flipped bit that still decodes must at least not move the time when it is a flag (DST, leap, DUT1)
        }
        printf("%s: %d minutes round trip, single errors caught or harmless %d / %d\n", stationName(st), n, caught, tried);
        // a marker in the wrong place
        std::vector<uint8_t> e = s;
        e[30] = SMark;
        Fields f;
        CHECK(!decodeMinute(st, e.data(), f, 2026), "%s: a marker in second 30 passed", stationName(st));
    }
    // DCF77 text: 14:03 UTC on 2026-10-12 is 16:03 CEST; the frame sent in 14:03 names 14:04
    {
        const int64_t M = daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 3 * 60;
        Fields f;
        CHECK(decodeMinute(Dcf77, encodeMinute(Dcf77, M).data(), f) && f.hour == 16 && f.minute == 4 && f.utcOffsetMin == 120 && f.weekday == 1, "DCF77 fields");
        printf("DCF77: %s\n", fieldsText(f, Dcf77).c_str());
        CHECK(decodeMinute(Jjy60, encodeMinute(Jjy60, M).data(), f) && f.hour == 23 && f.minute == 3 && f.utcOffsetMin == 540, "JJY fields");
        CHECK(decodeMinute(Msf, encodeMinute(Msf, M).data(), f) && f.hour == 15 && f.minute == 4 && f.dst == 1, "MSF fields");
        CHECK(decodeMinute(Wwvb, encodeMinute(Wwvb, M).data(), f) && f.hour == 14 && f.minute == 3 && f.doy == 285 && f.month == 10 && f.day == 12 && f.dstCode == 3, "WWVB fields");
    }
    // BPM: the WWVB layout in UTC, the UT1 flag, the site
    {
        const int64_t M = daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 3 * 60;
        Fields f;
        EncodeOpts o; o.dut1 = -3; o.ut1 = true;
        CHECK(decodeMinute(Bpm, encodeMinute(Bpm, M, o).data(), f, 2026) && f.utcMinute == M && f.dut1 == -3 && f.ut1 && f.utcOffsetMin == 0, "BPM fields");
        o.ut1 = false;
        CHECK(decodeMinute(Bpm, encodeMinute(Bpm, M, o).data(), f, 2026) && !f.ut1 && f.hour == 14 && f.doy == 285, "BPM UTC minute");
        CHECK(stationIsHf(Bpm) && stationKhz(Bpm) == 5000 && std::string(stationName(Bpm)) == "BPM", "BPM name");
        double la, lo;
        stationSite(Bpm, la, lo);
        const double km = distanceKm(54.687, 25.280, la, lo);   // Vilnius to Pucheng
        CHECK(km > 6500 && km < 7500, "BPM distance %.0f km", km);
    }
    // CHU frames
    {
        const std::vector<uint8_t> a = chuFrameA(285, 14, 3, 35);
        int doy, h, mi, s;
        CHECK(chuParseA(a.data(), doy, h, mi, s) && doy == 285 && h == 14 && mi == 3 && s == 35, "CHU A");
        const std::vector<uint8_t> b = chuFrameB(2026);
        CHECK(!chuParseA(b.data(), doy, h, mi, s) && chuIsB(b.data()) && !chuIsB(a.data()), "CHU B");
    }
    // the path: Vilnius to Mainflingen is about 1220 km, 4.1 ms on the ground wave
    {
        double la, lo;
        stationSite(Dcf77, la, lo);
        const double km = distanceKm(54.687, 25.280, la, lo);
        CHECK(km > 1180 && km < 1260, "Vilnius - Mainflingen %.0f km", km);
        CHECK(std::fabs(propagationMs(Dcf77, km) - km / 299.792458) < 1e-9, "ground wave");
        CHECK(propagationMs(Wwv, 2000) > 2000 / 299.792458, "sky wave longer");
        CHECK(stationForKhz(77.5, Msf) == Dcf77 && stationForKhz(60, Wwvb) == Wwvb && stationForKhz(10000, 0) == Wwv && stationForKhz(7850, 0) == -1 && stationForKhz(500, 0) == -1, "station by frequency");
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
