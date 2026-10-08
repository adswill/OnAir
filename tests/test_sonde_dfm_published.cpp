// DFM-17 values that rs1729/RS printed in its issue #62 ("Validate temperatures < -55c for DFM17", 2025-02-28 flight, sonde IDxB:25004808:DFM17),
// pushed through our encoder and decoder. This is a check of conventions (channel B = DFM-17, the serial number as a plain decimal, units,
// the sign of the vertical speed, UTC), not of the bit layout: those lines are decoded output, not frames. The temperatures there are
// the decoder's own (T=), which for DFM-17 below -55 C are known to be biased low in rs1729's get_Temp (the issue's subject): we take them as sent.
#include "data/sonde/bits/util.h"
#include "../core/src/sonde_bits_dm.h"
using namespace sbt;
using namespace dect2::sondebits;

struct Pub { int h, m; double s; double lat, lon, alt, vH, dir, vV, T; };

int main() {
    // [rs] issue #62: "[126] 2025-02-28 09:03:50.0 ... lat: 52.05290 lon: 7.55020 alt: 19512.7 vH: 26.86 D: 115.7 vV: 4.58 T=-59.7C",
    //      "[138] ... 09:04:02.0 ... 52.05154 7.55470 19573.8 27.42 119.7 4.16 T=-60.1C (IDxB:25004808:DFM17)",
    //      "[232] ... 09:05:36.0 ... 52.03874 7.59317 20009.5 36.61 109.7 3.50 T=-60.9C"
    const Pub lines[3] = {
        {9, 3, 50.0, 52.05290, 7.55020, 19512.7, 26.86, 115.7, 4.58, -59.7},
        {9, 4, 2.0, 52.05154, 7.55470, 19573.8, 27.42, 119.7, 4.16, -60.1},
        {9, 5, 36.0, 52.03874, 7.59317, 20009.5, 36.61, 109.7, 3.50, -60.9},
    };
    for (const auto& p : lines) {
        SondeTruth t;
        t.serial = "25004808";
        t.lat = p.lat; t.lon = p.lon; t.altM = p.alt; t.hSpeed = p.vH; t.headingDeg = p.dir; t.vSpeed = p.vV; t.tempC = p.T;
        t.sats = 12; t.batteryV = 2.8;
        t.unixTime = civilToUnix(2025, 2, 28, p.h, p.m, p.s);
        std::vector<uint8_t> s;
        for (int i = 0; i < 160; i++) { t.frame = i; const auto f = dfmSymbols(t, 17); s.insert(s.end(), f.begin(), f.end()); }
        auto d = makeDfmDecoder();
        const auto out = feed(*d, s, 4096);
        int n = 0;
        for (const auto& f : out) {
            if (!f.crcOk) continue;
            n++;
            CHECK(f.subtype == "DFM-17" && f.serial == "25004808", "IDxB:25004808:DFM17 -> %s %s", f.subtype.c_str(), f.serial.c_str());
            CHECK(std::fabs(f.lat - p.lat) < 5e-6 && std::fabs(f.lon - p.lon) < 5e-6, "position %.5f %.5f", f.lat, f.lon);
            CHECK(std::fabs(f.altM - p.alt) < 0.05 && std::fabs(f.hSpeed - p.vH) < 0.005 && std::fabs(f.headingDeg - p.dir) < 0.05 && std::fabs(f.vSpeed - p.vV) < 0.005, "alt %.2f vH %.2f D %.1f vV %.2f", f.altM, f.hSpeed, f.headingDeg, f.vSpeed);
            CHECK(f.hasTemp && std::fabs(f.tempC - p.T) < 0.05, "T %.2f", f.tempC);
            CHECK(f.hasTime && std::fabs(f.unixTime - t.unixTime) < 0.001, "time %.3f vs %.3f", f.unixTime, t.unixTime);
        }
        CHECK(n >= 20, "reports for %02d:%02d:%04.1f: %d", p.h, p.m, p.s, n);
    }
    if (fails()) { std::printf("%d failures\n", fails()); return 1; }
    std::printf("sonde dfm published values: ok\n");
    return 0;
}
