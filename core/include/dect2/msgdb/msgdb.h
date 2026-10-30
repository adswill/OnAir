// The unified decoded-message database: every decoder's messages (ADS-B, ACARS, VDL2, HFDL, pagers, AIS, APRS, sondes, LoRa, BLE, drone ID, DSC ...) in one
// SQLite file with full-text search. Voice audio and encrypted payloads are never stored (add() refuses them). Thread safe.
#pragma once
#include "dect2/automation/event.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2::msgdb {

struct Record {
    int64_t id = 0;
    double time = 0;          // Unix seconds, UTC
    std::string type;         // event type: "acars.message"
    std::string mode;         // "ACARS"
    double freqHz = 0;
    std::string ident;        // who: tail, ICAO hex, MMSI, call sign, RIC ...
    std::string text;         // what: the message text or a one-line summary (searched)
    double lat = 0, lon = 0;
    bool hasPos = false;
    std::string extra;        // all fields as "key=value" lines (searched)
    std::string source;       // the recording / IQ file this was decoded from (event field file / source_file / recording), empty for a live receiver
    int64_t sessionId = 0;    // the session (core/include/dect2/session) this record summarises: View > Messages jumps to the mode's Items tab with it; 0 = none
};

struct Filter {
    std::string text;         // full-text query: words are ANDed, a trailing * is a prefix; empty = all
    std::string mode;         // exact mode name, empty = any
    std::string type;         // exact event type, empty = any
    std::string ident;        // substring of the identity, empty = any
    double since = 0, until = 0;   // Unix seconds, 0 = open
    bool posOnly = false;
    double freqLo = 0, freqHi = 0;   // Hz, 0 = open (a record without a frequency never matches a set range)
    std::string source;       // substring of the source file name, empty = any
    int64_t sessionId = 0;    // only the record of this session (0 = any)
    bool relevance = false;   // order by text relevance (bm25: identity > message > extra) instead of newest first; needs text
    int limit = 500;
    int offset = 0;
    bool oldestFirst = false;
};

struct Limits {
    int64_t maxRows = 1000000;        // 0 = no limit
    double maxAgeDays = 0;            // 0 = keep forever
    int64_t maxBytes = 512ll << 20;   // size cap of the file, 0 = no cap; the oldest rows go first
    int64_t maxRowsPerFamily = 0;     // 0 = no quota; else no event family ("acars", "session" ...) keeps more rows than this, the oldest of that family go first,
                                      // so a chatty family cannot push the others out of the shared cap
};

struct Stats { int64_t rows = 0; int64_t bytes = 0; double oldest = 0, newest = 0; };

// The words of a user query as the index sees them: lower-case, split on anything that is not a letter or digit (non-ASCII letters stay
// inside words), a trailing * on a word marks a prefix. Used for the FTS query and shown by tests.
struct Token { std::string word; bool prefix = false; };
std::vector<Token> tokenize(const std::string& query);

// true when the event type is a decoded-message type the database keeps (not signal.*, receiver.*, satellite.*, time.*, gnss.*)
bool wantsType(const std::string& type);
// Converts an event to a record; false when it must not be stored (voice, encrypted, or a type outside the set).
bool fromEvent(const automation::Event& ev, Record& out);

class Db {
public:
    Db();
    ~Db();
    bool open(const std::string& path, std::string* err = nullptr);   // creates the file and the tables; ":memory:" for tests
    void close();
    bool isOpen() const;
    std::string path() const;

    // Synchronous insert. Refuses (false) records of kind voice/audio or marked encrypted. Does not prune.
    bool add(const Record& r);
    // Queue for the writer thread (never blocks the decoder); the thread inserts in batches and prunes now and then. start() before use.
    void enqueue(const automation::Event& ev);
    void start();
    void stop();   // flushes the queue

    void setLimits(const Limits& l);
    Limits limits() const;
    int64_t prune();              // applies the limits; returns the rows deleted
    void rebuildIndex();          // rebuilds and compacts the full-text index from the stored rows (after a crash or an upgrade)
    void clear();                 // deletes everything

    std::vector<Record> query(const Filter& f) const;
    int64_t count(const Filter& f) const;
    Stats stats() const;
    std::vector<std::string> modes() const;   // distinct modes in the store

    // Export of everything matching f (limit ignored): "csv", "json" or "adif" (rows with a call-sign-like identity).
    std::string exportAs(const Filter& f, const std::string& format) const;

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The program-wide store (opened by the application at start).
Db& global();

}  // namespace dect2::msgdb
