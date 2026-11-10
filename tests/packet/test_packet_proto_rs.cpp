// Golden-vector test of the Rust AX.25 / APRS layer (rust/onair_ax25, dect2::ax25 and dect2::aprs): FCS, frames built by the transmitter and damaged /
// truncated / random, address and path text, frame type names, the HDLC receiver fed with transmissions with and without noise, APRS information fields of
// every type with mutations, latitude / longitude text, ONAIR_DIFF_ITERS (default 200000) per family. Every result goes into a digest (tests/common/golden.h;
// doubles rounded to 1e-6) compared with the one recorded from the C++ layers the Rust code replaced.
#include "dect2/packet/packet_ax25.h"
#include "dect2/packet/packet_aprs.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::ax25;
namespace PA = dect2::aprs;

static golden::Digest dig;
static long framesParsed = 0, hdlcGood = 0, hdlcBad = 0, aprsTrue = 0;
static long typeCount[16];

static std::mt19937 g(31337);
static std::string rnd(const char* alphabet, size_t n) { std::string s; for (size_t i = 0; i < n; i++) s += alphabet[g() % strlen(alphabet)]; return s; }

static void addAddr(const A::Address& a) { dig.add(a.call, a.ssid, a.h); }
static void addFrame(const A::Frame& a) {
    addAddr(a.to); addAddr(a.from);
    dig.add((uint64_t)a.path.size());
    for (const auto& p : a.path) addAddr(p);
    dig.add(a.control, a.pid, a.info);
}
static void addInfo(const PA::Info& a) {
    const double* wa = &a.wx.windDirDeg;
    for (int i = 0; i < 9; i++) dig.add(*(wa + i));
    dig.add(a.type, a.hasPos, a.lat, a.lon, a.symTable, a.symCode);
    dig.add(a.hasAlt, a.altM, a.hasCourse, a.courseDeg, a.hasSpeed, a.speedKnots);
    dig.add(a.name, a.live, a.comment, a.text, a.msgNo, a.isAck);
    dig.add(a.isRej, a.micEStatus, a.hasWx, a.summary);
}

static std::string callsign(bool valid) {
    if (!valid) return rnd("ABCxyz09 -#", g() % 9);
    return rnd("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 1 + g() % 6);
}

static A::Frame randFrame(bool valid) {
    A::Frame f;
    f.to.call = callsign(valid); f.to.ssid = (int)(g() % 16); f.to.h = g() % 2;
    f.from.call = callsign(valid); f.from.ssid = (int)(g() % 16); f.from.h = g() % 2;
    const int np = (int)(g() % 9);
    for (int i = 0; i < np; i++) { A::Address a; a.call = callsign(valid); a.ssid = (int)(g() % 16); a.h = g() % 3 == 0; f.path.push_back(a); }
    static const uint8_t ctl[] = {0x03, 0x13, 0x00, 0x02, 0x10, 0x01, 0x05, 0x09, 0x0D, 0x2F, 0x3F, 0x6F, 0x43, 0x53, 0x0F, 0x1F, 0x63, 0x73, 0x87, 0xAF, 0xE3, 0xFF};
    f.control = g() % 4 ? 0x03 : ctl[g() % sizeof ctl];
    f.pid = (f.control & 1) == 0 || (f.control & 0xEF) == 0x03 ? (int)(g() % 256) : -1;
    f.info = rnd("!=/@>:;`'_ABCDEFGHIJ0123456789.,-/ \r\n\x01\xff", g() % 120);
    return f;
}

static void parseCheck(const std::vector<uint8_t>& d) {
    A::Frame a;
    const bool pa = A::parseFrame(d.data(), d.size(), a);
    dig.add(pa);
    if (pa) {
        addFrame(a);
        dig.add(a.tnc2(), a.pathStr(), a.typeName(), a.from.str(), a.isUi());
        framesParsed++;
    }
}

static void testFrames(long iters) {
    for (int c = 0; c < 256; c++) { A::Frame a; a.control = (uint8_t)c; dig.add(a.typeName()); }
    for (long i = 0; i < iters; i++) {
        const A::Frame af = randFrame(g() % 8 != 0);
        dig.add(af.tnc2(), af.pathStr(), af.typeName(), af.to.str(), af.from.str());
        std::vector<uint8_t> bytes = A::buildFrame(af);
        dig.add(bytes);
        dig.add(A::fcsOk(bytes.data(), bytes.size()));
        std::vector<uint8_t> body(bytes.begin(), bytes.end() - 2);
        parseCheck(body);
        const int how = (int)(g() % 5);
        if (how == 1 && !body.empty()) for (int k = (int)(1 + g() % 3); k > 0; k--) body[g() % body.size()] ^= (uint8_t)(1u << (g() % 8));
        else if (how == 2) body.resize(g() % (body.size() + 1));
        else if (how == 3) { body.resize(g() % 80); for (auto& x : body) x = (uint8_t)g(); }
        else if (how == 4 && body.size() > 16) body[g() % 14] = (uint8_t)g();
        parseCheck(body);
        std::vector<uint8_t> damaged = bytes;
        if (!damaged.empty() && g() % 2) damaged[g() % damaged.size()] ^= (uint8_t)(1u << (g() % 8));
        dig.add(A::fcs(damaged.data(), damaged.size()), A::fcsOk(damaged.data(), damaged.size()));
    }
    for (size_t n = 0; n < 4; n++) { const uint8_t z[4] = {1, 2, 3, 4}; dig.add(A::fcsOk(z, n)); }
}

struct Got { std::vector<uint8_t> data; bool ok; };

static void testHdlc(long iters) {
    for (long i = 0; i < iters / 10; i++) {
        const bool nrzi = g() % 3 != 0;
        std::vector<Got> ga;
        A::HdlcRx ha([&](const std::vector<uint8_t>& d, bool ok) { ga.push_back({d, ok}); }, nrzi);
        std::vector<uint8_t> bits;
        const int nf = 1 + (int)(g() % 4);
        for (int k = 0; k < nf; k++) {
            const A::Frame rf = randFrame(true);
            const std::vector<uint8_t> fb = A::buildFrame(rf);
            const std::vector<uint8_t> hb2 = ORD_CALL(A::hdlcBits, fb, 1 + (int)(g() % 6), 1 + (int)(g() % 3));   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
            bits.insert(bits.end(), hb2.begin(), hb2.end());
            if (g() % 3 == 0) for (int q = (int)(g() % 40); q > 0; q--) bits.push_back((uint8_t)(g() & 1));
        }
        std::vector<uint8_t> line = nrzi ? A::nrziEncode(bits, (uint8_t)(g() & 1)) : bits;
        const int noise = (int)(g() % 4);
        if (noise == 1) for (int q = (int)(g() % 6); q > 0; q--) line[g() % line.size()] ^= 1;
        else if (noise == 2) { const size_t s = g() % line.size(); for (size_t q = s; q < line.size() && q < s + 30; q++) line[q] = (uint8_t)(g() & 1); }
        else if (noise == 3) for (auto& x : line) if (g() % 50 == 0) x ^= 1;
        for (size_t k = 0; k < line.size(); k++) {
            ha.bit(line[k]);
            dig.add(ha.inFrame(), ha.flagSeen());
            if (k % 7 == 0) dig.add(ha.takeClockResync());
            if (g() % 5000 == 0) ha.reset();
        }
        dig.add((uint64_t)ga.size());
        for (const Got& x : ga) dig.add(x.ok, x.data);
        for (const Got& x : ga) (x.ok ? hdlcGood : hdlcBad)++;
    }
    for (long i = 0; i < iters / 20; i++) {   // plain random bit streams, long runs of ones included
        std::vector<Got> ga;
        A::HdlcRx ha([&](const std::vector<uint8_t>& d, bool ok) { ga.push_back({d, ok}); }, false);
        const int density = 1 + (int)(g() % 9);
        for (int k = 0; k < 3000; k++) { const int b = (int)(g() % 10) < density; ha.bit(b); }
        dig.add((uint64_t)ga.size());
    }
}

static const char* const kInfoSeeds[] = {
    "!4903.50N/07201.75W-Test 001234", "=4903.50N/07201.75W>123/045/A=001234 hello", "/092345z4903.50N/07201.75W>088/036", "@092345/4903.50N/07201.75W_220/004g005t077r000p000P000h50b09900",
    "!4903.5 N/07201.7 W-ambiguous", "!/5L!!<*e7>7P[", "=/5L!!<*e7> sT", "!\\5L!!<*e7OS]Svc", "@092345z/5L!!<*e7_7P[220/004g005t077", ";LEADER   *092345z4903.50N/07201.75W>088/036",
    ";LEADER   _092345z4903.50N/07201.75W>", ")AID #2!4903.50N/07201.75Wa", ")AID #2_4903.50N/07201.75Wa", ">Net Control Center", ">092345zStatus here", ":WB4APR   :Hello World{3",
    ":WB4APR   :ack3", ":WB4APR   :rej7", ":BLNA     :Bulletin text", "T#005,199,000,255,073,123,01101001", "$GPRMC,092751.000,A,5321.6802,N,00630.3372,W,0.06,31.66,280511,,,A*45",
    "<IGATE,MSG_CNT=10,LOC_CNT=5", "?APRS?", "?IGATE?", "}W1AW>APRS,TCPIP*:!4903.50N/07201.75W-", "!!0000006E0000", "_10090556c220s004g005t077r000p000P000h50b09900wRSW",
    "_10090556220/004g005t077r001p002P003h00b10132", "Xunknown type", "`(_f\"Oj/>comment", "'(_f\"Oj/]\"4V}Alt 1234", "`(_f\"Oj/`\"4V}", "`(_f\"Oj/>}ab!}x",
};

static std::string mutateInfo(std::string s) {
    static const char alpha[] = "0123456789NSEW/\\_!=@>:;)<?}`' ABCDEFGHxyz.,-{*#}\r\n\x01\x1c\x1d\xff\x7f";
    if (g() % 6 == 0) return rnd(alpha, g() % 60);
    for (int k = (int)(g() % 5); k > 0; k--) {
        switch (g() % 6) {
        case 0: if (!s.empty()) s[g() % s.size()] = alpha[g() % (sizeof alpha - 1)]; break;
        case 1: if (!s.empty()) s[g() % s.size()] = (char)g(); break;
        case 2: if (!s.empty()) ORD_CALL(s.erase, g() % s.size(), 1 + g() % 3); break;
        case 3: ORD_CALL(s.insert, g() % (s.size() + 1), 1, alpha[g() % (sizeof alpha - 1)]); break;
        case 4: s.resize(g() % (s.size() + 1)); break;
        default: s += rnd(alpha, g() % 12); break;
        }
    }
    return s;
}

static std::string randDest() {
    switch (g() % 5) {
    case 0: return "APRS";
    case 1: return ORD_CAT(rnd("0123456789ABCDEFGHIJKLPQRSTUVWXYZ", 6), (g() % 2 ? "-7" : ""));
    case 2: return "S32U6T";
    case 3: return rnd("0123456789KLPZ", 6);
    default: return rnd("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-", g() % 9);
    }
}

static void testAprs(long iters) {
    const size_t ns = sizeof kInfoSeeds / sizeof *kInfoSeeds;
    for (long i = 0; i < iters; i++) {
        std::string s = (g() % 10) ? kInfoSeeds[g() % ns] : std::string(1, (char)g());
        if (g() % 5) s = mutateInfo(s);
        const std::string dest = randDest();
        PA::Info a;
        const bool ra = PA::parse(dest, s, a);
        dig.add(ra); addInfo(a);
        aprsTrue += ra;
        for (size_t k = 0; k < 13; k++) { static const char* n[] = {"Position", "Mic-E", "Object", "Item", "Status", "Message", "Weather", "Telemetry", "Raw GPS", "Capabilities", "Query", "Third-party", "Other"}; if (a.type == n[k]) typeCount[k]++; }
    }
    for (int i = 0; i < 6; i++) { PA::Info a; dig.add(PA::parse("APRS", "", a)); addInfo(a); }
    static const double special[] = {0.0, -0.0, 25.2, -25.2, 90.0, -180.0, 179.99999, 1e-9, -1e-9, 0.00005, 0.00015, 1234.56789, -1e6, 1e15, NAN, INFINITY, -INFINITY};
    for (double v : special) dig.add(PA::fmtLat(v), PA::fmtLon(v));
    for (long i = 0; i < iters; i++) {
        const double frac = (double)g() / 4294967296.0 - 0.5;
        double v = frac * (g() % 2 ? 360.0 : 4.0);
        dig.add(PA::fmtLat(v), PA::fmtLon(v));
    }
}

int main() {
    const char* it = getenv("ONAIR_DIFF_ITERS");
    const long iters = it ? atol(it) : 200000;
    testFrames(iters);
    testHdlc(iters);
    testAprs(iters);
    printf("test_packet_proto_rs: frames parsed %ld, hdlc frames good %ld bad %ld, aprs parsed %ld; types: pos %ld mic-e %ld obj %ld item %ld status %ld msg %ld wx %ld tel %ld other %ld\n",
           framesParsed, hdlcGood, hdlcBad, aprsTrue, typeCount[0], typeCount[1], typeCount[2], typeCount[3], typeCount[4], typeCount[5], typeCount[6], typeCount[7], typeCount[12]);
    // re-recorded 2026-10-22: a message number now ends at '}' (the APRS 1.1 reply-ack form "{MM}AA"); every other result is unchanged (was 0x4d96e4dafc8fa36d)
    // re-recorded 2026-11-10: APRS position comments read PHG and RNG into text and apply the DAO extra precision to the position (was 0xbc227c25c7e45e24)
    return golden::check("packet_proto_rs", dig, 0xf181fa0713f6106full, !it);
}
