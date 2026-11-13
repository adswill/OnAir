// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Help > Report a problem: a window that shows what a bug report contains, asks a few yes/no questions that suggest a fix for
// the common causes, and saves ONE local zip (diagnostics.txt and README.txt) for a GitHub issue. Nothing is uploaded, and the
// zip has no recordings, settings, keys or passwords, and no file paths beyond the folder name.
#pragma once
#include "../app.h"

// Draws the window while a.reportOpen is set (report_problem.cpp).
void reportProblemWindow(App& a);
