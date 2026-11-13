// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Every set of orbital elements the program has: the built-in snapshot and the downloaded lists, for the modes that compare a measured carrier with satellites.
#pragma once
#include "dect2/geo/passes.h"
#include <string>
#include <vector>

namespace dect2 {
namespace satmisc {
std::vector<Tle> loadAllElements(const std::string& tleDir);   // reads every .txt and .tle file in the folder, newest epoch per satellite
} // namespace satmisc
} // namespace dect2
