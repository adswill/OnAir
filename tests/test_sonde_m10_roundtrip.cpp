// M10: truth -> frame -> symbols -> decoder, in every chunk size, both polarities, with errors, and the frames the decoder must leave alone.
#include "data/sonde/bits/util.h"
#include "../core/src/sonde_bits_dm.h"
#include <cstring>
using namespace sbt;
using namespace dect2::sondebits;

static SondeTruth truthN(int i) {
    static const double lat[] = {25.2048, -33.9, 47.8, 0.5, -0.5, 71.2, -54.8, 25.0};
    static const double lon[] = {55.2708, 151.2, -122.3, -0.25, 179.9, -179.9, -68.3, 55.0};
    SondeTruth t;
    t.serial = "310-2-11329";
    t.lat = lat[i % 8]; t.lon = lon[i % 8]; t.altM = 100.0 + 3100.0 * i; t.vSpeed = 5.0 - 0.8 * i; t.hSpeed = 6.0 + 4.0 * i;
    t.headingDeg = std::fmod(40.0 * i + 15.0, 360.0);
    t.sats = 5 + i; t.unixTime = 1767268800.0 + 17.25 + 1000.0 * i; t.frame = 7 + i;
    t.tempC = 30.0 - 12.0 * i; t.humidity = 10.0 + 11.0 * i; t.batteryV = 4.2 + 0.1 * (i % 5);
    return t;
}

static void expectFix(const SondeFix& f, const SondeTruth& t, const char* what) {
    CHECK(f.crcOk, "%s: checksum", what);
    CHECK(f.type == "M10" && f.subtype == "M10", "%s: type %s/%s", what, f.type.c_str(), f.subtype.c_str());
    CHECK(f.serial == t.serial, "%s: serial %s vs %s", what, f.serial.c_str(), t.serial.c_str());
    CHECK(f.hasPos && std::fabs(f.lat - t.lat) < 2e-7 && std::fabs(f.lon - t.lon) < 2e-7 && std::fabs(f.altM - t.altM) < 0.002, "%s: position %.7f %.7f %.3f vs %.7f %.7f %.3f", what, f.lat, f.lon, f.altM, t.lat, t.lon, t.altM);
    CHECK(f.hasVel && std::fabs(f.hSpeed - t.hSpeed) < 0.01 && std::fabs(f.vSpeed - t.vSpeed) < 0.0051, "%s: speed %.3f %.3f vs %.3f %.3f", what, f.hSpeed, f.vSpeed, t.hSpeed, t.vSpeed);
    double dh = std::fabs(f.headingDeg - t.headingDeg);
    dh = std::min(dh, 360.0 - dh);
    CHECK(dh < 0.3, "%s: heading %.2f vs %.2f", what, f.headingDeg, t.headingDeg);
    CHECK(f.sats == t.sats, "%s: sats %d vs %d", what, f.sats, t.sats);
    CHECK(f.hasTime && std::fabs(f.unixTime - std::floor(t.unixTime + 0.0005)) < 1.0 + 1e-9, "%s: time %.3f vs %.3f", what, f.unixTime, t.unixTime);
    CHECK(f.hasTemp && std::fabs(f.tempC - t.tempC) < 0.3, "%s: temperature %.2f vs %.2f", what, f.tempC, t.tempC);
    CHECK(f.hasHumidity && std::fabs(f.humidity - t.humidity) < 0.6, "%s: humidity %.2f vs %.2f", what, f.humidity, t.humidity);
    CHECK(std::fabs(f.batteryV - t.batteryV) < 0.01, "%s: battery %.3f vs %.3f", what, f.batteryV, t.batteryV);
    CHECK(f.frame == -1, "%s: no frame number", what);
}

static std::vector<uint8_t> bytesToSymbols(const std::vector<uint8_t>& fr) {
    std::vector<uint8_t> s;
    DmWriter w(s);
    for (int i = 0; i < 32; i++) w.bit(0);
    w.sync();
    for (uint8_t b : fr) w.byte(b);
    return s;
}

static void sealAt(std::vector<uint8_t>& f, size_t flen) {          // checksum over f[0..flen-2] into f[flen-1..flen]
    const uint16_t cs = sondeM10Checksum(f.data(), flen - 1);
    f[flen - 1] = (uint8_t)(cs >> 8); f[flen] = (uint8_t)cs;
}

int main() {
    Rng rng(5);
    // 1. eight truths, four chunk sizes, both polarities, noise around the frame
    for (int i = 0; i < 8; i++) {
        const SondeTruth t = truthN(i);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
            for (int inv = 0; inv < 2; inv++) {
                std::vector<uint8_t> s = randomSymbols(rng, 300 + (size_t)i * 13);
                const auto fr = m10Symbols(t);
                s.insert(s.end(), fr.begin(), fr.end());
                const auto tail = randomSymbols(rng, 400);
                s.insert(s.end(), tail.begin(), tail.end());
                if (inv) invert(s);
                auto d = makeM10Decoder();
                const auto out = feed(*d, s, chunk);
                char w[64]; std::snprintf(w, sizeof w, "truth %d chunk %zu inv %d", i, chunk, inv);
                CHECK(out.size() == 1, "%s: %zu reports", w, out.size());
                if (!out.empty()) expectFix(out[0], t, w);
            }
        }
    }
    // 2. 30 frames, one a second, the symbol clock offset does not matter to the decoder (it counts symbols)
    {
        auto d = makeM10Decoder();
        std::vector<uint8_t> s;
        std::vector<SondeTruth> ts;
        for (int i = 0; i < 30; i++) {
            SondeTruth t = truthN(i % 8);
            t.unixTime = 1767268800.0 + 17.0 + i; t.frame = i;
            ts.push_back(t);
            const auto fr = m10Symbols(t);
            s.insert(s.end(), fr.begin(), fr.end());
            const auto gap = randomSymbols(rng, 9615 - fr.size());
            s.insert(s.end(), gap.begin(), gap.end());
        }
        const auto out = feed(*d, s, 4096);
        CHECK(out.size() == 30, "30 frames: %zu reports", out.size());
        for (size_t i = 0; i < out.size() && i < 30; i++) CHECK(out[i].crcOk && std::fabs(out[i].unixTime - ts[i].unixTime) < 1.0, "frame %zu", i);
    }
    // a gap of 20 ms (192 symbols) inside the third of five frames: that frame is lost (checksum), the others decode, nothing wrong comes out
    {
        auto d = makeM10Decoder();
        std::vector<uint8_t> s;
        std::vector<SondeTruth> ts;
        for (int i = 0; i < 5; i++) {
            SondeTruth t = truthN(i); t.unixTime = 1767268800.0 + 17.0 + i; ts.push_back(t);
            auto fr = m10Symbols(t);
            if (i == 2) fr.erase(fr.begin() + 700, fr.begin() + 700 + 192);
            s.insert(s.end(), fr.begin(), fr.end());
            const auto gap = randomSymbols(rng, 9615 - fr.size());
            s.insert(s.end(), gap.begin(), gap.end());
        }
        const auto out = feed(*d, s, 4096);
        int ok = 0;
        for (const auto& f : out) if (f.crcOk) { ok++; bool known = false; for (const auto& t : ts) known |= std::fabs(f.lat - t.lat) < 2e-7; CHECK(known, "a report with a position that was never sent"); }
        CHECK(ok == 4, "gap in one frame: %d good reports of 4 intact frames", ok);
    }
    // 3. one wrong symbol in the data: the checksum catches it, a bad-frame report comes out; wrong symbols in the sync: still found
    {
        const SondeTruth t = truthN(2);
        auto s = m10Symbols(t);
        auto bad = s;
        bad[32 + 32 + 300] ^= 1;
        auto d = makeM10Decoder();
        auto out = feed(*d, bad, 4096);
        CHECK(out.size() == 1 && !out[0].crcOk, "wrong data symbol: %zu reports, ok %d", out.size(), out.empty() ? -1 : (int)out[0].crcOk);
        auto sy = s;
        sy[32 + 3] ^= 1; sy[32 + 17] ^= 1; sy[32 + 29] ^= 1;
        d = makeM10Decoder();
        out = feed(*d, sy, 4096);
        CHECK(out.size() == 1 && out[0].crcOk, "3 wrong sync symbols: %zu reports", out.size());
        sy[32 + 9] ^= 1; sy[32 + 22] ^= 1; sy[32 + 25] ^= 1;
        d = makeM10Decoder();
        out = feed(*d, sy, 4096);
        std::printf("m10: 6 wrong sync symbols: %zu reports (the sync tolerance is 4)\n", out.size());
    }
    // 4. reset() in the middle of a frame, then a whole frame
    {
        const SondeTruth t = truthN(1);
        const auto s = m10Symbols(t);
        auto d = makeM10Decoder();
        std::vector<SondeFix> out;
        d->push(s.data(), s.size() / 2, 0, out);
        d->reset();
        d->push(s.data(), s.size(), 1, out);
        CHECK(out.size() == 1 && out[0].crcOk, "after reset: %zu reports", out.size());
    }
    // 5. what must be ignored: no signal, an M20 frame, a type-0x49 frame, a frame with a wrong length byte
    {
        auto d = makeM10Decoder();
        auto out = feed(*d, randomSymbols(rng, 3000000), 65536);
        CHECK(countOk(out) == 0, "noise decoded as a frame");
        std::printf("m10: 3 million random symbols: %zu bad-frame reports\n", out.size());
        d = makeM10Decoder();
        out = feed(*d, m20Symbols(truthN(3)), 4096);
        CHECK(out.empty(), "an M20 frame gave %zu reports in the M10 decoder", out.size());
        auto f = m10FrameBytes(truthN(3));
        f[1] = 0x49; sealAt(f, 0x64);
        d = makeM10Decoder();
        out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.empty(), "type 0x49 frame gave %zu reports", out.size());
        f = m10FrameBytes(truthN(3)); f[0] = 0x20;
        d = makeM10Decoder();
        out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.empty(), "length byte 0x20 gave %zu reports", out.size());
    }
    // 6. no GPS fix (sub-id 0x23, as before the first fix): serial and sensors only
    {
        const SondeTruth t = truthN(4);
        auto f = m10FrameBytes(t);
        f[2] = 0x23; f[0x1D] = 1; sealAt(f, 0x64);
        auto d = makeM10Decoder();
        const auto out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.size() == 1 && out[0].crcOk && !out[0].hasPos && out[0].serial == t.serial && out[0].hasTemp, "no-fix frame: %zu reports", out.size());
    }
    // 7. aux frame (length 0x76: 18 more bytes before the checksum), and the 0x66 length of newer sondes
    for (int variant = 0; variant < 2; variant++) {
        const SondeTruth t = truthN(5);
        auto f = m10FrameBytes(t);
        const size_t flen = variant == 0 ? 0x76 : 0x66;
        const size_t extra = flen - 0x64;
        f.resize(flen + 1, 0);
        // the check bytes move to flen-1, flen; the bytes 0x63.. become aux data
        f[0] = (uint8_t)flen;
        for (size_t i = 0; i < extra; i++) f[0x63 + i] = (uint8_t)(0x40 + i);
        sealAt(f, flen);
        auto d = makeM10Decoder();
        const auto out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.size() == 1 && out[0].crcOk && out[0].hasPos && std::fabs(out[0].lat - t.lat) < 2e-7, "frame length 0x%zx: %zu reports", flen, out.size());
    }
    // 8. M10+ (Gtop GPS, type 0xAF): the layout of rs1729 m10m20mod.c, built by hand here: no real frame was available
    {
        std::vector<uint8_t> f(0x65, 0);
        f[0] = 0x64; f[1] = 0xAF;
        auto be32 = [&](size_t p, int32_t v) { f[p] = (uint8_t)(v >> 24); f[p + 1] = (uint8_t)(v >> 16); f[p + 2] = (uint8_t)(v >> 8); f[p + 3] = (uint8_t)v; };
        be32(0x04, 47123456); be32(0x08, -8765432);
        const int alt = -1250;                       // signed 24 bit, 0.01 m
        f[0x0C] = (uint8_t)(alt >> 16); f[0x0D] = (uint8_t)(alt >> 8); f[0x0E] = (uint8_t)alt;
        f[0x0F] = 0x01; f[0x10] = 0xF4;              // vE 5.00
        f[0x11] = 0xFE; f[0x12] = 0x0C;              // vN -5.00
        f[0x13] = 0x00; f[0x14] = 0x64;              // vU 1.00
        const int tm = 123012, dt = 150826;           // 12:30:12 and 15.08.26 as decimal digits
        f[0x15] = (uint8_t)(tm >> 16); f[0x16] = (uint8_t)(tm >> 8); f[0x17] = (uint8_t)tm;
        f[0x18] = (uint8_t)(dt >> 16); f[0x19] = (uint8_t)(dt >> 8); f[0x1A] = (uint8_t)dt;
        const uint8_t sn[5] = {0x02, 0x1a, 0x74, 0xcf, 0x4b};
        std::memcpy(&f[0x5D], sn, 5);
        sealAt(f, 0x64);
        auto d = makeM10Decoder();
        const auto out = feed(*d, bytesToSymbols(f), 4096);
        CHECK(out.size() == 1 && out[0].crcOk, "M10+: %zu reports", out.size());
        if (!out.empty()) {
            const auto& o = out[0];
            CHECK(o.subtype == "M10+" && o.serial == "704-2-23023", "M10+ serial %s", o.serial.c_str());
            CHECK(o.hasPos && std::fabs(o.lat - 47.123456) < 1e-9 && std::fabs(o.lon + 8.765432) < 1e-9 && std::fabs(o.altM + 12.5) < 1e-9, "M10+ position");
            CHECK(o.hasVel && std::fabs(o.hSpeed - 7.0711) < 1e-3 && std::fabs(o.vSpeed - 1.0) < 1e-9 && std::fabs(o.headingDeg - 135.0) < 1e-6, "M10+ velocity %.3f %.2f", o.hSpeed, o.headingDeg);
            CHECK(o.hasTime && std::fabs(o.unixTime - civilToUnix(2026, 8, 15, 12, 30, 12)) < 1e-6, "M10+ time");
        }
    }
    // 9. symbol error rate against frames received (every symbol error costs the frame: there is no error correction in the M10)
    {
        const SondeTruth t = truthN(0);
        const auto frame = m10Symbols(t);
        std::printf("m10: frames decoded out of 300 at symbol error rate p (expected (1-p)^%zu):\n", frame.size());
        const double ps[] = {0, 1e-5, 3e-5, 1e-4, 3e-4, 1e-3};
        for (double p : ps) {
            int good = 0;
            for (int k = 0; k < 300; k++) {
                auto s = frame;
                flipSymbols(rng, s, p);
                auto d = makeM10Decoder();
                good += countOk(feed(*d, s, 65536)) > 0;
            }
            const double expect = std::pow(1.0 - p, (double)frame.size());
            std::printf("  p=%-7g %3d  (%.0f%% expected)\n", p, good, 100.0 * expect);
            if (p == 0) CHECK(good == 300, "clean frames: %d / 300", good);
            if (p == 1e-4) CHECK(good >= (int)(300 * (expect - 0.12)), "p=1e-4: %d", good);
            if (p == 1e-3) CHECK(good >= (int)(300 * std::max(0.0, expect - 0.08)), "p=1e-3: %d", good);
        }
    }
    if (fails()) { std::printf("%d failures\n", fails()); return 1; }
    std::printf("sonde m10 roundtrip: ok\n");
    return 0;
}
