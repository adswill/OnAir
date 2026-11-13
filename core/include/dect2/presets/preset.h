// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-mode presets: named sets of the settings that matter for one mode (frequency, mode options, gain, bandwidth, squelch,
// audio, spectrum, radio, sample rate). Everything here is pure data and pure functions: (de)serialisation to a small versioned
// JSON (.onairpreset), merging of fields this version does not know, validation, and applying against what the radio can do.
// The app maps the flat setting keys to its own fields (app/state/presets_app.cpp); the store lives in its own file under the data dir.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "dect2/automation/json_mini.h"

namespace dect2::presets {

constexpr int kSchemaVersion = 1;   // written to every file; a newer file still loads (unknown fields are kept)

enum class Kind { Num, Flag, Text };
enum class Group { Tuning, Gain, Radio, Audio, Spectrum, Option, Location };

// One known setting key. Keys "opt.<name>" are mode options (any finite number, flag or short text); everything else is in settingSpecs().
struct SettingSpec {
    const char* key;
    Kind kind;
    double lo, hi;          // Num: accepted range (inclusive)
    Group group;
    bool sensitive;         // left out of exports unless the user ticks "include location"
};
const std::vector<SettingSpec>& settingSpecs();
const SettingSpec* findSpec(const std::string& key);   // also resolves "opt.*"

struct Value {
    Kind kind = Kind::Num;
    double num = 0;
    bool flag = false;
    std::string text;
    static Value n(double v) { Value x; x.kind = Kind::Num; x.num = v; return x; }
    static Value f(bool v) { Value x; x.kind = Kind::Flag; x.flag = v; return x; }
    static Value t(std::string v) { Value x; x.kind = Kind::Text; x.text = std::move(v); return x; }
    bool operator==(const Value& o) const { return kind == o.kind && num == o.num && flag == o.flag && text == o.text; }
};

struct Preset {
    int version = kSchemaVersion;
    std::string id;                          // stable within the store; built-ins are "builtin:<name>"
    std::string name;
    std::string mode;                        // ModeTuning::id of the mode it is for ("fm", "airband", ...)
    bool builtin = false;
    std::map<std::string, Value> values;     // known keys (validated on load and on apply)
    automation::json::Value extra;           // fields of the file this version does not know (top level), written back unchanged
    automation::json::Value extraValues;     // setting keys this version does not know, written back unchanged
};

// What the radio in use can do; applyPreset() skips what it cannot
struct RadioCaps {
    double minMhz = 0, maxMhz = 1e9;
    double minRate = 0, maxRate = 1e9;       // sample rate, Hz
    double minGainDb = 0, maxGainDb = 100;
    bool hasAutoGain = true, hasBiasTee = true;
    std::vector<std::string> antennas;       // empty: not selectable (an antenna in a preset is skipped)
};

struct ApplyResult {
    std::map<std::string, Value> apply;      // what the app should set now
    std::vector<std::string> notes;          // skipped because the radio cannot, or rejected (out of range / wrong type / unknown)
    int rejected = 0;                        // out of range or the wrong type
    int skipped = 0;                         // radio-incompatible, or a key this version does not know
};

// Validation of one value: empty string = fine, otherwise the reason
std::string validate(const std::string& key, const Value& v);
ApplyResult applyPreset(const Preset& p, const RadioCaps& caps);

struct ExportOptions { bool includeLocation = false; };
// Keys that could be a secret (key, token, password, secret, ...) are never written or read, in values or in extra, whatever the options
bool looksSecret(const std::string& key);

automation::json::Value toJson(const Preset& p, const ExportOptions& o = {});
// Lenient: unknown fields are kept, invalid known values are dropped with a note in err (still true); false only when it is not a preset at all
bool fromJson(const automation::json::Value& j, Preset& out, std::string& err);
std::string exportText(const Preset& p, const ExportOptions& o = {});   // the .onairpreset file
bool importText(const std::string& text, Preset& out, std::string& err);
// Edit a stored preset with a newer in-app view while keeping what the file had that we do not understand
void mergeUnknown(Preset& edited, const Preset& stored);

// The set of presets, favourites, recents and the per-mode default
struct Store {
    static constexpr size_t kMaxRecent = 8;
    std::vector<Preset> user;
    std::vector<std::string> recent;                   // ids, newest first
    std::map<std::string, std::string> defaultFor;     // mode -> preset id applied when entering the mode
    std::vector<std::string> favourite;                // ids (user and built-in)
    automation::json::Value extra;                     // unknown top-level fields of the store file

    const Preset* find(const std::string& id) const;   // user presets, then built-ins
    std::vector<const Preset*> forMode(const std::string& mode) const;   // favourites first, then by name; user's before built-ins
    std::vector<const Preset*> favourites() const;
    std::string add(Preset p);                         // gives it a unique id and a unique name within the mode; returns the id
    bool rename(const std::string& id, const std::string& name);   // refuses an empty or taken name, and built-ins
    bool remove(const std::string& id);                // refuses built-ins; clears recents, favourites and defaults that pointed at it
    bool setFavourite(const std::string& id, bool fav);
    bool isFavourite(const std::string& id) const;
    bool setDefault(const std::string& mode, const std::string& id);   // "" clears
    void touch(const std::string& id);                 // recent list
};

automation::json::Value storeToJson(const Store& s);
bool storeFromJson(const automation::json::Value& j, Store& out, std::string& err);
std::string storeText(const Store& s);
bool loadStoreText(const std::string& text, Store& out, std::string& err);
// Atomic write (temp file then rename). The caller picks the path: dataDir()/presets.json
bool saveStoreFile(const std::string& path, const Store& s, std::string& err);
bool loadStoreFile(const std::string& path, Store& out, std::string& err);   // a missing file is an empty store (true)

const std::vector<Preset>& builtinPresets();   // FM broadcast, airband AM, marine FM, ADS-B, ham SSB 20 m / 40 m, NOAA APT, DAB, DVB-T
std::string suggestedFileName(const Preset& p);   // "<name>.onairpreset" with path characters removed

}  // namespace dect2::presets
