// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sharing frequency lists: the file formats other programs use, an import preview with merge rules, the "My lists" model (lists = the groups of the
// memory bank: enable / disable / order) and the built-in lists of common services made from the compiled-in data (data/frequencies/*.tsv).
// Pure model, no UI, no files: the Memory panel and the tests call these. The OnAir formats themselves (JSON / CSV) stay in memory_bank.h.
//
// Formats read and written (all public and simple):
//   OnAirJson    {"format":"onair-memories",...}          everything a memory holds
//   OnAirCsv     the column list of memoryCsvColumns()    everything a memory holds
//   SdrppJson    SDR++ Frequency Manager {"lists":{name:{"bookmarks":{name:{frequency,bandwidth,mode}}}}}   mode: 0 NFM 1 WFM 2 AM 3 DSB 4 USB 5 CW 6 LSB 7 RAW
//   SdrSharpXml  SDR# frequency manager (frequencies.xml): <MemoryEntry> with IsFavourite, Name, Frequency (Hz), DetectorType, FilterBandwidth, GroupName, Notes
//   SdrCsv       a plain frequency CSV with the columns Group,Name,Frequency (Hz),Mode,Bandwidth (Hz),Comment (the layout SDRangel and SDR# users
//                keep their lists in; on reading the columns are found by name, in any order, and MHz columns are understood)
//   ChirpCsv     CHIRP: Location,Name,Frequency (MHz),Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,...,Mode,TStep,Skip,Comment ...
// What a format cannot hold is kept in the memory's `options` (key "duplex", "offsetHz", "tone", "rToneHz", "cToneHz", "dtcs", "tstepKHz",
// "demod") so that a round trip through CHIRP or SDR# does not lose a repeater's tone.
#pragma once
#include "dect2/channels/memory_bank.h"
#include <string>
#include <vector>

namespace dect2 {

enum class ListFormat { Unknown, OnAirJson, OnAirCsv, SdrppJson, SdrSharpXml, SdrCsv, ChirpCsv };
const char* listFormatName(ListFormat f);          // "CHIRP CSV" ...
const char* listFormatExt(ListFormat f);           // ".csv"
ListFormat detectListFormat(const std::string& text);

// the memories in a text, without touching any bank (rep: the skipped lines)
std::vector<Memory> parseList(const std::string& text, ListFormat fmt, MemoryLoadReport& rep, const std::string& defaultGroup = "");
std::string writeList(const std::vector<Memory>& v, ListFormat fmt);

// ---- import with a preview
enum class MergeRule {
    KeepBoth,        // add every row, also when the bank has the frequency and mode already
    KeepExisting,    // rows that are in the bank already are left out
    Replace,         // the imported settings replace the bank's entry (its id, order, created and favourite flag stay)
    FillEmpty,       // the bank's entry keeps what it has; empty fields (name, notes, group, bandwidth, gains, colour) come from the file, tags are joined
};
struct ImportRow {
    Memory m;
    bool duplicate = false;      // the bank already has this frequency (to 1 Hz) and mode
    uint64_t existingId = 0;
    bool take = true;            // the preview lets the user untick a row
};
struct ImportPreview {
    ListFormat format = ListFormat::Unknown;
    std::vector<ImportRow> rows;
    size_t fresh = 0, duplicates = 0;
    MemoryLoadReport rep;        // damaged lines
};
ImportPreview previewImport(const MemoryBank& bank, const std::string& text, ListFormat fmt = ListFormat::Unknown, const std::string& defaultGroup = "");
// applies the ticked rows; the result counts what was added (loaded) and what was left or merged (skipped = left out)
struct ImportResult { size_t added = 0, merged = 0, left = 0; };
ImportResult applyImport(MemoryBank& bank, const ImportPreview& p, MergeRule rule);

// ---- My lists
struct ListInfo { std::string name; bool enabled = true; };
class ListSet {
public:
    const std::vector<ListInfo>& lists() const { return v_; }
    void sync(const MemoryBank& bank);                        // a list for every group of the bank (new ones last, enabled); lists with no memory left are dropped unless `keepEmpty`
    void setKeepEmpty(bool k) { keepEmpty_ = k; }
    bool enabled(const std::string& name) const;              // unknown names count as enabled; the empty group ("no list") is always enabled
    bool setEnabled(const std::string& name, bool on);
    bool move(size_t from, size_t to);                        // order of the lists in the panel
    bool rename(MemoryBank& bank, const std::string& from, const std::string& to);   // renames the group of every memory in it
    // indexes into bank.all(): memories of enabled lists that match the query, lists in the set's order, memories in the bank's order inside a list
    std::vector<size_t> visible(const MemoryBank& bank, const MemoryQuery& q = MemoryQuery()) const;
    // the starred memories of the enabled lists, for the quick-tune bar that is shown in every mode (at most n)
    std::vector<size_t> quickTune(const MemoryBank& bank, size_t n) const;
    std::string toJson() const;                               // {"format":"onair-lists","version":1,"lists":[{"name":..,"enabled":..}]} in order
    bool fromJson(const std::string& text);
private:
    std::vector<ListInfo> v_;
    bool keepEmpty_ = false;
};

// ---- built-in lists of common services (from data/frequencies, compiled in). A list is one or more categories of that data.
struct BuiltinList { std::string name, description; std::vector<std::string> categories; };
const std::vector<BuiltinList>& builtinLists();            // Broadcast, Airband, Marine, Weather, Rail, Emergency services, Ham band plans, ISM / licence-free
const std::vector<std::string>& builtinRegions();          // the regions that occur in the data, GLOBAL first
// the memories of a built-in list for a region ("" = all regions; a region also takes the GLOBAL entries); group is the list's name
std::vector<Memory> builtinListMemories(const std::string& listName, const std::string& region = "");

} // namespace dect2
