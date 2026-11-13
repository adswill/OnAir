// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell tower positions from OpenCelliD (opencellid.org, data CC BY-SA 4.0): towers do not broadcast where they are, so a decoded cell's
// identity (MCC, MNC, TAC/LAC, cell id) is looked up in the public database with the user's own API key. Only those identifiers and the
// key are sent. Results are kept in a small CSV cache so repeat lookups and offline use need no network; "not found" is kept for a day.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dect2 {

struct OcidCellKey {
    int mcc = 0, mnc = 0;
    uint32_t lac = 0, cell = 0;
    std::string radio = "LTE";    // LTE, GSM, UMTS
    bool operator<(const OcidCellKey& o) const;
};

enum class OcidStatus { Found, NotFound, BadKey, Limit, Error };

struct OcidResult {
    OcidStatus status = OcidStatus::Error;
    double lat = 0, lon = 0;
    int range = 0, samples = 0;   // metres; measurements behind the position
    std::string message;          // the server's error text
};

// https://opencellid.org/cell/get?key=..&mcc=..&mnc=..&lac=..&cellid=..&radio=..&format=json (the key is percent-encoded)
std::string ocidUrl(const std::string& key, const OcidCellKey& k);
// {"lat":..,"lon":..,"range":..,"samples":..} or {"error":"..","code":N} (1 not found, 2 bad key, 6/7 limits)
OcidResult ocidParse(const std::string& body);

struct OcidEntry { OcidResult r; int64_t time = 0; };

class OcidCache {
public:
    static constexpr int64_t kNotFoundSecs = 86400;
    // a found position is kept for good; "not found" for a day; errors are never stored
    const OcidEntry* get(const OcidCellKey& k, int64_t now) const;
    void put(const OcidCellKey& k, const OcidResult& r, int64_t now);
    // an LTE cell of the same site (eNB = cell / 256, same operator) whose position is known: the sectors of a site share a mast
    const OcidEntry* findSite(const OcidCellKey& k) const;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
    size_t size() const { return m_.size(); }
private:
    std::map<OcidCellKey, OcidEntry> m_;
};

// at most one request per `interval` seconds
class OcidRateLimiter {
public:
    explicit OcidRateLimiter(double interval = 1.0) : iv_(interval) {}
    double waitFor(double now) const { return last_ < 0 ? 0 : std::max(0.0, last_ + iv_ - now); }
    bool tryTake(double now) { if (waitFor(now) > 0) return false; last_ = now; return true; }
private:
    double iv_, last_ = -1;
};

// The background lookups: request() never blocks; a worker thread asks the server (through `fetch`, which returns false when there is no
// answer) at most once a second and stores the answer in the cache, which is saved to `cachePath`.
class OcidLookup {
public:
    using Fetch = std::function<bool(const std::string& url, std::string& body)>;
    OcidLookup(Fetch fetch, std::string cachePath);
    ~OcidLookup();
    void setKey(const std::string& key);
    void request(const OcidCellKey& k);               // queued unless cached, queued already, or no key
    bool lookup(const OcidCellKey& k, OcidEntry& e);  // the cached answer
    std::string status();                             // the last outcome, for the status line
    size_t pending();
private:
    void run();
    Fetch fetch_;
    std::string path_, key_, status_;
    OcidCache cache_;
    std::deque<OcidCellKey> q_;
    std::set<OcidCellKey> queued_;
    std::set<OcidCellKey> probes_;     // other sectors asked for because the cell itself was not in the database
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false, halted_ = false;   // halted: bad key or daily limit; cleared by a new key
    std::thread th_;
};

} // namespace dect2
