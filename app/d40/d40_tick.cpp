// 4.0 wiring: the per-frame feeding of every card (docs/WIRING-4.0.md).
#include "d40_ui.h"

void d40Tick(App& a) {
    d40FmTick(a);
    d40SatTick(a);
    d40DabTick(a);
    if (a.engine.running() && a.rx.standard == 47) d40TetraTick(a.rx.tetra);
}
