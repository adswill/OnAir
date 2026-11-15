// The DRM channel box in the spectrum follows the Channel filter choice (app/drm/drm_filter_logic.h).
#include "../../app/drm/drm_filter_logic.h"
#include <cmath>
#include <cstdio>

static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main() {
    CHECK(std::fabs(drmfilter::boxWidthMhz(1, 0) - 0.010) < 1e-12);      // manual 10 kHz gives a 10 kHz box, also before anything is found
    CHECK(std::fabs(drmfilter::boxWidthMhz(1, 20) - 0.010) < 1e-12);     // and whatever the receiver found
    CHECK(std::fabs(drmfilter::boxWidthMhz(2, 9) - 0.020) < 1e-12);
    CHECK(std::fabs(drmfilter::boxWidthMhz(0, 0) - 0.020) < 1e-12);      // off, nothing found: the nominal width
    CHECK(std::fabs(drmfilter::boxWidthMhz(0, 10) - 0.010) < 1e-12);     // off: the detected occupancy
    CHECK(std::fabs(drmfilter::boxWidthMhz(0, 4.5) - 0.0045) < 1e-12);
    CHECK(drmfilter::passHz(0) == 0 && drmfilter::passHz(1) == 7000 && drmfilter::passHz(2) == 12000);
    std::printf(bad ? "FAILED\n" : "ok\n");
    return bad ? 1 : 0;
}
