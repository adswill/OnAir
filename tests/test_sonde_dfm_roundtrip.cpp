// DFM-06, DFM-09, DFM-17: truth -> frames -> symbols -> decoder. Chunk sizes, polarity, bit errors the Hamming code must fix,
// symbol error sweep, reset, noise.
#include "data/sonde/bits/util.h"
#include <cstring>
using namespace sbt;

static SondeTruth baseTruth(int variant) {
    SondeTruth t;
    t.serial = variant == 6 ? "A1B2C3" : (variant == 9 ? "21245678" : "23038743");
    t.lat = 48.4321987; t.lon = 9.0123456; t.altM = 9876.54; t.vSpeed = -14.37; t.hSpeed = 23.45; t.headingDeg = 256.78;
    t.sats = 11; t.unixTime = 1767268800.0 + 17.25; t.tempC = -37.4; t.batteryV = 2.93;
    return t;
}

// n frames of a static sonde
static std::vector<uint8_t> stream(const SondeTruth& t0, int variant, int n, int firstFrame = 0) {
    std::vector<uint8_t> s;
    for (int i = 0; i < n; i++) {
        SondeTruth t = t0;
        t.frame = firstFrame + i;
        const auto f = dfmSymbols(t, variant);
        s.insert(s.end(), f.begin(), f.end());
    }
    return s;
}

static const char* wantSub(int variant) { return variant == 6 ? "DFM-06" : (variant == 9 ? "DFM-09" : "DFM-17"); }

static int checkAll(const std::vector<SondeFix>& out, const SondeTruth& t, int variant, const char* what, int minOk) {
    int ok = 0, withTemp = 0, withBat = 0;
    for (const auto& f : out) {
        if (!f.crcOk) continue;
        ok++;
        CHECK(f.type == "DFM" && f.subtype == wantSub(variant), "%s: type %s subtype %s", what, f.type.c_str(), f.subtype.c_str());
        CHECK(f.serial == t.serial || (variant == 6 && f.serial == t.serial), "%s: serial %s vs %s", what, f.serial.c_str(), t.serial.c_str());
        CHECK(f.hasPos && std::fabs(f.lat - t.lat) < 6e-8 && std::fabs(f.lon - t.lon) < 6e-8, "%s: lat/lon %.8f %.8f", what, f.lat, f.lon);
        CHECK(std::fabs(f.altM - t.altM) < 0.0051, "%s: alt %.3f vs %.3f", what, f.altM, t.altM);
        CHECK(f.hasVel && std::fabs(f.hSpeed - t.hSpeed) < 0.0051 && std::fabs(f.vSpeed - t.vSpeed) < 0.0051 && std::fabs(f.headingDeg - t.headingDeg) < 0.0051, "%s: velocity %.3f %.3f %.3f", what, f.hSpeed, f.vSpeed, f.headingDeg);
        CHECK(f.sats == t.sats, "%s: sats %d", what, f.sats);
        CHECK(f.hasTime && std::fabs(f.unixTime - t.unixTime) < 0.0011, "%s: time %.3f vs %.3f", what, f.unixTime, t.unixTime);
        // temperature and battery come from the configuration cycle: the first reports may be without them
        if (f.hasTemp) { withTemp++; CHECK(std::fabs(f.tempC - t.tempC) < 0.05, "%s: temp %.3f vs %.3f", what, f.tempC, t.tempC); }
        if (f.batteryV >= 0) { withBat++; CHECK(std::fabs(f.batteryV - t.batteryV) < 0.0011, "%s: battery %.3f", what, f.batteryV); }
        CHECK(f.frame == (int)(((int64_t)std::floor(t.unixTime)) & 0xFF), "%s: frame number %d", what, f.frame);
    }
    CHECK(ok >= minOk, "%s: %d reports, wanted %d", what, ok, minOk);
    CHECK(withTemp >= ok * 7 / 10, "%s: temperature in %d of %d reports", what, withTemp, ok);
    if (variant != 6) CHECK(withBat >= ok * 7 / 10, "%s: battery in %d of %d reports", what, withBat, ok);
    return ok;
}

int main() {
    Rng rng(9);
    // 1. 40 s of each variant, chunk sizes, polarity, a random start offset
    for (int variant : {6, 9, 17}) {
        const SondeTruth t = baseTruth(variant);
        for (size_t chunk : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) {
            for (int inv = 0; inv < 2; inv++) {
                std::vector<uint8_t> s = randomSymbols(rng, 100 + rng.range(0, 700));
                const auto body = stream(t, variant, 178);              // 178 frames = 39.9 s
                s.insert(s.end(), body.begin(), body.end());
                if (inv) invert(s);
                auto d = makeDfmDecoder();
                const auto out = feed(*d, s, chunk);
                char w[64]; std::snprintf(w, sizeof w, "DFM-%02d chunk %zu inv %d", variant, chunk, inv);
                // 4.5 frames per report; the serial number needs two configuration cycles (about 5 s)
                const int ok = checkAll(out, t, variant, w, 30);
                if (chunk == 4096 && inv == 0) std::printf("DFM-%02d: %d reports in 39.9 s\n", variant, ok);
            }
        }
    }
    // 2. the rule above on its own: the same DFM-09 stream with the polarity inverted is called DFM-17 when the number is 23000000 or more, DFM-09 below
    {
        for (int big = 0; big < 2; big++) {
            SondeTruth t = baseTruth(9);
            t.serial = big ? "23038743" : "21245678";
            auto s = stream(t, 9, 150);
            invert(s);
            auto d = makeDfmDecoder();
            const auto out = feed(*d, s, 4096);
            bool any = false;
            for (const auto& f : out) if (f.crcOk) { any = true; CHECK(f.subtype == (big ? "DFM-17" : "DFM-09"), "[rs] inverted sn %s: %s", t.serial.c_str(), f.subtype.c_str()); }
            CHECK(any, "reports for %s", t.serial.c_str());
        }
    }
    // 3. one wrong bit in several codewords of a block is corrected: flip the second symbol of one pair in many codewords, results unchanged
    {
        const SondeTruth t = baseTruth(17);
        auto s = stream(t, 17, 178);
        int flips = 0;
        for (size_t f = 0; f < 178; f++) {
            // codeword i of a block of L words has its bit j at frame bit offset(L*j + i): flip bit j = i % 8 of 6 codewords in each data block and in the config block
            for (int p = 0; p < 3; p++) {
                const int at[3] = {16, 72, 176}, L[3] = {7, 13, 13};
                for (int i = 0; i < std::min(L[p], 6); i++) {
                    const int bit = at[p] + L[p] * ((i + (int)f) % 8) + i;
                    s[f * 560 + 2 * (size_t)bit + 1] ^= 1;
                    flips++;
                }
            }
        }
        auto d = makeDfmDecoder();
        const auto out = feed(*d, s, 4096);
        int corr = 0;
        for (const auto& f : out) if (f.crcOk) corr += f.corrected;
        const int ok = countOk(out);
        std::printf("DFM-17: %d bit errors injected, %d reports, %d codewords corrected\n", flips, ok, corr);
        CHECK(ok == 0 || corr > 0, "corrections were not counted");
        // blocks with more than 4 corrected codewords are dropped (as rs1729 does for its JSON output): here 6 per block, so nothing comes out
        CHECK(ok == 0, "blocks with 6 corrections should be dropped, %d reports", ok);
    }
    {
        const SondeTruth t = baseTruth(9);
        auto s = stream(t, 9, 178);
        int flips = 0;
        for (size_t f = 0; f < 178; f++) {
            // data blocks: 3 codewords per block, one bit each, in different bit positions. The configuration block only in every 4th
            // frame: like rs1729, the configuration channels are taken from blocks without corrections
            for (int p = (f % 4 == 0 ? 0 : 1); p < 3; p++) {
                const int at[3] = {16, 72, 176}, L[3] = {7, 13, 13};
                const int nw = p == 0 ? 1 : 3;
                for (int i = 0; i < nw; i++) {
                    const int bit = at[p] + L[p] * ((i * 3 + (int)f) % 8) + i;
                    s[f * 560 + 2 * (size_t)bit + 1] ^= 1;
                    flips++;
                }
            }
        }
        auto d = makeDfmDecoder();
        const auto out = feed(*d, s, 4096);
        int corr = 0;
        for (const auto& f : out) if (f.crcOk) corr += f.corrected;
        std::printf("DFM-09: %d bit errors injected (3 per data block), %zu reports, %d codewords corrected\n", flips, out.size(), corr);
        checkAll(out, t, 9, "3 bit errors per data block", 25);
        CHECK(corr >= 100, "corrected %d", corr);
    }
    // 4. two wrong bits in one codeword cost the block (detected, not repaired). Like rs1729 a report needs the blocks 0, 1, 2, 3, 4 and 8 from
    // the last 6 frames, so a block lost in a cycle is replaced only by one of the cycle before that is not too old
    // 4b.: every fifth frame has one such block, no wrong value comes out
    {
        const SondeTruth t = baseTruth(9);
        auto s = stream(t, 9, 178);
        for (size_t f = 0; f < 178; f += 5) {
            const int L = 13, at = 72 + (int)(f % 2) * 104;
            for (int j : {1, 5}) s[f * 560 + 2 * (size_t)(at + L * j + 4) + 1] ^= 1;
        }
        auto d = makeDfmDecoder();
        const auto out = feed(*d, s, 4096);
        int bad = 0;
        for (const auto& f : out) bad += !f.crcOk;
        std::printf("DFM-09: a lost block in every fifth frame: %d reports, %d bad frames\n", countOk(out), bad);
        CHECK(bad >= 34 && bad <= 38, "bad frames %d", bad);
        CHECK(countOk(out) >= 15, "reports %d", countOk(out));
        for (const auto& f : out) if (f.crcOk) CHECK(std::fabs(f.lat - t.lat) < 6e-8, "wrong value in a report");
    }
    // 5. symbol error sweep
    {
        const SondeTruth t = baseTruth(17);
        const auto clean = stream(t, 17, 178);
        std::printf("DFM-17, 40 s, reports (all wrong-valued ones counted) at symbol error rate p:\n");
        for (double p : {0.0, 1e-3, 3e-3, 1e-2, 3e-2, 1e-1}) {
            int sumOk = 0, sumWrong = 0, runs = 8;
            for (int k = 0; k < runs; k++) {
                auto s = clean;
                flipSymbols(rng, s, p);
                auto d = makeDfmDecoder();
                const auto out = feed(*d, s, 65536);
                for (const auto& f : out) {
                    if (!f.crcOk) continue;
                    sumOk++;
                    if (f.hasPos && (std::fabs(f.lat - t.lat) > 1e-6 || std::fabs(f.lon - t.lon) > 1e-6 || std::fabs(f.altM - t.altM) > 0.1)) sumWrong++;
                }
            }
            std::printf("  p=%-6g reports %5.1f per run, wrong %d\n", p, sumOk / (double)runs, sumWrong);
            if (p == 0) CHECK(sumOk >= runs * 30, "clean: %d", sumOk);
            if (p <= 1e-3 && p > 0) CHECK(sumOk >= runs * 28 && sumWrong == 0, "p=%g: ok %d wrong %d", p, sumOk, sumWrong);
            if (p <= 1e-2 && p > 1e-3) CHECK(sumWrong <= 1, "p=%g: wrong %d", p, sumWrong);
        }
    }
    // 6. reset() in the middle: the decoder forgets the serial number and learns it again
    {
        const SondeTruth t = baseTruth(9);
        const auto s = stream(t, 9, 300);
        auto d = makeDfmDecoder();
        std::vector<SondeFix> out;
        const size_t half = 560 * 100;
        d->push(s.data(), half, 0, out);
        const int before = countOk(out);
        d->reset();
        d->push(s.data() + half, s.size() - half, 22.4, out);
        CHECK(before >= 5 && countOk(out) - before >= 20, "reset: %d before, %d after", before, countOk(out) - before);
        checkAll(out, t, 9, "reset", 25);
    }
    // 6b. a gap of 20 ms (50 symbols) and a single lost symbol: those frames are lost, the decoder finds the next header, nothing wrong comes out
    {
        const SondeTruth t = baseTruth(9);
        auto s = stream(t, 9, 178);
        s.erase(s.begin() + 560 * 100 + 200, s.begin() + 560 * 100 + 250);
        s.erase(s.begin() + 560 * 140 + 333);
        auto d = makeDfmDecoder();
        const auto out = feed(*d, s, 4096);
        int bad = 0;
        for (const auto& f : out) bad += !f.crcOk;
        std::printf("DFM-09: gap and slip: %d reports, %d bad frames\n", countOk(out), bad);
        checkAll(out, t, 9, "gap", 28);
    }
    // 7. no signal
    {
        auto d = makeDfmDecoder();
        const auto out = feed(*d, randomSymbols(rng, 3000000), 65536);
        CHECK(countOk(out) == 0, "noise decoded");
        std::printf("dfm: 3 million random symbols: %zu bad-frame reports\n", out.size());
    }
    // 8. a moving sonde: reports follow it
    {
        auto d = makeDfmDecoder();
        std::vector<uint8_t> s;
        SondeTruth t = baseTruth(17);
        std::vector<SondeTruth> ts;
        for (int i = 0; i < 100; i++) {
            // the time stays the same within a second of reports; 22 s: no minute change (the minute comes from another block than the seconds)
            t.frame = i; t.unixTime = 1767268800.0 + 17.0 + std::floor(i * 0.224); t.altM = 20000.0 - 4.0 * (i * 0.224); t.lat += 0.0002; t.lon += 0.0003;
            ts.push_back(t);
            const auto f = dfmSymbols(t, 17);
            s.insert(s.end(), f.begin(), f.end());
        }
        const auto out = feed(*d, s, 4096);
        double lastT = 0; int n = 0;
        for (const auto& f : out) {
            if (!f.crcOk) continue;
            n++;
            CHECK(f.unixTime >= lastT, "time goes forward");
            lastT = f.unixTime;
            // the position of the report belongs to some frame within the last 6 frames before its id-8 block
            CHECK(f.altM < 20000.0 + 0.1 && f.altM > 20000.0 - 4.0 * 23.0, "altitude %.1f", f.altM);
            CHECK(f.lat > 48.4321987 - 1e-6 && f.lat < 48.4321987 + 0.0002 * 101, "latitude %.5f", f.lat);
        }
        CHECK(n >= 10, "moving sonde: %d reports in 22 s", n);
    }
    if (fails()) { std::printf("%d failures\n", fails()); return 1; }
    std::printf("sonde dfm roundtrip: ok\n");
    return 0;
}
