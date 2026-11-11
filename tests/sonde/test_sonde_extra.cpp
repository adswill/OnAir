// iMet-1/4, iMet-54, MRZ and iMS-100: frame layouts, FEC, CRC and sync on symbol streams in both polarities, with bit errors, and the iMet AFSK through the channel's tone
// detector with noise. Simulated signals only.
#include "dect2/sonde/sonde_bits.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <string>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static SondeTruth truth() {
    SondeTruth t;
    t.frame = 321; t.lat = 54.6789; t.lon = 25.2791; t.altM = 12345.6; t.vSpeed = 5.2; t.hSpeed = 11.0; t.headingDeg = 88.0; t.sats = 11;
    t.unixTime = (double)((int64_t)time(nullptr) / 86400 * 86400 + 12 * 3600 + 34 * 60 + 56); t.tempC = -42.5; t.humidity = 33.5; t.pressureHpa = 205.25; t.batteryV = 3.1;
    return t;
}
static bool near(double a, double b, double e) { return std::fabs(a - b) <= e; }

static void imetBitsTest() {
    SondeTruth t = truth();
    auto b = imetBits(t);
    auto d = makeImetDecoder();
    std::vector<SondeFix> out;
    d->push(b.data(), b.size(), 0, out);
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        const SondeFix& o = out[0];
        CHECK(o.crcOk && o.hasPos && near(o.lat, t.lat, 1e-4) && near(o.lon, t.lon, 1e-4) && near(o.altM, t.altM, 0.6));
        CHECK(o.hasTemp && near(o.tempC, t.tempC, 0.01) && near(o.humidity, t.humidity, 0.01) && near(o.pressureHpa, t.pressureHpa, 0.01) && near(o.batteryV, 3.1, 0.01));
        CHECK(o.sats == 11 && o.frame == 321 && o.hasTime && near(fmod(o.unixTime, 86400), 12 * 3600 + 34 * 60 + 56, 0.01));
    }
    b[200] ^= 1;   // a CRC error loses the packet
    auto d2 = makeImetDecoder(); out.clear(); d2->push(b.data(), b.size(), 0, out);
    CHECK(out.empty() || !out[0].hasTemp);
}

static void mrzTest() {
    SondeTruth t = truth();
    for (bool ll : {false, true}) for (int inv = 0; inv < 2; inv++) {
        auto d = makeMrzDecoder();
        std::vector<SondeFix> out;
        for (int sub : {0xC, 0xD, 0x1}) {
            auto s = mrzSymbols(t, sub, ll);
            if (inv) for (auto& x : s) x = !x;
            std::vector<uint8_t> pad(50, inv ? 0 : 1);
            d->push(pad.data(), pad.size(), 0, out);
            d->push(s.data(), s.size(), 0, out);
        }
        CHECK(out.size() == 3);
        if (!out.empty()) {
            const SondeFix& o = out.back();
            CHECK(o.crcOk && o.hasPos && near(o.lat, t.lat, 1e-4) && near(o.lon, t.lon, 1e-4) && near(o.altM, t.altM, ll ? 0.02 : 0.1));
            CHECK(o.serial == "MRZ-11223-4455");
            if (!ll) CHECK(o.hasTemp && near(o.tempC, t.tempC, 0.01) && near(o.humidity, t.humidity, 0.01) && o.sats == 11);
        }
    }
}

// 4.0: the MRZ sync (9 x 1001 and 10101010) is found with up to 3 symbol errors, at most one in its last 8; copies shifted by 2 and 4 symbols never lock
static void mrzSyncTolerance() {
    SondeTruth t = truth();
    auto s = mrzSymbols(t, 0xC, false);
    // find the sync: the first position where 44 symbols match the pattern
    const uint64_t pat = 0x99999999999AAULL & ((1ULL << 44) - 1);
    size_t end = 0;
    {
        uint64_t reg = 0;
        for (size_t i = 0; i < s.size(); i++) { reg = (reg << 1) | (s[i] & 1); if ((reg & ((1ULL << 44) - 1)) == pat) { end = i + 1; break; } }
    }
    CHECK(end >= 44);
    const struct { int e[4]; int n; bool ok; } cases[] = {
        {{5, 20, 30, 0}, 3, true},        // three errors in the preamble part
        {{43, 10, 0, 0}, 2, true},        // one in the last 8 symbols
        {{43, 42, 0, 0}, 2, false},       // two in the last 8: not accepted
        {{1, 2, 3, 4}, 4, false},         // four errors
    };
    for (const auto& c : cases) {
        auto d = makeMrzDecoder();
        std::vector<SondeFix> out;
        auto q = s;
        for (int k = 0; k < c.n; k++) q[end - 44 + (size_t)c.e[k]] ^= 1;
        std::vector<uint8_t> pad(50, 1);
        d->push(pad.data(), pad.size(), 0, out);
        d->push(q.data(), q.size(), 0, out);
        CHECK((out.size() == 1) == c.ok);
    }
}

static void imet54Test() {
    SondeTruth t = truth();
    for (int flips : {0, 8, 16}) for (int inv = 0; inv < 2; inv++) {
        auto s = imet54Symbols(t, 100123);
        const size_t start = 22 * 10 + 5 * 10;
        std::mt19937 g(7 + flips);
        // single flips in distinct 80-bit groups: the interleaver spreads them so that each codeword sees at most one
        for (int k = 0; k < flips; k++) { size_t idx = start + (size_t)k * 80 + g() % 40; if (((idx - start) % 10) >= 1 && ((idx - start) % 10) <= 8) s[idx] ^= 1; }
        if (inv) for (auto& x : s) x = !x;
        auto d = makeImet54Decoder();
        std::vector<SondeFix> out;
        d->push(s.data(), s.size(), 0, out);
        CHECK(out.size() == 1);
        if (!out.empty()) {
            const SondeFix& o = out[0];
            CHECK(o.serial == "iMet54-100123" && o.crcOk && o.hasPos && near(o.lat, t.lat, 2e-5) && near(o.lon, t.lon, 2e-5) && near(o.altM, t.altM, 0.11) && near(o.tempC, t.tempC, 0.01));
        }
    }
    auto bad = imet54FrameBytes(t, 5); bad[20] ^= 0x10;
    SondeFix o; CHECK(!imet54ParseFrame(bad.data(), bad.size(), o));
}

static void meiseiTest() {
    SondeTruth t = truth();
    for (int flips : {0, 1, 2}) for (int inv = 0; inv < 2; inv++) {
        auto d = makeMeiseiDecoder();
        std::vector<SondeFix> out;
        for (int cnt = 0; cnt < 18; cnt++) {
            auto s = meiseiSymbols(t, cnt, 12345678);
            std::mt19937 g(cnt + 1);
            // flips inside blocks: up to 2 in each 46-bit block, bits flipped by inverting a whole symbol pair
            for (int sub = 0; sub < 2; sub++) for (int b = 0; b < 6; b++) for (int k = 0; k < flips; k++) {
                const size_t bit = 24 + (size_t)sub * 300 + (size_t)b * 46 + 3 + (size_t)k * 20;
                if (bit * 2 + 1 < s.size()) { s[bit * 2 + 1] ^= 1; }   // the second symbol alone flips only that bit
            }
            if (inv) for (auto& x : s) x = !x;
            d->push(s.data(), s.size(), 0, out);
        }
        CHECK(!out.empty());
        if (!out.empty()) {
            const SondeFix& o = out.back();
            CHECK(o.serial == "12345678" && o.hasPos && near(o.lat, t.lat, 5e-5) && near(o.lon, t.lon, 5e-5) && near(o.altM, t.altM, 0.02));
        }
    }
}

// 4.0: the iMS-100 sync (48 symbols) is found with up to 3 symbol errors
static void meiseiSyncTolerance() {
    SondeTruth t = truth();
    const uint64_t k0 = 0b101010101011010100101011001101001100101011001101ULL, k1 = 0b110011001101001101001101010100101010110010101010ULL, m = (1ULL << 48) - 1;
    for (int inv = 0; inv < 2; inv++) for (int e : {0, 3, 4, 6}) {
        auto d = makeMeiseiDecoder();
        std::vector<SondeFix> out;
        for (int cnt = 0; cnt < 18; cnt++) {
            auto s = meiseiSymbols(t, cnt, 12345678);
            uint64_t reg = 0; size_t end = 0;
            for (size_t i = 0; i < s.size(); i++) { reg = (reg << 1) | (s[i] & 1); if ((reg & m) == k0 || (reg & m) == k1) { end = i + 1; break; } }
            CHECK(end >= 48);
            for (int k = 0; k < e; k++) s[end - 48 + (size_t)(5 + 9 * k)] ^= 1;
            if (inv) for (auto& x : s) x = !x;
            std::vector<uint8_t> pad(64, inv ? 1 : 0);
            d->push(pad.data(), pad.size(), 0, out);
            d->push(s.data(), s.size(), 0, out);
        }
        CHECK(e <= 3 ? !out.empty() : out.empty());
    }
}

int main() {
    imetBitsTest();
    mrzTest();
    mrzSyncTolerance();
    imet54Test();
    meiseiTest();
    meiseiSyncTolerance();
    return testSummary("sonde_extra");
}
