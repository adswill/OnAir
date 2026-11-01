// FY-3 AHRPT / MPT layers: the differential coder, the rail code, CADUs, VIRR frames, the bit stream framer and the MERSI reader, both ways.
#include "dect2/fy3/fy3_proto.h"
#include "dect2/hrpt/hrpt_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static void testDiff() {
    std::mt19937 rng(5);
    // encoder and decoder are inverses, for any starting state
    for (int st = 0; st < 4; st++) {
        fy3::DiffEncoder e; e.xp = st & 1; e.yp = st >> 1;
        fy3::DiffDecoder d; d.xp = st & 1; d.yp = st >> 1;
        int bad = 0;
        std::vector<int> xs, ys, his, los;
        for (int i = 0; i < 4000; i++) {
            const int hi = (int)(rng() & 1), lo = (int)(rng() & 1);
            int x, y;
            e.push(hi, lo, x, y);
            xs.push_back(x); ys.push_back(y); his.push_back(hi); los.push_back(lo);
            int h2, l2;
            d.push(x, y, h2, l2);
            if (h2 != hi || l2 != lo) bad++;
        }
        CHECK(bad == 0, "diff roundtrip start %d: %d wrong", st, bad);
        // the QPSK phase ambiguity: the rails after a rotation by 90 degrees are (not y, x) etc.; the output is the same after the first pair
        for (int rot = 1; rot < 4; rot++) {
            fy3::DiffDecoder dd;
            int diffs = 0;
            for (size_t i = 0; i < xs.size(); i++) {
                int x = xs[i], y = ys[i];
                for (int k = 0; k < rot; k++) { const int nx = y ^ 1, ny = x; x = nx; y = ny; }   // (I, Q) -> (-Q, I) with the bit 1 meaning +
                int h2, l2;
                dd.push(x, y, h2, l2);
                if (i > 0 && (h2 != his[i] || l2 != los[i])) diffs++;
            }
            CHECK(diffs == 0, "diff rotation %d: %d wrong", rot, diffs);
        }
    }
}

static void testRailCode() {
    std::mt19937 rng(11);
    std::vector<uint8_t> bits(6000);
    for (auto& b : bits) b = (uint8_t)(rng() & 1);
    hrpt::CcsdsEncoder enc;
    fy3::RailViterbi v;
    for (uint8_t b : bits) {
        uint8_t a, c;
        enc.push(b, a, c);
        // G2 is inverted in the code; the decoder expects the pair with the second symbol negated (transform 4 of pairXform)
        int oa, ob;
        fy3::pairXform(4, a ? 60 : -60, c ? 60 : -60, oa, ob);
        v.push(oa, ob);
    }
    v.flush();
    int bad = 0;
    const auto& o = v.out();
    CHECK(o.size() >= bits.size() - 8, "viterbi out %zu of %zu", o.size(), bits.size());
    for (size_t i = 0; i < std::min(o.size(), bits.size()); i++) if (o[i] != bits[i]) bad++;
    CHECK(bad == 0, "rail viterbi: %d wrong bits", bad);
    // the score of the right pairing is far above a wrong one
    auto score = [&](int align, int t) {
        hrpt::CcsdsEncoder e2; fy3::RailViterbi w;
        std::vector<int> s;
        for (uint8_t b : bits) { uint8_t a, c; e2.push(b, a, c); s.push_back(a ? 60 : -60); s.push_back(c ? 60 : -60); }
        for (size_t k = (size_t)align; k + 1 < s.size(); k += 2) { int oa, ob; fy3::pairXform(t, s[k], s[k + 1], oa, ob); w.push(oa, ob); w.out().clear(); }
        return w.score() / (double)(w.steps() * 120);
    };
    const double good = score(0, 4), wrongAlign = score(1, 4), wrongT = score(0, 0);
    printf("  path metric: right %.2f, wrong alignment %.2f, wrong polarity %.2f\n", good, wrongAlign, wrongT);
    CHECK(good > 0.95 && wrongAlign < 0.85 && wrongT < 0.85, "metrics");
}

static void testCadu() {
    fy3::Cadu c;
    c.scid = 51; c.vcid = 5; c.counter = 123456; c.fhp = 0x7FF;
    for (int i = 0; i < 882; i++) c.data[i] = (uint8_t)(i * 7 + 1);
    uint8_t cadu[1024];
    fy3::encodeVcdu(c, cadu);
    CHECK(cadu[0] == 0x1A && cadu[1] == 0xCF && cadu[2] == 0xFC && cadu[3] == 0x1D, "ASM");
    cadu[100] ^= 0xFF; cadu[500] ^= 0x5A; cadu[900] ^= 0x01;   // errors RS corrects
    fy3::Cadu d;
    CHECK(fy3::decodeCadu(cadu + 4, d), "decode");
    CHECK(d.scid == 51 && d.vcid == 5 && d.counter == 123456 && std::memcmp(d.data, c.data, 882) == 0, "fields %d %d %u", d.scid, d.vcid, d.counter);
    CHECK(d.rsErrors == 3, "RS errors %d", d.rsErrors);
    for (int i = 0; i < 60; i++) cadu[200 + i] ^= 0xFF;   // too many in one codeword
    CHECK(!fy3::decodeCadu(cadu + 4, d), "uncorrectable");
}

static void testVirr() {
    fy3::VirrLine l;
    l.days = 3123; l.ms = 41234567;
    for (int c = 0; c < fy3::kVirrChannels; c++)
        for (int x = 0; x < fy3::kVirrWidth; x++) l.px[(size_t)c * fy3::kVirrWidth + (size_t)x] = fy3::testPixel(c, x, 17);
    std::vector<uint8_t> f(fy3::kVirrFrameBytes + 8);
    fy3::encodeVirrFrame(l, f.data());
    fy3::VirrLine o;
    CHECK(fy3::decodeVirrFrame(f.data(), fy3::kVirrFrameBytes, o), "decode");
    CHECK(o.days == l.days && o.ms == l.ms, "time %u %u", o.days, o.ms);
    CHECK(o.px == l.px, "pixels");
    // through the framer, in 882 byte pieces, three frames, with a lost piece in the middle of the second
    std::vector<uint8_t> stream;
    for (int k = 0; k < 3; k++) { l.ms += 167; fy3::encodeVirrFrame(l, f.data()); stream.insert(stream.end(), f.begin(), f.begin() + fy3::kVirrFrameBytes); }
    fy3::FrameSync fs(fy3::kVirrSync, 60, fy3::kVirrFrameBytes * 8);
    int frames = 0, good = 0;
    uint32_t lastMs = 0;
    for (size_t at = 0; at < stream.size(); at += 882) {
        const size_t n = std::min<size_t>(882, stream.size() - at);
        if (at / 882 == 40) { fs.gap(882); continue; }   // the second frame loses a piece
        fs.push(&stream[at], n, [&](const std::vector<uint8_t>& fr) {
            frames++;
            fy3::VirrLine q;
            if (fy3::decodeVirrFrame(fr.data(), fr.size(), q)) { if (q.ms > lastMs) good++; lastMs = q.ms; }
        });
    }
    printf("  framer: %d frames, %d decoded in time order\n", frames, good);
    CHECK(frames >= 2, "frames %d", frames);
}

static void testMersi() {
    for (int inst : {fy3::Mersi1, fy3::MersiRM}) {
        const fy3::MersiCfg& c = fy3::mersiCfg(inst);
        fy3::BitWriter bw;
        const double t0 = 1.7e9 + 12345.0;
        auto pix = [](int ch, int x, int y) { return (uint16_t)(((ch * 311 + x * 7 + y * 13) & 0xFFF)); };
        fy3::encodeMersiSegment(inst, 0, t0, pix, bw);
        fy3::encodeMersiSegment(inst, 1, t0 + 1.5, pix, bw);
        while (bw.nbit) bw.put(0, 1);
        fy3::MersiReader r(inst);
        std::vector<fy3::MersiSegment> segs;
        r.onSegment = [&](const fy3::MersiSegment& s) { segs.push_back(s); };
        // in pieces of 882 bytes as the VCDUs carry them
        for (size_t at = 0; at < bw.bytes.size(); at += 882) r.push(&bw.bytes[at], std::min<size_t>(882, bw.bytes.size() - at));
        r.flush();
        printf("  MERSI %d: %zu segments, %llu lines\n", inst, segs.size(), (unsigned long long)r.linesTotal);
        CHECK(segs.size() == 2, "segments %zu", segs.size());
        if (segs.size() != 2) continue;
        CHECK(std::fabs(segs[0].time - t0) < 0.01 && std::fabs(segs[1].time - (t0 + 1.5)) < 0.01, "times %.3f %.3f", segs[0].time - t0, segs[1].time - t0);
        const int w = c.width250 / 4, nch = c.ch250 + c.ch1000;
        CHECK(segs[0].lines == c.ch250 * 40 + c.ch1000 * 10, "lines %d", segs[0].lines);
        // a 1 km channel pixel is the 12 bit value's top 8 bits
        int bad = 0;
        for (int row = 0; row < 10; row += 3)
            for (int ch = c.ch250; ch < nch; ch += 5)
                for (int x = 0; x < w; x += 97) {
                    const int want = pix(ch, x, 10 * 1 + row) >> 4;   // segment 1
                    if (segs[1].px[((size_t)row * (size_t)nch + (size_t)ch) * (size_t)w + (size_t)x] != want) bad++;
                }
        CHECK(bad == 0, "1 km pixels: %d wrong", bad);
        // a 250 m channel pixel is the mean of 16
        bad = 0;
        for (int ch = 0; ch < c.ch250; ch++)
            for (int row = 0; row < 10; row += 4)
                for (int x = 0; x < w; x += 211) {
                    int sum = 0;
                    for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) sum += pix(ch, 4 * x + dx, 40 * 1 + 4 * row + dy);
                    const int want = sum / 16 >> 4;
                    const int got = segs[1].px[((size_t)row * (size_t)nch + (size_t)ch) * (size_t)w + (size_t)x];
                    if (std::abs(got - want) > 1) bad++;
                }
        CHECK(bad == 0, "250 m pixels: %d wrong", bad);
    }
}

static void testMwri() {
    for (int inst : {fy3::Mwri, fy3::Mwri2}) {
        const fy3::MwriCfg& c = fy3::mwriCfg(inst);
        fy3::MwriLine l;
        l.time = 1.7e9 + 12345.0 + 0.016 * 7;
        l.px.resize((size_t)c.channels * (size_t)c.width);
        for (size_t i = 0; i < l.px.size(); i++) l.px[i] = (uint16_t)(i * 2654435761u >> 13);
        std::vector<uint8_t> f;
        fy3::encodeMwriFrame(inst, l, f);
        fy3::MwriLine o;
        CHECK(fy3::decodeMwriFrame(inst, f.data(), f.size(), o), "mwri decode");
        CHECK(o.px == l.px, "mwri %d pixels", inst);
        CHECK(std::fabs(o.time - l.time) < 0.02, "mwri %d time %.3f vs %.3f", inst, o.time - 1.7e9, l.time - 1.7e9);
        // through the framer: two frames back to back, in 882 byte pieces
        std::vector<uint8_t> s = f;
        s.insert(s.end(), f.begin(), f.end());
        fy3::FrameSync fs(c.sync, c.syncBits, c.frameBytes * 8);
        int frames = 0;
        for (size_t at = 0; at < s.size(); at += 882) fs.push(&s[at], std::min<size_t>(882, s.size() - at), [&](const std::vector<uint8_t>& fr) { fy3::MwriLine q; if (fy3::decodeMwriFrame(inst, fr.data(), fr.size(), q) && q.px == l.px) frames++; });
        CHECK(frames >= 1, "mwri %d framer %d", inst, frames);
    }
}

static void testTables() {
    int n; const fy3::Sat* s = fy3::sats(n);
    CHECK(n == 8 && s[2].scid == 51 && s[2].norad == 39260, "sats");
    CHECK(fy3::satIndexByScid(52) == 3 && fy3::satIndexByScid(1) == -1, "scid");
    CHECK(fy3::instForVcid(51, 5, 0) == fy3::Virr && fy3::instForVcid(51, 3, 1) == fy3::Mersi1 && fy3::instForVcid(52, 3, 0) == fy3::Mersi2 && fy3::instForVcid(52, 10, 0) == fy3::Mwri, "instruments");
    CHECK(fy3::instChannels(fy3::Virr) == 10 && fy3::instChannels(fy3::Mersi2) == 25 && fy3::instWidth(fy3::Mersi1) == 2048, "channel counts");
}

int main() {
    testDiff();
    testRailCode();
    testCadu();
    testVirr();
    testMersi();
    testMwri();
    testTables();
    return testSummary("fy3 proto");
}
