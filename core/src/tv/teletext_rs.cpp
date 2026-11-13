// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The teletext layer of teletext.h on top of the Rust crate rust/onair_dvbsi: Hamming codes, the G0 national option subsets, the renderer,
// the PES data unit walk and the data unit decoder. The decoder class with its page store stays in teletext.cpp.
#include "dect2/tv/teletext.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dvbsi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[10];
        onair_dvbsi_struct_sizes(s);
        dect2::rsAbiCheck("onair_dvbsi", s, {sizeof(OnairDvbsiTtxUnit), sizeof(OnairDvbsiTtxCell)});
    }
} abiCheck;
}

int ttxHamming84(uint8_t b) { return onair_dvbsi_ttx_hamming84(b); }
int ttxHamming2418(const uint8_t* b3) { return onair_dvbsi_ttx_hamming2418(b3); }
uint8_t ttxReverse8(uint8_t b) { return onair_dvbsi_ttx_reverse8(b); }
uint32_t ttxG0(uint8_t code, int national) { return onair_dvbsi_ttx_g0(code, national); }

void ttxRender(const TtxPage& p, TtxGrid& g) {
    uint8_t rows[25 * 40];
    for (int r = 0; r < 25; r++) memcpy(rows + r * 40, p.rows[r].data(), 40);
    OnairDvbsiTtxCell cells[25 * 40];
    onair_dvbsi_ttx_render(rows, p.rowMask, p.national, cells);
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const OnairDvbsiTtxCell& k = cells[r * 40 + c];
            TtxCell& o = g[r][c];
            o.ch = k.ch; o.mosaic = k.mosaic != 0; o.sextants = k.sextants; o.separated = k.separated != 0; o.fg = k.fg; o.bg = k.bg; o.dh = k.dh;
            o.flash = k.flash != 0; o.conceal = k.conceal != 0; o.boxed = k.boxed != 0;
        }
}

std::string ttxRowText(const TtxGrid& g, int row) {
    if (row < 0 || row >= 25) return std::string();
    OnairDvbsiTtxCell cells[25 * 40];
    for (int r = 0; r < 25; r++)
        for (int c = 0; c < 40; c++) {
            const TtxCell& o = g[r][c];
            OnairDvbsiTtxCell& k = cells[r * 40 + c];
            k.ch = o.ch; k.sextants = o.sextants; k.fg = o.fg; k.bg = o.bg; k.dh = o.dh; k.mosaic = o.mosaic; k.separated = o.separated; k.flash = o.flash;
            k.conceal = o.conceal; k.boxed = o.boxed;
        }
    uint8_t b[160];
    return std::string((const char*)b, onair_dvbsi_ttx_row_text(cells, row, b, sizeof b));
}

TtxUnit ttxDecodeUnit(const uint8_t* u44) {
    OnairDvbsiTtxUnit c;
    onair_dvbsi_ttx_decode_unit(u44, &c);
    TtxUnit r;
    r.kind = c.kind; r.serial = c.serial != 0; r.pageOk = c.page_ok != 0; r.erase = c.erase != 0; r.newsflash = c.newsflash != 0; r.subtitle = c.subtitle != 0;
    r.inhibit = c.inhibit != 0; r.mag = c.mag; r.row = c.row; r.number = c.number; r.subcode = c.subcode; r.national = c.national;
    for (int i = 0; i < 6; i++) { r.linkAct[i] = c.link_act[i]; r.linkPage[i] = c.link_page[i]; r.linkSub[i] = c.link_sub[i]; }
    memcpy(r.head, c.head, 32);
    memcpy(r.rowData, c.row_data, 40);
    return r;
}

size_t ttxPesUnits(const uint8_t* pes, size_t n, uint32_t* off, size_t cap) { return onair_dvbsi_ttx_pes_units(pes, n, off, cap); }

} // namespace dect2
