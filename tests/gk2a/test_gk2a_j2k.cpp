// JPEG 2000 decoder and DES (the GK-2A session layer): ffmpeg-made codestreams (lossless, tiled RPCL, 9/7), the encoder -> decoder round trip
// over odd sizes, precisions and levels, damaged streams, the FIPS 46-3 DES vector.
#include "dect2/common/des.h"
#include "dect2/image/j2k.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include "j2k_fixtures.inc"
#include "../common/test_util.h"
using namespace dect2;

static std::vector<uint16_t> fixturePic(int w, int h) {
    std::vector<uint16_t> v;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) v.push_back((uint16_t)((x * 7 + y * 13 + ((x * y) % 17) * 3 + (x * x + y) % 5) & 1023));
    return v;
}

int main() {
    {   // DES: the FIPS 46-3 / Stallings example, and ECB round trip
        const uint8_t k[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
        Des d; d.setKey(k);
        CHECK(d.block(0x0123456789ABCDEFull, false) == 0x85E813540F0AB405ull, "DES encrypt vector");
        CHECK(d.block(0x85E813540F0AB405ull, true) == 0x0123456789ABCDEFull, "DES decrypt vector");
        uint8_t buf[21]; for (int i = 0; i < 21; i++) buf[i] = (uint8_t)(i * 11);
        uint8_t ref[21]; memcpy(ref, buf, 21);
        d.ecb(buf, 21, false);
        CHECK(memcmp(buf, ref, 16) != 0 && memcmp(buf + 16, ref + 16, 5) == 0, "ECB leaves a short tail");
        d.ecb(buf, 21, true);
        CHECK(memcmp(buf, ref, 21) == 0, "ECB round trip");
    }
    {   // codestreams from another encoder
        auto a = j2k::decode(fx_a, sizeof fx_a);
        CHECK(a.ok && a.width == 48 && a.height == 24 && a.reversible && a.pix == fixturePic(48, 24), "ffmpeg lossless in a JP2 file: ok=%d %s", a.ok, a.error.c_str());
        auto b = j2k::decode(fx_b, sizeof fx_b);
        CHECK(b.ok && b.width == 100 && b.height == 40 && b.pix == fixturePic(100, 40), "ffmpeg tiled RPCL: ok=%d %s", b.ok, b.error.c_str());
        auto c = j2k::decode(fx_c, sizeof fx_c);
        const auto ref = fixturePic(40, 20);
        int worst = 0;
        for (size_t i = 0; i < ref.size() && c.pix.size() == ref.size(); i++) worst = std::max(worst, std::abs((int)c.pix[i] - (int)ref[i]));
        CHECK(c.ok && !c.reversible && c.pix.size() == ref.size() && worst < 40, "ffmpeg 9/7: ok=%d worst error %d %s", c.ok, worst, c.error.c_str());
    }
    {   // our encoder -> our decoder, many shapes
        std::mt19937 rng(11);
        int bad = 0;
        for (int t = 0; t < 30; t++) {
            const int w = 1 + (int)(rng() % 260), h = 1 + (int)(rng() % 150), prec = 8 + (int)(rng() % 9), lev = (int)(rng() % 6);
            std::vector<uint16_t> p((size_t)w * h);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int v = (int)((1 << prec) / 2 + (1 << (prec - 2)) * std::sin(x * 0.1) * std::cos(y * 0.07)) + (int)(rng() % (1u << (prec > 10 ? 6 : 3)));
                if (t % 7 == 0) v = (int)(rng() % (1u << prec));
                p[(size_t)y * w + x] = (uint16_t)std::max(0, std::min((1 << prec) - 1, v));
            }
            const auto cs = j2k::encodeLossless(p.data(), w, h, prec, lev);
            const auto im = j2k::decode(cs.data(), cs.size());
            if (cs.empty() || !im.ok || im.pix != p) { bad++; printf("  round trip %dx%d %d bit %d levels failed (%s)\n", w, h, prec, lev, im.error.c_str()); }
        }
        CHECK(bad == 0, "%d round trips failed", bad);
    }
    {   // damaged streams must not crash and must say so
        std::vector<uint16_t> p(64 * 40);
        for (size_t i = 0; i < p.size(); i++) p[i] = (uint16_t)((i * 37) & 1023);
        auto cs = j2k::encodeLossless(p.data(), 64, 40, 10, 3);
        auto cut = std::vector<uint8_t>(cs.begin(), cs.begin() + (std::ptrdiff_t)(cs.size() / 2));
        auto im = j2k::decode(cut.data(), cut.size());
        CHECK(!im.ok && !im.error.empty(), "truncated stream reported");
        std::mt19937 rng(3);
        for (int i = 0; i < 200; i++) { auto m = cs; for (int k = 0; k < 6; k++) m[rng() % m.size()] ^= (uint8_t)(1 + rng() % 255); (void)j2k::decode(m.data(), m.size()); }
        const uint8_t junk[16] = {1, 2, 3};
        CHECK(!j2k::decode(junk, sizeof junk).ok, "junk is not a picture");
    }
    return testSummary("gk2a j2k");
}
