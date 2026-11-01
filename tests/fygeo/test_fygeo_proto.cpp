// FengYun formats: the FY-4 file headers and names, JPEG 2000 round trip (12 bit, lossless), CADUs through transport stream packets (several PIDs,
// split anywhere), the S-VISSR PN preamble (ends with the sync window), scrambling, packing of the scan lines and the zero spare area.
#include "dect2/fygeo/fygeo_gen.h"
#include "dect2/fygeo/fygeo_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // ---- headers
    {
        fygeo::Fy4Headers h = fy4TestImageHeaders(3, 12, 1, "FY-4A");
        h.dataBits = 800;
        const std::vector<uint8_t> b = fygeo::buildFy4Headers(h);
        fygeo::Fy4Headers g;
        CHECK(fygeo::parseFy4Headers(b.data(), b.size(), g), "headers parse");
        CHECK(g.headerLength == b.size() && g.dataBits == 800 && g.hasImage && g.satName == "FY-4A" && g.instName == "AGRI", "fields %s %s", g.satName.c_str(), g.instName.c_str());
        CHECK(g.bpp == 12 && g.columns == kFy4TestW && g.lines == kFy4TestH && g.channel == 12 && g.totalSeg == kFy4TestSegs && g.segNo == 2 && g.segLine == kFy4TestH, "image fields");
        CHECK(g.hasNav && g.projection == "GEOS(104.7)" && g.cfac == h.cfac && g.loff == h.loff, "navigation");
        CHECK(g.annotation == h.annotation && g.hasTime && g.days == h.days && g.msOfDay == h.msOfDay, "annotation and time");
        CHECK(!g.hasKey, "no key");
        h.hasKey = true; h.key = 0x1234;
        const std::vector<uint8_t> k = fygeo::buildFy4Headers(h);
        CHECK(fygeo::parseFy4Headers(k.data(), k.size(), g) && g.hasKey && g.key == 0x1234, "key header");
        std::vector<uint8_t> cut(b.begin(), b.begin() + 20);
        CHECK(!fygeo::parseFy4Headers(cut.data(), cut.size(), g), "a cut header fails");
        const fygeo::Fy4Name n = fygeo::parseFy4Name(h.annotation);
        CHECK(n.ok && n.sat == "FY-4A" && n.region == "DISK" && n.instrument == "AGRI" && n.channel == "C012", "name %s %s", n.sat.c_str(), n.region.c_str());
        CHECK(!fygeo::parseFy4Name("something_else.txt").ok, "other names do not parse");
        CHECK(fygeo::fy4HeaderTime(h) > 1.7e9, "time stamp");
    }
    // ---- JPEG 2000
    {
        const std::vector<uint16_t> pic = fy4TestPicture(5);
        std::vector<uint8_t> cs = fygeo::j2kEncode(pic.data(), kFy4TestW, kFy4TestH, 12, 0);
        CHECK(cs.size() > 100 && cs[0] == 0xFF && cs[1] == 0x4F, "code stream %zu bytes", cs.size());
        int w, h, bpp;
        std::vector<uint16_t> px;
        CHECK(fygeo::j2kDecode(cs.data(), cs.size(), w, h, bpp, px), "decode");
        CHECK(w == kFy4TestW && h == kFy4TestH && bpp == 12, "size %d x %d, %d bit", w, h, bpp);
        CHECK(px.size() == (size_t)w * h && memcmp(px.data(), pic.data(), px.size() * 2) == 0, "lossless");
        std::vector<uint8_t> lossy = fygeo::j2kEncode(pic.data(), kFy4TestW, kFy4TestH, 12, 8);
        CHECK(lossy.size() < cs.size(), "lossy is smaller (%zu < %zu)", lossy.size(), cs.size());
        std::vector<uint8_t> dam = cs;
        dam.resize(dam.size() / 3);
        std::vector<uint16_t> p2;
        printf("  truncated code stream decodes: %d\n", (int)fygeo::j2kDecode(dam.data(), dam.size(), w, h, bpp, p2));
        std::vector<uint8_t> junk(500, 0x55);
        CHECK(!fygeo::j2kDecode(junk.data(), junk.size(), w, h, bpp, p2), "junk fails");
    }
    // ---- CADUs in transport stream packets
    {
        std::vector<uint8_t> ts;
        uint8_t cc3 = 0, cc9 = 0;
        std::vector<std::vector<uint8_t>> sent;
        for (int i = 0; i < 12; i++) {
            std::vector<uint8_t> c(1024);
            c[0] = 0x1A; c[1] = 0xCF; c[2] = 0xFC; c[3] = 0x1D;
            for (int k = 4; k < 1024; k++) c[(size_t)k] = (uint8_t)(k * 7 + i);
            sent.push_back(c);
            // two PIDs interleaved, plus a packet of another PID and a null packet in between
            if (i % 2) fygeo::caduToTs(c.data(), 3004, cc3, ts); else fygeo::caduToTs(c.data(), 3000, cc9, ts);
            uint8_t nul[188]; memset(nul, 0xFF, 188); nul[0] = 0x47; nul[1] = 0x1F; nul[2] = 0xFF; nul[3] = 0x10;
            ts.insert(ts.end(), nul, nul + 188);
        }
        fygeo::TsCaduExtractor ex;
        std::vector<std::vector<uint8_t>> got;
        std::vector<int> pids;
        // fed in odd sized pieces
        for (size_t o = 0; o < ts.size(); ) {
            const size_t n = std::min<size_t>(ts.size() - o, 188 * (1 + (o / 188) % 5));
            ex.push(ts.data() + o, n / 188, [&](const uint8_t* c, int pid) { got.emplace_back(c, c + 1024); pids.push_back(pid); });
            o += n;
        }
        // a CADU comes out when the next one of its PID starts, so the last of each PID is still inside
        CHECK(got.size() == 10, "%zu CADUs", got.size());
        for (size_t i = 0; i < got.size(); i++) CHECK(got[i] == sent[i], "CADU %zu content", i);
        CHECK(pids.size() >= 2 && pids[0] == 3000 && pids[1] == 3004, "PIDs found");
        CHECK(ex.pids().size() == 2, "PID list");
    }
    // ---- S-VISSR
    {
        const std::vector<uint8_t> pre = fygeo::svissrPreambleBits();
        CHECK(pre.size() == (size_t)fygeo::kSvissrPreamble, "preamble length");
        uint64_t w = 0;
        for (size_t i = pre.size() - 64; i < pre.size(); i++) w = w << 1 | pre[i];
        CHECK(w == fygeo::kSvissrMarker, "the preamble ends with the sync window");
        const auto& t = fygeo::svissrPnBytes();
        CHECK(t.size() == (size_t)fygeo::kSvissrFrameBytes, "table size");
        std::vector<uint8_t> ir[4], vis((size_t)4 * fygeo::kSvissrVisWidth), f((size_t)fygeo::kSvissrFrameBytes);
        for (auto& v : ir) v.resize(fygeo::kSvissrIrWidth);
        svissrTestLine(77, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        fygeo::svissrPack(f.data(), 1234, ir[0].data(), ir[1].data(), ir[2].data(), ir[3].data(), vis.data());
        CHECK(fygeo::svissrLineCounter(f.data()) == 1234, "line counter");
        int tot = 0;
        CHECK(fygeo::svissrSpareErrors(f.data(), &tot) == 0 && tot == 179 * 8, "spare is zero (%d bits)", tot);
        std::vector<uint8_t> o[4], ov((size_t)4 * fygeo::kSvissrVisWidth);
        for (auto& v : o) v.resize(fygeo::kSvissrIrWidth);
        fygeo::svissrUnpack(f.data(), o[0].data(), o[1].data(), o[2].data(), o[3].data(), ov.data());
        for (int k = 0; k < 4; k++) CHECK(o[k] == ir[k], "IR%d lines come back", k + 1);
        bool visOk = true;
        for (size_t i = 0; i < vis.size(); i++) visOk &= ov[i] == ((vis[i] & 0xFC) | 2);
        CHECK(visOk, "VIS lines come back (6 bit)");
        std::vector<uint8_t> s = f;
        fygeo::svissrScramble(s.data());
        CHECK(s != f, "scrambling changes the frame");
        fygeo::svissrDescramble(s.data());
        CHECK(s == f, "descrambling restores it");
        // the scrambled all-zero area looks like the PN sequence: not zero, not constant
        int ones = 0;
        std::vector<uint8_t> z((size_t)fygeo::kSvissrFrameBytes, 0);
        fygeo::svissrScramble(z.data());
        for (uint8_t b : z) ones += __builtin_popcount(b);
        CHECK(ones > fygeo::kSvissrFrameBits * 0.45 && ones < fygeo::kSvissrFrameBits * 0.55, "scrambled zeros are balanced (%d ones)", ones);
    }
    return testSummary("fygeo_proto");
}
