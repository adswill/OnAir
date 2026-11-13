// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sub-GHz sensors: the user's names for sensors ("Garden", "Freezer"), the hint to move a name to the new id a sensor takes after a battery change,
// and the sensor.reading event (automations, Integrations, Home Assistant). docs/SENSORS-HOME-3.9.md.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/automation/rules.h"
#include "dect2/sensors/sensors_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct SensorName {
    std::string name;
    std::string model, channel;   // from the key, kept so a hint can be found after the old sensor left the list
    int64_t lastSeen = 0;         // the last time the named sensor was heard (UTC s), 0 unknown
};

// Where a renamed sensor may have gone: the named sensor `fromKey` went silent and an unnamed one of the same model and channel (`toKey`) appeared
// just after. The UI offers to move the name.
struct SensorMoveHint { std::string fromKey, toKey, name; };

class SensorNames {
public:
    // model|id|channel (SensorDevice::key) split into its parts; false when the key is not of that form
    static bool splitKey(const std::string& key, std::string& model, std::string& id, std::string& channel);
    // sets (trimmed, at most 60 bytes, no control characters) or, for an empty name, removes
    void set(const std::string& key, const std::string& name, int64_t now = 0);
    std::string get(const std::string& key) const;   // "" when none
    bool has(const std::string& key) const { return byKey_.count(key) != 0; }
    void erase(const std::string& key) { byKey_.erase(key); }
    // moves the name of fromKey to toKey (the battery change); false when fromKey has no name
    bool move(const std::string& fromKey, const std::string& toKey, int64_t now = 0);
    // notes when each named sensor was last heard; true when something changed that is worth saving (at most every 10 min per sensor)
    bool touch(const SensorsTelemetry& t);
    // hints for the named sensors that went silent: an unnamed sensor of the same model and channel first heard after the named one was last heard
    // (within 6 h), while the named one has not been heard since. Each new sensor is offered for one name only, the closest in time.
    std::vector<SensorMoveHint> hints(const SensorsTelemetry& t, int64_t now) const;
    // what the receiver gets (setNames)
    std::map<std::string, std::string> plain() const;
    const std::map<std::string, SensorName>& all() const { return byKey_; }
    size_t size() const { return byKey_.size(); }
    // text form for the settings: one "key<TAB>name<TAB>lastSeen" line per sensor
    std::string serialize() const;
    void parse(const std::string& text);
private:
    std::map<std::string, SensorName> byKey_;
};

// The name to show: the user's name, else "model id" (or the model alone)
std::string sensorLabel(const SensorDevice& d, const SensorNames& names);

// sensor.reading: key, model, id, channel, kind, name (the user's name or "model id"), named, rssi_db, snr_db, battery_low, and every value the
// decoder gave (numbers as numbers: temperature_C, humidity, wind_avg_km_h, rain_mm, battery_ok ...; text as text). Mode "Sensors".
automation::Event sensorReadingEvent(const SensorDevice& d, const std::string& userName, double timeSec, double freqHz);

// A threshold alert for one sensor (Sensors view, Alert): fires when `field` goes above (or below) `value`, at most once per `cooldownSec` while it
// holds; notification and sound. The rule is new and enabled; the Automations window shows and edits it like any other.
automation::Rule sensorAlertRule(const std::string& key, const std::string& label, const std::string& field, bool above, double value, double cooldownSec = 1800);

}  // namespace dect2
