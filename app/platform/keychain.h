// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OS keychain for the voice key store's master key (dect2/voicekeys/key_persist.h: setSecretProvider). Each platform has one file that is
// compiled only on that system: keychain_mac.mm (Keychain, SecItem), keychain_win.cpp (Credential Manager, which stores under DPAPI),
// keychain_linux.cpp (libsecret, loaded with dlopen at run time; absent = no provider). Where no provider exists the store falls back to
// a passphrase the user types. Only a 32-byte random master key ever goes in; the keys themselves stay in the encrypted file.
#pragma once
#include <string>

namespace plat {

// Installs this system's keychain as the voice key store's secret provider. Returns its display name ("macOS Keychain",
// "Windows Credential Manager", "Secret Service (libsecret)"), or "" when the system has none (the passphrase fallback then applies).
// The probe writes and reads back nothing: it only checks that the service can be reached.
std::string installKeychainProvider();

}  // namespace plat
