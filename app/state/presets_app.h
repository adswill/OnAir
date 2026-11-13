// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The app side of the per-mode presets (core/include/dect2/presets/preset.h): capturing the settings of the mode on screen, applying a
// preset to them, the store file (dataDir()/presets.json), the per-mode default and the toolbar "Presets" drop-down.
#pragma once
#include <functional>
#include <string>
#include "dect2/presets/preset.h"

struct App;

namespace presetsapp {

// A setting that lives outside the tuner (the audio popup, the antenna choice, a mode's own options) joins the presets by registering a
// getter and a setter for its key (see settingSpecs()); the key then is saved, exported and applied like the built-in ones.
struct Provider {
    std::string key;
    std::function<bool(App&, dect2::presets::Value&)> get;    // false: not available in this mode, left out
    std::function<void(App&, const dect2::presets::Value&)> set;
};
void registerProvider(Provider p);

std::string modeKey(const App& a);                                  // ModeTuning::id of the mode on screen ("fm", "airband", "dvbt", ...)
dect2::presets::Preset capture(App& a, const std::string& name);    // the mode's current settings
dect2::presets::ApplyResult apply(App& a, const dect2::presets::Preset& p);   // validates, skips what the radio cannot, sets the rest, returns the notes
dect2::presets::Store& store();                                      // loaded on first use
void save();
void resetModeToDefaults(App& a);                                    // the mode's ModeTuning defaults; also clears nothing in the store
void applyModeDefault(App& a);                                       // the mode's default preset if it has one (called when a mode is entered)
void toolbarButton(App& a);                                          // the "Presets" drop-down (save as, recent, favourites, manage, import / export)

}  // namespace presetsapp
