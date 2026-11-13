// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Airband ATIS / VOLMET watcher (4.0). Works on the words of a transcribed (or typed) broadcast, no radio.
//  - atisInformation(): the ATIS letter in "information Kilo" / "ATIS India" / "information K" (phonetic alphabet, any case), 0 when none.
//  - AtisTracker: per channel the current letter; reports "new ATIS" when the letter changes (the weather or runway changed) and the age
//    of the information. A channel is an ATIS channel after two broadcasts with a letter, or when its label says ATIS / VOLMET.
//  - volmetStations(): the airports named in a VOLMET read-out ("London, Amsterdam, ..." or ICAO letters) for the schedule of a VOLMET channel.
// Why: the user tunes the ATIS channel and wants to know at a glance that "Kilo" is now "Lima", without listening to the loop.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

inline char atisInformation(const std::string& text) {
    static const char* ph[26] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet", "kilo", "lima", "mike",
                                 "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango", "uniform", "victor", "whiskey", "xray", "yankee", "zulu"};
    std::string t;
    for (char c : text) t += (char)std::tolower((unsigned char)c);
    // words
    std::vector<std::string> w; std::string cur;
    for (char c : t) { if (std::isalnum((unsigned char)c)) cur += c; else if (!cur.empty()) { w.push_back(cur); cur.clear(); } }
    if (!cur.empty()) w.push_back(cur);
    char found = 0;
    for (size_t i = 0; i + 1 < w.size(); i++) {
        if (w[i] != "information" && w[i] != "atis") continue;
        const std::string& n = w[i + 1];
        if (n == "x-ray") continue;
        for (int k = 0; k < 26; k++) if (n == ph[k] || (n == "alfa" && k == 0) || (n == "juliett" && k == 9)) found = (char)('A' + k);
        if (!found && n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') found = (char)(n[0] - 'a' + 'A');
        if (found) break;
    }
    return found;
}

class AtisTracker {
public:
    struct Chan { double freqHz = 0; char letter = 0; double sinceS = 0; int broadcasts = 0; int changes = 0; bool labelled = false; };
    // returns true when the letter changed from a known one
    bool feed(double freqHz, const std::string& label, const std::string& text, double nowS) {
        Chan& c = m_[key(freqHz)];
        c.freqHz = freqHz;
        std::string l; for (char ch : label) l += (char)std::tolower((unsigned char)ch);
        if (l.find("atis") != std::string::npos || l.find("volmet") != std::string::npos) c.labelled = true;
        const char x = atisInformation(text);
        if (!x) return false;
        c.broadcasts++;
        bool changed = false;
        if (c.letter && c.letter != x) { c.changes++; changed = true; }
        if (c.letter != x) { c.letter = x; c.sinceS = nowS; }
        return changed;
    }
    bool isAtis(double freqHz) const { auto it = m_.find(key(freqHz)); return it != m_.end() && (it->second.labelled || it->second.broadcasts >= 2); }
    const Chan* find(double freqHz) const { auto it = m_.find(key(freqHz)); return it == m_.end() ? nullptr : &it->second; }
    std::vector<Chan> atisChannels() const { std::vector<Chan> v; for (auto& kv : m_) if (kv.second.labelled || kv.second.broadcasts >= 2) v.push_back(kv.second); return v; }
    // seconds the current letter has been valid; -1 when unknown
    double ageS(double freqHz, double nowS) const { const Chan* c = find(freqHz); return c && c->letter ? nowS - c->sinceS : -1; }
private:
    static int64_t key(double hz) { return (int64_t)(hz / 1000.0 + 0.5); }   // 1 kHz: an 8.33 channel and its 25 kHz name are one
    std::map<int64_t, Chan> m_;
};

// ICAO codes (four letters, starting with a letter that is a region prefix) found in the text; "EGLL" and also spelled "echo golf lima lima"
inline std::vector<std::string> volmetStations(const std::string& text) {
    std::vector<std::string> out;
    auto add = [&](const std::string& s) { if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s); };
    // written ICAO codes must be in capitals in the source ("EGLL"), so the plain words "wind" or "over" are not taken for airports
    std::vector<std::string> w, raw; std::string cur, cr;
    for (char c : text) {
        if (std::isalnum((unsigned char)c)) { cur += (char)std::toupper((unsigned char)c); cr += c; }
        else if (!cur.empty()) { w.push_back(cur); raw.push_back(cr); cur.clear(); cr.clear(); }
    }
    if (!cur.empty()) { w.push_back(cur); raw.push_back(cr); }
    static const char* ph[26] = {"ALPHA", "BRAVO", "CHARLIE", "DELTA", "ECHO", "FOXTROT", "GOLF", "HOTEL", "INDIA", "JULIET", "KILO", "LIMA", "MIKE",
                                 "NOVEMBER", "OSCAR", "PAPA", "QUEBEC", "ROMEO", "SIERRA", "TANGO", "UNIFORM", "VICTOR", "WHISKEY", "XRAY", "YANKEE", "ZULU"};
    const std::string regions = "ABCDEFGHKLMNOPRSTUVWYZ";
    for (size_t i = 0; i < w.size(); i++) {
        bool alpha4 = raw[i].size() == 4, caps = true;
        for (char c : raw[i]) { if (!std::isalpha((unsigned char)c)) alpha4 = false; if (!std::isupper((unsigned char)c)) caps = false; }
        if (alpha4 && caps && regions.find(w[i][0]) != std::string::npos && w[i] != "METAR" && w[i] != "SPECI" && w[i] != "CAVOK" && w[i] != "TEMPO" && w[i] != "NOSIG") add(w[i]);
    }
    // spelled out
    for (size_t i = 0; i + 3 < w.size(); i++) {
        std::string code;
        for (int k = 0; k < 4; k++) { int f = -1; for (int j = 0; j < 26; j++) if (w[i + k] == ph[j]) f = j; if (f < 0) { code.clear(); break; } code += (char)('A' + f); }
        if (code.size() == 4 && regions.find(code[0]) != std::string::npos) { add(code); i += 3; }
    }
    return out;
}

} // namespace dect2
