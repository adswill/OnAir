// 4.0 group A - D features in the mode screens (docs/WIRING-4.0.md): the pure engines of core/include/dect2/d40 and friends, fed from the
// live engine data. d40Tick() runs every frame (from the Insights tick); each d40...Card() draws one compact card in its mode's screen.
// Settings persist through plat::prefs() (key prefix "d40"), tables as text files in plat::dataDir(). Offline; nothing leaves the computer.
#pragma once
#include "../app.h"

void d40Tick(App& a);
void d40FmCard(App& a);        // d40_fm_ui.cpp: station names list and DX opening alarm
void d40FmTick(App& a);
