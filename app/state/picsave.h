// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure rules of saving pictures (no ImGui, no disk: tests/app/test_picsave.cpp covers them): where a picture goes, what it is called,
// what happens when the name is taken, the words of an error, and the "deleted, undo for a few seconds" list.
//   <folder>/<mode>/<YYYYmmdd-HHMMSS>-<mode>.png     (folder: the "Save pictures to" setting, default <home>/Pictures/OnAir)
#pragma once
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace picsave {

inline std::string defaultFolder(const std::string& home) { return (home.empty() ? std::string(".") : home) + "/Pictures/OnAir"; }

// a name that is safe on every system: letters, digits, '-' and '_'; anything else becomes one '-'; never empty
inline std::string safe(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (ok) o += (char)c;
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? "picture" : o;
}

inline std::string stamp(time_t t) {   // local time, sortable
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tmv);
    return b;
}

inline std::string trimSlash(std::string f) { while (f.size() > 1 && (f.back() == '/' || f.back() == '\\')) f.pop_back(); return f; }

inline std::string folderFor(const std::string& folder, const std::string& mode) { return trimSlash(folder) + "/" + safe(mode); }
inline std::string fileName(const std::string& mode, time_t t, const std::string& ext = ".png") { return stamp(t) + "-" + safe(mode) + ext; }
inline std::string buildPath(const std::string& folder, const std::string& mode, time_t t, const std::string& ext = ".png") {
    return folderFor(folder, mode) + "/" + fileName(mode, t, ext);
}

// a name that is not taken: "x.png", then "x-2.png", "x-3.png", ...
inline std::string uniquePath(const std::string& path, const std::function<bool(const std::string&)>& exists) {
    if (!exists(path)) return path;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string base = hasExt ? path.substr(0, dot) : path, ext = hasExt ? path.substr(dot) : std::string();
    for (int n = 2; n < 10000; n++) { const std::string p = base + "-" + std::to_string(n) + ext; if (!exists(p)) return p; }
    return path;
}

inline std::string parentOf(const std::string& path) {
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}

// the sentences shown to the user
inline std::string savedText(const std::string& path) { return "Saved to " + path; }
inline std::string tooltipText(const std::string& folder, const std::string& mode) { return "Save this picture to " + folderFor(folder, mode); }
inline std::string failText(const std::string& folder, bool createFailed) {
    return std::string(createFailed ? "OnAir cannot create the folder " : "OnAir cannot write to ") + folder +
           ". Choose another folder under Save pictures to (it may be read-only, full or not allowed).";
}

// "deleted, undo for a few seconds": the picture stays hidden for `window` seconds and only then really goes
class PendingDelete {
public:
    void mark(const std::string& id, double now) { for (const auto& p : p_) if (p.id == id) return; p_.push_back({id, now}); }
    bool hidden(const std::string& id) const { for (const auto& p : p_) if (p.id == id) return true; return false; }
    bool canUndo() const { return !p_.empty(); }
    // brings the most recent one back; returns its id ("" when nothing is pending)
    std::string undo() { if (p_.empty()) return ""; std::string id = p_.back().id; p_.pop_back(); return id; }
    // the ones whose time is up: removed from the pending list and returned (the caller deletes them for real)
    std::vector<std::string> expire(double now, double window) {
        std::vector<std::string> out, keep;
        std::vector<Entry> k;
        for (const auto& p : p_) { if (now - p.t >= window) out.push_back(p.id); else k.push_back(p); }
        p_ = k;
        return out;
    }
    std::vector<std::string> flush() { std::vector<std::string> o; for (const auto& p : p_) o.push_back(p.id); p_.clear(); return o; }   // on close: delete them all
    size_t count() const { return p_.size(); }
private:
    struct Entry { std::string id; double t; };
    std::vector<Entry> p_;
};

}  // namespace picsave
