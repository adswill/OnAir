// The classic top bar's right-hand buttons (app/ui/top_row.h, #139 #424): at every window width from 320 to 3840 px and every UI scale the
// buttons stay inside the window, never overlap each other and never overlap the mode selector on its line. ImGui gives a click to the item
// drawn first, so an overlapped button did nothing (the Pi at 640x480 in #139: Touch, New interface, Server and the language).
#include "../../app/ui/top_row.h"
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    // the widths of 3.7 at scale 1 (radio settings, support, version, clear all, English, Server, New interface, Touch, Tour)
    const std::vector<float> base = {104, 58, 50, 62, 56, 50, 98, 46, 38};
    const float sp = 8, gap = 12, lineStart = 8;
    int cases = 0, ownLine = 0, wrapped = 0;
    for (float scale : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
        for (int withRadio = 0; withRadio < 2; withRadio++) {
            std::vector<float> w;
            for (size_t k = withRadio ? 0 : 1; k < base.size(); k++) w.push_back(base[k] * scale);
            for (float width = 320; width <= 3840; width += 16) {
                const float right = width - 8;
                for (float leftEnd : {60.f * scale, 250.f * scale, 600.f * scale, width * 0.9f}) {   // drop-down, segmented, wider than the window
                    const auto pl = toprow::layout(w, leftEnd, lineStart, right, sp, gap);
                    cases++;
                    CHECK(pl.size() == w.size(), "one place per button");
                    int line = 0;
                    float prevEnd = -1;
                    bool sawWrap = false;
                    for (size_t i = 0; i < pl.size(); i++) {
                        if (pl[i].newLine) { line++; prevEnd = -1; if (i) sawWrap = true; }
                        const float x0 = pl[i].x, x1 = pl[i].x + w[i];
                        CHECK(x0 >= lineStart - 0.01f, "width %.0f scale %.2f button %zu starts left of the window (%.1f)", width, scale, i, x0);
                        if (w[i] <= right - lineStart) CHECK(x1 <= right + 0.01f, "width %.0f scale %.2f leftEnd %.0f button %zu ends past the right edge (%.1f > %.1f)", width, scale, leftEnd, i, x1, right);
                        if (prevEnd >= 0) CHECK(x0 >= prevEnd + sp - 0.01f, "width %.0f scale %.2f button %zu overlaps the one before (%.1f < %.1f)", width, scale, i, x0, prevEnd + sp);
                        if (line == 0) CHECK(x0 >= leftEnd + gap - 0.01f, "width %.0f scale %.2f leftEnd %.0f button %zu sits on the mode selector (%.1f)", width, scale, leftEnd, i, x0);
                        prevEnd = x1;
                    }
                    if (pl[0].newLine) ownLine++;
                    if (sawWrap) wrapped++;
                }
            }
        }
    }
    // what 3.7 did at 640 px, scale 1: the language group at 640 - 12 - w with w leaving out Touch, the radio settings at 640 - 64 - 410
    {
        const float right = 632, leftEnd = 250;
        const auto pl = toprow::layout(std::vector<float>(base.begin(), base.end()), leftEnd, lineStart, right, sp, gap);
        CHECK(pl[0].newLine, "640 px with a 250 px selector: the buttons get their own line");
        const float old37RadioX = 640 - 64 - 410;   // 166: on top of the selector, which took the click
        CHECK(old37RadioX < leftEnd, "the 3.7 position overlapped the selector (the bug this guards)");
    }
    // a wide window keeps the old look: one line, right-aligned to the edge
    {
        std::vector<float> w(base.begin(), base.end());
        const auto pl = toprow::layout(w, 900, lineStart, 1912, sp, gap);
        float total = 0; for (size_t i = 0; i < w.size(); i++) total += w[i] + (i ? sp : 0);
        CHECK(!pl[0].newLine && pl[0].x == 1912 - total, "1920 px: on the selector's line, right-aligned");
        for (size_t i = 1; i < pl.size(); i++) CHECK(!pl[i].newLine, "1920 px: no wrap");
    }
    CHECK(ownLine > 0 && wrapped > 0, "the sweep reached the own-line and the wrapped layouts (%d, %d)", ownLine, wrapped);
    std::printf("top_row: %d layouts, %d on their own line, %d wrapped, %d failures\n", cases, ownLine, wrapped, failures);
    return failures ? 1 : 0;
}
