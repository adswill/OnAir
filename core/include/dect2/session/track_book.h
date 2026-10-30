// TrackBook: turns "I just saw this key" observations from a decoder into Sessions (docs/MODE-EXPERIENCE.md, 2.1 rules "tracks = same
// ICAO/MMSI/serial across gaps shorter than a limit", "pager = capcode plus a 10 min window").
//   observe(): the first sight of a key begins a session, later sights update it (at most every updateMs), a session whose key was not seen
//   for gapMs is ended by sweep() (which observe() calls itself about once a second). Only queue pushes into the SessionStore: it is safe to call
//   from a telemetry poll or a decoder thread. One book per (mode, receiver); a book is thread safe.
#pragma once
#include "dect2/session/session_store.h"
#include <cstdint>
#include <memory>
#include <string>

namespace dect2::sess {

struct BookConfig {
    uint16_t mode = 0;
    uint16_t receiver = 0;
    Kind kind = Kind::Track;
    int64_t gapMs = 120000;      // a key not seen for this long ends its session
    int64_t updateMs = 5000;     // at most one update per key in this time (a flag change goes through at once)
    size_t maxOpen = 4096;       // safety cap on open sessions
};

class TrackBook {
public:
    explicit TrackBook(const BookConfig& cfg, SessionStore* store = nullptr);   // nullptr: global()
    ~TrackBook();
    // `s` carries key, title, who, alias, position, level, flags, eventCount... id and times are managed here. lastSeenMs: when the key was last heard
    // (0 = nowMs). Returns the session id (0 if the store refused).
    uint64_t observe(Session s, int64_t nowMs, int64_t lastSeenMs = 0);
    void sweep(int64_t nowMs);                 // ends the idle ones
    void endAll(int64_t nowMs);                // mode change / shutdown
    void link(const std::string& key, LinkType t, const std::string& ref, int64_t wallMs = 0);
    size_t openCount() const;
    void setConfig(const BookConfig& cfg);
    BookConfig config() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

int64_t wallNowMs();

}  // namespace dect2::sess
