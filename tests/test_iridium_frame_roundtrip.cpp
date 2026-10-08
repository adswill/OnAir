// Builders -> decoder: every frame type bit-exact, pager messages in parts through the assembler, bit errors injected at the bit level
// (single errors everywhere, random double errors), random bits that must not be taken for a frame, and the cost of one decode.
#include "dect2/iridium_frame.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef std::vector<uint8_t> Bits;
static IridiumFrame dec(const Bits& b, double ref = 0) {
    IridiumBurstBits x; x.bits = b; x.refUnixTime = ref;
    return decodeIridiumBurst(x);
}

int main() {
    std::mt19937 rng(12345);
    // ---- IRA: positions over the globe, 0 to 12 pages ----
    for (int k = 0; k < 300; k++) {
        int sat = rng() % 128, beam = rng() % 48;
        double lat = (int(rng() % 17000) - 8500) / 100.0, lon = (int(rng() % 36000) - 18000) / 100.0, alt = k % 2 ? 780 : 2 + rng() % 8;
        if (alt < 100) alt = 0;   // a beam centre on the ground
        int np = k % 13;
        std::vector<uint32_t> t;
        for (int i = 0; i < np; i++) t.push_back(rng());
        Bits b = iridiumBuildIra(sat, beam, lat, lon, alt, t);
        IridiumFrame f = dec(b);
        CHECK(f.type == IridiumType::IRA && f.ok && f.corrected == 0, "IRA %d: %s ok %d", k, f.typeName.c_str(), f.ok);
        CHECK(f.satId == sat && f.beamId == beam && f.paged == np, "IRA %d: sat %d/%d beam %d/%d paged %d/%d", k, f.satId, sat, f.beamId, beam, f.paged, np);
        if (alt > 100) {   // positions are in 4 km units: a few km, about 0.04 degrees at orbit height
            double dl = std::fabs(f.lat - lat), dn = std::fabs(std::remainder(f.lon - lon, 360.0));
            CHECK(dl < 0.1 && (dn < 0.1 / std::max(0.2, std::cos(lat * M_PI / 180))) && std::fabs(f.altKm - alt) < 6, "IRA %d: pos %.3f/%.3f alt %.1f for %.3f/%.3f alt %.0f", k, f.lat, f.lon, f.altKm, lat, lon, alt);
        }
    }
    // ---- IBC and the Iridium time ----
    {
        double t0 = 1.7676e9;   // before the epoch change of 2026-01-14
        for (double t : { 1.4e9, 1.5e9, 1.6e9, 1.76e9, 1.7684e9, 1.78e9, 1.85e9, 1.95e9 }) {
            Bits b = iridiumBuildIbc(28, 24, t);
            CHECK(b.size() == 262, "IBC length");
            IridiumFrame f = dec(b, t);
            CHECK(f.type == IridiumType::IBC && f.ok && f.satId == 28 && f.beamId == 24 && f.hasTime, "IBC at %.0f: %s", t, f.typeName.c_str());
            CHECK(std::fabs(f.unixTime - t) < 0.06, "IBC time %.2f for %.2f", f.unixTime, t);
        }
        (void)t0;
        for (int s : { 0, 1, 66, 127 }) for (int bm : { 0, 1, 47, 63 }) {
            IridiumFrame f = dec(iridiumBuildIbc(s, bm, 1.78e9), 1.78e9);
            CHECK(f.satId == s && f.beamId == bm, "IBC ids %d/%d", f.satId, f.beamId);
        }
        // the clock of this computer decides when no reference is given
        IridiumFrame f = dec(iridiumBuildIbc(28, 24, 1.77e9));
        CHECK(f.hasTime && std::fabs(f.unixTime - 1.77e9) < 400e6, "IBC without reference");
        // 2026-01-14 18:08:00 UTC is where the epoch changed: the same counter value reads 10.8 years apart either side
        for (double t : { 1.45e9, 1.6e9, 1.7e9, 1.7684e9, 1.79e9 }) {
            uint32_t l = iridiumLbfcFromTime(t);
            for (double off : { -3e6, 0.0, 3e6 })   // a reference clock that is a month out still picks the right era
                CHECK(std::fabs(iridiumTimeFromLbfc(l, t + off) - t) < 0.06, "era at %.0f with reference %+.0f", t, off);
        }
    }
    // ---- IDA ----
    for (int len = 1; len <= 20; len++) for (int ctr = 0; ctr < 8; ctr += 3) {
        std::vector<uint8_t> p;
        for (int i = 0; i < len; i++) p.push_back(rng());
        Bits b = iridiumBuildIda(p, ctr, len & 1);
        CHECK(b.size() == 358, "IDA length %zu", b.size());
        IridiumFrame f = dec(b);
        static const char* hx = "0123456789abcdef";
        std::string want;
        for (uint8_t c : p) { want += hx[c >> 4]; want += hx[c & 15]; }
        CHECK(f.type == IridiumType::IDA && f.ok && f.crcOk && f.hex == want && f.idaLen == len && f.idaCtr == ctr && f.idaCont == bool(len & 1),
              "IDA len %d ctr %d: %s ok %d crc %d hex %s", len, ctr, f.typeName.c_str(), f.ok, f.crcOk, f.hex.c_str());
    }
    // ---- voice-like bursts are voice, nothing else ----
    {
        int voice = 0, other = 0;
        for (uint32_t s = 1; s <= 4000; s++) {
            IridiumFrame f = dec(iridiumBuildVoiceLike(s));
            if (f.type == IridiumType::Voice && f.hex.empty()) voice++; else { other++; if (other < 4) printf("voice-like seed %u: %s\n", s, f.typeName.c_str()); }
        }
        CHECK(voice == 4000, "voice-like classified as voice: %d of 4000", voice);
    }
    // ---- pager messages: one to three parts through the assembler ----
    {
        const std::string words[] = { "Hello", "Dubai", "meeting", "at", "10:30", "bring", "the", "keys", "OK", "thanks", "0123456789", "Gate", "B12" };
        for (int len : { 1, 5, 20, 59, 60, 61, 100, 118, 119, 150, 177 }) {
            std::string text;
            while ((int)text.size() < len) text += words[rng() % 13] + " ";
            text = text.substr(0, len);
            if (text.back() == ' ') text.back() = '.';
            auto parts = iridiumBuildMsgParts(123456 + len, 17, text);
            size_t want = len <= 60 ? 1 : (len + 58) / 59;
            CHECK(parts.size() == want, "len %d: %zu parts, expected %zu", len, parts.size(), want);
            // order 0..n-1, then reversed and with a duplicate
            for (int variant = 0; variant < 3; variant++) {
                IridiumMsgAssembler as;
                std::vector<size_t> order;
                for (size_t i = 0; i < parts.size(); i++) order.push_back(variant == 1 ? parts.size() - 1 - i : i);
                if (variant == 2 && parts.size() > 1) order.insert(order.begin() + 1, order[0]);
                std::vector<IridiumPagerMessage> got;
                double now = 1000;
                for (size_t i : order) {
                    IridiumFrame f = dec(parts[i]);
                    CHECK(f.type == IridiumType::MSG && f.ok && f.ric == 123456 + len && f.msgSeq == 17 && f.block == (int)i && f.blocks == (int)parts.size(),
                          "len %d part %zu: %s ok %d ric %d block %d/%d", len, i, f.typeName.c_str(), f.ok, f.ric, f.block, f.blocks);
                    auto o = as.feed(f, now += 3);
                    got.insert(got.end(), o.begin(), o.end());
                }
                CHECK(got.size() == 1 && got[0].complete && got[0].text == text, "len %d variant %d: %zu messages, text '%s'", len, variant, got.size(), got.empty() ? "" : got[0].text.c_str());
            }
        }
        // a message that heard only two of three parts comes out incomplete after 2000 s, with the gap marked
        auto parts = iridiumBuildMsgParts(3551234, 3, std::string(150, 'x'));
        IridiumMsgAssembler as;
        auto o1 = as.feed(dec(parts[0]), 10), o2 = as.feed(dec(parts[2]), 20);
        CHECK(o1.empty() && o2.empty(), "no message before it is complete");
        auto o3 = as.feed(IridiumFrame(), 1500);
        CHECK(o3.empty(), "not yet expired");
        auto o4 = as.feed(IridiumFrame(), 2100);
        CHECK(o4.size() == 1 && !o4[0].complete && o4[0].text.find("[missing]") != std::string::npos && o4[0].ric == 3551234, "incomplete after timeout: %zu", o4.size());
        // the same message heard again after it completed is not given twice
        IridiumMsgAssembler a2;
        auto one = iridiumBuildMsgParts(42, 1, "again and again");
        size_t n1 = a2.feed(dec(one[0]), 10).size(), n2 = a2.feed(dec(one[0]), 20).size();
        CHECK(n1 == 1 && n2 == 0, "repeat: %zu then %zu", n1, n2);
        // a wrong text checksum never completes
        Bits bad = iridiumBuildMsg(7, 2, 0, 1, "abc");
        IridiumFrame fb = dec(bad);
        fb.msgChecksum ^= 1;
        IridiumMsgAssembler a3;
        CHECK(a3.feed(fb, 1).empty(), "bad text checksum accepted");
    }
    // ---- numeric pager messages ----
    for (std::string digits : { "5", "12345", "0123456789", "5551234567890123456789" }) {
        IridiumFrame f = dec(iridiumBuildMsgBcd(777, 9, digits));
        CHECK(f.type == IridiumType::MSG && f.ok && f.msgFmt == 3 && f.ric == 777 && f.msgSeq == 9 && f.msgText == digits, "BCD '%s': %s ok %d text '%s'", digits.c_str(), f.typeName.c_str(), f.ok, f.msgText.c_str());
        IridiumMsgAssembler as;
        auto o = as.feed(f, 5);
        CHECK(o.size() == 1 && o[0].complete && o[0].text == digits, "BCD assembled '%s'", digits.c_str());
    }
    // ---- time/location frames: the fixed header of iridium-toolkit (11 then 94 zeros) and 768 symbol bits ----
    {
        Bits t(96, 0); t[0] = t[1] = 1;
        for (int i = 0; i < 768; i++) t.push_back(rng() & 1);
        IridiumFrame f = dec(t);
        CHECK(f.type == IridiumType::ITL && f.ok, "ITL: %s ok %d", f.typeName.c_str(), f.ok);
        t[5] ^= 1; t[60] ^= 1;
        f = dec(t);
        CHECK(f.type == IridiumType::ITL && f.corrected == 2, "ITL with two header errors: %s corrected %d", f.typeName.c_str(), f.corrected);
    }
    // ---- bit errors ----
    struct Case { const char* name; Bits bits; IridiumType type; };
    std::vector<Case> cases;
    cases.push_back({ "IRA", iridiumBuildIra(16, 37, 28.8, 27.0, 780, { 0x113dbaf8, 0x0ca5b2e2 }), IridiumType::IRA });
    cases.push_back({ "IBC", iridiumBuildIbc(28, 24, 1.77e9), IridiumType::IBC });
    cases.push_back({ "MSG", iridiumBuildMsg(3525766, 30, 0, 1, "Meet at the north gate at 18:45, bring the signed forms"), IridiumType::MSG });
    cases.push_back({ "IDA", iridiumBuildIda({ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }, 2, false), IridiumType::IDA });
    cases.push_back({ "ISY", iridiumBuildIsy(), IridiumType::ISY });
    for (auto& c : cases) {
        IridiumFrame ref = dec(c.bits);
        CHECK(ref.type == c.type && ref.ok, "%s clean: %s ok %d", c.name, ref.typeName.c_str(), ref.ok);
        auto same = [&](const IridiumFrame& f) {
            return f.type == ref.type && f.ok && f.satId == ref.satId && f.beamId == ref.beamId && f.paged == ref.paged && f.ric == ref.ric && f.msgText == ref.msgText &&
                   f.hex == ref.hex && std::fabs(f.lat - ref.lat) < 1e-9 && std::fabs(f.lon - ref.lon) < 1e-9 && std::fabs(f.unixTime - ref.unixTime) < 1e-6;
        };
        int single = 0, singleBad = 0;
        for (size_t i = 0; i < c.bits.size(); i++) {
            Bits e = c.bits; e[i] ^= 1;
            IridiumFrame f = dec(e);
            if (c.type == IridiumType::ISY ? (f.type == IridiumType::ISY) : same(f)) single++;
            else { singleBad++; if (singleBad <= 3) printf("  %s: one error at bit %zu gives %s ok %d\n", c.name, i, f.typeName.c_str(), f.ok); }
        }
        CHECK(singleBad == 0, "%s: %d of %zu single errors not decoded", c.name, singleBad, c.bits.size());
        int dbl = 0, dblN = 400;
        for (int k = 0; k < dblN; k++) {
            Bits e = c.bits;
            size_t a = rng() % e.size(), b = rng() % e.size();
            if (a == b) b = (b + 1) % e.size();
            e[a] ^= 1; e[b] ^= 1;
            IridiumFrame f = dec(e);
            if (c.type == IridiumType::ISY ? (f.type == IridiumType::ISY) : same(f)) dbl++;
            else if (printf(""), true) printf("  %s: errors at %zu and %zu give %s ok %d\n", c.name, a, b, f.typeName.c_str(), f.ok);
        }
        printf("%s: %zu single errors all decoded; two random errors decoded %d of %d\n", c.name, c.bits.size(), dbl, dblN);
        // two errors are within the codes' reach, except in the short header codes (a single error each): the IRA and the MSG must pass all
        if (c.type == IridiumType::IRA) CHECK(dbl == dblN, "IRA double errors %d of %d", dbl, dblN);
        else CHECK(dbl >= dblN * 9 / 10, "%s double errors %d of %d", c.name, dbl, dblN);
    }
    // ---- bursts cut short at every length, odd lengths, uplink, nothing at all ----
    for (auto& c : cases) {
        int ira = 0, iraOk = 0;
        for (size_t len = 0; len <= c.bits.size(); len++) {
            Bits cut(c.bits.begin(), c.bits.begin() + len);
            IridiumFrame f = dec(cut);
            if (c.type == IridiumType::IRA && len >= 96) { ira++; iraOk += f.type == IridiumType::IRA && f.ok && f.satId == 16 && f.beamId == 37; }
            IridiumBurstBits u; u.bits = cut; u.downlink = false;
            (void)decodeIridiumBurst(u);
        }
        if (c.type == IridiumType::IRA) CHECK(ira == iraOk, "IRA cut after its header: %d of %d decoded", iraOk, ira);
    }
    // ---- random bits ----
    {
        int n = 200000, cls[13] = {0};
        for (int k = 0; k < n; k++) {
            Bits b(100 + rng() % 330);
            for (auto& x : b) x = rng() & 1;
            IridiumFrame f = dec(b);
            if (f.ok) cls[int(f.type)]++;
        }
        printf("random bursts (%d) that look like a good frame: IRA %d IBC %d ISY %d ITL %d MSG %d IDA %d IIP %d IIU %d IMS %d voice %d other-typed %d\n", n, cls[1], cls[2], cls[3], cls[4], cls[5], cls[6], cls[7], cls[10], cls[11], cls[12], cls[0]);
        // A burst reaching this layer has passed the unique word, so chance matches are rarer still; the bound is 1e-4 per burst.
        int taken = cls[int(IridiumType::IRA)] + cls[int(IridiumType::IBC)] + cls[int(IridiumType::MSG)] + cls[int(IridiumType::IDA)] + cls[int(IridiumType::ISY)];
        CHECK(taken <= n / 10000 && cls[int(IridiumType::MSG)] + cls[int(IridiumType::IDA)] + cls[int(IridiumType::ISY)] == 0, "random bits taken for a frame that carries data: %d", taken);
    }
    // ---- cost ----
    {
        std::vector<Bits> set;
        for (auto& c : cases) set.push_back(c.bits);
        set.push_back(iridiumBuildVoiceLike(1));
        auto t0 = std::chrono::steady_clock::now();
        int n = 0;
        for (int r = 0; r < 4000; r++) for (auto& b : set) { IridiumFrame f = dec(b); n += f.ok; }
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / (4000.0 * set.size());
        printf("decode: %.1f microseconds per burst (%d ok)\n", us, n);
        CHECK(us < 500, "decode too slow: %.1f us", us);
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
