// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Commands received over MQTT (Integrations > MQTT > "Accept commands", off by default): <base>/cmd/<name> with the value as the payload.
// They run on the same receiver controls as the remote API (RemoteApiHost), so the app and the headless server share them. Receive controls only:
// there is no transmit command here and none can be added by a payload.
//   start [device index]   stop   frequency 145.5MHz   samplerate 2.4MHz | auto   gain 30   autogain on|off   mode <family>[/<sub>]
//   volume 0.5 | 50%   mute on|off   record iq|audio|stop
#pragma once
#include "dect2/net/remote_api.h"
#include <string>

namespace dect2::integrations {

// "" when it ran, else a short reason (unknown command, a value that is not understood, or the host's own error). Never throws.
std::string runCommand(RemoteApiHost& host, const std::string& name, const std::string& payload);

// The numbers' text forms, public for the tests: "145.5MHz", "7100 kHz", "1.09 GHz", "2400000" (a plain number is Hz). false: not a number.
bool parseHertz(const std::string& text, double& hz);

// The topic filter the Hub subscribes to for a base: "<base>/cmd/+". Empty when the base is not usable (empty, wildcards, leading or trailing /).
std::string commandFilter(const std::string& base);
// The command name of a received topic, or empty when it is not exactly "<base>/cmd/<name>".
std::string commandName(const std::string& base, const std::string& topic);

}  // namespace dect2::integrations
