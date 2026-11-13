// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Result filters of the LTE and NR cell lists (#164): which cells the table, the sites view and the export show. Pure functions on the telemetry,
// so the same rules serve the screens and the tests. A filter never changes what the receiver finds, only what is listed.
#pragma once
#include "dect2/cell/cell_tel.h"
#include "dect2/nr/nr_tel.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace dect2 {

struct CellFilter {
    std::vector<int> hideBands;      // cells whose band is one of these are left out
    std::vector<int> onlyBands;      // when not empty: only cells in these bands
    std::string text;                // operator / PLMN / eNB / TAC / PCI search (case-insensitive substring); matches any of the cell's PLMNs
    float minRsrpDb = -200;          // weaker cells are left out (dBFS, the radio's scale)
    bool confirmedBandOnly = false;  // only cells whose band SIB1 confirmed (not the first band that holds the frequency)
    bool identifiedOnly = false;     // only cells whose SIB1 was read
    bool hideBarred = false;
    bool collapseDuplicates = false; // one row per (PLMN, cell id): the same cell heard on a second frequency is dropped, the stronger one stays
};

// "14 28, 64" -> {14, 28, 64}: any run of digits is a number, anything else separates
inline std::vector<int> parseBandList(const std::string& s) {
    std::vector<int> v;
    for (size_t i = 0; i < s.size();) {
        if (s[i] >= '0' && s[i] <= '9') { char* e; v.push_back((int)strtol(s.c_str() + i, &e, 10)); i = (size_t)(e - s.c_str()); } else i++;
    }
    return v;
}

inline std::string lowerText(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }

// The searchable text of a cell: every PLMN ("310-410", "313-100"), the operator, the eNB, TAC, cell id and PCI.
inline std::string cellSearchText(const CellTower& c) {
    std::string s;
    for (const auto& p : c.plmns) s += p + " ";
    s += c.op + " ";
    if (c.hasSib1) s += "enb " + std::to_string(c.enb) + " tac " + std::to_string(c.tac) + " id " + std::to_string(c.cellId) + " ";
    s += "pci " + std::to_string(c.pci);
    return lowerText(s);
}

inline bool cellPasses(const CellTower& c, const CellFilter& f) {
    auto in = [](const std::vector<int>& v, int b) { return std::find(v.begin(), v.end(), b) != v.end(); };
    if (c.band > 0 && in(f.hideBands, c.band)) return false;
    if (!f.onlyBands.empty() && !(c.band > 0 && in(f.onlyBands, c.band))) return false;
    if (c.rsrpDb < f.minRsrpDb) return false;
    if (f.confirmedBandOnly && !c.bandConfirmed) return false;
    if (f.identifiedOnly && !c.hasSib1) return false;
    if (f.hideBarred && c.barred) return false;
    if (!f.text.empty() && cellSearchText(c).find(lowerText(f.text)) == std::string::npos) return false;
    return true;
}

// Indices of the cells to list, in telemetry order. With collapseDuplicates a cell that another listed cell has the same PLMN and cell id as is dropped
// when it is the weaker one (cells without SIB1 have no id and are all kept).
inline std::vector<size_t> filterCells(const std::vector<CellTower>& cells, const CellFilter& f) {
    std::vector<size_t> v;
    for (size_t i = 0; i < cells.size(); i++) if (cellPasses(cells[i], f)) v.push_back(i);
    if (!f.collapseDuplicates) return v;
    std::vector<size_t> out;
    for (size_t i : v) {
        const CellTower& a = cells[i];
        bool drop = false;
        if (a.hasSib1 && !a.plmns.empty()) for (size_t j : v) {
            const CellTower& b = cells[j];
            if (j == i || !b.hasSib1 || b.plmns.empty() || b.plmns[0] != a.plmns[0] || b.cellId != a.cellId) continue;
            if (b.rsrpDb > a.rsrpDb || (b.rsrpDb == a.rsrpDb && j < i)) { drop = true; break; }
        }
        if (!drop) out.push_back(i);
    }
    return out;
}

// NR cells: the same band, level and MIB rules (a gNB's PLMN is in SIB1, which is not decoded)
struct NrFilter {
    std::vector<int> hideBands, onlyBands;
    float minRsrpDb = -200;
    bool mibOnly = false;            // only cells whose PBCH decoded
    bool hideBarred = false;
};
inline bool nrCellPasses(const NrCell& c, const NrFilter& f) {
    auto in = [](const std::vector<int>& v, int b) { return std::find(v.begin(), v.end(), b) != v.end(); };
    if (c.band > 0 && in(f.hideBands, c.band)) return false;
    if (!f.onlyBands.empty() && !(c.band > 0 && in(f.onlyBands, c.band))) return false;
    if (c.rsrpDb < f.minRsrpDb) return false;
    if (f.mibOnly && !c.hasMib) return false;
    if (f.hideBarred && c.hasMib && c.barred) return false;
    return true;
}

} // namespace dect2
