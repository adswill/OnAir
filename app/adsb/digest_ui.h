// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The daily digest: at a chosen local time one session.digest event with a line from every mode that has something to say ("312 aircraft,
// farthest 412 km; 2 new DXCC; 41 Meteor lines lost"). The built-in recipe "Daily digest" sends it as a notification (or to ntfy / Telegram).
#pragma once
#include <functional>
#include <string>
// a mode's line for today ("" when it has nothing); registered once by the mode, called on the interface thread
void digestRegister(const std::string& key, std::function<std::string()> line);
void digestTick();            // every frame (automation_ui.cpp)
std::string digestText();     // what it would say now
