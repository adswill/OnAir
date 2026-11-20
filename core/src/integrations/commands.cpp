// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/integrations/commands.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace dect2::integrations {

namespace {
std::string trimLower(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    std::string o = s.substr(a, b - a);
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}
bool parseNumber(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* e = nullptr;
    v = std::strtod(t.c_str(), &e);
    return e != t.c_str() && *e == 0 && std::isfinite(v);
}
bool parseOnOff(const std::string& payload, bool& on) {
    const std::string t = trimLower(payload);
    if (t == "on" || t == "1" || t == "true" || t == "yes") { on = true; return true; }
    if (t == "off" || t == "0" || t == "false" || t == "no") { on = false; return true; }
    return false;
}
}  // namespace

bool parseHertz(const std::string& text, double& hz) {
    std::string t = trimLower(text);
    double mul = 1;
    auto cut = [&](const char* unit, double m) {
        const std::string u(unit);
        if (t.size() > u.size() && t.compare(t.size() - u.size(), u.size(), u) == 0) { t.erase(t.size() - u.size()); mul = m; return true; }
        return false;
    };
    if (!(cut("ghz", 1e9) || cut("mhz", 1e6) || cut("khz", 1e3) || cut("hz", 1) || cut("g", 1e9) || cut("m", 1e6) || cut("k", 1e3))) {}
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    double v;
    if (!parseNumber(t, v) || v < 0) return false;
    hz = v * mul;
    return true;
}

std::string commandFilter(const std::string& base) {
    if (base.empty() || base.size() > 200 || base.front() == '/' || base.back() == '/') return std::string();
    for (char c : base) if (c == '+' || c == '#' || c == 0) return std::string();
    return base + "/cmd/+";
}

std::string commandName(const std::string& base, const std::string& topic) {
    const std::string prefix = base + "/cmd/";
    if (base.empty() || topic.size() <= prefix.size() || topic.compare(0, prefix.size(), prefix) != 0) return std::string();
    const std::string n = topic.substr(prefix.size());
    return n.find('/') == std::string::npos ? n : std::string();
}

std::string runCommand(RemoteApiHost& host, const std::string& name, const std::string& payload) {
    std::string err;
    const std::string p = trimLower(payload);
    if (name == "start") {
        double v = -1;
        if (!p.empty() && (!parseNumber(p, v) || v < 0 || v != std::floor(v))) return "start takes no value or a device number";
        return host.start(p.empty() ? -1 : (int)v, err) ? std::string() : err;
    }
    if (name == "stop") return host.stop(err) ? std::string() : err;
    if (name == "frequency" || name == "samplerate") {
        double hz = 0;
        if (name == "samplerate" && (p == "auto" || p == "0")) return host.setSampleRateHz(0, err) ? std::string() : err;
        if (!parseHertz(payload, hz) || hz <= 0) return name + " needs a number such as 145.5MHz";
        return (name == "frequency" ? host.setFrequencyHz(hz, err) : host.setSampleRateHz(hz, err)) ? std::string() : err;
    }
    if (name == "gain") {
        double v;
        if (!parseNumber(p, v)) return "gain needs a number in dB";
        return host.setGainDb(v, err) ? std::string() : err;
    }
    if (name == "autogain") {
        bool on;
        if (!parseOnOff(payload, on)) return "autogain needs on or off";
        return host.setAutoGain(on, err) ? std::string() : err;
    }
    if (name == "mode") {   // "<family>" or "<family>/<sub id>"
        const size_t sl = p.find('/');
        double fam;
        if (!parseNumber(sl == std::string::npos ? p : p.substr(0, sl), fam) || fam < 0 || fam != std::floor(fam)) return "mode needs a family number, optionally /sub-id";
        return host.setMode((int)fam, sl == std::string::npos ? std::string() : p.substr(sl + 1), err) ? std::string() : err;
    }
    if (name == "volume") {
        std::string t = p;
        double div = 1;
        if (!t.empty() && t.back() == '%') { t.pop_back(); div = 100; }
        double v;
        if (!parseNumber(t, v)) return "volume needs 0 to 1 or a percentage";
        v /= div;
        if (v < 0 || v > 1) return "volume must be between 0 and 1 (or 0% and 100%)";
        return host.setVolume(v, err) ? std::string() : err;
    }
    if (name == "mute") {
        bool on;
        if (!parseOnOff(payload, on)) return "mute needs on or off";
        return host.setMute(on, err) ? std::string() : err;
    }
    if (name == "record") {
        if (p == "stop" || p == "off") return host.recordStop(err) ? std::string() : err;
        if (p == "iq" || p == "audio") return host.recordStart(p, err) ? std::string() : err;
        return "record needs iq, audio or stop";
    }
    return "unknown command";
}

}  // namespace dect2::integrations
