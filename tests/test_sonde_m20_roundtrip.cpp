// M20: truth -> frame -> symbols -> decoder (chunk sizes, polarity, errors), and the frames the decoder must leave alone.
#include "data/sonde/bits/util.h"
#include "../core/src/sonde_bits_dm.h"
#include <cstring>
using namespace sbt;
using namespace dect2::sondebits;

static SondeTruth truthN(int i) {
    static const double lat[] = {25.2048, -33.9, 47.8, 0.5, -0.5, 71.2, -54.8, 25.0};
    static const double lon[] = {55.2708, 151.2, -122.3, -0.25, 179.9, -179.9, -68.3, 55.0};
    SondeTruth t;
    t.serial = "211-4-01234";
    t.lat = lat[i % 8]; t.lon = lon[i % 8]; t.altM = 100.0 + 3100.0 * i; t.vSpeed = 5.0 - 0.8 * i; t.hSpeed = 6.0 + 4.0 * i;
    t.headingDeg = std::fmod(40.0 * i + 15.0, 360.0);
    t.unixTime = 1767268800.0 + 17.0 + 1000.0 * i; t.frame = 7 + i;
    t.tempC = 30.0 - 12.0 * i; t.batteryV = 2.9 + 0.1 * (i % 5);
    return t;
}

static void expectFix(const SondeFix& f, const SondeTruth& t, const char* what) {
    CHECK(f.crcOk, "%s: checksum", what);
    CHECK(f.type == "M20" && f.subtype == "M20", "%s: type %s/%s", what, f.type.c_str(), f.subtype.c_str());
    CHECK(f.serial == t.serial, "%s: serial %s vs %s", what, f.serial.c_str(), t.serial.c_str());
    CHECK(f.hasPos && std::fabs(f.lat - t.lat) < 6e-7 && std::fabs(f.lon - t.lon) < 6e-7 && std::fabs(f.altM - t.altM) < 0.0051, "%s: position %.7f %.7f %.3f vs %.7f %.7f %.3f", what, f.lat, f.lon, f.altM, t.lat, t.lon, t.altM);
    CHECK(f.hasVel && std::fabs(f.hSpeed - t.hSpeed) < 0.01 && std::fabs(f.vSpeed - t.vSpeed) < 0.0051, "%s: speed %.3f %.3f vs %.3f %.3f", what, f.hSpeed, f.vSpeed, t.hSpeed, t.vSpeed);
    double dh = std::fabs(f.headingDeg - t.headingDeg);
    dh = std::min(dh, 360.0 - dh);
    CHECK(dh < 0.3, "%s: heading %.2f vs %.2f", what, f.headingDeg, t.headingDeg);
    CHECK(f.hasTime && std::fabs(f.unixTime - t.unixTime) < 0.51, "%s: time %.3f vs %.3f", what, f.unixTime, t.unixTime);
    CHECK(f.hasTemp && std::fabs(f.tempC - t.tempC) < 0.3, "%s: temperature %.2f vs %.2f", what, f.tempC, t.tempC);
    CHECK(std::fabs(f.batteryV - t.batteryV) < 0.0066, "%s: battery %.3f vs %.3f", what, f.batteryV, t.batteryV);
    CHECK(!f.hasHumidity && !f.hasPressure && f.sats == -1, "%s: only what the sonde frame is known to carry", what);
}

static std::vector<uint8_t> bytesToSymbols(const std::vector<uint8_t>& fr) {
    std::vector<uint8_t> s;
    DmWriter w(s);
    for (int i = 0; i < 32; i++) w.bit(0);
    w.sync();
    for (uint8_t b : fr) w.byte(b);
    return s;
}
static void sealAt(std::vector<uint8_t>& f, size_t flen) {
    const uint16_t cs = sondeM10Checksum(f.data(), flen - 1);
    f[flen - 1] = (uint8_t)(cs >> 8); f[flen] = (uint8_t)cs;
}

int main() {
    Rng rng(6);
    for (int i = 0; i < 8; i++) {
        const SondeTruth t = truthN(i);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
            for (int inv = 0; inv < 2; inv++) {
                std::vector<uint8_t> s = randomSymbols(rng, 300 + (size_t)i * 13);
                const auto fr = m20Symbols(t);
                s.insert(s.end(), fr.begin(), fr.end());
                const auto tail = randomSymbols(rng, 400);
                s.insert(s.end(), tail.begin(), tail.end());
                if (inv) invert(s);
                auto d = makeM20Decoder();
                const auto out = feed(*d, s, chunk);
                char w[64]; std::snprintf(w, sizeof w, "truth %d chunk %zu inv %d", i, chunk, inv);
                CHECK(out.size() == 1, "%s: %zu reports", w, out.size());
                if (!out.empty()) expectFix(out[0], t, w);
            }
        }
    }
    // a height below sea level (signed 24 bit, as dxlAPRS and rdz_ttgo_sonde read it)
    {
        SondeTruth t = truthN(0); t.altM = -151.37; t.lat = 31.5; t.lon = 35.4;
        SondeFix fx;
        CHECK(m20ParseFrame(m20FrameBytes(t).data(), 70, fx) && fx.hasPos && std::fabs(fx.altM + 151.37) < 0.0051, "negative height %.2f", fx.altM);
    }
    // the serial number: all 120 year-month values, the 8 digits, the 13-bit number, both values of the single bit
    {
        int n = 0;
        for (int y = 0; y < 10; y++) for (int m = 1; m <= 12; m += 5) for (int dd = 1; dd <= 8; dd += 7) for (int b = 0; b < 2; b++) {
            char sn[16]; std::snprintf(sn, sizeof sn, "%d%02d-%d-%d%04d", y, m, dd, b, (n * 997) % 8192);
            SondeTruth t = truthN(0); t.serial = sn;
            SondeFix fx;
            CHECK(m20ParseFrame(m20FrameBytes(t).data(), 70, fx) && fx.serial == sn, "serial %s -> %s", sn, fx.serial.c_str());
            n++;
        }
    }
    // a serial number that does not fit is replaced by a valid one, the same one every time
    {
        SondeTruth t = truthN(0); t.serial = "not a serial";
        SondeFix a, b;
        CHECK(m20ParseFrame(m20FrameBytes(t).data(), 70, a) && m20ParseFrame(m20FrameBytes(t).data(), 70, b) && a.serial == b.serial && a.serial.size() == 11, "replacement serial %s", a.serial.c_str());
    }
    // 30 frames in a row
    {
        auto d = makeM20Decoder();
        std::vector<uint8_t> s;
        std::vector<SondeTruth> ts;
        for (int i = 0; i < 30; i++) {
            SondeTruth t = truthN(i % 8); t.unixTime = 1767268800.0 + 17.0 + i; t.frame = i;
            ts.push_back(t);
            const auto fr = m20Symbols(t);
            s.insert(s.end(), fr.begin(), fr.end());
            const auto gap = randomSymbols(rng, 9600 - fr.size());
            s.insert(s.end(), gap.begin(), gap.end());
        }
        const auto out = feed(*d, s, 4096);
        CHECK(out.size() == 30, "30 frames: %zu reports", out.size());
        for (size_t i = 0; i < out.size() && i < 30; i++) CHECK(out[i].crcOk && std::fabs(out[i].unixTime - ts[i].unixTime) < 0.51, "frame %zu", i);
    }
    // a gap of 20 ms (192 symbols) inside the third of five frames: that frame is lost (checksum), the others decode, nothing wrong comes out
    {
        auto d = makeM20Decoder();
        std::vector<uint8_t> s;
        std::vector<SondeTruth> ts;
        for (int i = 0; i < 5; i++) {
            SondeTruth t = truthN(i); t.unixTime = 1767268800.0 + 17.0 + i; ts.push_back(t);
            auto fr = m20Symbols(t);
            if (i == 2) fr.erase(fr.begin() + 700, fr.begin() + 700 + 192);
            s.insert(s.end(), fr.begin(), fr.end());
            const auto gap = randomSymbols(rng, 9600 - fr.size());
            s.insert(s.end(), gap.begin(), gap.end());
        }
        const auto out = feed(*d, s, 4096);
        int ok = 0;
        for (const auto& f : out) if (f.crcOk) { ok++; bool known = false; for (const auto& t : ts) known |= std::fabs(f.lat - t.lat) < 6e-7; CHECK(known, "a report with a position that was never sent"); }
        CHECK(ok == 4, "gap in one frame: %d good reports of 4 intact frames", ok);
    }
    // errors
    {
        const SondeTruth t = truthN(2);
        auto s = m20Symbols(t);
        auto bad = s;
        bad[32 + 32 + 200] ^= 1;
        auto d = makeM20Decoder();
        auto out = feed(*d, bad, 4096);
        CHECK(out.size() == 1 && !out[0].crcOk, "wrong data symbol: %zu reports", out.size());
        auto sy = s;
        sy[32 + 3] ^= 1; sy[32 + 17] ^= 1; sy[32 + 29] ^= 1;
        d = makeM20Decoder();
        out = feed(*d, sy, 4096);
        CHECK(out.size() == 1 && out[0].crcOk, "3 wrong sync symbols: %zu reports", out.size());
    }
    // reset in the middle of a frame
    {
        const auto s = m20Symbols(truthN(1));
        auto d = makeM20Decoder();
        std::vector<SondeFix> out;
        d->push(s.data(), s.size() / 2, 0, out);
        d->reset();
        d->push(s.data(), s.size(), 1, out);
        CHECK(out.size() == 1 && out[0].crcOk, "after reset: %zu reports", out.size());
    }
    // what must be ignored
    {
        auto d = makeM20Decoder();
        auto out = feed(*d, randomSymbols(rng, 3000000), 65536);
        CHECK(countOk(out) == 0, "noise decoded as a frame");
        std::printf("m20: 3 million random symbols: %zu bad-frame reports\n", out.size());
        d = makeM20Decoder();
        out = feed(*d, m10Symbols(truthN(3)), 4096);
        CHECK(out.empty(), "an M10 frame gave %zu reports in the M20 decoder", out.size());
        auto f = m20FrameBytes(truthN(3));
        f[1] = 0x9F; sealAt(f, 0x45);
        d = makeM20Decoder();
        out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.empty(), "a frame of another type gave %zu reports", out.size());
    }
    // the shorter frame (length byte 0x43, seen by rs1729 on some firmware): checked and parsed with the same offsets
    {
        const SondeTruth t = truthN(4);
        auto f = m20FrameBytes(t);
        f.resize(0x43 + 1);
        f[0] = 0x43;
        sealAt(f, 0x43);
        auto d = makeM20Decoder();
        const auto out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.size() == 1 && out[0].crcOk && out[0].hasPos && std::fabs(out[0].lat - t.lat) < 6e-7, "frame length 0x43: %zu reports", out.size());
    }
    // symbol error rate against frames received (no error correction in the M20 either)
    {
        const SondeTruth t = truthN(0);
        const auto frame = m20Symbols(t);
        std::printf("m20: frames decoded out of 300 at symbol error rate p (expected (1-p)^%zu):\n", frame.size());
        for (double p : {0.0, 1e-4, 3e-4, 1e-3, 3e-3}) {
            int good = 0;
            for (int k = 0; k < 300; k++) {
                auto s = frame;
                flipSymbols(rng, s, p);
                auto d = makeM20Decoder();
                good += countOk(feed(*d, s, 65536)) > 0;
            }
            const double expect = std::pow(1.0 - p, (double)frame.size());
            std::printf("  p=%-7g %3d  (%.0f%% expected)\n", p, good, 100.0 * expect);
            if (p == 0) CHECK(good == 300, "clean frames: %d / 300", good);
            if (p == 1e-3) CHECK(good >= (int)(300 * (expect - 0.1)), "p=1e-3: %d", good);
        }
    }
    if (fails()) { std::printf("%d failures\n", fails()); return 1; }
    std::printf("sonde m20 roundtrip: ok\n");
    return 0;
}
