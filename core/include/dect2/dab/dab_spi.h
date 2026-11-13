// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB Service and Programme Information (SPI, the electronic programme guide): ETSI TS 102 818 (XML), ETSI TS 102 371 (binary encoding and
// transport as MOT objects, user application type 0x007 "EPG" of FIG 0/13). Written from the public specifications only.
//
// Layers: MOT objects (dab_mot) -> SpiTree (the XML elements of the object, from the TS 102 371 tag-length-value encoding or from plain XML) ->
// SpiStore (services, programmes and the schedule: what the guide shows), with now / next and an XMLTV exporter.
#pragma once
#include "dect2/dab/dab_mot.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

// ---------------------------------------------------------------- tree
struct SpiNode {
    std::string name;                         // element name without namespace prefix
    std::map<std::string, std::string> attr;  // attributes, values as in the XML form (times ISO 8601, durations PTnHnMnS, bearers "dab:...")
    std::string text;                         // character data
    std::vector<SpiNode> kids;
    const std::string& get(const char* key) const;   // "" when absent
    const SpiNode* child(const char* nm) const;      // the first child of that name, or null
};

// TS 102 371 binary object -> tree (clause 4). Defensive: bounded depth, size and element count; false when the object is not a valid binary object.
bool spiParseBinary(const uint8_t* d, size_t n, SpiNode& top);
// Plain XML (TS 102 818) -> tree. Tolerant: prefixes are dropped, entities expanded, comments / processing instructions / DOCTYPE skipped.
bool spiParseXml(const char* d, size_t n, SpiNode& top);

// ---------------------------------------------------------------- time
// "2003-12-18T17:00:00Z", "2003-12-18T18:00:00+01:00" (also +01), fractions ignored. utc = seconds since 1970; offMin = the offset to UTC in minutes.
bool spiParseTime(const std::string& s, int64_t& utc, int& offMin);
// "PT1H30M", "PT45S", "P1DT2H", "PT90M" -> seconds
bool spiParseDuration(const std::string& s, int& seconds);
std::string spiFormatTime(int64_t utc, int offMin);           // ISO 8601 with offset ("Z" for 0)
int64_t spiMjdToUtc(int mjd, int hour, int minute, int second);

// ---------------------------------------------------------------- model
struct SpiMedia {
    std::string type;       // multimedia type: "logo_colour_square", "logo_unrestricted", "logo_colour_rectangle"; "" for other media
    std::string url;        // the url attribute (a MOT ContentName, a "file:" or "http(s):" URL)
    std::string mime;
    int w = 0, h = 0;
};

struct SpiService {
    uint32_t sid = 0;
    int ecc = -1;                       // from the bearer, -1 = unknown
    int eid = -1;
    std::string shortName, mediumName, longName, description, genre;
    std::vector<SpiMedia> media;
    std::string display() const { return !mediumName.empty() ? mediumName : !longName.empty() ? longName : shortName; }
};

struct SpiProgramme {
    std::string key;                    // "s<shortId>" or "c<crid>"
    std::string crid;
    uint32_t shortId = 0;
    int version = 0;
    std::string shortName, mediumName, longName, shortDesc, longDesc, lang;
    std::vector<std::string> genres;    // "3.6.8" style codes (classification scheme number first), or the URN of the XML form
    std::vector<SpiMedia> media;
    std::vector<std::string> links;
    std::vector<std::string> groups;    // the crids of the programme groups it belongs to
    bool recommended = false;
    std::string title() const { return !mediumName.empty() ? mediumName : !longName.empty() ? longName : shortName; }
    std::string description() const { return !longDesc.empty() ? longDesc : shortDesc; }
};

struct SpiEvent {
    uint32_t sid = 0;
    int64_t start = 0;                  // UTC
    int duration = 0;                   // seconds, 0 = not stated
    int offMin = 0;                     // local time offset of the broadcast, minutes
    std::string progKey;                // SpiProgramme::key
    bool operator<(const SpiEvent& o) const { return sid != o.sid ? sid < o.sid : start != o.start ? start < o.start : progKey < o.progKey; }
};

struct SpiGroup {
    std::string crid, name, desc, type;
    int numOfItems = 0;
};

struct SpiSnapshot {
    uint64_t seq = 0;
    std::map<uint32_t, SpiService> services;
    std::map<std::string, SpiProgramme> programmes;
    std::vector<SpiEvent> events;       // sorted by sid, start
    std::vector<SpiGroup> groups;
    int ecc = -1, eid = -1;             // the ensemble the information describes (SI "ensemble" element)
};

// What now and next say for a service
struct SpiNowNext {
    bool haveNow = false, haveNext = false;
    SpiEvent now, next;
};

struct SpiStats {
    uint64_t objects = 0, objectsBad = 0, binary = 0, xml = 0, compressed = 0;
    uint64_t siObjects = 0, piObjects = 0, giObjects = 0;
    uint64_t events = 0;                // events in the store
};

class SpiStore {
public:
    // One MOT object (header mode, or from a directory). true: it was SPI and accepted.
    bool addObject(const MotObject& o);
    // Already parsed
    bool addTree(const SpiNode& top, const MotObject* from = nullptr);
    void clear();
    uint64_t seq() const;
    SpiSnapshot snapshot() const;
    SpiStats stats() const;
    SpiNowNext nowNext(uint32_t sid, int64_t utcNow) const;
    // Drops the events that ended more than keepSec before `utcNow` and caps the store (events 60000, programmes 20000)
    void prune(int64_t utcNow, int keepSec = 3600);
    // XMLTV (xmltv.dtd): <channel id="<sid hex>.dab"> and <programme>. Only the events between from and to (UTC; to <= from: all).
    std::string toXmltv(int64_t from = 0, int64_t to = 0) const;
    // Persistence: a compact text form (versioned), and back. load() merges.
    std::string serialize() const;
    bool deserialize(const std::string& text);
    // The events of one service that are on at or after `from`, in order (at most max)
    std::vector<SpiEvent> upcoming(uint32_t sid, int64_t from, size_t max = 64) const;

private:
    void handleEpg(const SpiNode& top);
    void handleSi(const SpiNode& top);
    mutable std::mutex mu_;
    uint64_t seq_ = 0;
    SpiSnapshot s_;
    std::set<SpiEvent> ev_;
    SpiStats st_;
};

// ---------------------------------------------------------------- decoder
// MSC data groups of the SPI data service (packet mode or X-PAD) -> MOT directory mode objects -> SpiStore, with the acquisition progress.
class DabSpiDecoder {
public:
    DabSpiDecoder();
    void reset();                                       // forgets the carousel position; the store is kept (clearStore() empties it)
    void clearStore();   // the model and the pictures
    void dataGroup(const uint8_t* d, size_t n);
    void motObject(const MotObject& o);                 // objects from another MOT decoder (the PAD)
    void setObjectTap(std::function<void(const MotObject&)> cb) { tap_ = std::move(cb); }   // every object as it completes (tools, tests)
    SpiStore& store() { return store_; }
    const SpiStore& store() const { return store_; }
    struct Progress {
        int expected = 0;                               // objects the MOT directory announces (0 = no directory yet)
        int have = 0;                                   // of those, received
        double percent = 0;                             // 0..100; -1 when it cannot be told (no directory: objects arrive in header mode)
        uint64_t groups = 0;
        uint64_t changeCount = 0;                       // counts the changes of the store contents
    };
    Progress progress() const;
    MotStats motStats() const { return mot_.stats(); }
    // The pictures (MOT content type image) of the carousel by ContentName, still encoded: subtype per EN 301 234 (0 GIF, 1 JPEG, 2 BMP, 3 PNG); decode with
    // decodeSlideImage (dab_pad.h). Bounded to 8 MB, oldest first out. imageSeq() counts the pictures received.
    bool image(const std::string& name, int& subtype, std::vector<uint8_t>& data) const;
    uint64_t imageSeq() const { std::lock_guard<std::mutex> lk(mu_); return imageSeq_; }
    std::vector<MotObject> directoryEntries() const { std::lock_guard<std::mutex> lk(mu_); return dirEntries_; }   // the objects the MOT directory announces (headers only)
private:
    void note(const MotObject& o);
    MotDecoder mot_;
    SpiStore store_;
    std::set<std::string> dirNames_, haveNames_;
    size_t dirCount_ = 0;
    std::vector<MotObject> dirEntries_;
    struct Img { int subtype = 0; std::vector<uint8_t> data; };
    std::map<std::string, Img> images_;
    std::vector<std::string> imageOrder_;
    size_t imageBytes_ = 0;
    uint64_t imageSeq_ = 0;
    uint64_t changes_ = 0;
    bool resetReq_ = false;
    std::function<void(const MotObject&)> tap_;
    std::map<std::string, uint64_t> hashes_;            // ContentName -> content hash, to tell a repeat from an update
    mutable std::mutex mu_;
};

// The SpiStore -> disk. A file per ensemble in dir ("spi-<eid hex>.txt"), bounded: files older than maxAgeDays or beyond maxFiles are deleted.
bool spiSave(const SpiStore& st, const std::string& dir, uint16_t eid);
bool spiLoad(SpiStore& st, const std::string& dir, uint16_t eid, int64_t utcNow, int maxAgeDays = 14);
void spiCleanCache(const std::string& dir, int64_t utcNow, int maxAgeDays = 14, size_t maxFiles = 24);

// The classification scheme code of a genre ("urn:tva:metadata:cs:ContentCS:2005:3.6.8" or "3.6.8") -> "3.6.8"; "" when it has none.
std::string spiGenreCode(const std::string& href);

} // namespace dect2
