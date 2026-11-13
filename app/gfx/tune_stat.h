// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Counters for the feel of tuning: how long the UI thread spends in the retune and in the settings save that used to follow every tuning
// event, and how many saves the debounce saved. Printed after a --shot run (shotReport) and by the tune_stat test. No ImGui.
#pragma once
#include <chrono>
namespace tunestat {
struct Stats {
    unsigned retunes = 0, saves = 0, savesDeferred = 0;
    double retuneMsSum = 0, retuneMsMax = 0, saveMsSum = 0, saveMsMax = 0;
    double saveMsMean() const { return saves ? saveMsSum / saves : 0; }
    double retuneMsMean() const { return retunes ? retuneMsSum / retunes : 0; }
};
// times the scope it lives in as one retune
struct RetuneTimer {
    Stats& s; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    explicit RetuneTimer(Stats& st) : s(st) {}
    ~RetuneTimer() { const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); s.retunes++; s.retuneMsSum += ms; if (ms > s.retuneMsMax) s.retuneMsMax = ms; }
};
}
