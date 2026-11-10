// Drag to tune (app/gfx/drag_tune.h): hit testing of the plot and its frequency scale, what each press does in both settings and on touch,
// the snap steps across modes and zoom levels (with fine steps), the live retune filter, the VFO grab and the right-click menu rule.
#include "../../app/gfx/drag_tune.h"
#include <cmath>
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static bool near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
int main() {
    using namespace dragtune;
    // ---- hit testing: a 1000 x 300 plot at (100, 50); a 24 px scale strip inside at the bottom, or under it (a look's strip)
    const float x0 = 100, y0 = 50, x1 = 1100, y1 = 350;
    check(hitArea(600, 200, x0, y0, x1, y1, 24, false) == Area::Body, "middle of the plot is the body");
    check(hitArea(600, 340, x0, y0, x1, y1, 24, false) == Area::Scale, "bottom strip is the scale");
    check(hitArea(600, 326, x0, y0, x1, y1, 24, false) == Area::Scale, "scale edge (y1 - 24) counts as the scale");
    check(hitArea(600, 325, x0, y0, x1, y1, 24, false) == Area::Body, "just above the scale is the body");
    check(hitArea(600, 360, x0, y0, x1, y1, 24, false) == Area::Outside, "below the plot (inside labels) is outside");
    check(hitArea(50, 200, x0, y0, x1, y1, 24, false) == Area::Outside, "left of the plot is outside");
    check(hitArea(1101, 340, x0, y0, x1, y1, 24, false) == Area::Outside, "right of the plot is outside, even at scale height");
    check(hitArea(600, 340, x0, y0, x1, y1, 0, false) == Area::Body, "no scale (waterfall): the bottom is the body");
    check(hitArea(600, 360, x0, y0, x1, y1, 24, true) == Area::Scale, "look strip under the plot is the scale");
    check(hitArea(600, 374, x0, y0, x1, y1, 24, true) == Area::Scale, "look strip lower edge");
    check(hitArea(600, 375, x0, y0, x1, y1, 24, true) == Area::Outside, "below the look strip is outside");
    check(hitArea(600, 340, x0, y0, x1, y1, 24, true) == Area::Body, "with the strip below, the plot bottom is body");
    check(hitArea(600, 40, x0, y0, x1, y1, 24, true) == Area::Outside, "above the plot is outside");
    for (int sweep = 0; sweep <= 100; sweep++) {   // every row of the plot is exactly one of body or scale, the boundary moves with the strip height
        const float y = y0 + (y1 - y0) * sweep / 100.f;
        const Area a = hitArea(600, y, x0, y0, x1, y1, 30, false);
        if (a == Area::Outside || (a == Area::Scale) != (y >= y1 - 30)) { check(false, "row sweep"); break; }
    }

    // ---- what a press does
    check(pressAction(Tune, 0, Area::Body, false, false) == Press::Tune, "Tune: left on the body tunes");
    check(pressAction(Tune, 0, Area::Scale, false, false) == Press::PanX, "Tune: left on the scale pans x");
    check(pressAction(Tune, 1, Area::Body, false, false) == Press::PanXY, "Tune: right drag pans");
    check(pressAction(Tune, 2, Area::Body, false, false) == Press::PanXY, "Tune: middle drag pans");
    check(pressAction(Tune, 2, Area::Scale, false, false) == Press::PanXY, "Tune: middle on the scale pans");
    check(pressAction(Tune, 3, Area::Body, false, false) == Press::Nothing, "Tune: back button does nothing");
    check(pressAction(Tune, 0, Area::Outside, false, false) == Press::Nothing, "outside: nothing");
    check(pressAction(Pan, 0, Area::Body, false, false) == Press::PanXY, "Pan: left pans (old OnAir)");
    check(pressAction(Pan, 0, Area::Scale, false, false) == Press::PanXY, "Pan: left on the scale pans too");
    check(pressAction(Pan, 1, Area::Body, false, false) == Press::Nothing, "Pan: right is the menu, not a pan");
    check(pressAction(Pan, 2, Area::Body, false, false) == Press::Nothing, "Pan: middle unchanged");
    check(pressAction(Tune, 0, Area::Body, true, false) == Press::PanXY, "touch: one finger pans");
    check(pressAction(Tune, 0, Area::Body, true, true) == Press::Tune, "touch: long press tunes");
    check(pressAction(Tune, 0, Area::Scale, true, true) == Press::PanX, "touch: the scale pans even after a long press");

    // ---- steps
    check(niceStep(1) == 1 && niceStep(1.5) == 2 && niceStep(2) == 2 && niceStep(3) == 5 && niceStep(7) == 10, "1-2-5");
    check(near(niceStep(0.03), 0.05, 1e-12) && niceStep(4500) == 5000 && niceStep(20001) == 50000, "1-2-5 across decades");
    check(niceStep(0) == 1 && niceStep(-5) == 1 && niceStep(NAN) == 1, "bad input: 1");
    // FM: 100 kHz whatever the zoom; fine 10 kHz at most
    check(dragStepHz(1e5, 1e5, 10000, false) == 1e5 && dragStepHz(1e5, 1e5, 10, false) == 1e5, "FM: the channel step at every zoom");
    check(dragStepHz(1e5, 1e5, 10000, true) == 1e4, "FM fine zoomed out: a tenth of the channel step");
    check(dragStepHz(1e5, 1e5, 50, true) == 50, "FM fine zoomed in: a pixel");
    check(dragStepHz(12500, 12500, 5, true) == 5 && dragStepHz(12500, 12500, 5000, true) == 1250, "DMR fine");
    // no channel step: the click grid or two pixels, whichever is coarser
    check(dragStepHz(0, 1000, 10, false) == 1000, "zoomed in: the click grid (1 kHz)");
    check(dragStepHz(0, 1000, 10000, false) == 20000, "10 MHz over 1000 px: 20 kHz");
    check(dragStepHz(0, 1000, 2000, false) == 5000, "2 kHz/px: 5 kHz");
    check(dragStepHz(0, 1000, 2000, true) == 500, "fine never above a tenth of the coarse step");
    check(dragStepHz(0, 1000, 0.3, true) == 1, "fine never below 1 Hz");
    check(dragStepHz(0, 0, 0, false) >= 1 && dragStepHz(0, 0, 0, true) >= 1, "degenerate input stays >= 1 Hz");
    for (double hpp = 0.1; hpp < 1e6; hpp *= 1.37) {   // a zoom sweep: fine <= coarse, both >= 1, coarse >= grid, coarse grows with hz/px
        static double prev = 0;
        const double c = dragStepHz(0, 1000, hpp, false), f = dragStepHz(0, 1000, hpp, true);
        if (!(f <= c && f >= 1 && c >= 1000 && c >= prev)) { check(false, "zoom sweep"); break; }
        prev = c;
    }
    check(snapHz(100049999, 1e5) == 1e8 && snapHz(100050001, 1e5) == 100100000, "snap rounds to the nearest step");
    check(snapHz(123.4, 0) == 123.4, "no step: unchanged");
    // a drag from 99.95 to 100.25 MHz in 1 kHz pointer increments, FM step: retunes exactly on the channel changes
    {
        int retunes = 0; double last = NAN;
        for (double hz = 99.95e6; hz <= 100.25e6; hz += 1000) {
            const double s = snapHz(hz, 1e5);
            if (changed(last, s)) { retunes++; last = s; }
        }
        check(retunes == 4, "FM drag 99.95 -> 100.25: 100.0, 100.1, 100.2 and the start");
    }
    check(changed(NAN, 5) && !changed(5, 5.3) && changed(5, 5.5), "changed: first, sub-hertz, one hertz");
    check(moved(4, 0) && moved(0, -4) && !moved(3, 3), "a 4 px move is a drag");

    // ---- VFO grab: three channels, 12.5 kHz wide, one overlapping
    {
        const double c[3] = {145.500, 145.5125, 146.000}, h[3] = {0.00625, 0.00625, 0.00625};
        const double perPx = 0.0001;   // 100 Hz per pixel
        check(grabVfo(c, h, 3, 145.497, perPx, 6) == 0, "inside the first band");
        check(grabVfo(c, h, 3, 145.507, perPx, 6) == 1, "overlap: the nearest centre wins");
        check(grabVfo(c, h, 3, 145.505, perPx, 6) == 0, "overlap, nearer the first");
        check(grabVfo(c, h, 3, 145.700, perPx, 6) == -1, "between bands: none (the main receiver tunes)");
        const double n[1] = {146.0}, z[1] = {0};
        check(grabVfo(n, z, 1, 146.0005, perPx, 6) == 0, "a zero-width VFO is still grabbed within 6 px");
        check(grabVfo(n, z, 1, 146.0007, perPx, 6) == -1, "but not at 7 px");
        check(grabVfo(c, h, 0, 145.5, perPx, 6) == -1, "no VFOs");
        check(near(vfoDragCentre(145.503, 0.003, 100), 145.500, 1e-9), "grab offset keeps the centre under the grab point");
        check(near(vfoDragCentre(145.6031, 0.003, 100), 145.6001, 1e-9), "moved and snapped to 100 Hz");
    }

    // ---- the right-click menu
    check(menuOpens(Pan, true, false, 0) && !menuOpens(Pan, false, true, 0), "Pan: on the press");
    check(!menuOpens(Tune, true, false, 0) && menuOpens(Tune, false, true, 2), "Tune: on a still release");
    check(!menuOpens(Tune, false, true, 40), "Tune: a right drag pans, no menu");
    // ---- the wheel over the scale
    check(near(scaleWheelPan(2.0, 1), -0.1) && near(scaleWheelPan(2.0, -2), 0.2), "a notch pans a twentieth of the view");

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("drag_tune: all passed\n");
    return 0;
}
