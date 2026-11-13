// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared telemetry JSON formatting has no dependency on the HTTP server.
#include "dect2/net/remote_api.h"
#include "dect2/common/str.h"
#include <cmath>
#include <cstdio>
namespace dect2 {
std::string jsonEscape(const std::string& s) { return dect2::str::jsonEscape(s); }
std::string jsonNum(double v) {
    if (!std::isfinite(v)) return "null";
    char b[40];
    snprintf(b, sizeof b, "%.10g", v);
    return b;
}
} // namespace dect2
