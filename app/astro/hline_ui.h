// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The hydrogen line toolkit tabs of the Radio astronomy screen (Velocity, Map, Pointing, Calibration / RFI, Log); the maths is core/include/dect2/astro/hline.h.
#pragma once
#include <string>
#include <vector>

struct App;

struct HlineIn {
    bool have = false;                         // a processed spectrum exists
    const std::vector<double>* ratio = nullptr; // (on - off) / off with the baseline removed (fraction of Tsys), lowest frequency first
    const std::vector<double>* on = nullptr;    // the integrated spectrum (linear power)
    double centerHz = 0, rate = 0, restHz = 0, integSec = 0, edge = 0.08;
};
enum class HlineTab { Velocity, Map, Pointing, Calibration, Log };
void hlineTab(App& a, HlineTab t, const HlineIn& in);
void hlineLog(const std::string& line);
