// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tools > Backup and restore: one .onairbackup archive of settings, memories, presets, scheduler, logbook, layouts, scan results and
// station lists (and, only with "include secrets" + a passphrase, the voice keys and API credentials, encrypted). The logic is in
// core/src/backup/backup.cpp; this file is the window, the file collection and the automatic weekly backup (docs/BACKUP.md).
#pragma once
#include "../app.h"

// Draws the window while a.backupOpen is set, and runs the automatic backup check every frame (cheap).
void backupWindow(App& a);
