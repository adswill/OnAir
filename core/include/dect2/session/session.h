// The Session record of the mode experience framework (docs/MODE-EXPERIENCE.md, 2.1): one call, thread or track that a mode heard, as the
// user will see it in the Items tab (Calls, Threads, Tracks). The decoders fill it, the SessionStore (session_store.h) keeps it.
// Variable text is std::string so long callsigns, DevEUI + DevAddr pairs and UTF-8 names are never cut.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace dect2::sess {

// What a Session is. The user sees Call, Thread, Track (3.4); the other kinds are reserved for later waves and stored the same way.
enum class Kind : uint8_t { Call = 1, Thread = 2, Track = 3, Pass = 4, Programme = 5, Device = 6, Reception = 7, Observation = 8 };
const char* kindName(Kind k);                // "call", "thread", "track", "pass", ...
const char* kindEventType(Kind k);           // "session.call", "session.thread", ...
const char* kindTitle(Kind k);               // what the user sees: "Calls", "Threads", ...

// Mirrors voice::Verdict in three states for storage: what the content is, not what the monitor did with it.
enum class Verdict : uint8_t { Clear = 0, Encrypted = 1, Unreadable = 2, Test = 3 };
const char* verdictName(Verdict v);

enum Flags : uint16_t {
    FlagEmergency = 1, FlagLateEntry = 2, FlagPriority = 4, FlagWatched = 8, FlagStarred = 16, FlagRecording = 32, FlagOrphanClosed = 64,
};

enum class LinkType : uint8_t { Audio = 1, Image = 2, Message = 3, Position = 4, Transcript = 5, Iq = 6, Telemetry = 7 };
const char* linkTypeName(LinkType t);

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

struct Session {
    uint64_t id = 0;                 // makeId(day, mode, sequence), assigned once at begin(); never changes
    uint16_t mode = 0;               // mode number (docs/MODE-NUMBERS.md)
    uint16_t receiver = 0;           // receiver instance index
    uint8_t kind = (uint8_t)Kind::Call;
    std::string key;                 // natural key: "TG 1234/slot1", "capcode 1234567", "ICAO 4CA7B1", "MMSI ..."
    std::string title;               // human title built by the adapter: "DMR TG 91 Worldwide, 12 s"
    std::string who[2], alias[2];    // source and target ids and aliases
    int64_t startWall = 0;           // UTC milliseconds
    int64_t endWall = 0;             // 0 = active
    uint32_t durationMs = 0;
    float levelDb = 0, levelPeakDb = 0, snrDb = 0, errRate = -1.f;
    uint64_t freqHz = 0;
    int32_t carrierId = -1;
    int16_t slot = -1;               // -1 when not applicable
    double lat = kNaN, lon = kNaN;   // NaN if none
    float altM = (float)kNaN;
    uint8_t verdict = (uint8_t)Verdict::Clear;
    uint16_t flags = 0;
    uint32_t eventCount = 0;

    bool active() const { return endWall == 0; }
    bool hasPosition() const { return std::isfinite(lat) && std::isfinite(lon); }
    bool encrypted() const { return verdict == (uint8_t)Verdict::Encrypted; }
};

struct SessionLink {
    uint64_t sessionId = 0;
    uint8_t type = (uint8_t)LinkType::Audio;
    std::string ref;                 // file path, gallery key, message id, "lat,lon" ...
    int64_t wall = 0;                // UTC milliseconds
};

// ---- ids. Layout (63 bits, always positive): day since 1970-01-01 UTC (15 bits) << 48 | mode (16 bits) << 32 | per-day per-mode sequence (32 bits).
// The mode is part of the id so a session id is unique across all modes in one table, and the sequence is monotonic per day and mode
// (the store keeps it), so two sessions with the same key in the same millisecond never collide. The id is assigned once; a later
// change of startWall (late entry, ACARS reassembly) does not touch it.
constexpr uint64_t makeId(uint32_t day, uint16_t mode, uint32_t seq) { return ((uint64_t)(day & 0x7FFF) << 48) | ((uint64_t)mode << 32) | seq; }
constexpr uint32_t idDay(uint64_t id) { return (uint32_t)(id >> 48) & 0x7FFF; }
constexpr uint16_t idMode(uint64_t id) { return (uint16_t)((id >> 32) & 0xFFFF); }
constexpr uint32_t idSeq(uint64_t id) { return (uint32_t)(id & 0xFFFFFFFFu); }
constexpr uint32_t dayOfWall(int64_t wallMs) { return wallMs <= 0 ? 0u : (uint32_t)(wallMs / 86400000); }

// The sequence counters of the ids: (day, mode) -> last sequence handed out. Thread safe (one mutex), never touches the disk.
class IdAllocator {
public:
    IdAllocator();
    ~IdAllocator();
    uint64_t next(int64_t startWallMs, uint16_t mode);
    void raise(uint32_t day, uint16_t mode, uint32_t seq);   // the store loads the highest sequence of each (day, mode) found in its database
    void clear();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::sess
