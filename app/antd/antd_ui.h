// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Antenna designer: parts shared by the window (antd_ui.cpp) and the calculators (antd_calc_ui.cpp).
#pragma once
#include "../app.h"
#include "dect2/antd/antd.h"
#include "dect2/antd/antd_tools.h"
#include <string>
#include <vector>

namespace antdui {

// lengths shown in metres or feet (radius in mm or inches)
struct Units {
    int sys = 0;   // 0 metric, 1 imperial
    double len(double m) const { return sys ? m / 0.3048 : m; }
    double lenIn(double v) const { return sys ? v * 0.3048 : v; }
    const char* lenUnit() const { return sys ? "ft" : "m"; }
    double rad(double m) const { return sys ? m / 0.0254 : m * 1000; }
    double radIn(double v) const { return sys ? v * 0.0254 : v / 1000; }
    const char* radUnit() const { return sys ? "in" : "mm"; }
};

// a polar plot of a cut (gain dBi per step degrees): fullCircle 0..360; compass: 0 at the top, clockwise; else 0 on the right (elevation)
void polarPlot(const char* id, const std::vector<double>& g, double stepDeg, bool fullCircle, bool compass, float size, const char* title,
               double markDeg = -1);
// the Calculators tab; fMHz and z0 start from the designer's frequency and reference impedance; setBand puts a preset into the sweep
void calculatorsTab(double fMHz, double z0, const Units& u, double* setF0MHz, double* setF1MHz, bool* bandPicked);

} // namespace antdui
