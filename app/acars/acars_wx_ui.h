// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airport weather from datalink text (3.9, docs/SMART5-avia-3.9.md): one table fed by ACARS, VDL2 and HFDL (core: dect2/acars/acars_metar.h).
#pragma once
#include <cstdint>
#include <string>

void acarsWxFeed(int source, uint64_t serial, const std::string& text, int64_t wallTime);   // source 0 ACARS, 1 VDL2, 2 HFDL; each serial once
void acarsWxView();
