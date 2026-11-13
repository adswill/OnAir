// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Internal to app/memory: the pieces memory_ui.cpp (the panel) and memory_edit.cpp (the edit and duplicate dialogs) share.
#pragma once
#include "memory_options.h"
#include "dect2/channels/memory_formats.h"

namespace memui {

// Opens the edit dialog for m. isNew: a memory about to be stored (the dialog asks about a duplicate of the same frequency and mode);
// otherwise m is the memory being edited (m.id) and the dialog updates it.
void openEdit(const dect2::Memory& m, bool isNew);
// Draws the dialogs (call every frame from the panel's window or tab); returns the id of a memory the dialog stored (0: none this frame).
uint64_t editDialogs();
// "name  MHz  mode" for a menu or a message
std::string memoryTitle(const dect2::Memory& m);
// the readout's digit grouping: 93.900.000 (MHz . kHz . Hz), as the frequency digits at the top show it
std::string freqReadout(uint64_t hz);

// memory_lists_ui.cpp: the import preview, Export as, "My lists" and the quick-tune bar's model
dect2::ListSet& memoryLists();
bool memoryListEnabled(const std::string& group);   // false: the list of that group is switched off
void importButton(dect2::MemoryBank& b);
void exportAs(dect2::MemoryBank& b);
void importPopup(dect2::MemoryBank& b);
void listsSection(dect2::MemoryBank& b);
void statusLine();

}  // namespace memui
