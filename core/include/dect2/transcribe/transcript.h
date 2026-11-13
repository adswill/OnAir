// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Transcripts: what was said on a voice channel, as text. Pure data and rules (no radio, no UI, no speech engine): the entry, a store that is
// searched and filtered, the exports (TXT, SRT, JSON, CSV) and a file (one JSON object per line) with a retention limit.
#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct TranscriptEntry {
    uint64_t id = 0;             // assigned by the store
    int64_t wall = 0;            // Unix seconds when the transmission started
    int64_t startMs = 0;         // the same in milliseconds (not stored in the file; 0 for entries loaded from it): the captions' audio clock
    double freqHz = 0;           // 0 when not known
    std::string mode;            // "airband", "marine", "audio" (the sound of any other mode)
    std::string channel;         // "118.700 Tower"
    double durSec = 0;
    std::string text;            // as spoken
    std::string lang;            // language of `text` ("de"), "" when not known
    std::string translation;     // English translation of `text` (live translation), "" when none
    float confidence = 0;        // 0..1, mean probability of the words
    int turn = 0;                // transmission counter on this channel since the start
    std::string speaker;         // a guess from the wording: "ATC" (starts with the callsign), "Aircraft" (ends with it), "" (unknown)
    std::string callsign;        // flight id spoken in it, normalised ("DLH123"), "" when none
    std::string reg;             // registration spoken in it, normalised ("N123AB"), "" when none
    std::string aircraft;        // the aircraft on the screen it was matched to (ICAO address in hex, or its callsign), "" when none
};

struct TranscriptFilter {
    std::string query;           // every word must be found (case ignored) in the text, channel, callsign, registration or aircraft
    double freqHz = 0;           // 0: any; else within 5 kHz
    std::string aircraft;        // callsign, registration or ICAO address: entries linked to it
    std::string airport;         // found in the channel name or the text (an ICAO code such as "EYVI", or a name)
    std::string mode;            // "" any
    int64_t from = 0, to = 0;    // Unix seconds, 0 = open
};

class TranscriptStore {
public:
    void setLimits(size_t maxEntries, int retentionDays);
    uint64_t add(TranscriptEntry e);                                  // assigns the id; appends to the file when one is attached
    std::vector<TranscriptEntry> find(const TranscriptFilter& f, size_t limit = 0) const;   // newest first
    // Entries linked to an aircraft by the flight id or the registration it carries (normalised as acars_log.h does), newest first.
    std::vector<TranscriptEntry> forAircraft(const std::string& callsign, const std::string& reg, size_t limit = 0) const;
    size_t size() const;
    uint64_t version() const;                                         // grows with every change (for a screen that caches the list)
    void clear();
    void prune(int64_t nowWall);                                      // retention and size; rewrites the file when something went
    // The file: loads what it holds (older than the retention is skipped), then every add() is appended. false: not writable.
    bool attachFile(const std::string& path, std::string* err = nullptr);
    void detachFile();
    static bool matches(const TranscriptEntry& e, const TranscriptFilter& f);

private:
    void rewriteLocked();
    mutable std::mutex mu_;
    std::deque<TranscriptEntry> v_;                                   // oldest first
    size_t max_ = 20000;
    int days_ = 7;
    uint64_t nextId_ = 1, ver_ = 0;
    std::string path_;
};

// One entry as one line of JSON, and back; false for a line that is not one.
std::string transcriptToJson(const TranscriptEntry& e);
bool transcriptFromJson(const std::string& line, TranscriptEntry& e);

// Exports of a list (any order: written oldest first). SRT times are offsets from the first entry; each cue is "[channel] text".
std::string transcriptsTxt(std::vector<TranscriptEntry> v);
std::string transcriptsSrt(std::vector<TranscriptEntry> v);
std::string transcriptsJson(std::vector<TranscriptEntry> v);          // a JSON array
std::string transcriptsCsv(std::vector<TranscriptEntry> v);           // RFC 4180, a header row, UTF-8

}  // namespace dect2
