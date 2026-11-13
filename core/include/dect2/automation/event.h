// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The normalised event every decoder hands to the automations: a type ("adsb.aircraft"), the time, the mode, the frequency and named fields.
#pragma once
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace dect2::automation {

inline double wallNow() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }   // Unix seconds

constexpr size_t kMaxFieldText = 2000;   // longer text is cut when it goes into an event

struct Value {
    enum T { Null, Num, Str, Bool } t = Null;
    double n = 0;
    bool b = false;
    std::string s;
    static Value number(double v) { Value x; x.t = Num; x.n = v; return x; }
    static Value string(std::string v) { Value x; x.t = Str; x.s = std::move(v); return x; }
    static Value boolean(bool v) { Value x; x.t = Bool; x.b = v; return x; }
    bool null() const { return t == Null; }
    bool truthy() const { return t == Bool ? b : t == Num ? n != 0 : t == Str ? !s.empty() : false; }
    // for templates and string functions: whole numbers without decimals
    std::string text() const {
        switch (t) {
        case Num: {
            char buf[48];
            if (!std::isfinite(n)) return "nan";
            if (std::fabs(n) < 1e15 && n == std::floor(n)) snprintf(buf, sizeof buf, "%.0f", n);
            else snprintf(buf, sizeof buf, "%.6g", n);
            return buf;
        }
        case Str: return s;
        case Bool: return b ? "true" : "false";
        default: return {};
        }
    }
};

struct Event {
    std::string type;           // "adsb.aircraft", "pager.message", ... (docs/automations.md lists them)
    double time = 0;            // Unix seconds, wall clock
    std::string mode;           // the mode that produced it ("ADS-B", "POCSAG" ...); may be empty
    double freqHz = 0;          // the receiver frequency when known, else 0
    std::map<std::string, Value> f;

    Event() = default;
    explicit Event(std::string t) : type(std::move(t)) {}
    Event& num(const std::string& k, double v) { f[k] = Value::number(v); return *this; }
    Event& str(const std::string& k, std::string v) { if (v.size() > kMaxFieldText) v.resize(kMaxFieldText); f[k] = Value::string(std::move(v)); return *this; }
    Event& flag(const std::string& k, bool v) { f[k] = Value::boolean(v); return *this; }
    Event& at(double t, const std::string& m, double hz) { time = t; mode = m; freqHz = hz; return *this; }
};

}  // namespace dect2::automation
