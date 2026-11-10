// Played list of FM (RadioText+) and DAB (DL Plus), and the broadcast.* events for alerts: played_ui.cpp. docs/PLAYED-3.9.md.
#pragma once
#include <string>
struct App;
// what the playing station says now; called by the FM and DAB per-frame hooks when the telemetry changes
void playedNote(const char* source, const std::string& station, double freqMhz, const std::string& artist, const std::string& title,
                const std::string& text, const std::string& pty, bool trafficAnnouncement);
// the list with search and export (FM and DAB tabs)
void playedPanel(App& a, const char* id);
