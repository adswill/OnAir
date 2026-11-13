// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The events the receivers hand to the automations, with their fields: one table that the editor's drop-downs, the sample events of the
// Test button and docs/automations.md are made from.
#pragma once
#include "dect2/automation/event.h"
#include <vector>

namespace dect2::automation {

struct FieldInfo {
    const char* name;
    char kind;            // 'n' number, 's' text, 'b' yes/no
    const char* what;
    const char* sample;   // a value for the sample event
};
struct EventInfo {
    const char* type;
    const char* title;    // shown in the editor
    const char* source;   // the mode(s) that emit it
    const char* mode;     // sample Event::mode
    double sampleFreqHz;
    std::vector<FieldInfo> fields;
};

const std::vector<EventInfo>& eventCatalog();
const EventInfo* findEventInfo(const std::string& type);
// An event with every field of the type filled with its sample value (time = now). An unknown type gives an event with no fields.
Event sampleEvent(const std::string& type, double now);
// true when `trigger` ("adsb.aircraft", "adsb.*" or "*") covers the event type
bool triggerMatches(const std::string& trigger, const std::string& type);

}  // namespace dect2::automation
