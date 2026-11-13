// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Saving pictures to a folder the user chose (Save pictures to: setting), with a clear message of what was saved and where.
// The rules are in state/picsave.h (tested). Used by the Images window, the weather-fax list and the SSTV list.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace picui {
std::string folder();                                                              // the setting, else <home>/Pictures/OnAir
std::string saveRgba(const char* mode, time_t t, const uint8_t* rgba, int w, int h);   // writes <folder>/<mode>/<stamp>-<mode>.png; the path, "" on failure (status says why)
std::string saveCopy(const char* mode, time_t t, const std::string& pngFile);          // copies an existing PNG the same way
void tooltipFor(const char* mode);                                                  // call right after the save icon: "Save this picture to <folder>/<mode>"
void settingRow();                                                                  // Save pictures to: [folder] [Choose...] [Open folder]
void statusLine();                                                                  // "Saved to <path>" + Open folder, or the error; fades after a while
}
