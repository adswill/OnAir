// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Ham logbook: every QSO with all its ADIF fields, edit and delete with undo, dupe check, ADIF import and export (unknown fields kept
// for the round trip), award tracking (DXCC, WAS, grids, CQ zones: worked / confirmed per band and per mode class), statistics, and the
// worked-before index the decode list colours with. Pure logic: no files are touched except by load() / save(), nothing is sent anywhere.
#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dect2::ham {

struct LogQso {
    long long id = 0;                       // unique within the book while it is open (not written to ADIF)
    std::string call, grid, mode, submode, band;
    double freqHz = 0, startUtc = 0, endUtc = 0;
    std::string rstSent, rstRcvd, name, qth, comment, state, country, cont, myCall, myGrid, txPwr;
    int dxcc = 0, cqz = 0, ituz = 0;        // 0: not known
    bool qslSent = false, qslRcvd = false, lotwRcvd = false;
    std::string stx, srx, contestId;        // contest exchange as sent / received (STX_STRING / SRX_STRING), CONTEST_ID
    std::map<std::string, std::string> extra;   // every other ADIF field (upper-case name), written back unchanged
    bool confirmed() const { return qslRcvd || lotwRcvd; }
};

// ---- ADIF (any program's file) ----
using AdifFields = std::map<std::string, std::string>;   // upper-case field name -> value
std::vector<AdifFields> adifReadRecords(const std::string& text);   // the header (before <EOH>) is skipped; field lengths are bytes
LogQso adifToQso(const AdifFields& f);
std::string qsoToAdif(const LogQso& q);                  // one record ending with <EOR>
std::string adifDate(double utc);                        // 20261022
std::string adifTime(double utc);                        // 120530
double adifUtc(const std::string& date, const std::string& time);   // 0 when the date does not read
std::string modeClass(const std::string& mode);          // "CW", "PHONE" or "DIGI" (the DXCC award columns)

// ---- the logbook ----
class Logbook {
public:
    const std::vector<LogQso>& qsos() const { return q_; }
    size_t size() const { return q_.size(); }
    const LogQso* find(long long id) const;
    int indexOf(long long id) const;                     // -1 when not there

    long long add(LogQso q);                             // the new id; band filled from the frequency, call and grid upper-cased
    bool update(const LogQso& q);                        // by q.id; false when that id is not in the book
    bool remove(long long id);
    // one step back / forward through add, update, remove and import (an import is one step)
    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const;                       // "Undo delete K1ABC", "" when nothing
    std::string redoLabel() const;

    // the QSO this one duplicates: same call (portable suffixes ignored), same band (when perBand) and mode class or mode (when perMode),
    // within withinSec of utc (0: ever). -1: no dupe. exceptId is left out (the QSO being edited).
    int findDupe(const std::string& call, const std::string& band, const std::string& mode, bool perBand = true, bool perMode = true,
                 double utc = 0, double withinSec = 0, long long exceptId = 0) const;
    // the indices of the QSOs matching every word of the query (call, grid, mode, band, name, QTH, comment, country, state, date
    // "20261022"); "band:20m", "mode:FT8", "call:K1", "dxcc:291", "cont:EU" match one field. Newest first.
    std::vector<size_t> search(const std::string& query) const;

    struct ImportResult { int added = 0, dupes = 0, bad = 0; };
    // dupes (same call, band, mode and start within 60 s) are skipped when skipDupes
    ImportResult importAdif(const std::string& text, bool skipDupes = true);
    std::string exportAdif() const;
    std::string exportAdif(const std::vector<size_t>& indices) const;

    bool load(const std::string& path, std::string& err);   // replaces the book; a missing file is an empty book
    bool save(const std::string& path, std::string& err) const;   // the whole file, written to path.tmp and renamed
    void clear() { q_.clear(); undo_.clear(); redo_.clear(); }
    // fills in DXCC, country, continent, zones of a QSO that does not have them (set by the app: the prefix database)
    std::function<void(LogQso&)> enrich;

private:
    struct Step { int kind; std::vector<std::pair<int, LogQso>> before, after; std::string label; };   // kind 0 add 1 update 2 remove 3 import
    void push(Step s);
    void apply(const Step& s, bool forward);
    long long nextId_ = 1;
    std::vector<LogQso> q_;
    std::vector<Step> undo_, redo_;
};

std::string baseCall(const std::string& call);          // "DL/LY1ABC/P" -> "LY1ABC" (the longest part with a digit and letters)

// ---- awards ----
enum class Award { Dxcc, Was, Grid, Cqz };
const char* awardName(Award a);
int awardTotal(Award a);                                 // 340 DXCC (current entities), 50 WAS, 40 CQ zones, 0 grids (no fixed total)
const std::vector<std::string>& usStates();              // the 50 two-letter codes
// the key a QSO counts for: DXCC the entity number (or the country name when the number is not known), WAS the state (US QSOs only),
// grid the 4 character square, CQ zone the zone number. "" when the QSO does not count.
std::string awardKey(const LogQso& q, Award a);

struct AwardRow {
    std::string key, label;                              // label: the country name for DXCC
    char any = 0;                                        // 'W' worked, 'C' confirmed
    std::map<std::string, char> byBand, byClass;         // band -> 'W' / 'C'; "CW" / "PHONE" / "DIGI" -> 'W' / 'C'
};
struct AwardTable {
    std::vector<AwardRow> rows;                          // sorted by key
    std::vector<std::string> bands;                      // the bands that occur, in frequency order
    int worked = 0, confirmed = 0, total = 0;
    std::map<std::string, std::pair<int, int>> perBand;  // band -> worked, confirmed
};
// modeFilter / bandFilter: "" all
AwardTable awardTable(const std::vector<LogQso>& log, Award a, const std::string& modeFilter = "", const std::string& bandFilter = "");

// ---- statistics ----
struct LogStats {
    int qsos = 0, calls = 0, dxcc = 0, grids = 0, confirmed = 0;
    std::map<std::string, int> perBand, perMode, perCont;
    std::map<int, int> perHour;                          // UTC hour -> QSOs
    double firstUtc = 0, lastUtc = 0;
    double bestKm = 0; std::string bestCall;             // the longest distance (needs both grids)
    double qsoPerHourLast = 0;                           // QSOs in the hour before the newest one
};
LogStats logStats(const std::vector<LogQso>& log, const std::string& myGrid);
int bandOrder(const std::string& band);                  // for sorting: 2190m first, 70cm and up last; unknown bands after

// ---- worked before, for the decode list ----
struct DecodeMarks { bool workedCall = false, workedCallBand = false, newDxcc = false, newDxccBand = false, newGrid = false, newGridBand = false; };
class WorkedIndex {
public:
    void build(const std::vector<LogQso>& log);
    void add(const LogQso& q);
    // dxccKey: the entity of the decoded call (from the prefix database: number as text, or name); "" when not known (no DXCC marks then)
    DecodeMarks mark(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, const std::string& dxccKey) const;
private:
    std::set<std::string> call_, callBandMode_, dxcc_, dxccBand_, grid_, gridBand_;
};

}  // namespace dect2::ham
