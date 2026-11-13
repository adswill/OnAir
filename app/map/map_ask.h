// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The map-server start-up question: when it is asked. Pure, so it can be tested.
#pragma once
#include <string>

// Asked at every start of the app, even after consent was given, unless the user chose "No, do not ask again" for this very version of
// the app (a different version string asks again), and never while transmitting.
inline bool mapShouldAsk(bool /*mapConsent*/, bool mapDeclined, const std::string& declinedVersion, const std::string& currentVersion, bool transmitting) {
    if (transmitting) return false;
    return !(mapDeclined && declinedVersion == currentVersion);
}
