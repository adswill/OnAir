// APRS / Packet protocol layers: CRC, bit stuffing and HDLC, AX.25 frames, APRS parsing. Pure functions, no signal; instant.
#include "dect2/packet_aprs.h"
#include "dect2/packet_ax25.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void testCrc() {
    // the CRC-16/X-25 check value of "123456789" is 0x906E
    const uint8_t d[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(ax25::fcs(d, 9) == 0x906E, "fcs %04x", ax25::fcs(d, 9));
    std::vector<uint8_t> v(d, d + 9);
    ax25::appendFcs(v);
    CHECK(ax25::fcsOk(v.data(), v.size()), "fcs of an appended frame");
    v[3] ^= 0x10;
    CHECK(!ax25::fcsOk(v.data(), v.size()), "a flipped bit must fail");
}

static void testStuffing() {
    // five ones in a row get a zero after them, and the receiver takes it out again
    std::vector<uint8_t> fr = {0xFF, 0xFF, 0x7E, 0x1F, 0x00, 0xFC, 0xFF, 0xF8, 0x3E, 0x7C, 0xFE, 0x01, 0x80, 0xFF, 0x7F, 0xE0, 0x3F};
    ax25::appendFcs(fr);
    const std::vector<uint8_t> bits = ax25::hdlcBits(fr, 3, 3);
    int run = 0, maxRun = 0;
    for (size_t i = 24; i + 24 < bits.size(); i++) { run = bits[i] ? run + 1 : 0; if (run > maxRun) maxRun = run; }
    CHECK(maxRun <= 5, "longest run of ones inside the frame %d", maxRun);
    std::vector<std::vector<uint8_t>> got;
    ax25::HdlcRx rx([&](const std::vector<uint8_t>& d, bool ok) { if (ok) got.push_back(d); });
    for (uint8_t b : ax25::nrziEncode(bits)) rx.bit(b);
    CHECK(got.size() == 1 && got[0] == std::vector<uint8_t>(fr.begin(), fr.end() - 2), "frame back through NRZI and unstuffing (%zu)", got.size());
    // the same through the G3RUH scrambler and a descrambler
    uint32_t st = 0;
    const std::vector<uint8_t> sc = ax25::scramble(ax25::nrziEncode(bits), st);
    got.clear();
    ax25::HdlcRx rx2([&](const std::vector<uint8_t>& d, bool ok) { if (ok) got.push_back(d); });
    uint32_t sr = 0;
    for (uint8_t r : sc) { const uint32_t d = r ^ ((sr >> 11) & 1u) ^ ((sr >> 16) & 1u); sr = ((sr << 1) | r) & 0x1FFFFu; rx2.bit((int)d); }
    CHECK(got.size() == 1, "frame back through the scrambler (%zu)", got.size());
    // a bad FCS is reported as such
    fr.back() ^= 1;
    int bad = 0;
    ax25::HdlcRx rx3([&](const std::vector<uint8_t>&, bool ok) { if (!ok) bad++; });
    for (uint8_t b : ax25::nrziEncode(ax25::hdlcBits(fr, 3, 3))) rx3.bit(b);
    CHECK(bad == 1, "bad FCS reported %d times", bad);
}

static void testFrame() {
    ax25::Frame f;
    f.from.call = "A61QQ"; f.from.ssid = 9;
    f.to.call = "APRS";
    f.path.push_back({"WIDE1", 1, true});
    f.path.push_back({"WIDE2", 1, false});
    f.info = "!4903.50N/07201.75W-Test";
    const std::vector<uint8_t> b = ax25::buildFrame(f);
    CHECK(ax25::fcsOk(b.data(), b.size()), "built frame has a good FCS");
    ax25::Frame g;
    CHECK(ax25::parseFrame(b.data(), b.size() - 2, g), "parse");
    CHECK(g.from.str() == "A61QQ-9" && g.to.str() == "APRS" && g.path.size() == 2 && g.info == f.info && g.control == 3 && g.pid == 0xF0, "fields");
    CHECK(g.tnc2() == "A61QQ-9>APRS,WIDE1-1*,WIDE2-1:!4903.50N/07201.75W-Test", "tnc2 '%s'", g.tnc2().c_str());
    CHECK(g.isUi() && g.typeName() == "UI", "type");
}

static aprs::Info P(const char* dest, const std::string& info, bool* ok = nullptr) {
    aprs::Info a;
    const bool r = aprs::parse(dest, info, a);
    if (ok) *ok = r;
    return a;
}

static void testAprs() {
    bool ok;
    // APRS101 chapter 8 example: uncompressed position without timestamp
    aprs::Info a = P("APRS", "!4903.50N/07201.75W-Test 001234", &ok);
    CHECK(ok && a.type == "Position" && a.hasPos && near(a.lat, 49.058333, 1e-5) && near(a.lon, -72.029167, 1e-5) && a.symTable == '/' && a.symCode == '-' && a.comment == "Test 001234",
          "uncompressed %.5f %.5f '%s'", a.lat, a.lon, a.comment.c_str());
    // with a time stamp, messaging, course/speed and altitude
    a = P("APRS", "@092345z4903.50N/07201.75W>088/036/A=001234mobile", &ok);
    CHECK(ok && a.hasPos && a.hasCourse && a.courseDeg == 88 && a.hasSpeed && near(a.speedKnots, 36, 1e-9) && a.hasAlt && near(a.altM, 1234 * 0.3048, 0.01) && a.symCode == '>', "timestamp position");
    // position ambiguity: spaces count as zero
    a = P("APRS", "!49  .  N/072  .  W-", &ok);
    CHECK(ok && near(a.lat, 49.0, 1e-6) && near(a.lon, -72.0, 1e-6), "ambiguity");
    // APRS101 chapter 9 compressed example: 49 30 N, 72 45 W, 88 degrees, 36.2 knots
    a = P("APRS", "=/5L!!<*e7>7P[", &ok);
    CHECK(ok && a.hasPos && near(a.lat, 49.5, 1e-3) && near(a.lon, -72.75, 1e-3) && a.symTable == '/' && a.symCode == '>', "compressed pos %.4f %.4f", a.lat, a.lon);
    CHECK(a.hasCourse && a.courseDeg == 88 && a.hasSpeed && near(a.speedKnots, 36.2, 0.1), "compressed course %d speed %.2f", a.courseDeg, a.speedKnots);
    // Mic-E, a packet from Dire Wolf's notes: N1JDU-9>ECCU8Y: lat 42 25.89 N, lon 071 02.99 W, speed 0, course 344, symbol />
    a = P("ECCU8Y", std::string("'cZ\x7f") + "l#H>/]Go fly a kite!", &ok);
    CHECK(ok && a.type == "Mic-E" && near(a.lat, 42 + 25.89 / 60, 1e-4) && near(a.lon, -(71 + 2.99 / 60), 1e-4), "mic-e position %.5f %.5f", a.lat, a.lon);
    CHECK(a.hasSpeed && near(a.speedKnots, 0, 1e-9) && a.hasCourse && a.courseDeg == 344 && a.symCode == '>' && a.symTable == '/', "mic-e speed %.1f course %d sym %c%c", a.speedKnots, a.courseDeg, a.symTable, a.symCode);
    // Mic-E south and east with a message code in the first three letters ("En Route" = P P 0), and an altitude
    a = P("RU2305", std::string("`") + (char)(55 + 28) + (char)(18 + 28) + (char)(27 + 28) + (char)(1 + 28) + (char)(2 + 28) + (char)(47 + 28) + std::string(">/\"xT}hi"), &ok);
    CHECK(ok && a.hasPos && near(a.lat, -(25 + 23.05 / 60), 1e-4) && near(a.lon, 55 + 18.27 / 60, 1e-4), "mic-e hemispheres %.4f %.4f", a.lat, a.lon);
    CHECK(a.hasAlt && near(a.altM, 6249, 0.5) && a.comment == "hi" && a.hasSpeed && near(a.speedKnots, 10, 1e-9) && a.courseDeg == 247, "mic-e altitude %.0f '%s'", a.altM, a.comment.c_str());
    CHECK(a.micEStatus == "En Route", "mic-e status '%s'", a.micEStatus.c_str());
    // object
    a = P("APRS", ";LEADER   *092345z4903.50N/07201.75W>088/036", &ok);
    CHECK(ok && a.type == "Object" && a.name == "LEADER" && a.live && near(a.lat, 49.058333, 1e-5) && a.hasSpeed, "object '%s'", a.name.c_str());
    a = P("APRS", ")AID #2!4903.50N/07201.75WA", &ok);
    CHECK(ok && a.type == "Item" && a.name == "AID #2" && a.live && a.symCode == 'A', "item '%s'", a.name.c_str());
    // message, with number; ack
    a = P("APRS", ":WU2Z     :Testing{003", &ok);
    CHECK(ok && a.type == "Message" && a.name == "WU2Z" && a.text == "Testing" && a.msgNo == "003", "message '%s' '%s' '%s'", a.name.c_str(), a.text.c_str(), a.msgNo.c_str());
    a = P("APRS", ":WU2Z     :ack003", &ok);
    CHECK(ok && a.isAck && a.msgNo == "003", "ack");
    // status
    a = P("APRS", ">Net Control Center", &ok);
    CHECK(ok && a.type == "Status" && a.text == "Net Control Center", "status");
    // APRS101 chapter 12 weather examples: positionless, and with a position
    a = P("APRS", "_10090556c220s004g005t077r000p000P000h50b09900wRSW", &ok);
    CHECK(ok && a.type == "Weather" && a.hasWx && near(a.wx.windDirDeg, 220, 1e-9) && near(a.wx.windMph, 4, 1e-9) && near(a.wx.gustMph, 5, 1e-9) && near(a.wx.tempF, 77, 1e-9)
          && near(a.wx.humidityPct, 50, 1e-9) && near(a.wx.pressureMbar, 990.0, 1e-9), "positionless weather");
    a = P("APRS", "@092345z4903.50N/07201.75W_220/004g005t-07r001p002P003h00b10132", &ok);
    CHECK(ok && a.type == "Weather" && near(a.wx.tempF, -7, 1e-9) && near(a.wx.humidityPct, 100, 1e-9) && near(a.wx.rain1hIn, 0.01, 1e-9) && near(a.wx.rain24hIn, 0.02, 1e-9)
          && near(a.wx.rainMidnightIn, 0.03, 1e-9) && near(a.wx.pressureMbar, 1013.2, 1e-9) && near(a.wx.windMph, 4, 1e-9), "weather with position");
    // telemetry is shown raw
    a = P("APRS", "T#005,199,000,255,073,123,01101001", &ok);
    CHECK(ok && a.type == "Telemetry" && a.text.rfind("005,199", 0) == 0, "telemetry");
    // rubbish is refused, not crashed on
    P("APRS", "", &ok); CHECK(!ok, "empty");
    a = P("APRS", "!49", &ok); CHECK(ok && a.type == "Other" && !a.hasPos, "short position");
    P("ABC", "`short", &ok); CHECK(!ok, "short mic-e");
    P("APRS", ";X", &ok); CHECK(!ok, "short object");
}

int main() {
    testCrc();
    testStuffing();
    testFrame();
    testAprs();
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
