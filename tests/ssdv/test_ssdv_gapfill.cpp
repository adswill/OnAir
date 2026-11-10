// SSDV: packets dropped from a real SSDV stream made from a test picture, the gaps found and painted in. Simulated (ssdv encoder).
#include "dect2/ssdv/ssdv_proto.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2::ssdv;
#include "../common/test_util.h"

static double mse(const Rgb& a, const Rgb& b) { double s = 0; for (size_t i = 0; i < a.rgb.size(); i++) { const double d = (double)a.rgb[i] - b.rgb[i]; s += d * d; } return s / a.rgb.size(); }

int main() {
    const auto jpg = testJpeg(320, 240);
    const auto pk = encodePackets(jpg, "TEST", 1, 4, true);
    CHECK(pk.size() > 12, "packets %zu", pk.size());
    Rgb ref; { Assembler a; for (auto& p : pk) a.add(p.data()); const auto j = a.jpeg(); CHECK(decodeJpeg(j.data(), j.size(), ref), "reference"); CHECK(a.gapMcus().empty(), "no gap when complete"); }
    for (const std::set<int>& drop : {std::set<int>{5}, std::set<int>{3, 4, 9}, std::set<int>{(int)pk.size() / 2, (int)pk.size() / 2 + 2}}) {
        Assembler a; for (int i = 0; i < (int)pk.size(); i++) if (!drop.count(i)) a.add(pk[(size_t)i].data());
        Rgb im; const auto j = a.jpeg(); CHECK(decodeJpeg(j.data(), j.size(), im), "decode");
        const auto g = a.gapMcus();
        CHECK(!g.empty(), "gaps found");
        const double before = mse(im, ref);
        const int n = fillGaps(im, g);
        const double after = mse(im, ref);
        printf("%zu packets lost: %d MCUs filled, picture error %.0f -> %.0f\n", drop.size(), n, before, after);
        CHECK(n > 0, "filled"); CHECK(after < before * 0.7, "better: %.0f %.0f", before, after);
    }
    {   // a detailed MCU in a gap range stays
        Rgb im; im.w = 32; im.h = 32; im.rgb.assign(32 * 32 * 3, 128);
        for (int i = 0; i < 16; i++) im.rgb[(size_t)i * 3] = i % 2 ? 0 : 255;
        const auto keep = im.rgb[0];
        fillGaps(im, {{0, 0}});
        CHECK(im.rgb[0] == keep, "not flat: untouched");
    }
    return testSummary("ssdv_gapfill");
}
