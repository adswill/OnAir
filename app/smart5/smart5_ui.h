// The 3.9 round-5 smart engines of core/include/dect2/smart5 (docs/SMART5-*-3.9.md) in the mode screens: shared helpers.
// Each area file feeds its engines from smart5Tick() (every frame, from the Insights tick, whatever screen is open) and draws a
// compact card in its mode's screen: one status line on top, the tables under "Details". Findings go to View > Insights and out
// as automation events "insight.<kind>". Offline; nothing leaves the computer.
#pragma once
#include "../app.h"
#include <string>

namespace s5ui {

double wall();                                       // Unix seconds
bool every(double& last, double sec);                // true at most once per sec (wall clock)
std::string utc(double t, bool date = false);        // "14:05" or "2026-11-11 14:05" UTC
// a finding for View > Insights and the automation event insight.<kind>; area "broadcast", "tv", "satellite", "gnss", "science"
void post(const char* area, const char* kind, int severity, const std::string& key, const std::string& text);
// posts once when `on` turns true (state is the caller's memory of the last value)
void edge(bool& state, bool on, const char* area, const char* kind, int severity, const std::string& key, const std::string& text);
// one status line: amber or red when severity > 0
void line(int severity, const std::string& text);
// the card's header ("Smart checks"); false when folded. The details go under detailsHeader().
bool card(const char* id);
bool details(const char* id);
std::string readData(const char* file);              // plat::dataDir()/file, "" when absent
void writeData(const char* file, const std::string& text);

}  // namespace s5ui

// app/smart5/*.cpp: feeding (called every frame) and the per-area summaries for View > Insights
void smart5Tick(App& a);
void smart5BroadcastTick(App& a);
void smart5TvTick(App& a);
void smart5SatTick(App& a);
void smart5SciTick(App& a);
void smart5InsightBroadcast(App& a);   // the Insights window's "Broadcast and TV" tab
void smart5InsightTv(App& a);
void smart5InsightSat(App& a);         // the "Satellites and science" tab
void smart5InsightSci(App& a);
