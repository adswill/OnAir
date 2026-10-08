// Aero positions: ADS-C known answers, builders, and the text position formats.
// ADS-C sources (libacars, github.com/szpajder/libacars):
//   doc/PROG_GUIDE.md: the VQ-BPJ label B6 message (waypoint change event + predicted route) and the N572UP H1 message
//     ("#M1B/B6 ..." basic report + earth reference) with libacars' decoded values;
//   examples/adsc_get_position.c: four more real downlinks (VT-ANB, A6-PFE, HB-JNB, SP-LRH) with no decoded values given:
//     their CRCs must hold and the positions must be where those flights were (Europe, near Poland).
//   JAERO aerol.cpp (comment in ParserISU::parse): a real A6 uplink to HB-JHM (contract request).
// Text sources: airframesio/acars-decoder-typescript lib/plugins/*.test.ts (Label_H1_POS, Label_20_POS, Label_16_N_Space), messages
// and expected values copied from those tests.
#include "dect2/aero_adsc.h"
#include "dect2/aero_pos.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void textPos(const char* label, const char* text, double lat, double lon, double tol, int alt) {
    AeroPosition p;
    const bool ok = aeroParseTextPosition(label, text, p);
    CHECK(ok && near(p.lat, lat, tol) && near(p.lon, lon, tol), "%s '%s': %d %.4f %.4f, want %.4f %.4f", label, text, ok, p.lat, p.lon, lat, lon);
    if (alt >= 0) CHECK(ok && p.hasAlt && p.altFt == alt, "%s '%s': altitude %d, want %d", label, text, p.altFt, alt);
}

int main() {
    CHECK(aeroArincCrc((const uint8_t*)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");

    {   // PROG_GUIDE.md example 1, label B6
        AeroAdscMessage m;
        const bool f = aeroDecodeAdsc("/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90880DF97", false, m);
        CHECK(f && m.found && m.crcOk && m.complete, "VQ-BPJ found %d crc %d complete %d", m.found, m.crcOk, m.complete);
        CHECK(m.groundAddr == "LPAFAYA" && m.airReg == "VQ-BPJ", "addresses '%s' '%s'", m.groundAddr.c_str(), m.airReg.c_str());
        CHECK(m.tags.size() == 2 && m.tags[0] == 20 && m.tags[1] == 13, "groups");
        CHECK(m.hasPos && m.posTag == 20 && near(m.lat, 50.3429604, 1e-6) && near(m.lon, 16.3785553, 1e-6) && m.altFt == 37000, "position %.7f %.7f %d", m.lat, m.lon, m.altFt);
        CHECK(near(m.timeSec, 396.0, 1e-9) && m.accuracy == 6 && m.redundancyOk && m.tcasOk, "time %.3f accuracy %d", m.timeSec, m.accuracy);
        CHECK(m.hasRoute && near(m.next.lat, 51.7226028, 1e-6) && near(m.next.lon, 19.7335052, 1e-6) && m.next.altFt == 37000 && m.next.etaSec == 1090, "next waypoint");
        CHECK(near(m.nextNext.lat, 52.5409126, 1e-6) && near(m.nextNext.lon, 21.9525719, 1e-6) && m.nextNext.altFt == 37000, "next + 1 waypoint");
        // one hex digit changed: the CRC must catch it and no position is taken
        AeroAdscMessage bad;
        aeroDecodeAdsc("/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90881DF97", false, bad);
        CHECK(bad.found && !bad.crcOk, "damaged message passes the CRC");
        AeroPosition p;
        CHECK(!aeroPositionFromMessage("B6", "/LPAFAYA.ADS.VQ-BPJ1423CCA85D2D090886301D0D24C7D0704309088442255CC87CE2C90881DF97", false, p), "position from a damaged message");
    }
    {   // PROG_GUIDE.md example 2, label H1 with sublabel and MFI
        AeroPosition p;
        std::string detail;
        const bool ok = aeroPositionFromMessage("H1", "#M1B/B6 LHWE1YA.ADS.N572UP07263B5872A048C9F21C1F0E5B88D700000239", false, p, &detail);
        CHECK(ok && p.source == 1 && near(p.lat, 53.7634850, 1e-6) && near(p.lon, 20.1490974, 1e-6) && p.altFt == 35996, "N572UP %.7f %.7f %d", p.lat, p.lon, p.altFt);
        CHECK(near(p.secPastHour, 3207.0, 1e-9) && p.hasTrack && near(p.trackDeg, 257.4, 0.05) && p.hasSpeed && near(p.speedKt, 430.0, 1e-9), "track %.2f speed %.1f", p.trackDeg,
              p.speedKt);
        AeroAdscMessage m;
        aeroDecodeAdsc("#M1B/B6 LHWE1YA.ADS.N572UP07263B5872A048C9F21C1F0E5B88D700000239", false, m);
        CHECK(m.accuracy == 7 && m.vsFpm == 0 && m.groundAddr == "LHWE1YA" && m.airReg == "N572UP", "N572UP fields");
        CHECK(detail.find("Basic report") != std::string::npos && detail.find("ground speed 430.0") != std::string::npos, "detail:\n%s", detail.c_str());
    }
    {   // examples/adsc_get_position.c: CRC holds, position over Europe (all four were heard near Poland)
        const char* ex[4] = {"/BOMASAI.ADS.VT-ANB072501A070A988CA73248F0E5DC10200000F5EE1ABC000102B885E0A19F5",
                             "/AUHASMO.ADS.A6-PFE0724D9586A36C92B2DCF1F0E74A8E4807C0F7219AF407C10422E9E08A1C4",
                             "/CTUE1YA.ADS.HB-JNB1424AB686D9308CA2EBA1D0D24A2C06C1B48CA004A248050667908CA004BF6",
                             "/YQXE2YA.ADS.SP-LRH1424FD087806C0B527769F0D2500B877ED00B5401E2516707755C01340B768"};
        for (const char* e : ex) {
            AeroAdscMessage m;
            aeroDecodeAdsc(e, false, m);
            CHECK(m.found && m.crcOk && m.complete && m.hasPos && m.lat > 49 && m.lat < 55 && m.lon > 14 && m.lon < 24, "%s: crc %d %.4f %.4f", e, m.crcOk, m.lat, m.lon);
        }
    }
    {   // the real uplink from JAERO's comment: a contract request, found and its CRC good, no position
        AeroAdscMessage m;
        aeroDecodeAdsc("/PIKCPYA.ADS.HB-JHM07040B000C000D010E011000440D", true, m);
        CHECK(m.found && m.uplink && m.crcOk && !m.hasPos && m.airReg == "HB-JHM", "uplink found %d crc %d", m.found, m.crcOk);
    }
    {   // builders round trip, at the field resolutions (lat/lon 90/2^19 deg, 4 ft, 1/8 s, track 90/2^10 deg, 0.5 kt)
        std::vector<uint8_t> g = aeroAdscBasicGroup(-33.94612, 151.17722, 37004, 1234.375, 5);
        auto add = [&](const std::vector<uint8_t>& x) { g.insert(g.end(), x.begin(), x.end()); };
        add(aeroAdscFlightIdGroup("QFA1"));
        add(aeroAdscEarthRefGroup(287.3, 471.5, -1200));
        add(aeroAdscPredictedRouteGroup({-33.5, 150.2, 37000, 600}, {-32.9, 148.8, 39000, -1}));
        add(aeroAdscMeteoGroup(42.5, 265, -54.25));
        const std::string txt = aeroBuildAdscText("SYDCAYA", "VH-OQA", g);
        AeroAdscMessage m;
        CHECK(aeroDecodeAdsc(txt, false, m) && m.crcOk && m.complete, "own message '%s'", txt.c_str());
        const double lsb = 90.0 / (1 << 19);
        CHECK(near(m.lat, -33.94612, lsb) && near(m.lon, 151.17722, lsb) && m.altFt == 37004 && near(m.timeSec, 1234.375, 1e-9) && m.accuracy == 5, "basic %.6f %.6f %d",
              m.lat, m.lon, m.altFt);
        CHECK(m.flightId == "QFA1" && m.airReg == "VH-OQA", "flight id '%s'", m.flightId.c_str());
        CHECK(m.hasEarth && near(m.trackDeg, 287.3, 90.0 / 1024) && near(m.groundKt, 471.5, 1e-9) && m.vsFpm == -1200, "earth %.2f %.1f %d", m.trackDeg, m.groundKt, m.vsFpm);
        CHECK(m.hasRoute && near(m.next.lat, -33.5, lsb) && near(m.nextNext.lon, 148.8, lsb) && m.next.etaSec == 600 && m.nextNext.altFt == 39000, "route");
        CHECK(m.hasMeteo && near(m.windKt, 42.5, 1e-9) && near(m.windDeg, 265, 90.0 / 128) && near(m.tempC, -54.25, 1e-9), "meteo %.1f %.1f %.2f", m.windKt, m.windDeg, m.tempC);
    }
    {   // text formats, values from the airframes decoder tests
        textPos("H1", "POSN43312W123174,EASON,215754,370,EBINY,220601,ELENN,M48,02216,185/TS215754,0921227A40", 43.52, -123.29, 1e-3, 37000);
        textPos("H1", "POSN45209W122550,PEGTY,220309,134,MINNE,220424,HISKU,M6,060013,269,366,355K,292K,730A5B", 45.348, -122.917, 5e-3, 13400);
        textPos("H1", "POSN43030W122406,IBALL,220516,380,AARON,220816,MOXEE,M47,0047,86/TS220516,092122BF64", 43.05, -122.677, 5e-3, 38000);
        textPos("H1", "POSN33225W079428,SCOOB,232933,340,ENEME,235712,FETAL,M42,003051,15857F6", 33.375, -79.713, 5e-3, 34000);
        textPos("H1", "POSN38531W078000,CSN-01,112309,310,CYN-02,114151,ACK,M40,26067,22479226", 38.885, -78.0, 5e-3, 31000);
        textPos("H1", "F37AMCLL93#M1BPOS/ID746026,,/DC03032024,173207/MR1,/ET031846/PSN42579W108090,173207,320,WAIDE,031759,WEDAK,M49,267070,T468/CG264,110,360/FB742/VR324E17",
                42.965, -108.15, 1e-9, 32000);
        textPos("H1", "/.POS/TS100316,210324/PSS35333W058220,,100316,250,S37131W059150,101916,S39387W060377,M23,27282,241,780,MANUAL,0,813E711", -35.555, -58.367, 1e-3, 25000);
        textPos("20", "POSN38160W077075,,211733,360,OTT,212041,,N42,19689,40,544", 38.16, -77.075, 1e-3, -1);
        textPos("16", "N 44.203,W 86.546,31965,6, 290", 44.203, -86.546, 1e-9, 31965);
        textPos("16", "N 28.177/W 96.055", 28.177, -96.055, 1e-9, -1);
        textPos("16", "N 44.988,W121.644,35940,6, 170", 44.988, -121.644, 1e-9, 35940);
        AeroPosition p;
        CHECK(!aeroParseTextPosition("H1", "POS Bogus message", p), "bogus POS");
        CHECK(!aeroParseTextPosition("H1", "#M1BPOS Bogus message", p), "bogus #M1BPOS");
        CHECK(!aeroParseTextPosition("H1", "WX OMDB 071300Z 32012KT CAVOK 35/17 Q1008 NOSIG", p), "weather text");
        CHECK(!aeroParseTextPosition("20", "POSUNKNOWN", p), "label 20 POSUNKNOWN");
        CHECK(aeroParseTextPosition("H1", "POSN43312W123174,EASON,215754,370", p) && p.secOfDay == 21 * 3600 + 57 * 60 + 54, "report time %d", p.secOfDay);
    }
    printf(fails ? "aero adsc: %d FAILED\n" : "aero adsc: all passed\n", fails);
    return fails ? 1 : 0;
}
