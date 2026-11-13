// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The built-in frequency database: well-known frequencies of every mode (calling channels, beacons, band plans, satellite downlinks ...), compiled
// in from data/frequencies/*.tsv, plus the user's own entries. Pure model: search, filters, conversion to a Memory (core/include/dect2/channels/memory_bank.h)
// and import / export (the Memory JSON and CSV files, so a user's list opens in the Memory panel and the other way round). No UI, no network.
//
// The data files (see docs/frequency-db.md): `freq_mhz|name|bandwidth_khz|notes[|mode|region|category]`, with @mode / @region / @category / @source
// lines setting the defaults for the lines below them. `mode` is the id a Memory uses (the Scanner's mode id).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "dect2/channels/memory_bank.h"

namespace dect2 {

struct FreqEntry {
    uint64_t hz = 0;
    std::string mode;        // mode id (a Memory's `mode`)
    std::string name;
    std::string region;      // GLOBAL, EU, NA, US, UK, DE, R1 ...
    std::string category;    // one of freqCategories() for built-in entries; free text for the user's
    std::string notes;
    std::string source;      // where the fact comes from (a note per group of entries)
    double bwHz = 0;         // bandwidth hint (0: none)
    bool user = false;       // the user's own entry (not part of the build)
};

struct FreqParseReport { size_t loaded = 0, skipped = 0; std::vector<std::string> log; };

// parses one data file's text (or a whole concatenation); bad lines are skipped with one log line each
std::vector<FreqEntry> parseFreqTsv(const std::string& text, FreqParseReport& rep);
// the built-in table, parsed on first use, ordered by mode then frequency
const std::vector<FreqEntry>& builtinFrequencies();
// the data files compiled in (tests): count, and the text of file i (nullptr past the end)
size_t freqDataFileCount();
const char* freqDataFile(size_t i);

bool freqModeIdValid(const std::string& id);          // a mode id a Memory (and so the app) knows: protocol id, mode id, or dvb / atsc / dab / atsc3 / isdbt / fm
std::string freqModeName(const std::string& id);      // "FT8", "ADS-B" ...; the id itself when unknown
const std::vector<std::string>& freqCategories();     // the fixed vocabulary of the built-in data
std::string freqKey(const FreqEntry& e);              // "mode:hz": favourites and duplicates are decided on it

struct FreqQuery {
    std::string text;        // words, all must match: name, notes, mode, region, category, or a frequency in MHz ("162.4", "14")
    std::string mode, region, category;   // "" = any; a region also lets GLOBAL entries through
    bool favouritesOnly = false;
    bool userOnly = false;
};
bool freqMatches(const FreqEntry& e, const FreqQuery& q, const std::set<std::string>* favourites = nullptr);
// indices into v: matches ordered by frequency (then name)
std::vector<size_t> freqSearch(const std::vector<FreqEntry>& v, const FreqQuery& q, const std::set<std::string>* favourites = nullptr);
std::vector<std::string> freqDistinct(const std::vector<FreqEntry>& v, int field);   // field 0 mode, 1 region, 2 category; sorted
// the entries of one mode, by frequency (the Tuner's "Known frequencies")
std::vector<size_t> freqForMode(const std::vector<FreqEntry>& v, const std::string& mode);

Memory freqToMemory(const FreqEntry& e);               // what "Add to memory" and a tune hand over: name, frequency, mode, bandwidth; region -> group, category -> tags
FreqEntry freqFromMemory(const Memory& m);

// the user's entries as a Memory file (JSON or CSV) and back; import skips entries already in `into` (same mode and frequency)
std::string freqExportJson(const std::vector<FreqEntry>& v);
std::string freqExportCsv(const std::vector<FreqEntry>& v);
FreqParseReport freqImportText(const std::string& text, std::vector<FreqEntry>& into);

} // namespace dect2
