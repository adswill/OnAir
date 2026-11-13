// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The one recordings folder (docs/RECORDINGS-FOLDER.md). The user may choose it in Settings (pref "recordingsDir"); the default
// is <dataDir>/recordings. The pure half (recdir::resolve) needs no GUI, so tests link it; app code calls recordingsDir().
#pragma once
#include <string>

namespace recdir {

// Strips trailing '/' and '\' (not from a bare root), keeps spaces and non-ASCII bytes as they are.
std::string normalise(std::string p);
// Chosen folder (may be empty) or the default: creates it on demand and checks it can be written. When the chosen one cannot be
// made or written, the default is used and *fellBack is set (the caller warns once). The result has no trailing slash.
std::string resolve(const std::string& chosen, const std::string& defaultDir, bool* fellBack = nullptr);
// Free bytes on the disk holding the folder, UINT64_MAX when unknown.
unsigned long long freeBytes(const std::string& dir);

}  // namespace recdir

// The recordings folder: the user's choice or <dataDir>/recordings, created on demand. UTF-8, no trailing slash.
std::string recordingsDir();
// A sibling folder of the recordings ("audio-recordings" ...): <dataDir>/name by default (unchanged); <chosen folder>/name once the user chose one.
std::string recordingsSub(const char* name);
// The default (<dataDir>/recordings) and the user's choice ("" = default); setting takes effect at once.
std::string defaultRecordingsDir();
std::string chosenRecordingsDir();
void setRecordingsDir(const std::string& path);
// Opens a folder in Finder / Explorer / the file manager (creates it first).
void revealFolder(const std::string& dir);

namespace kit {
void folderButton(const char* id, const std::string& dir);   // the same button for any folder
// A small "Open recordings folder" button; the tooltip shows the path. Put it next to the record controls of a mode.
void recordingsButton(const char* id = "rb", const char* sub = nullptr);   // sub: a sibling folder such as "audio-recordings"
}  // namespace kit
