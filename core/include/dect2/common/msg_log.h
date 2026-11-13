// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Text log of received messages (pagers, selcall, digital voice text, ...): one line per message with the time, mode, frequency, address
// and text. Used by the "Save..." button and the "Auto-save to file" switch of the message tabs. The time follows the Time display
// setting (UTC or local, dect2/common/time_fmt.h); the data itself is never changed. Pure code: tests call it with an explicit zone.
#pragma once
#include "time_fmt.h"
#include "u8file.h"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dect2::msglog {

struct Row {
    int64_t epoch = 0;     // seconds since 1970 (UTC), 0 = unknown
    std::string mode;      // e.g. "POCSAG 1200"
    double freqHz = 0;     // 0 = unknown
    std::string address;   // capcode, callsign, ID ... (may be empty)
    std::string text;      // the message
};

// A message on one line: control characters and line breaks become one space each.
inline std::string oneLine(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (c == '\n' || c == '\r' || c == '\t' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

// "2026-10-22 14:03:11 (UTC) | POCSAG 1200 | 466.2300 MHz | 1234567 | the text"
inline std::string formatLine(const Row& r, timefmt::Zone z, bool label) {
    char f[32];
    if (r.freqHz > 0) snprintf(f, sizeof f, "%.4f MHz", r.freqHz / 1e6); else snprintf(f, sizeof f, "-");
    std::string o = timefmt::format(r.epoch, z, timefmt::Style::DateClock, label);
    o += " | " + (r.mode.empty() ? std::string("-") : oneLine(r.mode));
    o += " | "; o += f;
    o += " | " + (r.address.empty() ? std::string("-") : oneLine(r.address));
    o += " | " + oneLine(r.text);
    return o;
}

inline std::string headerLine(timefmt::Zone z) {
    return std::string("# OnAir message log: time (") + timefmt::zoneName(z) + ") | mode | frequency | address | text";
}

// The whole list as the text of a file (header first).
inline std::string formatAll(const std::vector<Row>& rows, timefmt::Zone z, bool label) {
    std::string o = headerLine(z) + "\n";
    for (const Row& r : rows) o += formatLine(r, z, label) + "\n";
    return o;
}

inline bool saveAll(const std::string& path, const std::vector<Row>& rows, timefmt::Zone z, bool label, std::string& err) {
    std::ofstream o(u8file(path), std::ios::binary);
    if (!o) { err = "cannot write " + path; return false; }
    o << formatAll(rows, z, label);
    o.flush();
    if (!o) { err = "cannot write " + path; return false; }
    return true;
}

// "base.log" + 2026-10-22 -> "base-2026-10-22.log" (the day is in the display zone).
inline std::string dayPath(const std::string& base, int64_t epoch, timefmt::Zone z) {
    struct tm g;
    const time_t t = (time_t)(epoch > 0 ? epoch : 0);
    if (z == timefmt::Zone::Local) localTime(t, &g); else gmTime(t, &g);
    char d[16];
    snprintf(d, sizeof d, "-%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    const size_t slash = base.find_last_of("/\\");
    const size_t dot = base.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return base + d + ".log";
    return base.substr(0, dot) + d + base.substr(dot);
}

// Appends messages to a file as they arrive; a new file each day (rotation by name). The user picks the base name.
class AutoSaver {
public:
    void setBase(const std::string& base) { base_ = base; }
    const std::string& base() const { return base_; }
    // The file a message from this time goes to.
    std::string pathFor(int64_t epoch, timefmt::Zone z) const { return dayPath(base_, epoch, z); }
    // Adds one line to that day's file; makes the folder and writes the header when the file is new. False (and err) when it fails.
    bool append(const Row& r, timefmt::Zone z, bool label, std::string& err) {
        if (base_.empty()) { err = "no file chosen"; return false; }
        const std::string p = pathFor(r.epoch > 0 ? r.epoch : unixNow(), z);
        std::error_code ec;
        const std::filesystem::path fp = u8file(p);
        if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
        const bool fresh = !std::filesystem::exists(fp, ec);
        std::ofstream o(fp, std::ios::binary | std::ios::app);
        if (!o) { err = "cannot write " + p; return false; }
        if (fresh) o << headerLine(z) << "\n";
        o << formatLine(r, z, label) << "\n";
        o.flush();
        if (!o) { err = "cannot write " + p; return false; }
        return true;
    }
private:
    std::string base_;
};

} // namespace dect2::msglog
