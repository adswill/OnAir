// Interpretation of the AIS application and network messages (ais_apps.h). Real sentences and their expected values come from the gpsd regression
// logs (test/daemon/ais-nmea-type6-fid55.log and .chk, BSD-2-Clause); messages with no public example are built field by field from the published
// layouts. Damaged cases: wrong lengths, truncation and random bit errors must never crash or read past the payload.
#include "dect2/ais/ais_apps.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

using namespace dect2;
using namespace dect2::ais;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static Bits fromSentences(std::initializer_list<const char*> ss) {
    std::string chars; int fill = 0;
    for (const char* s : ss) { std::string c; int fc, fn; if (!parseNmea(s, c, fill, fc, fn)) { printf("bad sentence %s\n", s); fails++; } chars += c; }
    return unarmour(chars, fill);
}
static std::string field(const AisAppRecord& r, const char* key) {
    for (const auto& f : r.fields) if (f.key == key) return f.value;
    return "";
}
static bool decode(const Bits& b, AisMsg& m, AisAppRecord& r) { decodeMessage(b, m); return interpretApp(b, m, r); }

static void realSamples() {
    AisMsg m; AisAppRecord r;
    {   // IMO 289 weather (gpsd: lat 3087858, lon 18672, wind 4, dir 222, humidity 69, air 158, pressure 213)
        CHECK(decode(fromSentences({"!AIVDM,2,1,8,B,8h2=Vg@0Gh0TN1N>vAB0@PegFPClDIFcgwl?wnSwe7wvlOwwsAwwnSGm,0*29", "!AIVDM,2,2,8,B,wvh0,0*44"}), m, r));
        CHECK(m.type == 8 && m.dac == 1 && m.fi == 31 && m.mmsi == 2320061);
        CHECK(r.category == AIS_APP_WEATHER && r.hasPos);
        CHECK(std::fabs(r.lat - 3087858 / 60000.0) < 1e-6 && std::fabs(r.lon - 18672 / 60000.0) < 1e-6);
        CHECK(field(r, "Wind") == "4 kn" && field(r, "Wind direction") == "222 deg");
        CHECK(field(r, "Humidity") == "69 %" && field(r, "Air temperature") == "15.8 C" && field(r, "Pressure") == "1012 hPa");
    }
    {   // IMO 236 weather (lat 3616491, lon 1586491, wind 14, dir 176, humidity 78, air 819 = 21.9 C, pressure 217 = 1017 hPa)
        CHECK(decode(fromSentences({"!AIVDM,2,1,5,A,8@2<HV@0BkLfrhhJWJT0SQrhF;<qoweVwwwwwwwwwwwwwwwwwwwwwwww,0*49", "!AIVDM,2,2,5,A,wt0,2*22"}), m, r));
        CHECK(m.dac == 1 && m.fi == 11 && r.category == AIS_APP_WEATHER);
        CHECK(std::fabs(r.lat - 3616491 / 60000.0) < 1e-6 && std::fabs(r.lon - 1586491 / 60000.0) < 1e-6);
        CHECK(field(r, "Air temperature") == "21.9 C" && field(r, "Pressure") == "1017 hPa" && field(r, "Wind") == "14 kn" && field(r, "Humidity") == "78 %");
        CHECK(field(r, "Wave height").empty() && field(r, "Salinity").empty());     // every "not available" stays out
    }
    {   // inland static data
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,83aIj>0j2d<tLL=MM1T0a?a@0000,0*02"}), m, r));
        CHECK(m.dac == 200 && m.fi == 10 && field(r, "ENI") == "03110554" && field(r, "Length") == "80.0 m" && field(r, "Ship type") == "Motor freighter (8010)");
    }
    {   // inland persons on board, addressed
        CHECK(decode(fromSentences({"!AIVDM,1,1,,B,63aGD6@0RW?8<SL4000000000000,0*11"}), m, r));
        CHECK(m.type == 6 && m.dac == 200 && m.fi == 55 && field(r, "Crew") == "4" && field(r, "Passengers") == "0");
    }
    {   // AtoN monitoring: 481 x 0.05 V
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,6>jCJfkfJjOt>dap@104P00,2*51"}), m, r));
        CHECK(m.dac == 235 && m.fi == 10 && field(r, "Supply") == "24.05 V" && field(r, "Light") == "off" && field(r, "RACON") == "none");
    }
    {   // legacy persons on board (FI 40), 16 data bits: 0x00C0 >> 3 = 24
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,63iHnT00OEGp06P0h0,4*3F"}), m, r));
        CHECK(m.dac == 1 && m.fi == 40 && field(r, "Persons") == "24");
    }
    {   // binary acknowledge
        CHECK(decode(fromSentences({"!AIVDM,1,1,,B,73aBL800RW?;,0*77"}), m, r));
        CHECK(m.type == 7 && m.valid && m.destMmsi == 2268402 && r.category == AIS_APP_NETWORK && field(r, "To").find("002268402, sequence") == 0);
    }
    {   // DGNSS broadcast: position in 1/10 minute
        CHECK(decode(fromSentences({"!AIVDM,1,1,,B,A@4757QAv0agH2Jd1CT`1gui12p=wDP9:P;w<@Hw,0*5C"}), m, r));
        CHECK(m.type == 17 && m.valid && r.hasPos);
        CHECK(std::fabs(r.lon - 83936 / 600.0) < 1e-6 && std::fabs(r.lat - 21371 / 600.0) < 1e-6);
        CHECK(field(r, "Correction data") == "160 bits");
    }
    {   // data link management
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,D02VqTQc@Jfp00K6EcIJ>5b@s6D,2*4F"}), m, r));
        CHECK(m.type == 20 && m.valid && r.fields.size() == 4);
        CHECK(field(r, "Reservation") == "offset 1716, 1 slots, 5 min, increment 750");
    }
    {   // group assignment with an empty region: nothing to draw
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,Gh2E:qP00000000000000000000,2*67"}), m, r));
        CHECK(m.type == 23 && m.valid && r.shapes.empty() && field(r, "Station type") == "all mobiles");
    }
    {   // DAC 367 FI 33 uses the environmental report layout
        CHECK(decode(fromSentences({"!AIVDM,1,1,,A,8h3QiWAKpCDN`20544T000ip0000,0*33"}), m, r));
        CHECK(m.dac == 367 && m.fi == 33 && r.title == "Environmental report");
        CHECK(field(r, "Reports") == "1" && field(r, "Site 1") == "water level 0.81 m (relative to datum) day 10 07:42 UTC");
    }
    {   // US encrypted / unpublished content stays a name only
        const Bits b = fromSentences({"!AIVDM,1,1,,A,8h3QiWAKpCDN`20544T000ip0000,0*33"});
        Bits c = b;
        putU(c, 40, 10, 366); putU(c, 50, 6, 56);
        CHECK(decode(c, m, r) && r.title == "Binary, DAC 366 FI 56" && r.fields.empty());
    }
}

static Bits header(int type, uint32_t mmsi) { Bits b; putU(b, 0, 6, (uint32_t)type); putU(b, 8, 30, mmsi); return b; }
static void pad(Bits& b, size_t n) { if (b.size() < n) b.resize(n); }

static void generated() {
    AisMsg m; AisAppRecord r;
    {   // area notice with a circle followed by a polygon and a text sub-area
        Bits b = header(8, 2320061); putU(b, 40, 10, 1); putU(b, 50, 6, 22);
        putU(b, 56, 10, 77); putU(b, 66, 7, 35);    // linkage, entry prohibited
        putU(b, 73, 4, 11); putU(b, 77, 5, 9); putU(b, 82, 5, 12); putU(b, 87, 6, 30); putU(b, 93, 18, 240);
        size_t p = 111;
        putU(b, p, 3, 0); putU(b, p + 3, 2, 1); putS(b, p + 5, 25, 30000); putS(b, p + 30, 24, 3300000); putU(b, p + 54, 3, 4); putU(b, p + 57, 12, 50);   // circle 500 m at 55 N, 0.5 E
        p += 87;
        putU(b, p, 3, 4); putU(b, p + 3, 2, 2);     // polygon, scale 100 m
        putU(b, p + 5, 10, 0); putU(b, p + 15, 10, 10);     // north 1 km
        putU(b, p + 25, 10, 180); putU(b, p + 35, 10, 10);  // east 1 km
        putU(b, p + 45, 10, 720); putU(b, p + 55, 10, 0);   // nothing more
        putU(b, p + 65, 10, 720); putU(b, p + 75, 10, 0);
        p += 87;
        putU(b, p, 3, 5); putText(b, p + 3, 84, "SECURITY ZONE");
        p += 87;
        pad(b, p);
        CHECK(decode(b, m, r));
        CHECK(r.category == AIS_APP_NOTICE && r.title == "Area notice");
        CHECK(field(r, "Notice") == "Entry prohibited" && field(r, "Duration") == "240 min" && field(r, "Text") == "SECURITY ZONE" && field(r, "Start") == "11-09 12:30 UTC");
        CHECK(r.shapes.size() == 2 && r.shapes[0].kind == 0 && r.shapes[0].radiusM == 500.0);
        CHECK(std::fabs(r.shapes[0].lat - 55.0) < 1e-9 && std::fabs(r.shapes[0].lon - 0.5) < 1e-9);
        CHECK(r.shapes.size() == 2 && r.shapes[1].kind == 4 && r.shapes[1].pts.size() == 3);
        if (r.shapes.size() == 2 && r.shapes[1].pts.size() == 3) {
            CHECK(std::fabs(r.shapes[1].pts[1].lat - (55.0 + 1000.0 / 111195.0)) < 1e-3);
            CHECK(r.shapes[1].pts[2].lon > r.shapes[1].pts[1].lon);
        }
        // the same notice, addressed
        Bits a = header(6, 2320061); putU(a, 40, 30, 244000000); putU(a, 72, 10, 1); putU(a, 82, 6, 23);
        a.resize(88 + (b.size() - 56));
        for (size_t i = 0; i < b.size() - 56; i++) a[88 + i] = b[56 + i];
        CHECK(decode(a, m, r) && r.title == "Area notice" && r.shapes.size() == 2 && m.destMmsi == 244000000);
    }
    {   // route
        Bits b = header(8, 2320061); putU(b, 40, 10, 1); putU(b, 50, 6, 27);
        putU(b, 56, 10, 3); putU(b, 66, 3, 1); putU(b, 69, 5, 2); putU(b, 112, 5, 2);
        putS(b, 117, 28, 6000000); putS(b, 145, 27, 33000000); putS(b, 172, 28, 6600000); putS(b, 200, 27, 33600000);
        CHECK(decode(b, m, r) && r.title == "Route information" && r.shapes.size() == 1 && r.shapes[0].pts.size() == 2);
        CHECK(field(r, "Route type") == "recommended" && field(r, "Sender") == "authority");
        CHECK(std::fabs(r.lat - 55.0) < 1e-3);
    }
    {   // text, broadcast and addressed
        Bits b = header(8, 2320061); putU(b, 40, 10, 1); putU(b, 50, 6, 29); putU(b, 56, 10, 9); putText(b, 66, 66, "HELLO WORLD");
        CHECK(decode(b, m, r) && field(r, "Text") == "HELLO WORLD" && r.category == AIS_APP_TEXT);
        Bits t = header(12, 244000000); putU(t, 40, 30, 244000001); putText(t, 72, 60, "KEEP CLEAR"); pad(t, 136);
        CHECK(decode(t, m, r) && m.valid && r.title == "Addressed safety text" && field(r, "Text") == "KEEP CLEAR" && field(r, "To") == "244000001");
    }
    {   // inland water levels
        Bits b = header(8, 2110000); putU(b, 40, 10, 200); putU(b, 50, 6, 24); putText(b, 56, 12, "DE");
        putU(b, 68, 11, 123); putS(b, 79, 14, 456); putU(b, 93, 11, 7); putS(b, 104, 14, -12); pad(b, 168);
        CHECK(decode(b, m, r) && r.category == AIS_APP_WATER && field(r, "Gauge") == "123: 456 cm" && r.fields.size() == 3);
    }
    {   // inland signal status: nine digits, green then red
        Bits b = header(8, 2110000); putU(b, 40, 10, 200); putU(b, 50, 6, 40); putS(b, 56, 28, 3000000); putS(b, 84, 27, 30000000); putU(b, 111, 4, 2); putU(b, 124, 3, 1);
        putU(b, 127, 30, 450000000); pad(b, 168);
        CHECK(decode(b, m, r) && field(r, "Lights") == "green, red" && field(r, "Direction") == "upstream" && r.hasPos && std::fabs(r.lat - 50.0) < 1e-6);
    }
    {   // EMMA warning with its stretch of the river
        Bits b = header(8, 2110000); putU(b, 40, 10, 200); putU(b, 50, 6, 23);
        putU(b, 56, 8, 26); putU(b, 64, 4, 11); putU(b, 68, 5, 9); putU(b, 73, 8, 26); putU(b, 81, 4, 11); putU(b, 85, 5, 10); putU(b, 90, 5, 6); putU(b, 95, 6, 0); putU(b, 101, 5, 18); putU(b, 106, 6, 0);
        putS(b, 112, 28, 3000000); putS(b, 140, 27, 30000000); putS(b, 167, 28, 3060000); putS(b, 195, 27, 30060000);
        putU(b, 222, 4, 8); putS(b, 226, 9, 5); putS(b, 235, 9, 9); putU(b, 244, 2, 3); putU(b, 246, 4, 3); pad(b, 256);
        CHECK(decode(b, m, r) && r.category == AIS_APP_NOTICE && field(r, "Type") == "flood" && field(r, "Wind") == "E" && r.shapes.size() == 1);
    }
    {   // traffic signal, VTS target, environmental sea state and air gap
        Bits b = header(8, 2320061); putU(b, 40, 10, 1); putU(b, 50, 6, 19); putText(b, 66, 120, "NIEUWE WATERWEG"); putS(b, 186, 25, 240000); putS(b, 211, 24, 3120000); putU(b, 237, 5, 2); putU(b, 253, 5, 3); pad(b, 360);
        CHECK(decode(b, m, r) && field(r, "Signal") == "do not proceed" && field(r, "Station") == "NIEUWE WATERWEG" && r.hasPos);
        Bits v = header(8, 2320061); putU(v, 40, 10, 1); putU(v, 50, 6, 17);
        putU(v, 56, 2, 0); putU(v, 58, 12, 0); putU(v, 70, 30, 244000000); putS(v, 104, 24, 3120000); putS(v, 128, 25, 240000); putU(v, 153, 9, 90); putU(v, 168, 10, 12); pad(v, 178);
        CHECK(decode(v, m, r) && field(r, "Target") == "MMSI 244000000, 52.0000 4.0000, 12 kn, 90 deg" && r.hasPos);
        Bits e = header(8, 2320061); putU(e, 40, 10, 1); putU(e, 50, 6, 26);
        putU(e, 56, 4, 10); putU(e, 60, 5, 9); putU(e, 65, 5, 12); putU(e, 70, 6, 0); putU(e, 76, 7, 5); putU(e, 83, 13, 4510); putU(e, 96, 13, 3000);
        pad(e, 56 + 112);
        CHECK(decode(e, m, r) && field(r, "Site 5") == "air gap 30.00 m, air draught 45.10 m");
    }
    {   // the assignment and interrogation commands
        Bits b = header(15, 2320061); putU(b, 40, 30, 244000000); putU(b, 70, 6, 5); pad(b, 88);
        CHECK(decode(b, m, r) && m.valid && field(r, "Station 1") == "244000000 asked for message 5");
        Bits c = header(16, 2320061); putU(c, 40, 30, 244000000); putU(c, 70, 12, 100); putU(c, 82, 10, 20); pad(c, 96);
        CHECK(decode(c, m, r) && m.valid && field(r, "Station 1") == "244000000, offset 100, increment 20");
        Bits d = header(10, 2320061); putU(d, 40, 30, 244000000); pad(d, 72);
        CHECK(decode(d, m, r) && m.valid && field(r, "To") == "244000000");
        Bits g = header(23, 2320061); putS(g, 40, 18, 6000); putS(g, 58, 17, 33000); putS(g, 75, 18, 3000); putS(g, 93, 17, 32000); putU(g, 110, 4, 2); putU(g, 146, 4, 5); pad(g, 160);
        CHECK(decode(g, m, r) && field(r, "Report interval") == "30 s" && r.shapes.size() == 1 && r.shapes[0].pts.size() == 5 && r.hasPos);
        Bits k = header(22, 2320061); putU(k, 40, 12, 2087); putU(k, 52, 12, 2088); putS(k, 69, 18, 6000); putS(k, 87, 17, 33000); putS(k, 104, 18, 3000); putS(k, 122, 17, 32000); pad(k, 168);
        CHECK(decode(k, m, r) && field(r, "Channel A") == "2087" && r.shapes.size() == 1);
        Bits s = header(25, 2320061); putU(s, 38, 1, 1); putU(s, 39, 1, 1); putU(s, 40, 30, 244000000); putU(s, 72, 10, 1); putU(s, 82, 6, 0); pad(s, 120);
        CHECK(decode(s, m, r) && m.valid && r.dac == 1 && r.fi == 0 && r.destMmsi == 244000000);
        Bits t = header(26, 2320061); putU(t, 39, 1, 0); pad(t, 200);
        CHECK(decode(t, m, r) && m.valid && r.title == "Multi slot binary");
        Bits ts = header(13, 2320061); putU(ts, 40, 30, 244000000); pad(ts, 72);
        CHECK(decode(ts, m, r) && m.valid && r.title == "Safety acknowledge");
    }
}

static void damaged() {
    AisMsg m; AisAppRecord r;
    // truncated: application messages with a body shorter than the layout are not interpreted
    Bits w = fromSentences({"!AIVDM,2,1,8,B,8h2=Vg@0Gh0TN1N>vAB0@PegFPClDIFcgwl?wnSwe7wvlOwwsAwwnSGm,0*29", "!AIVDM,2,2,8,B,wvh0,0*44"});
    Bits t(w.begin(), w.begin() + 200);
    CHECK(!decode(t, m, r));
    Bits a = header(8, 2320061); putU(a, 40, 10, 1); putU(a, 50, 6, 22); pad(a, 100);
    CHECK(!decode(a, m, r));
    // inland static with an impossible hazard code
    Bits i = fromSentences({"!AIVDM,1,1,,A,83aIj>0j2d<tLL=MM1T0a?a@0000,0*02"}); putU(i, 141, 3, 6);
    CHECK(!decode(i, m, r));
    // a message of a known type with a wrong length is not valid and not accepted
    Bits s = header(16, 2320061); pad(s, 72);
    CHECK(!decodeMessage(s, m) && !m.valid);
    CHECK(!acceptableAis(m, false));
    // out-of-range coordinates draw nothing
    Bits o = header(8, 2320061); putU(o, 40, 10, 1); putU(o, 50, 6, 22); putU(o, 66, 7, 35);
    putU(o, 111, 3, 0); putS(o, 116, 25, 181000); putS(o, 141, 24, 91000); pad(o, 56 + 87 + 55);
    CHECK(decode(o, m, r) && r.shapes.empty() && !r.hasPos);
    // random bit errors and lengths: no crash, and every shape stays bounded and finite
    std::mt19937 g(7);
    const int types[] = {6, 7, 8, 10, 12, 13, 14, 15, 16, 17, 20, 22, 23, 25, 26};
    for (int it = 0; it < 200000; it++) {
        Bits b(38 + g() % 1100);
        for (auto& x : b) x = g() & 1;
        putU(b, 0, 6, (uint32_t)types[g() % (sizeof types / sizeof *types)]);
        if (b.size() > 56 && (g() & 1)) { putU(b, 40, 10, (g() & 1) ? 1 : 200); putU(b, 50, 6, g() % 40); }
        if (b.size() > 88 && (g() & 1)) { putU(b, 72, 10, (g() & 1) ? 1 : 200); putU(b, 82, 6, g() % 56); }
        decodeMessage(b, m);
        if (interpretApp(b, m, r)) {
            CHECK(r.shapes.size() <= 16);
            for (const auto& sh : r.shapes) { CHECK(std::isfinite(sh.lat) && std::fabs(sh.lat) <= 90.01 && sh.pts.size() <= 16); }
            if (r.hasPos) CHECK(std::isfinite(r.lat) && std::fabs(r.lat) <= 90.01 && std::fabs(r.lon) <= 180.01);
        }
        if (fails > 20) return;
    }
}

int main() {
    realSamples();
    generated();
    damaged();
    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
