// The tinted mode list (app/ui/row_tint.h): on every theme, light and dark and high contrast, the text on a tinted heading, mode row, hovered
// row and selected row keeps at least 4.5:1 once liftText has moved it (text that already held it is left as the theme has it), for the real
// category colours and a grid of others; and the real colours stay visibly tinted, with the selected row clearly stronger than the others.
#include "ui/row_tint.h"
#include <cstdio>
#include <vector>

using contrast::Rgb;
using rowtint::Look;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static Rgb c8(int r, int g, int b) { return {r / 255.f, g / 255.f, b / 255.f}; }

int main() {
    // the panes and text colours of the new interface's palettes (ui2_theme.cpp; rail = the side bar). Daylight is written in light colours
    // and turned into dark source colours by L(): the same turn-over as invertLightness.
    struct Theme { const char* name; Rgb rail, text, dim, bright; };
    auto L = [](int r, int g, int b) { return contrast::invertLightness(c8(r, g, b)); };
    const std::vector<Theme> themes = {
        {"instrument", {0.135f, 0.138f, 0.142f}, {0.86f, 0.87f, 0.88f}, {0.60f, 0.61f, 0.62f}, {1, 1, 1}},
        {"mono", {0.055f, 0.055f, 0.055f}, {0.84f, 0.84f, 0.84f}, {0.50f, 0.50f, 0.50f}, {1, 1, 1}},
        {"dev A", c8(37, 37, 38), c8(204, 204, 204), c8(133, 133, 133), c8(240, 240, 240)},
        {"dev B", c8(26, 26, 26), c8(200, 200, 200), c8(120, 120, 120), c8(235, 235, 235)},
        {"studio", c8(33, 38, 54), c8(216, 222, 238), c8(140, 150, 176), c8(246, 248, 255)},
        {"mono pro", c8(0, 0, 0), c8(208, 204, 194), c8(128, 120, 102), c8(255, 240, 210)},
        {"daylight", L(255, 255, 255), L(24, 31, 46), L(88, 99, 120), L(0, 0, 0)},
        {"black panes", {0, 0, 0}, {1, 1, 1}, {0.6f, 0.6f, 0.6f}, {1, 1, 1}},   // the High contrast style
        {"terminal", {0.037f, 0.037f, 0.036f}, {0.82f, 0.81f, 0.78f}, {0.46f, 0.45f, 0.42f}, {0.96f, 0.95f, 0.92f}},
    };
    // the category colours (kGroupCols in app/ui/mode_table.cpp), then a grid of other colours so a new category is covered too
    std::vector<Rgb> cats = {
        c8(92, 188, 220), c8(92, 210, 148), c8(132, 160, 240), c8(80, 170, 210), c8(186, 150, 236), c8(150, 206, 104), c8(226, 168, 112),
        c8(214, 206, 96), c8(110, 214, 196), c8(120, 150, 230), c8(218, 130, 214), c8(236, 128, 140), c8(204, 176, 132), c8(150, 162, 178),
        c8(150, 170, 210), c8(236, 160, 84), c8(120, 200, 170)};
    const size_t nReal = cats.size();
    for (int r = 0; r <= 255; r += 51) for (int g = 0; g <= 255; g += 51) for (int b = 0; b <= 255; b += 51) cats.push_back(c8(r, g, b));

    const Look looks[3] = {Look::Dark, Look::Light, Look::HighContrast};
    const char* lookName[3] = {"dark", "light", "high contrast"};
    struct Row { const char* name; float want; bool selected; };
    const Row rows[4] = {{"heading", rowtint::kHeadAlpha, false}, {"mode", rowtint::kModeAlpha, false}, {"hover", rowtint::kHoverAlpha, false}, {"selected", rowtint::kSelAlpha, true}};
    int checked = 0;
    for (int li = 0; li < 3; li++)
        for (const Theme& t : themes) {
            const Rgb pane = t.rail;
            for (const Row& row : rows) {
                // the texts the row draws: the selected one its name bright and its second line in the text colour; the others text and dim
                const Rgb texts[2] = {row.selected ? t.bright : t.text, row.selected ? t.text : t.dim};
                for (size_t ci = 0; ci < cats.size(); ci++) {
                    const float a = rowtint::safeAlpha(pane, cats[ci], row.want, looks[li]);
                    CHECK(a >= 0 && a <= row.want + 1e-6f, "%s %s %s: alpha %.3f out of range", lookName[li], t.name, row.name, a);
                    const Rgb fill = rowtint::mix(pane, cats[ci], a);
                    for (const Rgb& tx : texts) {
                        const float got = rowtint::shownRatio(fill, rowtint::liftText(fill, tx, looks[li]), looks[li]);
                        CHECK(got >= rowtint::kMinRatio - 0.01f, "%s %s %s cat %zu: contrast %.2f", lookName[li], t.name, row.name, ci, got);
                        // text that already held the ratio is drawn as the theme has it
                        if (rowtint::shownRatio(fill, tx, looks[li]) >= rowtint::kMinRatio) {
                            const Rgb same = rowtint::liftText(fill, tx, looks[li]);
                            CHECK(same.r == tx.r && same.g == tx.g && same.b == tx.b, "%s %s %s: a legible text colour was changed", lookName[li], t.name, row.name);
                        }
                        checked++;
                    }
                }
            }
            // the real categories: visibly tinted, and the selection clearly stronger than a plain mode row
            if (looks[li] == Look::HighContrast) continue;
            for (size_t ci = 0; ci < nReal; ci++) {
                const float head = rowtint::safeAlpha(pane, cats[ci], rowtint::kHeadAlpha, looks[li]);
                const float mode = rowtint::safeAlpha(pane, cats[ci], rowtint::kModeAlpha, looks[li]);
                const float s = rowtint::safeAlpha(pane, cats[ci], rowtint::kSelAlpha, looks[li]);
                CHECK(head >= 0.12f, "%s %s cat %zu: heading tint cut to %.3f", lookName[li], t.name, ci, head);
                CHECK(mode >= 0.07f, "%s %s cat %zu: mode row tint cut to %.3f", lookName[li], t.name, ci, mode);
                CHECK(s >= mode + 0.10f, "%s %s cat %zu: selected %.3f not clearly above a mode row %.3f", lookName[li], t.name, ci, s, mode);
            }
        }
    // the mix itself
    const Rgb m = rowtint::mix({0, 0, 0}, {1, 0.5f, 0}, 0.5f);
    CHECK(std::fabs(m.r - 0.5f) < 1e-6f && std::fabs(m.g - 0.25f) < 1e-6f && m.b == 0, "mix");
    std::printf("row_tint: %d combinations checked, %d failures\n", checked, failures);
    return failures ? 1 : 0;
}
