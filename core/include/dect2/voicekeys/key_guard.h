// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Where the voice key file lives and what must never leave the computer with it. Header-only so the bug-report writer, the crash-log
// collector and their tests share one list. Nothing here touches key material: it only looks at file names.
//
// Rule: a file that holds voice keys (the encrypted store, a passphrase export, a CSV export) is never put in a bug-report zip, a crash
// log bundle, an IQ/recording metadata folder, or anything served by the web server or the remote API.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace dect2 {
namespace voicekeys {

inline const char* keyFolderName() { return "voicekeys"; }            // inside the app data folder
inline const char* keyFileName() { return "keys.oavk"; }              // the encrypted store
inline const char* keyFileExt() { return ".oavk"; }                   // the encrypted store and passphrase exports
inline const char* keyExportPrefix() { return "onair-voice-keys"; }   // the names the Keys window suggests for exports

// True when `path` (any separators, any case) names a file that may hold voice key material.
inline bool isKeyMaterialPath(const std::string& path) {
    std::string p;
    for (char c : path) p += (char)std::tolower((unsigned char)c);
    std::replace(p.begin(), p.end(), '\\', '/');
    const size_t slash = p.find_last_of('/');
    const std::string base = slash == std::string::npos ? p : p.substr(slash + 1);
    if (base.find(keyExportPrefix()) == 0) return true;
    const std::string ext = keyFileExt();
    if (base.size() >= ext.size() && base.compare(base.size() - ext.size(), ext.size(), ext) == 0) return true;
    const std::string dir = std::string("/") + keyFolderName() + "/";   // anything inside the key folder
    if (("/" + p).find(dir) != std::string::npos) return true;
    return false;
}

// Remove key-material paths from a list that is about to be bundled (bug report, crash logs, diagnostics).
template <class Vec> inline void dropKeyMaterial(Vec& names) {
    names.erase(std::remove_if(names.begin(), names.end(), [](const typename Vec::value_type& n) { return isKeyMaterialPath(n); }), names.end());
}

} // namespace voicekeys
} // namespace dect2
