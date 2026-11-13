// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Memories: the frequencies a user keeps, with the mode and the receiver settings they were stored with ("memory channels" of a scanner or
// a radio). Pure model, no UI and no engine: the Memory panel (app/memory) captures and recalls them, the tests drive the bank directly.
//
// File format (memories.json, version 1): {"format":"onair-memories","version":1,"memories":[{...}, ...]}; every field of an entry is optional
// except "freqHz". Unknown fields and entries without a usable frequency are skipped when loading (one log line each), so a file written by
// a later version still opens. CSV: UTF-8, a header row naming the columns (memoryCsvColumns()); a file without a header is read as
// "frequency,name" lines (frequency in MHz, or Hz / kHz when it is clearly one of those).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

constexpr int kMemoryFileVersion = 1;
constexpr size_t kMemoryCap = 10000;   // more is a sign of a runaway import, not of a person's memories

struct Memory {
    uint64_t id = 0;               // stable inside one bank (never reused while the program runs); not written to CSV
    std::string name;
    uint64_t freqHz = 0;
    // the mode: `mode` is the mode id the Scanner also uses (a protocol id such as "pocsag", the mode's own id, or dvb / atsc / dab / atsc3 /
    // isdbt / fm); family + subId say the same in the app's own numbers; stdMode is the engine's standard code (informative)
    std::string mode, subId;
    int family = -1, stdMode = 0;
    double sampleRateHz = 0;       // 0: automatic
    double bandwidthHz = 0;        // channel bandwidth where the mode has one (0: the mode's own)
    std::string radio;             // the radio it was stored with (informative: a recall never changes the radio)
    bool hasGain = false;          // the gains below were captured
    int lna = 0, vga = 0;          // HackRF style gains
    bool amp = false;
    double gainDb = 0;             // a radio with one gain control
    std::map<std::string, std::string> options;   // mode options as key=value (memory_options registry in the app)
    std::string group, tags;       // group: one heading in the panel; tags: free words separated by commas
    std::string notes;
    uint32_t colour = 0;           // 0xRRGGBB, 0: none
    int64_t lastUsed = 0;          // unix time of the last recall (0: never)
    int64_t created = 0;
    bool favourite = false;
    bool scan = false;             // part of the memory scan
    double dwellSec = 0;           // memory scan: listening time here (0: the scan's default)
};

enum class MemorySort { Manual, Name, Frequency, LastUsed, Group, Mode };

struct MemoryQuery {
    std::string text;              // matches name, notes, tags, group, mode, or a frequency typed in MHz ("162.4", "162")
    uint64_t loHz = 0, hiHz = 0;   // hiHz 0: no upper limit
    std::string tag;               // one tag (whole word, case-insensitive)
    std::string mode;              // mode id
    std::string group;
    bool favouritesOnly = false;
};

// what add() did
enum class MemoryAdd { Added, Duplicate, Full };
struct MemoryAddResult { MemoryAdd what = MemoryAdd::Added; size_t index = 0; uint64_t id = 0; };
enum class DupPolicy { Keep, Update, Skip };   // an entry with the same frequency and mode: add anyway, replace its settings, or leave the bank alone

struct MemoryLoadReport { size_t loaded = 0, skipped = 0; std::vector<std::string> log; bool ok = true; std::string error; };

class MemoryBank {
public:
    const std::vector<Memory>& all() const { return v_; }
    size_t size() const { return v_.size(); }
    bool empty() const { return v_.empty(); }
    const Memory* find(uint64_t id) const;
    Memory* find(uint64_t id);
    int indexOf(uint64_t id) const;                  // -1 when not there

    // a copy with a fresh id (any id it carried is ignored) and `created` filled in; Full once kMemoryCap is reached
    MemoryAddResult add(Memory m, DupPolicy dup = DupPolicy::Keep, int64_t now = 0);
    const Memory* findDuplicate(uint64_t freqHz, const std::string& mode, uint64_t exceptId = 0) const;   // same frequency (to 1 Hz) and mode
    bool update(uint64_t id, Memory m);              // replaces everything but id and created; false when the id is unknown
    bool remove(uint64_t id, Memory* removed = nullptr, size_t* pos = nullptr);
    void restore(Memory m, size_t pos);              // the undo of remove(): same id, same place (bounded by the size)
    bool move(size_t from, size_t to);               // manual order
    void touch(uint64_t id, int64_t now);            // lastUsed
    void clear() { v_.clear(); }

    // indexes into all(), filtered and ordered; favourites first when favouritesFirst (the panel's default)
    std::vector<size_t> view(const MemoryQuery& q, MemorySort sort, bool descending = false, bool favouritesFirst = false) const;
    std::vector<size_t> search(const MemoryQuery& q) const { return view(q, MemorySort::Manual); }
    std::vector<std::string> groups() const;         // distinct non-empty group names, in order of first use
    std::vector<size_t> recent(size_t n) const;      // the n most recently used (lastUsed > 0), newest first
    void sortInPlace(MemorySort sort, bool descending = false);   // makes an order permanent

    // ---- files (memory_io.cpp)
    std::string toJson() const;
    std::string toCsv() const;
    // replaces the bank's content. A corrupt file loads as far as it can; report.ok is false only when nothing at all could be read.
    MemoryLoadReport fromJson(const std::string& text);
    // adds to the bank, as import does: JSON (ours, or an SDR++ frequency manager export), our CSV, or "frequency,name" lines
    MemoryLoadReport importText(const std::string& text, DupPolicy dup = DupPolicy::Keep, const std::string& defaultGroup = "");
    // paths are UTF-8 (dect2::u8file); save writes name.tmp and renames it over the file, so a crash never leaves half a file
    bool save(const std::string& path, std::string* err = nullptr) const;
    MemoryLoadReport load(const std::string& path);
    bool exportFile(const std::string& path, bool csv, std::string* err = nullptr) const;
    MemoryLoadReport importFile(const std::string& path, DupPolicy dup = DupPolicy::Keep);

private:
    std::vector<Memory> v_;
    uint64_t nextId_ = 1;
};

const std::vector<std::string>& memoryCsvColumns();
std::string memoryFreqMhzText(uint64_t hz);          // "162.400": MHz with at least 3 decimals, up to 6 (Hz) when the frequency needs them
bool memoryParseFreq(const std::string& text, uint64_t& hz);   // "162.4", "162,4", "162.4 MHz", "146520 kHz", "7074000" -> Hz

// ---- recall: the app (or a test) implements the host; the order of the calls is the contract
// A recall sets the mode first (the options and gains belong to it), then the options, the gains, and the frequency last.
class MemoryHost {
public:
    virtual ~MemoryHost() = default;
    virtual bool hasMode(const std::string& modeId) = 0;                  // built into this program
    virtual bool setMode(const Memory& m, std::string& note) = 0;         // false: refused (note says why, e.g. "stop the receiver first")
    virtual void setOption(const std::string& key, const std::string& value) = 0;
    virtual void setGains(const Memory& m) = 0;
    virtual void setSampleRate(double hz) = 0;
    virtual void setFrequencyHz(uint64_t hz) = 0;
};
struct RecallResult { bool ok = false, modeChanged = false; std::string note; };
RecallResult memoryRecall(MemoryHost& host, const Memory& m, bool withMode = true);

} // namespace dect2
