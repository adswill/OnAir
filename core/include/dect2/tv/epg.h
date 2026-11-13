// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The programme guide (EPG) database of the TV recorder, written from ETSI EN 300 468 (SI in DVB systems) and ETSI TS 102 323 (CRIDs).
//   - EIT sections (table 0x4E/0x4F present/following, 0x50..0x6F schedule; actual and other transport stream) parsed with every descriptor
//     the guide needs: short and extended event (with the character set prefix of the text), content (genre), parental rating, content identifier
//     (programme and series CRID), plus free_CA_mode and running_status;
//   - TOT/TDT (time and local time offset descriptors, so a start time can be shown in the broadcaster's time zone with its summer time change);
//   - EpgDb: events per service (original_network_id, transport_stream_id, service_id), bounded and expired, searchable, saved to JSON;
//   - EpgCollector: transport stream packets in, EpgDb filled (PIDs 0x10 NIT, 0x11 SDT, 0x12 EIT, 0x14 TDT/TOT).
// Independent of the Rust/C++ parser pair in ts_proto: this layer only adds what the guide needs and uses tsp::crc32 and dvbText.
#pragma once
#include "dect2/tv/tvjson.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct ServiceKey {
    int onid = 0, tsid = 0, sid = 0;
    bool operator<(const ServiceKey& o) const { return onid != o.onid ? onid < o.onid : tsid != o.tsid ? tsid < o.tsid : sid < o.sid; }
    bool operator==(const ServiceKey& o) const { return onid == o.onid && tsid == o.tsid && sid == o.sid; }
};

// One programme.
struct EpgItem {
    int eventId = 0;
    int64_t start = 0;                 // seconds since 1970, UTC
    int duration = 0;                  // seconds
    int running = 0;                   // running_status: 0 undefined, 1 not running, 2 starts in a few seconds, 3 pausing, 4 running
    bool freeCa = false;               // free_CA_mode: the event is scrambled
    std::string lang, title, text, extended;
    std::vector<uint8_t> content;      // content_nibble_level_1 << 4 | level_2 per entry (0x1x movie, 0x2x news, 0x4x sport...)
    int rating = 0;                    // minimum age in years from the parental rating descriptor, 0 = none given
    std::string crid;                  // programme CRID (crid_type 0x01), as broadcast ("authority/identifier")
    std::string seriesCrid;            // series CRID (crid_type 0x02)
    bool other = false;                // came from the table of another transport stream
    int64_t end() const { return start + duration; }
    int genre() const { return content.empty() ? -1 : content[0] >> 4; }
};

// ---- parsing -----------------------------------------------------------------------------------------------------------------------------------
struct EitSection {
    int tableId = 0, sid = 0, tsid = 0, onid = 0, version = 0, secNum = 0, lastSec = 0;
    bool current = true;
    std::vector<EpgItem> events;
};
// A whole section (table_id to CRC_32 inclusive). False: too short, wrong table id, length field wrong or CRC wrong. A damaged event loop
// keeps the events decoded before the damage. Never reads past `len`.
bool parseEitSection(const uint8_t* s, int len, EitSection& out);

// ---- the language of the guide texts (#268). A broadcaster may send an event's name and text in several languages (Finland: Finnish and
// Swedish), one short event descriptor (and extended event descriptors) per language. With a preferred language set (ISO 639-2, "fin"), the
// texts of that language are used for every event that has them; an event without it keeps the default (the last language broadcast).
void epgSetPreferredLanguage(const std::string& iso639);   // "" = no preference; case does not matter; read by both EIT parsers
std::string epgPreferredLanguage();
std::string epgLanguageKey(const std::string& iso639);   // lower case, the terminology code of a pair (ger -> deu): two codes of one language compare equal
// Where the texts of one language sit in a section (offsets from the section start), for every event that has that language.
struct EpgLangPick {
    std::string lang;
    bool hasShort = false;
    int titleOff = 0, titleLen = 0, textOff = 0, textLen = 0;
    std::vector<std::pair<int, int>> ext;   // extended event texts of that language: offset, length, in the order sent
};
// s: a whole EIT section. Events by event_id. False: nothing found (no preference, damaged section, or no event has the language).
bool epgPickLanguage(const uint8_t* s, int len, const std::string& want, std::map<int, EpgLangPick>& out);

struct LocalTimeOffset {
    std::string country;               // ISO 3166 alpha-3, e.g. "LTU"
    int region = 0;                    // country_region_id
    int offsetMin = 0;                 // current offset from UTC in minutes (negative west of Greenwich)
    int64_t changeUtc = 0;             // time_of_change (UTC seconds): when nextOffsetMin takes over
    int nextOffsetMin = 0;
};
struct TotInfo { int64_t utc = 0; bool hasOffsets = false; std::vector<LocalTimeOffset> offsets; };
// TDT (table 0x70, no CRC) or TOT (0x73, CRC checked). False for anything else, too short, or a bad CRC.
bool parseTimeSection(const uint8_t* s, int len, TotInfo& out);
// Offset to add to UTC at `utc` in this zone, in seconds: the next offset applies from changeUtc on.
int zoneOffsetAt(const LocalTimeOffset& z, int64_t utc);
// "2026-10-17 21:05" for a UTC time in that zone (empty zone list = UTC)
std::string formatInZone(int64_t utc, const LocalTimeOffset* z);
// MJD + BCD time as in EIT/TDT/TOT (5 bytes) to unix seconds; false for the "undefined" all-ones value or a bad BCD digit.
bool dvbTime(const uint8_t* p, int64_t& unixSec);

// ---- database ----------------------------------------------------------------------------------------------------------------------------------
struct EpgLimits {
    size_t maxPerService = 3000;       // events kept per service (the farthest in the future are dropped first)
    size_t maxServices = 400;
    int64_t keepPastSec = 6 * 3600;    // events that ended longer ago are expired
    int64_t maxFutureSec = 21 * 86400; // events starting later than this are ignored (corrupt start times)
};

tvjson::Value epgItemToValue(const EpgItem& e);
EpgItem epgItemFromValue(const tvjson::Value& x);

struct EpgHit { ServiceKey key; EpgItem ev; };

class EpgDb {
public:
    void setLimits(const EpgLimits& l);
    EpgLimits limits() const;
    // Adds or replaces an event (same event id). An event of another id whose time overlaps is removed: the broadcaster rescheduled.
    // `now` bounds what is accepted (expired and far-future events are refused). Returns true when the database changed.
    bool ingest(const ServiceKey& k, const EpgItem& ev, int64_t now);
    void ingestSection(const EitSection& s, int64_t now);
    size_t prune(int64_t now);         // expire; returns the number removed
    void clear();

    std::vector<ServiceKey> services() const;
    std::vector<EpgItem> events(const ServiceKey& k) const;                        // sorted by start
    std::vector<EpgItem> eventsBetween(const ServiceKey& k, int64_t from, int64_t to) const;   // overlapping [from, to)
    bool nowNext(const ServiceKey& k, int64_t now, EpgItem* cur, EpgItem* next) const;   // cur: false-valued when nothing is on (returns whether cur was found)
    bool find(const ServiceKey& k, int eventId, EpgItem& out) const;
    // Case-insensitive match of the words of `query` (all of them) in title, text and extended text.
    std::vector<EpgHit> search(const std::string& query, size_t maxHits = 200) const;
    // Every later event with this series CRID (all services), sorted by start.
    std::vector<EpgHit> seriesEvents(const std::string& seriesCrid, int64_t from) const;
    size_t count() const;

    std::string toJson() const;
    bool fromJson(const std::string& json);      // replaces the content; false on a parse error (database unchanged)
    bool save(const std::string& path) const;    // via a temporary file and rename
    bool load(const std::string& path);

private:
    void trimLocked(std::vector<EpgItem>& v) const;
    mutable std::mutex mu_;
    EpgLimits lim_;
    std::map<ServiceKey, std::vector<EpgItem>> db_;
};

// ---- collector ---------------------------------------------------------------------------------------------------------------------------------
class SectionAssembler {
public:
    // One packet of the PID; calls `emit` for each complete section. A continuity error drops the partial section.
    template <class F> void feed(const uint8_t* pkt, F&& emit) {
        if (pkt[0] != 0x47 || (pkt[1] & 0x80)) return;
        const bool pusi = pkt[1] & 0x40;
        const int afc = (pkt[3] >> 4) & 3, cc = pkt[3] & 15;
        if (!(afc & 1)) return;
        int off = 4;
        if (afc & 2) off += 1 + pkt[4];
        if (off >= 188) return;
        if (lastCc_ >= 0 && cc == lastCc_) return;             // duplicate packet
        if (lastCc_ >= 0 && cc != ((lastCc_ + 1) & 15)) { buf_.clear(); active_ = false; }
        lastCc_ = cc;
        const uint8_t* p = pkt + off; int n = 188 - off;
        if (pusi) {
            const int ptr = p[0];
            if (1 + ptr > n) { buf_.clear(); active_ = false; return; }
            if (active_) { buf_.insert(buf_.end(), p + 1, p + 1 + ptr); drain(emit); }
            buf_.clear(); active_ = true;
            buf_.insert(buf_.end(), p + 1 + ptr, p + n);
        } else if (active_) buf_.insert(buf_.end(), p, p + n);
        else return;
        drain(emit);
    }
    void reset() { buf_.clear(); active_ = false; lastCc_ = -1; }
private:
    template <class F> void drain(F&& emit) {
        for (;;) {
            if (buf_.size() < 3) return;
            if (buf_[0] == 0xFF) { buf_.clear(); active_ = false; return; }   // stuffing
            const size_t total = 3 + (((buf_[1] & 0x0F) << 8) | buf_[2]);
            if (total > 4096) { buf_.clear(); active_ = false; return; }
            if (buf_.size() < total) return;
            emit(buf_.data(), (int)total);
            buf_.erase(buf_.begin(), buf_.begin() + total);
            if (buf_.empty()) { active_ = false; return; }
        }
    }
    std::vector<uint8_t> buf_;
    bool active_ = false;
    int lastCc_ = -1;
};

class EpgCollector {
public:
    explicit EpgCollector(EpgDb* db) : db_(db) {}
    void feed(const uint8_t* pkt188);
    // The time of the stream: the TDT/TOT value advanced by the packets since (stream seconds are passed in by the caller), else the computer's clock.
    void advance(double dtSeconds);
    int64_t utcNow() const;
    bool haveStreamTime() const;
    ServiceKey currentTs() const;      // onid / tsid seen on the SDT, sid 0
    std::string serviceName(const ServiceKey& k) const;
    std::vector<LocalTimeOffset> zones() const;
    uint64_t sections() const;         // EIT sections accepted
    uint64_t badSections() const;      // EIT sections rejected (CRC, structure)
    void setFixedNow(int64_t t);       // tests: a fixed clock instead of the computer's

private:
    void section(int pid, const uint8_t* s, int len);
    int64_t nowLocked() const;
    EpgDb* db_;
    mutable std::mutex mu_;
    std::map<int, SectionAssembler> asm_;
    int64_t tdt_ = 0, fixedNow_ = 0;
    double sinceTdt_ = 0;
    int onid_ = 0, tsid_ = 0;
    std::map<ServiceKey, std::string> names_;
    std::vector<LocalTimeOffset> zones_;
    uint64_t ok_ = 0, bad_ = 0;
};

} // namespace dect2
