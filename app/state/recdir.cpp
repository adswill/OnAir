// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "recdir.h"
#include <cstdio>
#include <filesystem>
#include <mutex>
#include "../platform/platform.h"
namespace fs = std::filesystem;
static fs::path P(const std::string& u8) { return fs::path(reinterpret_cast<const char8_t*>(u8.c_str())); }

std::string defaultRecordingsDir() { return plat::dataDir() + "/recordings"; }
std::string chosenRecordingsDir() { return plat::basePrefs().getS("recordingsDir", ""); }
void setRecordingsDir(const std::string& path) { plat::basePrefs().setS("recordingsDir", recdir::normalise(path)); }

std::string recordingsDir() {
    static std::mutex m;
    static bool warned = false;
    std::lock_guard<std::mutex> lk(m);
    bool fb = false;
    const std::string r = recdir::resolve(plat::basePrefs().getS("recordingsDir", ""), defaultRecordingsDir(), &fb);
    if (fb && !warned) { warned = true; fprintf(stderr, "OnAir: the chosen recordings folder cannot be written; using %s\n", r.c_str()); }
    return r;
}

std::string recordingsSub(const char* name) {
    const std::string ch = recdir::normalise(plat::basePrefs().getS("recordingsDir", ""));
    return (ch.empty() ? plat::dataDir() : recordingsDir()) + "/" + name;
}

void revealFolder(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    std::string err;
#if defined(_WIN32)
    plat::spawnProgram("explorer.exe", {dir}, err);
#elif defined(__APPLE__)
    plat::spawnProgram("/usr/bin/open", {dir}, err);
#else
    plat::spawnProgram("xdg-open", {dir}, err);
#endif
}
