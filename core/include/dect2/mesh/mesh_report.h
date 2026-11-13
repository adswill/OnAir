// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A short text report of a mesh (LoRa) session: what was heard, how well, and each node with its signal range. Plain text that reads
// well pasted into a chat or an e-mail; the tables stay in the CSV / JSON export.
#pragma once
#include "mesh_tel.h"
#include <string>

namespace dect2 {

// plan: what the receiver listened to ("ANZ 915  919.875 MHz"); nowUnix: the clock time that matches t.timeSec (signal times become
// clock times from it); utc: clock times in UTC instead of local time (tests)
std::string meshReport(const MeshTelemetry& t, const std::string& plan, double nowUnix, bool utc = false);

} // namespace dect2
