// The SSTV mode table, shared by the decoder (hfdig_sstv.cpp) and the test audio (hfdig_sstv_gen.cpp).
// Timings are the published ones: J. L. Barber (N7CXI), "Proposal for SSTV Mode Specifications", Dayton SSTV forum, 2000 (Martin, Scottie,
// Robot, PD, VIS codes), cross-checked against the line times in the documentation of MMSSTV and slowrx. Wraase SC2-180 as in slowrx.
// Tones: sync 1200 Hz, black 1500 Hz, white 2300 Hz, porches and the Robot chroma separators as described per mode.
#pragma once
#include "dect2/hfdig_sstv.h"
#include <vector>

namespace dect2 {
namespace sstv {

enum class Kind { Seq, Robot36, Robot72, PD };
enum Comp { kR = 0, kG, kB, kY, kRY, kBY, kY2 };   // what a scan carries (kY2: the second luma of a PD line)

struct Scan { double startMs, lenMs; int px; Comp comp; };
struct Fixed { double startMs, lenMs, hz; };       // a stretch of constant tone other than the sync (hz < 0: Robot 36 separator, 1500 on even lines, 2300 on odd)

struct Spec {
    SstvModeInfo info;
    Kind kind;
    double syncOffMs;                // from the start of the line (the first thing sent) to the start of the sync pulse; Scottie sends its sync in the middle
    int totalLines;                  // transmitted lines
    std::vector<Scan> scans;
    std::vector<Fixed> fixed;
};

inline std::vector<Spec> buildTable() {
    std::vector<Spec> t;
    // Robot 36: sync 9, porch 3 (1500), Y 88 ms, separator 4.5 (1500 even / 2300 odd), porch 1.5 (1900), chroma 44 ms (R-Y on even, B-Y on odd lines)
    t.push_back({{8, "Robot 36", 320, 240, 150.0, 9.0}, Kind::Robot36, 0, 240,
                 {{12, 88, 320, kY}, {106, 44, 160, kRY}}, {{100, 4.5, -1}, {104.5, 1.5, 1900}}});
    // Robot 72: sync 9, porch 3, Y 138 ms, separator 4.5 (1500), porch 1.5 (1900), V 69 ms, separator 4.5 (2300), porch 1.5 (1900), U 69 ms
    t.push_back({{12, "Robot 72", 320, 240, 300.0, 9.0}, Kind::Robot72, 0, 240,
                 {{12, 138, 320, kY}, {156, 69, 160, kRY}, {231, 69, 160, kBY}},
                 {{150, 4.5, 1500}, {154.5, 1.5, 1900}, {225, 4.5, 2300}, {229.5, 1.5, 1900}}});
    // Martin: sync 4.862, porch 0.572 (1500), then green, blue, red, each followed by 0.572 of 1500
    auto martin = [&](int vis, const char* name, double scan) {
        const double a = 4.862 + 0.572, step = scan + 0.572;
        t.push_back({{vis, name, 320, 256, a + 3 * step, 4.862}, Kind::Seq, 0, 256,
                     {{a, scan, 320, kG}, {a + step, scan, 320, kB}, {a + 2 * step, scan, 320, kR}}, {}});
    };
    martin(44, "Martin M1", 146.432);
    martin(40, "Martin M2", 73.216);
    // Scottie: separator 1.5 (1500), green, separator 1.5, blue, SYNC 9, porch 1.5 (1500), red
    auto scottie = [&](int vis, const char* name, double scan) {
        t.push_back({{vis, name, 320, 256, 3 * scan + 13.5, 9.0}, Kind::Seq, 2 * scan + 3.0, 256,
                     {{1.5, scan, 320, kG}, {scan + 3.0, scan, 320, kB}, {2 * scan + 13.5, scan, 320, kR}}, {}});
    };
    scottie(60, "Scottie S1", 138.240);
    scottie(56, "Scottie S2", 88.064);
    scottie(76, "Scottie DX", 345.600);
    // PD: sync 20, porch 2.08 (1500), then Y of the odd row, R-Y, B-Y, Y of the even row, each T long; two picture rows per line
    auto pd = [&](int vis, const char* name, int w, int h, double scan) {
        const double a = 20.0 + 2.08;
        t.push_back({{vis, name, w, h, a + 4 * scan, 20.0}, Kind::PD, 0, h / 2,
                     {{a, scan, w, kY}, {a + scan, scan, w, kRY}, {a + 2 * scan, scan, w, kBY}, {a + 3 * scan, scan, w, kY2}}, {}});
    };
    pd(93, "PD50", 320, 256, 91.520);
    pd(99, "PD90", 320, 256, 170.240);
    pd(95, "PD120", 640, 496, 121.600);
    pd(97, "PD180", 640, 496, 183.040);
    // Wraase SC2-180: sync 5.5225, porch 0.5 (1500), red, green, blue 235 ms each
    t.push_back({{55, "Wraase SC2-180", 320, 256, 5.5225 + 0.5 + 3 * 235.0, 5.5225}, Kind::Seq, 0, 256,
                 {{6.0225, 235, 320, kR}, {241.0225, 235, 320, kG}, {476.0225, 235, 320, kB}}, {}});
    return t;
}

inline const std::vector<Spec>& table() {
    static const std::vector<Spec> t = buildTable();
    return t;
}

// colour of a pixel from luma and the two colour differences (0 .. 255, 128 = none), as slowrx and the ITU based SSTV modes do
inline uint8_t clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5); }
inline void yuvToRgb(double y, double ry, double by, uint8_t* o) {
    o[0] = clamp8(y + 1.4 * (ry - 128));
    o[1] = clamp8(y - 0.71 * (ry - 128) - 0.33 * (by - 128));
    o[2] = clamp8(y + 1.78 * (by - 128));
}
inline void rgbToYuv(double r, double g, double b, double& y, double& ry, double& by) {
    y = 0.299 * r + 0.587 * g + 0.114 * b;
    ry = 128 + (r - y) / 1.4;
    by = 128 + (b - y) / 1.78;
    ry = ry < 0 ? 0 : ry > 255 ? 255 : ry;
    by = by < 0 ? 0 : by > 255 ? 255 : by;
}

} // namespace sstv
} // namespace dect2
