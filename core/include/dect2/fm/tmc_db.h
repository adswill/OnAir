// RDS-TMC in plain words: the user's own event list and location table (ISO 14819-2 / -3, as the national TMC services and TISA publish
// them), imported from their files, and the decoded messages turned into text (event, road, location, direction, extent, duration,
// diversion) and written to a log. No country database ships with OnAir: the location tables are licensed national data. Without the
// files the messages keep their codes and the app says how to import them.
//
// Files it reads (the columns and the delimiter are found from the file itself; UTF-8, UTF-8 with BOM or Latin-1 / Windows-1252):
//  - event lists: a code column (CODE, ECD, Event code, Nr, Nummer ...) and one or more text columns (Text, Description, Beschreibung,
//    English, Deutsch, TEXT_DE ...); optional duration type (T / Duration type: D dynamic, L longer-lasting). The language of each text
//    column comes from its name, the file name or the words in it (German and English are recognised, others are kept as "xx").
//  - a location table as one list (the "LCL" format of the German BASt and others: LOCATION CODE, TYPE, ROAD_NUMBER, ROADNAME,
//    FIRST_NAME, SECOND_NAME, AREA_REFERENCE, LINEAR_REFERENCE, NEGATIVE_OFFSET, POSITIVE_OFFSET, EXIT_NUMBER ...)
//  - or the TISA exchange format files (POINTS, ROADS, SEGMENTS, POFFSETS, SOFFSETS, NAMES, ADMINISTRATIVEAREA, OTHERAREAS: LCD, CLASS,
//    TCD, STCD, ROADNUMBER, RNID, N1ID, N2ID, POL_LCD, ROA_LCD, SEG_LCD, NEG_OFF_LCD, POS_OFF_LCD, NID, NAME ...), several files at once.
#pragma once
#include "dect2/fm/rds_tmc.h"
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {
namespace tmcdb {

// One table of a text file: the delimiter found, the header (when the first line is one) and the rows. Quotes ("a;b") are honoured.
struct Csv {
    char delim = ';';
    bool header = false;
    std::vector<std::string> cols;                 // header names as given (empty without a header)
    std::vector<std::vector<std::string>> rows;
};
// Text to UTF-8 (a BOM is dropped; text that is not valid UTF-8 is read as Windows-1252), then split into lines and fields.
std::string toUtf8(const std::string& raw);
Csv parseCsv(const std::string& utf8Text);
std::string normCol(const std::string& name);      // "Location Code" -> "locationcode": lower case, letters and digits only

struct Event {
    int code = 0;
    std::map<std::string, std::string> text;       // language ("en", "de", "xx") -> text
    char durType = 0;                              // 'D' dynamic, 'L' longer-lasting, 0 not given (the built-in table decides)
};

struct Location {
    int code = 0;
    std::string type;                              // "P1.3", "L1.1", "A6.0" ... (class letter, type, subtype), "" unknown
    std::string roadNumber, roadName;              // "A5", "Hafraba"
    std::string firstName, secondName;             // a point: its name (and a second one); a road or segment: the names of its two ends
    std::string exitNumber;                        // junction number ("12")
    int area = 0;                                  // area reference (the location code of the area it lies in)
    int linear = 0;                                // linear reference (the segment or road it lies on)
    int negOff = 0, posOff = 0;                    // the neighbours on the road in the negative and the positive direction
    bool isPoint() const { return !type.empty() && (type[0] == 'P' || type[0] == 'p'); }
    bool isLinear() const { return !type.empty() && (type[0] == 'L' || type[0] == 'l'); }
};

// What an import found (for the import dialog and the tests)
struct ImportReport {
    bool ok = false;
    std::string kind;                              // "event list", "location table", "TISA points", "TISA names" ... ("" not recognised)
    std::string message;                           // a sentence for the user (English)
    int rows = 0;                                  // records taken
    char delim = 0;
    std::vector<std::string> languages;            // event list: the languages found
    int cid = -1, tabcd = -1;                      // location table: country id and table number when the file gives them
};

class Database {
public:
    // Reads one file (UTF-8 path) and adds what it holds. nameHint: the file's name (it helps to tell the language and the TISA file kind).
    ImportReport importFile(const std::string& path);
    ImportReport importText(const std::string& raw, const std::string& nameHint);
    void clear();

    bool hasEvents() const { return !events_.empty(); }
    bool hasLocations() const { return !locs_.empty(); }
    size_t eventCount() const { return events_.size(); }
    size_t locationCount() const { return locs_.size(); }
    std::vector<std::string> languages() const;    // of the event texts, most common first
    int tableNumber() const { return tabcd_; }     // the TABCD (= LTN) the files gave, -1 unknown
    const std::vector<std::string>& sources() const { return sources_; }

    // The event text in a language ("" or "auto": English when present, else the first there is); nullptr when the code is not listed.
    const Event* event(int code) const;
    std::string eventText(int code, const std::string& lang) const;
    const Location* location(int code) const;
    // The road a location lies on: its own number / name, else those of its linear reference (followed up to 4 steps)
    void roadOf(const Location& l, std::string& number, std::string& name, std::string& end1, std::string& end2) const;
    // The location reached from `code` by `steps` offsets (negative or positive direction); 0 when the chain breaks
    int follow(int code, int steps, bool positive) const;

private:
    ImportReport importEvents(const Csv& c, const std::string& nameHint);
    ImportReport importLcl(const Csv& c);
    ImportReport importTisa(const Csv& c, const std::string& kind);
    void resolveNames();
    std::unordered_map<int, Event> events_;
    std::unordered_map<int, Location> locs_;
    // TISA: names by NID and the name ids of each location, resolved after every import
    std::unordered_map<int, std::string> names_;
    struct NameRef { int n1 = 0, n2 = 0, rn = 0; };
    std::unordered_map<int, NameRef> nameRefs_;
    std::vector<std::string> sources_;
    int tabcd_ = -1;
};

// A message in words. Every field is filled in English or the chosen language where the files give the text; the codes stay when not.
struct Plain {
    std::string event;                             // "Stau" / "stationary traffic" / "event 101 (?)"
    std::vector<std::string> details;              // additional events, speed limit, length, diversion via ..., destination ...
    std::string road;                              // "A5" or "A5 Hafraba" ("" unknown)
    std::string location;                          // "Frankfurt-West (12)" / "location 4321 (?)"
    std::string direction;                         // "towards Basel" / "positive direction"
    std::string extent;                            // "to Darmstadt (2 locations further)" / "+2 locations" / ""
    std::string duration;                          // "1 hour" / "" (the event's default)
    std::string diversion;                         // "diversion advised" / ""
    bool eventKnown = false, locationKnown = false;
    std::string line() const;                      // all of it in one line: "A5 Frankfurt-West towards Basel: stationary traffic ..."
};
// lang: "" or "auto" (English when the list has it), "en", "de", ... The built-in English table is used for codes the files lack.
Plain describe(const TmcMessage& m, const Database* db, const std::string& lang);

// The log: every new message (and every change of one) once, as a CSV row and a line of text. Repeats of the same message are not logged
// again. basePath "…/tmc-log" writes "…/tmc-log.csv" and "…/tmc-log.txt" (appended; the CSV gets its header when it is new).
struct Station { double freqMhz = 0; int pi = 0; std::string ps; int ltn = -1; int sid = -1; };
class Log {
public:
    ~Log();
    void setPath(const std::string& basePath);     // "" stops the log
    const std::string& path() const { return base_; }
    void setEnabled(bool on) { on_ = on; }
    bool enabled() const { return on_ && !base_.empty(); }
    // Logs what is new in msgs (now: seconds since 1970). Returns how many messages were written.
    int update(const std::vector<TmcMessage>& msgs, const Station& st, const Database* db, const std::string& lang);
    std::string lastError() const { return err_; }
    static std::string csvHeader();
    static std::string csvRow(const TmcMessage& m, const Plain& p, const Station& st);
    static std::string textLine(const TmcMessage& m, const Plain& p, const Station& st);
private:
    bool open();
    std::string base_, err_;
    bool on_ = true;
    FILE* csv_ = nullptr;
    FILE* txt_ = nullptr;
    std::vector<std::string> seen_;                // keys of the messages already logged (bounded)
};

// The folder the app keeps the imported files in (settings folder / "tmc"), and loading every file there.
ImportReport loadFolder(Database& db, const std::string& folder);

} // namespace tmcdb
} // namespace dect2
