// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by app_state.cpp and prefs.cpp: the few mode helpers the settings loader needs.
// It exists so prefs.cpp can be a file of its own (it was part of app_state.cpp).
#pragma once
#include "../app.h"

void setFamilyFlags(App& a, int f);   // app_state.cpp: the booleans (fmMode, dabMode ...) that follow family f
int firstSubOf(int f);                // app_state.cpp: the first protocol entry of a family that is only listed as its protocols, else 0
std::string slotKey(int slot);        // app_state.cpp: the suffix of the per-mode settings: the family number, or "." and the protocol's id
