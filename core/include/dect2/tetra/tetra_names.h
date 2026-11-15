// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA name book: the user's own names for terminal numbers (ISSI) and talkgroup numbers (GSSI), shown beside the numbers on the TETRA
// screens and in the "speaking now" line. The data belongs to the user: it is never shipped, lives in <data folder>/tetra_names.csv and is
// only displayed; nothing is decoded differently because of it. docs/TETRA-NAMES.md.
//
// Own CSV (export format, always read):  type,id,name,notes,mcc,mnc,colour_code,site     type = ISSI | GSSI; mcc, mnc, colour_code optional
// (empty = any network). An entry with a network (mcc and mnc) applies only to that network; an entry without one applies everywhere.
// Tolerant text import (no header): one id per line, as   id;name   id,name   id<tab>name   id name   ISSI=name   1234=name   ISSI 1234 name
// GSSI:1234;name ; an optional third field is the notes; '#' starts a comment; quotes, BOM and CRLF are fine. Lines without an id are skipped.
// The layout of the files of the SDR# plugin "TETRA Trunk Tracker" could not be confirmed from public documentation (docs/TETRA-NAMES.md):
// whatever of them is plain "id<separator>name" reads, nothing is guessed beyond that.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace dect2 {

enum class TetraIdKind { Issi = 0, Gssi = 1 };
const char* tetraIdKindName(TetraIdKind k);   // "ISSI" "GSSI"

struct TetraNameEntry {
    TetraIdKind kind = TetraIdKind::Issi;
    uint32_t id = 0;
    std::string name, notes, site;
    int mcc = -1, mnc = -1, colourCode = -1;   // -1: any
};

class TetraNames {
public:
    // Reads text (CSV with header, or the tolerant line formats) into the book; rows with the same (type, id, mcc, mnc) replace what is there.
    // defaultKind is used for rows that name no type. Returns the rows added or replaced; unusable rows are counted in *skipped.
    size_t import(const std::string& text, TetraIdKind defaultKind, size_t* skipped = nullptr);
    std::string exportCsv(bool allKinds = true, TetraIdKind only = TetraIdKind::Issi) const;
    void set(const TetraNameEntry& e);                  // adds or replaces; an empty name removes
    bool erase(TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1);
    // The entry for an id: the one of this network when there is one, else the any-network one, else (network unknown: mcc < 0) any entry of the id.
    bool find(TetraIdKind k, uint32_t id, int mcc, int mnc, TetraNameEntry& out) const;
    std::string label(TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1) const;   // "" when unknown
    size_t size() const { return m_.size(); }
    size_t count(TetraIdKind k) const;
    void clear() { m_.clear(); }
    std::vector<TetraNameEntry> entries(TetraIdKind k, const std::string& filter = "") const;   // sorted by id; the filter matches id, name, notes, site (case-insensitive words)
private:
    std::map<std::tuple<int, uint32_t, int, int>, TetraNameEntry> m_;
};

// "1234" -> "1234  Name" (the number alone when the book has no name)
std::string tetraNameWithId(TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1);

// The book of the app, safe to use from the UI and loaders. File: the user's data folder; the UI loads it once and saves after a change.
std::string tetraNameLabel(TetraIdKind k, uint32_t id, int mcc = -1, int mnc = -1);
TetraNames tetraNamesSnapshot();
void tetraNamesEdit(const std::function<void(TetraNames&)>& f);
uint64_t tetraNamesRevision();                          // grows with every change
bool tetraNamesLoadFile(const std::string& path, TetraNames& into, size_t* skipped = nullptr);   // false: file unreadable
bool tetraNamesSaveFile(const std::string& path, const TetraNames& from);                         // false: not written

}  // namespace dect2
