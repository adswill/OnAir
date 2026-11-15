// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The TETRA name book (dect2/tetra/tetra_names.h) on the TETRA screen: the file <data folder>/tetra_names.csv is read at the start and written
// after a change (tick), "Names" is the editor (add, edit, delete, search, import, export), withName() puts a name beside a number and
// speakingLine() shows who is speaking now. docs/TETRA-NAMES.md.
#pragma once
#include "dect2/tetra/tetra_names.h"
#include <string>

struct App;

namespace tetranames {

void tick();                                                     // load once, save after an edit (call every frame, cheap)
std::string nameOf(dect2::TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1);   // the name or ""
std::string withName(dect2::TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1);  // "1234" or "1234  Name"
void view(App& a);                                               // the "Names" tab: editor, import, export
void speakingLine(App& a);                                       // "Speaking now: ISSI 1234 Name to GSSI 91 Name (slot 2)" or a quiet line

}  // namespace tetranames
