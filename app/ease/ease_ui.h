// Small shared pieces of the first-use helpers (docs/EASE2-3.8.md).
#pragma once
#include "app.h"

namespace easeui {

// A geostationary satellite chosen by position: at most once, only while the user has not chosen one or tuned elsewhere.
struct SatPick { double prevMhz = 0; bool undoable = false; };
// Returns the index picked on this call (the caller tunes to it), else -1. prefKey is the saved satellite choice of the mode.
int preselectSat(App& a, const char* prefKey, const double* slotLon, int count, SatPick& st);
// "Point the antenna south (173 deg), 27.6 deg up" for the chosen slot; with Undo right after an automatic pick. Returns true when Undo was pressed.
bool satPointing(App& a, double slotLonDeg, SatPick& st);

// A voice mode whose codec is not installed: said on the mode's own screen before the first call, with the install guide; Hide is remembered per mode.
void codecNotice(App& a);

}
