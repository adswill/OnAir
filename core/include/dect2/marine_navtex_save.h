// Saves every NAVTEX message the receiver publishes as a text file, once: <dir>/<YYYY-MM-DD>/<HHMMSS>_<B1B2B3B4>.txt (UTC).
// The receiver already merges repeats of one message into one entry; this also remembers (as 64 bit hashes, so the memory stays small
// however long the session runs) what it has written, so a message heard again later, or an entry that is only updated, is not written twice.
#pragma once
#include "dect2/marine_tel.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace dect2 {

class NavtexSaver {
public:
    // dir: UTF-8 folder (created on demand). freqHz: the dial frequency for the header line, 0 = unknown.
    // Returns the number of files written now. After a failure error() says why; a failed message is not retried every frame.
    int update(const std::vector<NavtexMessage>& msgs, const std::string& dir, double freqHz) {
        int written = 0;
        for (const NavtexMessage& m : msgs) {
            if (m.textCut) continue;                 // shortened to keep the report small: not the message any more
            const uint64_t h = hashOf(m);
            if (seen_.count(h)) continue;
            if (seen_.size() > 100000) seen_.clear();   // a very long session: stay bounded (a rare double save beats unbounded growth)
            seen_.insert(h);
            if (write(m, dir, freqHz)) { written++; saved_++; }
            else failed_ = true;
        }
        return written;
    }
    const std::string& error() const { return err_; }
    bool failed() const { return failed_; }
    void clearError() { err_.clear(); failed_ = false; }
    uint64_t saved() const { return saved_; }
    const std::string& lastPath() const { return last_; }

    static std::string idOf(const NavtexMessage& m) {
        std::string id;
        for (char c : m.header) id += (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
        if (id.empty()) id = "XXXX";
        if (id.size() > 8) id.resize(8);
        return id;
    }

private:
    std::unordered_set<uint64_t> seen_;
    std::string err_, last_;
    bool failed_ = false;
    uint64_t saved_ = 0;

    static uint64_t hashOf(const NavtexMessage& m) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xFF; h *= 1099511628211ull; };
        mix(std::string(1, m.station)); mix(std::string(1, m.subject)); mix(std::to_string(m.number)); mix(m.text);
        return h;
    }
    static std::filesystem::path fsPath(const std::string& p) { return std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())); }

    bool write(const NavtexMessage& m, const std::string& dir, double freqHz) {
        time_t tt = (time_t)(m.rxTime > 0 ? m.rxTime : (int64_t)time(nullptr));
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        char day[16], hms[16], stamp[40];
        strftime(day, sizeof day, "%Y-%m-%d", &g);
        strftime(hms, sizeof hms, "%H%M%S", &g);
        strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S UTC", &g);
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path folder = fsPath(dir) / fsPath(day);
        fs::create_directories(folder, ec);
        if (ec) { err_ = "could not create " + dir + " (" + ec.message() + ")"; return false; }
        fs::path file;
        for (int n = 0; n < 100; n++) {      // two messages with the same second and id: number the second
            std::string name = std::string(hms) + "_" + idOf(m) + (n ? "_" + std::to_string(n + 1) : std::string()) + ".txt";
            file = folder / fsPath(name);
            if (!fs::exists(file, ec)) break;
        }
        std::ofstream f(file, std::ios::binary);
        if (!f) { err_ = "could not write to " + dir + " (disk full or read-only?)"; return false; }
        char hdr[400];
        snprintf(hdr, sizeof hdr, "NAVTEX %s\nreceived: %s\nfrequency: ", m.header.c_str(), stamp);
        f << hdr;
        if (freqHz > 0) { snprintf(hdr, sizeof hdr, "%.1f kHz", freqHz / 1e3); f << hdr; } else f << "unknown";
        snprintf(hdr, sizeof hdr, "\nstation %c, subject %c%s%s, number %s\ncharacters: %u, unreadable: %u (%.1f %%)%s\n----\n",
                 m.station, m.subject, m.subjectName.empty() ? "" : " ", m.subjectName.c_str(),
                 m.number >= 0 ? std::to_string(m.number).c_str() : "?", m.chars, m.errors, 100.f * m.cer,
                 m.complete ? "" : "\nINCOMPLETE: no NNNN received (transmission cut short)");
        f << hdr << m.text << "\n";
        f.flush();
        if (!f) { err_ = "could not write to " + dir + " (disk full?)"; return false; }
        last_ = (folder / file.filename()).string();
        return true;
    }
};

} // namespace dect2
