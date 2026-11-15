// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Detached panels (issue #242), see detach.cpp: OS windows that mirror the spectrum, the waterfall and the message log.
#pragma once
struct App;

namespace detach {
enum Kind { Spectrum = 0, Waterfall = 1, Messages = 2, kCount = 3 };
const char* title(int k);
void frame(App& a);   // inside the ImGui frame: opens and closes the windows (App::allowDetach, App::detached) and builds their pictures
void present();       // after the main frame: draws them
void shutdown();      // before the graphics back end shuts down
}
