// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Rules and scripts of the automations, their JSON files (recipes), and the text templates of the actions.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/automation/json_mini.h"
#include <map>
#include <string>
#include <vector>

namespace dect2::automation {

// One thing a rule does. type: notify, sound, record_iq, record_audio, tune, memory, log, webhook, screenshot, run. p: its settings; every value is a
// template ("{{callsign}} squawks {{squawk}}") filled from the event when the action runs.
struct ActionSpec {
    std::string type;
    std::map<std::string, std::string> p;
};
const std::vector<std::string>& actionTypes();

struct Rule {
    std::string id;                  // letters, digits, '-' and '_' (file name); made from the name when empty
    std::string name;
    std::string note;
    bool enabled = true;
    std::string trigger;             // event type, "adsb.*" (a family) or "*"
    std::string when;                // expression over the event (expr.h) for what the rows below cannot say; empty: always
    // the rows of the editor's condition builder: field, operator (== != < <= > >= contains startswith endswith matches in has) and value
    struct Cond { std::string field, op, value; };
    std::vector<Cond> conds;
    bool condAny = false;            // the rows are joined by "or" (else "and"); `when` is always "and"ed to them
    // limits
    double cooldownSec = 60;         // after it fired (for the same key) it stays quiet this long
    std::string cooldownKey;         // event field that tells things apart ("hex", "mmsi" ...): one cooldown each; empty: one for the rule
    int maxPerHour = 30;             // 0: no limit
    int windowFromHour = -1, windowToHour = -1;     // fires only from this local hour to that one (to may be before from: over midnight); -1: any time
    // geofence on the event's lat / lon
    enum Geo { GeoOff, GeoCircle, GeoPolygon } geo = GeoOff;
    bool geoHome = false;            // the circle is centred on the receiver position
    double geoLat = 0, geoLon = 0, geoRadiusKm = 20;
    std::string geoPolygon;          // "lat,lon; lat,lon; ..."
    bool geoInside = true;           // fire when inside (false: when outside)
    bool geoEnterOnly = false;       // fire only when the thing comes in (was outside, or new): needs cooldownKey to tell things apart
    std::vector<ActionSpec> actions;
};

struct Script {
    std::string id, name, note;
    bool enabled = true;
    std::vector<std::string> events; // event types / families it wants (like Rule::trigger); empty: "*"
    std::string code;                // Lua with function on_event(e)
};

struct Recipe {
    enum Kind { KindRule, KindScript } kind = KindRule;
    Rule rule;
    Script script;
    std::string title() const { return kind == KindRule ? rule.name : script.name; }
};

// The condition the engine evaluates: the builder rows joined, then `when` anded to them. "" when the rule has neither.
std::string conditionExpr(const Rule& r);
const std::vector<std::string>& condOps();

// ---- files ----
json::Value ruleToJson(const Rule& r);
json::Value scriptToJson(const Script& s);
json::Value recipeToJson(const Recipe& r);                        // {"format":"onair-automation","version":1,"kind":"rule"|"script", ...}
std::string recipeToText(const Recipe& r);                        // pretty JSON text
// Reads a recipe from text. Checks everything (types, limits, action names). A recipe from outside is never trusted: unless fromStore (the program's
// own saved rules) it comes in switched off (enabled = false). A "run" action in a file still does nothing until the user confirmed it (engine.h).
bool recipeFromText(const std::string& text, Recipe& out, std::string& err, bool fromStore = false);
std::string makeId(const std::string& name);                      // "Squawk 7700!" -> "squawk-7700"
bool validId(const std::string& id);
// Checks a rule on its own: expressions parse, actions are known and have what they need. Returns "" when fine, else the first problem.
std::string validateRule(const Rule& r);

// ---- templates ----
// {{name}} is replaced by the event field (or type, mode, freq, freq_mhz, time, time_iso, rule), nothing for an unknown name. The text put in is
// never looked at again. asJson: the values are escaped for the inside of a JSON string. The result is cut at 8000 characters.
std::string renderTemplate(const std::string& tpl, const Event& ev, const std::string& ruleName, bool asJson = false);
// one CSV field: quotes when needed; a value that starts with = + - @ gets a ' in front (spreadsheet formulas)
std::string csvField(const std::string& s);
// The URL and the JSON body of a webhook action. preset: generic (the body is the template, or a default JSON of the event), discord, slack, ntfy, telegram.
bool buildWebhook(const ActionSpec& a, const Event& ev, const std::string& ruleName, std::string& url, std::string& body, std::string& err);
// 64-bit FNV-1a hex of text (the confirmation of "run" actions)
std::string hashText(const std::string& s);
// The text that a "run" confirmation is for: rule id, program and arguments
std::string runFingerprint(const Rule& r);

}  // namespace dect2::automation
