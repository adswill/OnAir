// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Report a problem, the part of the report window (debug_report.cpp) that came from the first problem report: a few yes/no
// questions that suggest a fix for the common causes, and the radio and mode text. Also the text cleaning both use: folder paths
// shortened and lines that may hold a secret left out.
#pragma once
#include "../app.h"

// The troubleshooter questions (yes/no, with a hint for each No), drawn inside the report window; Reset clears the answers.
void reportTroubleshooter();
void reportTroubleshooterReset();
// The answered questions, one per line ("  <question> yes|no"); empty when none was answered.
std::string reportTroubleshooterAnswers();

// Shared with the debug report (debug_report.cpp): folder paths shortened (personal folders become "~" or the folder name) ...
std::string reportCleanText(const std::string& text);
// ... a line that may hold a key, token, password or serial ...
bool reportMayHoldSecret(const std::string& line);
// ... and the version, this computer, the selected source, the active mode, the sample rate and the enabled features.
std::string reportRadioAndMode(App& a);
