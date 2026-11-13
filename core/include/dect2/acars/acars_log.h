// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A bounded log of the interesting ACARS messages per aircraft, shared by the ACARS, VDL2 and HFDL screens, the ADS-B screens and the
// map upload. Pure data and rules (no radio, no UI): classification of a message by its label and text, redaction of phone numbers and
// e-mail addresses, and a store that attaches messages to an aircraft by ICAO address, registration or flight, never by a guess.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dect2 {

enum class AcarsCat : uint8_t { Ignore = 0, Free, Crew, Ops, Company, Maint, Fault };

const char* acarsCatKey(AcarsCat c);        // "ignore", "free", "crew", "ops", "company", "maint", "fault": the key on the wire
const char* acarsCatName(AcarsCat c);       // English name for the screens ("Free text", "Crew", ...); translated by the caller
inline bool acarsInteresting(AcarsCat c) { return c != AcarsCat::Ignore; }

// Label (two characters, "_d" for the DEL form) and printable text of a block: link tests, acknowledgements, squitters, media
// advisories, empty frames and pure binary or hex blobs are Ignore; labels in the table (acars_label_cat.inc) give their category;
// a label that is not in the table but carries readable words is Free. Keywords in the text (fault, CFDS, ECAM, captain, cabin, ...)
// refine a Free, Ops or Company message into Fault, Maint or Crew.
AcarsCat acarsClassify(const std::string& label, const std::string& text);

// Phone numbers (a + or 00 prefix, or 9 or more digits grouped by spaces, dots, dashes or brackets) and e-mail addresses become
// "[number]" and "[email]". Coordinates, times, flight levels and short numbers stay.
std::string acarsRedact(const std::string& text);

// Registration as a key: upper case, no dashes, dots or spaces ("A6-EDA" and "a6eda" are the same).
std::string acarsNormReg(const std::string& reg);
// Flight id as a key: upper case, leading zeros of the number removed ("EK0201" and "EK201" are the same).
std::string acarsNormFlight(const std::string& flight);

struct AcarsLogEntry {
    int64_t wall = 0;            // Unix seconds when decoded
    std::string mode;            // "ACARS", "VDL2" or "HFDL"
    double freqMHz = 0;
    std::string label;
    AcarsCat cat = AcarsCat::Free;
    std::string text;            // redacted, at most kAcarsLogTextMax characters
    std::string flight;          // flight id the message carried, "" when none
    bool downlink = true;
};

constexpr size_t kAcarsLogPerAircraft = 50;
constexpr int64_t kAcarsLogMaxAgeSec = 48 * 3600;
constexpr size_t kAcarsLogMaxAircraft = 400;
constexpr size_t kAcarsLogTextMax = 500;

// Who a message belongs to, as far as the message says: any of them may be missing.
struct AcarsIdent {
    uint32_t icao = 0;           // 24-bit address (VDL2 source, HFDL logon), 0 when not known
    std::string reg;             // registration as sent
    std::string flight;          // flight id as sent
};

// Who an aircraft on the screen is. callsignHolders: how many aircraft on the screen have this callsign (the flight id is only used
// when it is exactly one: two aircraft with the same callsign get nothing from it).
struct AcarsAircraftKey {
    uint32_t icao = 0;
    std::string reg;             // from the registration database, "" when not known
    std::string callsign;        // ADS-B callsign, "" when not known
    int callsignHolders = 1;
};

class AcarsLog {
public:
    // Store a message. Entries judged Ignore are dropped unless keepIgnored (a second log that backs "Show all messages").
    // The same label and text from the same aircraft within 30 s is a repeat (two receivers, or a resend) and is dropped.
    bool add(const AcarsIdent& id, AcarsLogEntry e, bool keepIgnored = false);
    // The messages of one aircraft, newest first, at most kAcarsLogPerAircraft.
    std::vector<AcarsLogEntry> messagesFor(const AcarsAircraftKey& k, int64_t nowWall) const;
    size_t count(const AcarsAircraftKey& k, int64_t nowWall) const { return messagesFor(k, nowWall).size(); }
    void prune(int64_t nowWall);
    void clear() { buckets_.clear(); alias_.clear(); }
    size_t aircraft() const { return buckets_.size(); }
private:
    struct Bucket { std::deque<AcarsLogEntry> v; int64_t last = 0; };   // newest at the back
    static std::string keyHex(uint32_t icao);
    std::map<std::string, Bucket> buckets_;                // "H:XXXXXX", "R:REG" (registration only), "F:FLIGHT" (neither)
    std::unordered_map<std::string, uint32_t> alias_;      // normalised registration > ICAO address, learned from messages that carry both
};

// One entry as JSON for the map upload: {"t":secs,"m":"VDL2","f":136.975,"l":"H1","c":"crew","x":"text"}; strings escaped, control
// characters as \u00XX, valid UTF-8 kept, invalid bytes replaced by '?'.
std::string acarsJsonEscape(const std::string& s);
std::string acarsEntryJson(const AcarsLogEntry& e);

} // namespace dect2
