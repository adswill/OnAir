// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Lua scripts of the automations, in a sandbox. A script defines function on_event(e); e is a table with type, mode, freq (Hz), time and the event's
// fields. Available: base functions (no load, dofile, require, collectgarbage), table, math, utf8 and a reduced string library (no pattern matching:
// find is plain text only; use contains / starts_with / ends_with / matches (glob) / replace). No io, os, package, debug, coroutine.
// Actions: notify(title, text), sound(file), record_iq(seconds), record_audio(seconds), tune(freq_hz, mode), memory(name), webhook(url, text_or_table),
//          log(text), log_row(file, a, b, ...), screenshot(); helpers: distance_km(lat, lon), bearing_deg(lat, lon), now(), json(table), hour().
// Limits per call: 1 million VM instructions and 250 ms; memory 16 MB for the whole script; at most 20 actions per call; text 8000 characters.
#pragma once
#include "dect2/automation/event.h"
#include "dect2/automation/rules.h"
#include <map>
#include <memory>
#include <string>

namespace dect2::automation {

class ScriptApi {
public:
    virtual ~ScriptApi() = default;
    // an action of the script: kind as in ActionSpec::type (never "run"); false and detail when it did not work
    virtual bool act(const std::string& kind, const std::map<std::string, std::string>& p, std::string& detail) = 0;
    virtual void note(const std::string& text) = 0;                    // log(text) and error messages
    virtual bool station(double& lat, double& lon) = 0;                // the receiver position, false when not set
    virtual double now() = 0;
    virtual int tzOffsetSec() = 0;
};

class ScriptHost {
public:
    ScriptHost(const Script& s, ScriptApi* api);
    ~ScriptHost();
    bool loaded() const { return err_.empty(); }
    const std::string& error() const { return err_.empty() ? lastRunError_ : err_; }   // the load error, else the last run-time error
    // Calls on_event(e). False with error() filled when it failed (the script stays loaded and gets the next event).
    bool onEvent(const Event& ev);
    int errors() const { return errors_; }
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    std::string err_, lastRunError_;
    int errors_ = 0;
};

// Checks the syntax only (runs nothing); "" when fine, else Lua's message.
std::string checkScriptSyntax(const std::string& code);

}  // namespace dect2::automation
