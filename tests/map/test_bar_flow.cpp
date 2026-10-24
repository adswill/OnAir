// The button line over a map wraps instead of running off the edge, at any width
#include "../../app/map/bar_flow.h"
#include <cstdio>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    const float widths[] = {26, 26, 60, 60, 90, 200, 60, 110, 120, 140, 130, 150};
    for (float panel = 360; panel <= 2000; panel += 40) {
        const float left = 8, right = panel - 8;
        float last = left - 6;
        for (float w : widths) {
            const barflow::Spot s = barflow::next(last, w, 6, left, right);
            // an item fits on its line unless it is alone on it and wider than the line
            CHECK(s.x + w <= right + 0.01f || s.x == left, "panel %.0f item %.0f at %.0f runs past %.0f", panel, w, s.x, right);
            CHECK(s.x >= left, "panel %.0f starts left of the line", panel);
            last = s.x + w;
        }
    }
    CHECK(!barflow::wraps(8, 5000, 8, 100), "the first item of a line never wraps");
    CHECK(barflow::wraps(50, 60, 8, 100) && !barflow::wraps(50, 40, 8, 100), "wrap threshold");
    if (!fails) printf("bar_flow ok\n");
    return fails ? 1 : 0;
}
