// Weather fax picture rotation: each turn on a small asymmetric picture, four turns = identity, opposite turns cancel, sizes, odd shapes.
#include "dect2/marine/wefax_rotate.h"
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> turn(const std::vector<uint8_t>& p, int w, int h, int t, int& ow, int& oh) {
    std::vector<uint8_t> o;
    wefaxRotate(p.data(), w, h, t, o, ow, oh);
    return o;
}

int main() {
    // 3 x 2:   1 2 3
    //          4 5 6
    const std::vector<uint8_t> p = {1, 2, 3, 4, 5, 6};
    int ow, oh;
    auto cw = turn(p, 3, 2, 1, ow, oh);
    CHECK(ow == 2 && oh == 3 && cw == (std::vector<uint8_t>{4, 1, 5, 2, 6, 3}), "90 cw");
    auto r180 = turn(p, 3, 2, 2, ow, oh);
    CHECK(ow == 3 && oh == 2 && r180 == (std::vector<uint8_t>{6, 5, 4, 3, 2, 1}), "180");
    auto ccw = turn(p, 3, 2, 3, ow, oh);
    CHECK(ow == 2 && oh == 3 && ccw == (std::vector<uint8_t>{3, 6, 2, 5, 1, 4}), "90 ccw");
    CHECK(turn(p, 3, 2, 0, ow, oh) == p && ow == 3 && oh == 2, "0 turns");
    CHECK(turn(p, 3, 2, -1, ow, oh) == ccw, "-1 turn is ccw");
    CHECK(turn(p, 3, 2, 5, ow, oh) == cw, "5 turns wrap");
    // four turns come back; a turn and its opposite cancel; two 90 = 180
    std::vector<uint8_t> q = p; int w = 3, h = 2;
    for (int i = 0; i < 4; i++) { int nw, nh; q = turn(q, w, h, 1, nw, nh); w = nw; h = nh; }
    CHECK(q == p && w == 3 && h == 2, "4 x 90 cw");
    int w1, h1, w2, h2;
    auto a = turn(p, 3, 2, 1, w1, h1);
    auto b = turn(a, w1, h1, 3, w2, h2);
    CHECK(b == p && w2 == 3 && h2 == 2, "cw then ccw");
    auto c = turn(a, w1, h1, 1, w2, h2);
    CHECK(c == r180, "two 90 = 180");
    // a tall fax-like picture with a marker at the first pixel of the last row
    const int W = 1204, H = 37;
    std::vector<uint8_t> big((size_t)W * H, 0);
    big[(size_t)(H - 1) * W] = 255;
    auto rb = turn(big, W, H, 1, ow, oh);
    CHECK(ow == H && oh == W && rb[0] == 255, "marker after 90 cw");
    int cnt = 0; for (uint8_t v : rb) cnt += v == 255;
    CHECK(cnt == 1, "pixels kept");
    // 32-bit pixels and empty input
    std::vector<uint32_t> px = {0xFF000001u, 0xFF000002u}, po;
    wefaxRotate(px.data(), 2, 1, 1, po, ow, oh);
    CHECK(ow == 1 && oh == 2 && po[0] == 0xFF000001u && po[1] == 0xFF000002u, "32-bit");
    // the fit zoom: a 1809 x 165 strip and a tall 1809 x 3000 picture in an 800 x 600 box, all four turns: turned 90 degrees the whole picture is inside the box
    for (int turns = 0; turns < 4; turns++) for (int hh : {165, 3000}) {
        int ow, oh;
        wefaxRotatedSize(1809, hh, turns, ow, oh);
        const float z = wefaxFitZoom(800, 600, ow, oh, turns);
        if (turns & 1) CHECK(ow * z <= 800.01f && oh * z <= 600.01f && (ow * z > 799.f || oh * z > 599.f), "turn %d 1809x%d: fit %.3f shows %.0f x %.0f of 800 x 600", turns, hh, z, ow * z, oh * z);
        else CHECK(std::fabs(z - 800.f / ow) < 1e-6, "turn %d 1809x%d: as received it is fitted by width (%.3f)", turns, hh, z);
    }
    CHECK(wefaxFitZoom(0, 0, 0, 0, 1) == 1.f, "degenerate box");
    wefaxRotate<uint8_t>(nullptr, 0, 0, 1, a, ow, oh);
    CHECK(a.empty(), "empty");
    return testSummary("marine_wefax_rotate");
}
