// The SessionStore: what the decoders talk to (docs/MODE-EXPERIENCE.md, 2.2 and 2.2a).
//
//  - Decoders and audio threads call begin / update / end / link. These only put a small item on a bounded queue (8192 items by default) and
//    return: no SQLite, no file I/O, no allocation of anything but the item's own strings. When the queue is full the item is dropped and a
//    counter goes up (dropped(), shown in More > Signal and as a header badge). Every item carries the whole session, so a dropped begin is
//    healed by the next update or end.
//  - One writer thread applies the queue: it keeps the hot window (the last 200 sessions per mode and receiver, in memory), writes the database
//    in transactions (every 250 ms or 256 items), closes sessions that were left open by a crash, prunes with the limits, and emits the
//    end-of-session summary events (session.call / session.thread / session.track, at most 50 per second per mode) on the automation bus.
//    Persistence never goes through the bus.
//  - The UI reads with query() / queryAsync() (own connection, committed data) or window() (the hot window), and re-queries only when the
//    generation counter gen(mode) changed (CachedList).
//  - Logging (session metadata) is on by default and can be switched off per mode; audio and IQ are never recorded by the store itself.
//    Personal identifiers are replaced by short tags unless the mode has "Keep real ids" (privacy.h).
//
// Thread safety: every member may be called from any thread.
#pragma once
#include "dect2/automation/engine.h"
#include "dect2/session/privacy.h"
#include "dect2/session/session.h"
#include "dect2/session/session_db.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::sess {

struct RetentionPolicy {
    int audioDays = 30;
    int64_t audioMaxBytes = 5ll << 30;
    int iqDays = 7;
    int64_t iqMaxBytes = 10ll << 30;
    int logDays = 90;                  // sessions (DbLimits::logDays)
    int positionDays = 30;             // position history
    int trashDays = 7;                 // files a user deleted by hand stay this long in the trash folder
};

struct StoreConfig {
    size_t queueMax = 8192;
    int commitMs = 250;
    int commitItems = 256;
    size_t hotWindow = 200;
    DbLimits limits;                   // per-mode quota and size cap; logDays / positionDays are set from the retention policy
    RetentionPolicy retention;
    int64_t orphanStaleMs = 5000;      // a heartbeat older than this means no other instance is writing
    bool defaultLogging = true;        // the session metadata of every mode is kept (the user can switch a mode off)
    int eventsPerSecPerMode = 50;
    int pruneEverySec = 60;
    int retentionEverySec = 3600;
    std::vector<std::string> audioRoots, iqRoots;   // folders the retention may delete library files from (nothing outside them is ever deleted)
    std::string trashDir;              // user deletions go here; empty = <db folder>/trash
    bool loadSalt = true;              // load / create the per-install salt in <dataDir>/keys
};

struct StoreStats {
    uint64_t enqueued = 0;             // items accepted
    uint64_t dropped = 0;              // items refused because the queue was full
    uint64_t applied = 0;              // items the writer thread has processed
    uint64_t commits = 0;
    uint64_t dbErrors = 0;
    uint64_t queued = 0;               // waiting now
    uint64_t eventsEmitted = 0;
    uint64_t eventsThrottled = 0;      // end-of-session events skipped by the rate limit
    uint64_t queriesRun = 0;
    uint64_t refusedLinks = 0;         // audio / IQ / text links of encrypted or unreadable sessions that were refused
    int64_t maxEnqueueNs = 0;          // longest begin / update / end / link call seen (a measure, 0 until a call)
};

class SessionStore {
public:
    SessionStore();
    ~SessionStore();

    // dataDir: the app's data folder. The database is <dataDir>/sessions/sessions.db, the salt <dataDir>/keys/session_salt.bin. Starts the threads.
    bool open(const std::string& dataDir, const StoreConfig& cfg = StoreConfig(), std::string* err = nullptr);
    // a file of your own (tests); ":memory:" for no file at all (the reader then shares the writer connection)
    bool openAt(const std::string& dbPath, const StoreConfig& cfg = StoreConfig(), std::string* err = nullptr);
    void close();                      // drains the queue, commits, stops the threads
    bool isOpen() const;
    const OpenInfo& openInfo() const;
    std::string dbPath() const;

    // ---- decoder side: enqueue only, never blocks
    // Assigns and returns the session id (s.id is ignored) and queues the begin. The mode, receiver, kind, key, startWall are taken from `s`;
    // startWall 0 = now. Works (hot window, ids) when logging is off for the mode. Pass the RAW ids and aliases: the writer thread replaces the
    // personal ones by short tags (privacy.h) in the key, who, alias and title, so an adapter never hashes anything itself.
    uint64_t begin(Session s);
    void update(const Session& s);     // the whole session as the decoder holds it now
    void end(Session s);               // endWall 0 = now; durationMs is filled when 0
    void link(uint64_t id, LinkType type, const std::string& ref, int64_t wallMs = 0);
    void setStarred(uint64_t id, bool on);
    void setNote(uint64_t id, const std::string& note, const std::string& tags);

    // ---- settings
    void setLogging(uint16_t mode, bool on);   // "Keep history" for one mode (the in-memory window works either way)
    bool logging(uint16_t mode) const;
    void setLoggingDefault(bool on);           // the master switch for the modes that have no choice of their own
    bool loggingDefault() const;
    void setModeName(uint16_t mode, const std::string& name);   // "DMR": the name used in the summary events and exports
    std::string modeName(uint16_t mode) const;
    void setLimits(const DbLimits& l);
    void setRetention(const RetentionPolicy& r);
    RetentionPolicy retention() const;
    void setEngine(automation::Engine* eng);   // where the summary events go (default: automation::global())
    void setEventsPerSecond(int n);
    void setLibraryRoots(const std::vector<std::string>& audio, const std::vector<std::string>& iq);   // where the retention may delete files

    // ---- the visible state and the settings
    // "Logging: ON" / "Logging: OFF": what the mode header shows (docs/MODE-EXPERIENCE.md, 2.7). The first-run notice explains what is saved and where;
    // it is needed until the user has acknowledged it once.
    std::string loggingLabel() const;
    bool firstRunNoticeNeeded() const;
    void acknowledgeFirstRunNotice();
    // The settings as key=value lines (logging per mode, limits, retention, the notice). open() loads <dataDir>/sessions/settings.txt and the writer
    // thread saves it a moment after a change.
    std::string saveSettings() const;
    void loadSettings(const std::string& text);

    // ---- reading
    std::vector<Session> window(uint16_t mode, int receiver = -1, size_t max = 200) const;   // the hot window, newest first
    uint64_t gen(uint16_t mode) const;         // changes whenever a session of this mode changed (sessionGen of the telemetry)
    uint64_t genAll() const;
    Page query(const Query& q) const;          // committed rows, on the caller's thread (not the UI's); Page::gen is gen() when it was read
    void queryAsync(const Query& q, std::function<void(Page&&)> done);   // on the reader thread
    bool get(uint64_t id, Session& out) const;
    std::vector<SessionLink> links(uint64_t id) const;
    bool note(uint64_t id, Note& out) const;
    DbStats dbStats() const;

    // ---- status
    StoreStats stats() const;
    uint64_t dropped() const;
    // Waits until everything queued so far is written and committed (tests, shutdown, export).
    void flush();
    // Test hook: the writer thread stops taking items from the queue (so the drop counter can be checked exactly) and goes on when released.
    void pauseWriter(bool on);

    // ---- export (JSONL and the others are export formats only)
    // format: "csv", "json", "jsonl". anonymise: ids, aliases, notes and the receiver location are left out. Text is escaped (privacy.h).
    std::string exportAs(const Query& q, const std::string& format, bool anonymise = false) const;

    // ---- retention and deletion
    // Runs the cleanup now (the thread does it every retentionEverySec): sessions by age and quota, position history, library files by age and size.
    // Files of starred sessions are kept. Returns the number of library files deleted.
    int runRetention();
    // "Delete all data for this mode" / "Delete everything": rows, notes, links, and the library files the links point at (inside the audio and IQ roots).
    // deleteEverything also makes a new salt. Returns the number of sessions removed.
    int64_t deleteMode(uint16_t mode);
    int64_t deleteEverything();

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

// The program-wide store (opened by the application at start; closed, never destroyed).
SessionStore& global();

// A page of the Items list for the UI: asks the store again only when the query or the generation counter changed, and never on the UI thread.
// Call page() every frame; it returns the last page read. loading() is true while a refresh is on its way.
class CachedList {
public:
    explicit CachedList(SessionStore& store);
    ~CachedList();
    void setQuery(const Query& q);     // a different query invalidates the page
    const Query& query() const;
    std::shared_ptr<const Page> page();   // never null; the shared pointer keeps the page alive while a refresh replaces it
    bool loading() const;
    uint64_t refreshes() const;        // pages asked for so far (a test checks this stays at one per generation change)
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

}  // namespace dect2::sess
