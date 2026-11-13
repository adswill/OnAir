// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Callsign lookup for the QSO panel (4.0): name, QTH, state, grid, zones of the station being worked, filled in automatically from
//  1. the callbook cache (a file in the user's own folder: what earlier lookups found),
//  2. earlier QSOs in the logbook (the most recent non-empty value of each field),
//  3. optionally QRZ.com XML or HamQTH, ONLY when the user switches it on and enters an account. The credentials stay in the local settings;
//     they are sent only to that service, in an HTTPS POST body (never in a URL), and never logged.
// Pure logic plus the client class; the network goes through an UpTransport (ham_upload.h), so tests use a fake one.
#pragma once
#include "dect2/ham/ham_log.h"
#include "dect2/ham/ham_upload.h"
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::ham {

struct CallbookEntry {
    std::string call, name, qth, state, county, country, grid, qslVia;
    double lat = 0, lon = 0;
    bool hasPos = false;
    int dxcc = 0, cqz = 0, ituz = 0;
    bool lotw = false, eqsl = false;
    std::string source;          // "QRZ", "HamQTH", "log", "cache"
    double fetchedUtc = 0;
    bool empty() const { return name.empty() && qth.empty() && grid.empty() && state.empty() && country.empty(); }
};

// ---- the services' answers ----
struct CallbookReply { bool ok = false; std::string sessionKey, error; CallbookEntry e; };
CallbookReply parseQrzXml(const std::string& xml);       // <QRZDatabase><Callsign>..</Callsign><Session><Key/><Error/></Session>
CallbookReply parseHamQthXml(const std::string& xml);    // <HamQTH><session><session_id/><error/></session><search>..</search>

// ---- from the logbook ----
CallbookEntry callbookFromLog(const std::vector<LogQso>& log, const std::string& call);
// fills the empty fields of a QSO from an entry (name, QTH, state, county, grid, country, zones, DXCC); returns the number of fields filled
int callbookFill(LogQso& q, const CallbookEntry& e);
// e's empty fields from more (e wins where both have a value)
void callbookMerge(CallbookEntry& e, const CallbookEntry& more);

// ---- the cache file: one line per call, tab separated ----
std::string callbookCacheLine(const CallbookEntry& e);
bool callbookCacheParse(const std::string& line, CallbookEntry& e);

enum class CallbookService { Off = 0, Qrz = 1, HamQth = 2 };
struct CallbookConfig {
    CallbookService service = CallbookService::Off;   // off by default: nothing is sent anywhere
    std::string user, password;
    double maxAgeDays = 180;                          // a cached entry older than this is looked up again (when online)
    bool ready() const { return service != CallbookService::Off && !user.empty() && !password.empty(); }
};
struct CallbookRequest { std::string url, body; };
CallbookRequest callbookLoginRequest(const CallbookConfig& c);
CallbookRequest callbookLookupRequest(const CallbookConfig& c, const std::string& session, const std::string& call);

class CallbookClient {
public:
    explicit CallbookClient(UpTransport t = curlTransport()) : tr_(std::move(t)) {}
    void setConfig(const CallbookConfig& c);
    CallbookConfig config() const;
    bool loadCache(const std::string& path);           // later stores go to the same file
    size_t cacheSize() const;
    // Without the network: the cache and the log, merged (cache first). Empty entry when nothing is known.
    CallbookEntry local(const std::string& call, const std::vector<LogQso>* log) const;
    // Blocking (run it on a worker thread): the cache when fresh, else the online service when it is switched on and has an account
    // (logs in when there is no session yet, and once more when the session has expired), stored in the cache. False + err when nothing was found.
    bool lookup(const std::string& call, double nowUtc, CallbookEntry& out, std::string& err);
    void forgetSession();                              // the account changed
    int requests() const { return requests_; }        // made since start (tests)
private:
    bool store(const CallbookEntry& e);
    mutable std::mutex mu_;
    UpTransport tr_;
    CallbookConfig cfg_;
    std::string session_, path_;
    std::map<std::string, CallbookEntry> cache_;
    int requests_ = 0;
};

}  // namespace dect2::ham
