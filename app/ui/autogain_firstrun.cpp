// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The setup's and the lessons' gain call (autogain_iface.h) on top of the real auto gain of autogain-030 (app/panels/autogain_ui.cpp,
// autogainRun(App&, int how)). It replaces the stand-in that only switched on the old AGC loop.
#include "autogain_iface.h"
#include "../app.h"

bool autogainRun(App& a, int radio, const std::string& mode) {
    (void)mode;   // the auto gain reads the mode from the engine standard of the running receiver
    if (radio < 0 || radio >= (int)a.devices.size() || !a.devices[radio].isRadio()) return false;
    if (radio == a.devIdx && autogainRun(a, 1)) return true;   // 1 = Auto (keeps adjusting, a.agcOn)
    // not running yet (or another radio): Auto starts with the receiver, as the AGC switch always did
    if (!a.agcOn) { a.agcOn = true; agcReset(a); }
    return true;
}
