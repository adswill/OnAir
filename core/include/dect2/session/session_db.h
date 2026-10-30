// The session database: the indexed cold store of the mode experience framework (docs/MODE-EXPERIENCE.md, 2.2). A SQLite file
// (<dataDir>/sessions/sessions.db) with the same library as the message database (core/src/msgdb) but its own file and limits.
//
//  - Tables: sessions (primary key id, indexes on (mode, start), (mode, key), (start) ...), links, notes (id -> note and tags, edited in place),
//    meta (heartbeat), and an FTS5 index over title, ids, aliases and key.
//  - Upsert by id: begin inserts a row with end = 0, update and end replace it. No duplicate begin / end records.
//  - Crash recovery: the store writes a heartbeat; on open the rows that are still active and were last touched before this run are closed with
//    the orphanClosed flag and end = their last update time (once the heartbeat is stale, so a second program instance is not hurt).
//    A file that is damaged beyond SQLite's own recovery is moved aside and a new one is made.
//  - Limits: a row quota per mode (oldest finished, not starred, first), a size cap, an age limit for logs and one for position history.
//  - Reads use their own connection (the file is in WAL mode), so a long query never holds up the writer.
// JSONL is an export format only (session_store.h exportAs).
//
// Thread safety: every member may be called from any thread. Writes are meant for one writer thread (the SessionStore's), which brackets a batch
// with beginTx() and commitTx().
#pragma once
#include "dect2/session/session.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2::sess {

struct DbLimits {
    int64_t perModeRows = 200000;      // 0 = no limit
    int64_t maxBytes = 2ll << 30;      // 0 = no cap; the used part of the file
    int logDays = 90;                  // finished sessions older than this go (starred ones stay); 0 = keep
    int positionDays = 30;             // position links older than this go; 0 = keep
};

enum class Sort : uint8_t { ActiveThenNewest, Newest, Oldest, Longest, Title };

struct Query {
    int32_t mode = -1;                 // -1 = any mode
    int32_t receiver = -1;             // -1 = any receiver
    int kind = -1;                     // -1 = any
    int verdict = -1;                  // -1 = any
    std::string text;                  // full text over title, ids, aliases and key (words ANDed, trailing * = prefix); empty = all
    bool activeOnly = false;
    bool starredOnly = false;
    bool withAudio = false;            // has an Audio link
    int64_t since = 0, until = 0;      // start time window, UTC ms; 0 = open
    Sort sort = Sort::ActiveThenNewest;
    size_t offset = 0;
    size_t limit = 100;
};

struct Page {
    std::vector<Session> rows;
    int64_t total = 0;                 // rows matching the filter (not just this page)
    uint64_t gen = 0;                  // the store's generation counter when the page was read (0 from the bare database)
    double ms = 0;                     // time the query took
};

struct Note { std::string note, tags; };

struct DbStats {
    int64_t rows = 0, links = 0, bytes = 0;
    int64_t oldest = 0, newest = 0;    // start times, UTC ms
};

// A file a link points at (audio, IQ), with what the retention needs to decide
struct LibFile { uint64_t sessionId = 0; std::string ref; int64_t wall = 0; bool starred = false; };

struct PruneResult { int64_t byAge = 0, byQuota = 0, bySize = 0, positions = 0; };

struct OpenInfo {
    bool created = false;              // a new file
    bool quarantined = false;          // the old file was damaged: moved to quarantinedTo, a new one made
    std::string quarantinedTo;
    int orphansClosed = 0;
};

class SessionDb {
public:
    SessionDb();
    ~SessionDb();

    // Opens (creates) the file. ":memory:" for tests (one connection then). ownerOnly: the folder and file are created 0700 / 0600.
    bool open(const std::string& path, std::string* err = nullptr, OpenInfo* info = nullptr, bool ownerOnly = true);
    void close();
    bool isOpen() const;
    std::string path() const;

    // ---- writing (one writer thread)
    bool beginTx();
    bool commitTx();
    bool inTx() const;
    // updMs: the wall time of this write (kept as the row's last update, used by the crash recovery)
    bool upsert(const Session& s, int64_t updMs);
    bool addLink(const SessionLink& l);
    bool setNote(uint64_t id, const std::string& note, const std::string& tags);
    bool setStarred(uint64_t id, bool on);
    bool dropContentLinks(uint64_t id);   // audio, message, transcript and IQ links of a session (it turned out encrypted or unreadable)
    bool removeLinksByRef(const std::string& ref);
    bool setMeta(const std::string& key, const std::string& value);
    // Closes the sessions that are still active from before `startedMs` (the start of this run). When the heartbeat is younger than staleMs
    // another program instance may still be writing: nothing is done and -1 is returned. force: skip that check (tests, a known single instance).
    int recoverOrphans(int64_t nowMs, int64_t startedMs, int64_t staleMs, bool force = false);
    int closeActive(int64_t nowMs, int64_t sinceMs);   // a clean shutdown: the sessions written by this run that are still active end now (not an orphan)
    PruneResult prune(const DbLimits& lim, int64_t nowMs);
    int64_t deleteMode(uint16_t mode, std::vector<std::string>* refs = nullptr);   // rows, links and notes of one mode; refs: the link refs that were removed
    int64_t deleteAll(std::vector<std::string>* refs = nullptr);
    void compact();                    // incremental vacuum + WAL checkpoint

    // ---- reading (own connection)
    bool get(uint64_t id, Session& out) const;
    Page query(const Query& q) const;
    int64_t count(const Query& q) const;
    std::vector<SessionLink> links(uint64_t id) const;
    std::vector<SessionLink> linksOfType(uint8_t type, int32_t mode = -1) const;
    std::vector<LibFile> libraryFiles(uint8_t linkType) const;
    bool note(uint64_t id, Note& out) const;
    std::string meta(const std::string& key) const;
    DbStats stats() const;
    std::map<uint16_t, int64_t> countsByMode() const;
    // the highest sequence of every (day, mode) in the file, to start the id counters above them
    std::vector<std::pair<std::pair<uint32_t, uint16_t>, uint32_t>> maxSequences() const;
    bool integrityOk() const;

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::sess
