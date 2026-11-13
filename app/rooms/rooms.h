// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live rooms (Server > Go live): the receiver's waterfall and, if chosen, decoded ACARS text are streamed to an audience on the web. See docs/live-rooms.md.
#pragma once
#include <string>
#include <vector>
struct App;

void roomsMenu(App& a, bool menu);                                   // Server menu / popup: "Go live..." or the live status with Stop
void roomsPopups(App& a);                                            // the consent window; keeps the mode and frequency of the room current
void roomsFeedRow(const std::vector<float>& dbfs, float minDb, float maxDb);   // every spectrum frame; about one waterfall line a second is kept
void roomsFeedText(const std::string& redactedText);                 // decoded text (already redacted); kept only when the room shares text
bool roomsLive();
void roomsShutdown();                                                // stops the room (tells the server) and joins the thread
