// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir's own process CPU time (user + system, seconds) behind one function per platform (proc_cpu.cpp):
// macOS and Linux getrusage, Windows GetProcessTimes. The status strip takes deltas of it once a second.
#pragma once

namespace plat {
double processCpuSeconds();
}
