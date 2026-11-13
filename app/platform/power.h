// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Power and load: is the computer on its battery, can the window be seen, App Nap while receiving, and the process's CPU per thread and
// wake-ups for View > Performance overlay. power.cpp (all systems), power_cocoa.mm (macOS window and activity parts).
#pragma once
#include <string>
#include <vector>

struct GLFWwindow;

namespace power {

bool onBattery();                         // the computer runs on its battery (read again every 10 s; false when the system cannot tell)
bool windowOccluded(GLFWwindow* window);  // macOS: no part of the window can be seen (covered, another Space, the screen off); false elsewhere
void setReceiving(bool on);               // macOS: while a receiver runs the app is kept out of App Nap (the radio's timers stay on time)

struct ThreadCpu { std::string name; float pct = 0; };   // pct: percent of one core
struct Usage {
    float processPct = -1;       // the whole process, percent of one core
    float wakeupsPerSec = -1;    // macOS: the process's wake-ups from idle; Linux: context switches; -1 when unknown
    const char* wakeupsName = "wake-ups";
    std::vector<ThreadCpu> threads;   // the busiest first
};
// The load since the previous call (call it about once a second); false the first time and where it is not supported.
bool sampleUsage(Usage& u);

}   // namespace power
