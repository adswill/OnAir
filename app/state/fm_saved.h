// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM saved-stations library: the list the user builds by hand, separate from the band-scan results.
// The preferences store mhz <US> name per record, records joined by <RS>. Those separators survive
// Prefs::setS(), which turns '|', newlines and carriage returns into spaces.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct FmSavedStation {
    std::string name;
    double mhz = 0;
};

// Per-receiver editor scratch. Not written to the preferences. One copy per App, so two receivers
// do not share a filter, a rename box, or a delete confirmation.
struct FmSavedEdit {
    int editing = -1;
    char name[64] = "";
    char filter[64] = "";
    bool focus = false;
    int confirmDel = -1;
    bool askConfirm = false;
};

inline std::string fmSavedTrim(const std::string& s) {
    const size_t b = s.find_first_not_of(" -");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" -");
    return s.substr(b, e - b + 1);
}

inline std::string fmSavedEncode(const std::vector<FmSavedStation>& v) {
    std::string st;
    for (const auto& r : v) {
        std::string name = r.name;
        for (char& c : name) if (c == '\x1e' || c == '\x1f' || c == '\n' || c == '\r' || c == '|') c = ' ';
        char b[32];
        snprintf(b, sizeof b, "%.1f", r.mhz);
        st += b;
        st += '\x1f';
        st += name;
        st += '\x1e';
    }
    return st;
}

inline std::vector<FmSavedStation> fmSavedDecode(const std::string& st) {
    std::vector<FmSavedStation> out;
    size_t pos = 0;
    while (pos < st.size()) {
        size_t rec = st.find('\x1e', pos);
        if (rec == std::string::npos) rec = st.size();
        const std::string line = st.substr(pos, rec - pos);
        pos = rec + 1;
        const size_t us = line.find('\x1f');
        if (us == std::string::npos) continue;
        FmSavedStation r;
        r.mhz = atof(line.substr(0, us).c_str());
        r.name = line.substr(us + 1);
        if (r.mhz >= 76.0 && r.mhz <= 108.01) out.push_back(r);   // 76 MHz: the band scan reaches down to extended FM (Brazil) and Japan's band
    }
    return out;
}

inline std::string fmSavedLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return s;
}

inline bool fmSavedMatches(const FmSavedStation& st, const char* filter) {
    if (!filter || !filter[0]) return true;
    char fl[16];
    snprintf(fl, sizeof fl, "%.1f", st.mhz);
    return fmSavedLower(st.name + ' ' + fl).find(fmSavedLower(filter)) != std::string::npos;
}

inline bool fmSavedRename(std::vector<FmSavedStation>& v, int i, const std::string& raw) {
    if (i < 0 || i >= (int)v.size()) return false;
    v[(size_t)i].name = fmSavedTrim(raw);
    return true;
}

inline bool fmSavedRemove(std::vector<FmSavedStation>& v, int i) {
    if (i < 0 || i >= (int)v.size()) return false;
    v.erase(v.begin() + (size_t)i);
    return true;
}

inline void fmSavedValidateEdit(FmSavedEdit& e, int n) {
    if (e.editing < -1 || e.editing >= n) e.editing = -1;
    if (e.confirmDel < -1 || e.confirmDel >= n) { e.confirmDel = -1; e.askConfirm = false; }
}
