// Drag to tune, like SDR++ (Settings > Radio > Drag on spectrum): the pure decisions, tested in tests/app/test_drag_tune.cpp.
// Tune (the default): a left press on the spectrum or the waterfall tunes there and a held drag keeps tuning live, snapped to the mode's
// channel step (or a step that suits the zoom); Shift or Ctrl during the drag gives fine steps. Panning: a left drag on the frequency scale
// (the strip with the frequency labels), a right or middle drag anywhere, and the wheel over the frequency scale. A grabbed multi-receiver
// VFO moves alone. Pan (the old OnAir): a left drag pans, a click tunes on release.
// On touch a one-finger drag always pans (the screen has no other way to scroll); a long press then a drag tunes.
#pragma once
#include <cmath>

namespace dragtune {

enum Mode { Tune = 0, Pan = 1 };

// Where a press lands on a plot: on the frequency scale strip, in the body, or outside. The plot is x0..x1, y0..y1 (screen pixels, y down).
// The scale strip is scalePx tall at the bottom of the plot (inside: labels drawn inside the plot), or, for a look's strip, the scalePx
// under the plot (below: true).
enum class Area { Outside, Body, Scale };
inline Area hitArea(float x, float y, float x0, float y0, float x1, float y1, float scalePx, bool below) {
    if (x < x0 || x > x1) return Area::Outside;
    if (below) {
        if (scalePx > 0 && y > y1 && y <= y1 + scalePx) return Area::Scale;
        return y >= y0 && y <= y1 ? Area::Body : Area::Outside;
    }
    if (y < y0 || y > y1) return Area::Outside;
    return scalePx > 0 && y >= y1 - scalePx ? Area::Scale : Area::Body;
}

// What a press does. button: 0 left, 1 right, 2 middle. longPress: a touch finger rested before it moved.
enum class Press { Nothing, Tune, PanX, PanXY };
inline Press pressAction(Mode mode, int button, Area area, bool touch, bool longPress) {
    if (area == Area::Outside) return Press::Nothing;
    if (mode == Pan) return button == 0 ? Press::PanXY : Press::Nothing;   // the old behaviour: only the left button pans; right is the menu
    if (button == 1 || button == 2) return Press::PanXY;
    if (button != 0) return Press::Nothing;
    if (area == Area::Scale) return Press::PanX;
    if (touch) return longPress ? Press::Tune : Press::PanXY;
    return Press::Tune;
}

// A 1-2-5 step at or above v (v > 0)
inline double niceStep(double v) {
    if (!(v > 0) || !std::isfinite(v)) return 1;
    const double mag = std::pow(10.0, std::floor(std::log10(v)));
    const double n = v / mag;
    const double s = n <= 1 + 1e-9 ? 1 : n <= 2 + 1e-9 ? 2 : n <= 5 + 1e-9 ? 5 : 10;
    return s * mag;
}

// The step (Hz) a drag snaps to. modeStepHz: the mode's channel step (FM 100 kHz, DMR 12.5 kHz, the user's snap), 0 when the mode has none;
// clickGridHz: what a click rounds to (1 kHz); hzPerPx: the zoom. Without a mode step the drag snaps to the click grid or to about two
// pixels, whichever is coarser, so a zoomed-out drag does not hunt through hertz. fine (Shift / Ctrl): about one pixel, at most a tenth of
// the coarse step, never below 1 Hz.
inline double dragStepHz(double modeStepHz, double clickGridHz, double hzPerPx, bool fine) {
    const double grid = clickGridHz >= 1 ? clickGridHz : 1;
    const double coarse = modeStepHz >= 1 ? modeStepHz : std::fmax(grid, niceStep(2 * std::fmax(hzPerPx, 1e-6)));
    if (!fine) return coarse;
    const double f = std::fmin(niceStep(std::fmax(1.0, hzPerPx)), coarse / 10);
    return f >= 1 ? f : 1;
}
inline double snapHz(double hz, double stepHz) { return stepHz >= 1 ? std::round(hz / stepHz) * stepHz : hz; }

// The press moved far enough to be a drag (not a click)
inline bool moved(double dxPx, double dyPx) { return std::fabs(dxPx) >= 4 || std::fabs(dyPx) >= 4; }

// A live drag retunes only when the snapped frequency changed (sub-hertz noise of the mapping never retunes)
inline bool changed(double lastHz, double newHz) { return !std::isfinite(lastHz) || std::fabs(newHz - lastHz) >= 0.5; }

// The multi-receiver VFO a press grabs: the one whose band (centre +- half width, at least minPx either side) holds x; the nearest centre
// wins when bands overlap. -1: none (the press tunes the main receiver). Frequencies in MHz, perPx MHz per pixel.
inline int grabVfo(const double* centre, const double* half, int n, double x, double perPx, double minPx) {
    int best = -1; double bd = 1e300;
    for (int i = 0; i < n; i++) {
        const double h = std::fmax(half[i], perPx * minPx);
        const double d = std::fabs(x - centre[i]);
        if (d <= h && d < bd) { bd = d; best = i; }
    }
    return best;
}
// A grabbed VFO keeps the point it was grabbed by under the pointer: its new centre, snapped
inline double vfoDragCentre(double pointerMhz, double grabOffsetMhz, double stepHz) { return snapHz((pointerMhz - grabOffsetMhz) * 1e6, stepHz) / 1e6; }

// The right-click menu of the spectrum: in Tune a right drag pans, so the menu opens on a release that did not move; in Pan on the press.
inline bool menuOpens(Mode mode, bool rightPressed, bool rightReleased, double dragPx) {
    if (mode == Pan) return rightPressed;
    return rightReleased && dragPx < 4;
}

// The wheel over the frequency scale pans the view (SDR++): a notch moves it by a twentieth of its width
inline double scaleWheelPan(double spanMhz, float wheel) { return -(double)wheel * spanMhz / 20.0; }

}  // namespace dragtune
