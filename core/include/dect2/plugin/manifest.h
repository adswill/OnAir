// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The manifest.json of a plugin: parsing and validation. Hostile or malformed input is refused with a message, never crashes
// (the JSON reader is bounded in size and depth), and nothing in a manifest can name a path outside the plugin's own folder.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::plugin {

constexpr uint32_t kAbiVersion = 1;            // onair_plugin.h ONAIR_PLUGIN_ABI_VERSION
constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kMaxModes = 16, kMaxSettings = 64, kMaxOptions = 64;

enum Permission : uint32_t { PermNetwork = 1, PermFilesystem = 2, PermAudioOut = 4, PermRecord = 8, PermSpawn = 16 };
const char* permissionName(uint32_t bit);       // "network", "filesystem" ...
std::vector<std::string> permissionNames(uint32_t mask);

struct ModeDecl {
    std::string id, name, group;                // id: [a-z0-9_-]{1,32}; group is shown only as a hint (modes sit under "Plugins")
    double defaultHz = 100e6, bandwidthHz = 200e3;
    std::vector<double> sampleRates;            // rates the mode works with; empty = any
};

struct SettingDef {
    std::string key, label, type;               // type: slider, checkbox, combo, text
    double min = 0, max = 1, def = 0, step = 0;
    std::string defText;
    bool defFlag = false;
    std::vector<std::string> options;           // combo: the choices; the value stored is the index
};

struct Manifest {
    std::string id, name, version, author, license, homepage, description, kind;   // kind: mode, action, source, panel-lite
    std::string minOnAir;                       // "0.2.7" or empty
    uint32_t abi = 0;
    uint32_t permissions = 0;
    std::string libMac, libLinux, libWindows;   // file names inside the plugin folder
    std::vector<ModeDecl> modes;
    std::vector<SettingDef> settings;
    std::string libraryFile() const;            // for this platform ("" when the manifest has none for it)
};

bool validId(const std::string& id);
// -1 / 0 / 1; compares dotted numbers ("0.2.10" > "0.2.9"), ignores a suffix after '-' or '+'
int compareVersions(const std::string& a, const std::string& b);
// A version spec: "*" (every version), "1.2.0" (exactly) or "<=1.2.0" (this and older). Used by the store's revocation list.
bool versionSpecMatches(const std::string& spec, const std::string& version);
// false and err on any problem. hostVersion: the running OnAir version ("0.2.7") for minimum-version checks (empty skips it).
bool parseManifest(const std::string& text, Manifest& out, std::string& err, const std::string& hostVersion = {});

} // namespace dect2::plugin
