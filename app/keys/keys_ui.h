// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Keys window and the per-call key status line (keys_ui.cpp). See docs/KEYS.md.
#pragma once
#include <string>

struct App;

void voiceKeysTick(App& a);                                 // once per frame: opens the key store at the first frame (keychain, legal notice, load)
void voiceKeysWindow(App& a);                               // the window, while a.voiceKeysOpen is set
// Draw a call's key status text (the core's string: "encrypted: add your key", "decrypted with key <name>", "key mismatch").
// Returns false (and draws nothing) when the call is in the clear. The add-your-key and mismatch texts get a button that opens the window.
bool voiceKeysStatus(App& a, const std::string& coreStatus);
void voiceKeysLink(App& a);                                 // a small "Voice keys..." button for panels that do not show a per-call status
