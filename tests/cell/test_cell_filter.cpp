// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Cell list filters (#164): hidden / only bands, text search over every PLMN (AT&T 310-410 with FirstNet 313-100), minimum level, confirmed band,
// identified, barred, one row per cell id, and the NR band / level / MIB rules.
#include "dect2/cell/cell_filter.h"
#include "dect2/cell/cell_rx.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static CellTower cell(int band, bool confirmed, float rsrp, std::vector<std::string> plmns, uint32_t id, int pci) {
    CellTower c;
    c.band = band; c.bandConfirmed = confirmed; c.rsrpDb = rsrp; c.pci = pci;
    c.hasSib1 = !plmns.empty(); c.plmns = plmns; c.cellId = id; c.enb = id >> 8; c.tac = 100;
    if (!plmns.empty()) { c.mcc = 310; c.mnc = 410; c.op = "AT&T"; }
    return c;
}

int main() {
    {
        CHECK(parseBandList("14 28, 64") == std::vector<int>({14, 28, 64}) && parseBandList("").empty() && parseBandList("x") .empty(), "band list text");
        CHECK(std::string(lteOperatorName(313, 100)) == "FirstNet" && std::string(lteOperatorName(310, 410)) == "AT&T", "FirstNet and AT&T names");
    }
    std::vector<CellTower> v = {
        cell(66, true, -60, {"310-410", "313-100"}, 0x1234501, 10),   // shared network, strong
        cell(14, false, -80, {}, 0, 20),                               // frequency only: band guessed
        cell(28, true, -95, {"246-01"}, 0x2222201, 30),
        cell(66, true, -90, {"310-410", "313-100"}, 0x1234501, 11),   // the same cell id on a second frequency, weaker
    };
    v[3].barred = true;
    v[2].mcc = 246; v[2].mnc = 1; v[2].op = "Telia";
    CellFilter f;
    CHECK(filterCells(v, f).size() == 4, "no filter lists everything");
    f.hideBands = {14, 28};
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 3}), "hidden bands 14 and 28");
    f = CellFilter(); f.onlyBands = {28};
    CHECK(filterCells(v, f) == std::vector<size_t>({2}), "only band 28");
    f = CellFilter(); f.text = "313-100";
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 3}), "text matches the second PLMN (FirstNet)");
    f.text = "at&t";
    CHECK(filterCells(v, f).size() == 2, "text matches the operator name, case-insensitive");
    f.text = "pci 30";
    CHECK(filterCells(v, f) == std::vector<size_t>({2}), "text matches the PCI");
    f = CellFilter(); f.minRsrpDb = -85;
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 1}), "minimum level");
    f = CellFilter(); f.confirmedBandOnly = true;
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 2, 3}), "confirmed band only drops the guessed band 14");
    f = CellFilter(); f.identifiedOnly = true;
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 2, 3}), "identified only");
    f = CellFilter(); f.hideBarred = true;
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 1, 2}), "barred hidden");
    f = CellFilter(); f.collapseDuplicates = true;
    CHECK(filterCells(v, f) == std::vector<size_t>({0, 1, 2}), "same PLMN and cell id once, the stronger kept");
    f.hideBands = {66};
    CHECK(filterCells(v, f) == std::vector<size_t>({1, 2}), "filters combine");

    NrCell a, b, c;
    a.band = 78; a.rsrpDb = -70; a.hasMib = true;
    b.band = 41; b.rsrpDb = -100; b.hasMib = false;
    c.band = 78; c.rsrpDb = -90; c.hasMib = true; c.barred = true;
    NrFilter nf;
    CHECK(nrCellPasses(a, nf) && nrCellPasses(b, nf) && nrCellPasses(c, nf), "NR: no filter");
    nf.hideBands = {41};
    CHECK(nrCellPasses(a, nf) && !nrCellPasses(b, nf), "NR: hidden band");
    nf = NrFilter(); nf.minRsrpDb = -95; nf.mibOnly = true; nf.hideBarred = true;
    CHECK(nrCellPasses(a, nf) && !nrCellPasses(b, nf) && !nrCellPasses(c, nf), "NR: level, MIB and barred");
    return testSummary("cell filter");
}
