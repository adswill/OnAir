// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared between the toolbar_*.cpp files (the top bar, source options, rig windows): the few helpers more than one of them calls.
// Declared here, defined in toolbar_source.cpp and toolbar_rig.cpp.
#pragma once
#include "../app.h"

std::string rateWarning(const App& a);   // toolbar_source.cpp: why the radio's rate is too low for the mode (empty when it is fine)
void hamlibFollowRig(App& a);            // toolbar_rig.cpp: take the rig's dial and mode over into the frequency box
