// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// APRS Weather and Messages views (packet_board_ui.cpp, docs/APRS-BOARD-3.9.md)
#pragma once
#include <string>

struct App;

namespace aprsboard {
void weatherView(App& a, std::string& selCall);   // the weather stations table and the 24 h graph of the selected one
void messagesView(App& a);                       // conversations, bulletins, the user's callsign
const char* myCall(App& a);                      // packetMyCall, or the APRS-IS login callsign when that is empty
int unreadToMe(App& a);                          // messages to the user's callsign not opened yet in the Messages view
}
