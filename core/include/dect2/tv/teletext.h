// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// EBU teletext (ETS 300 706) carried in DVB PES packets (EN 300 472): extraction, page assembly and a character-cell
// renderer (colours, mosaics, double height) that a UI can draw directly.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct TtxPage {
    int number = 0;            // 100..899 (decimal digits only)
    int subcode = 0;
    int national = 0;          // C12..C14 national option subset
    bool subtitle = false;     // C6
    bool newsflash = false;    // C5
    bool inhibit = false;      // C10
    std::array<std::array<uint8_t, 40>, 25> rows{};
    uint32_t rowMask = 0;      // rows received
    uint64_t version = 0;
    bool serial = false;       // C11: serial transmission (a header of any magazine ends the page)
    bool complete = false;     // the next header after this page's rows has been seen
    int fastext[6] = {0, 0, 0, 0, 0, 0};   // packet X/27/0 links: red, green, yellow, cyan, index (white), spare; page number 100..899, 0 = none
    int fastextSub[6] = {0, 0, 0, 0, 0, 0};
};

struct TtxCell {
    uint32_t ch = ' ';         // character (Unicode) when !mosaic
    bool mosaic = false;
    uint8_t sextants = 0;      // bit0 TL, 1 TR, 2 ML, 3 MR, 4 BL, 5 BR
    bool separated = false;
    uint8_t fg = 7, bg = 0;    // 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white
    uint8_t dh = 0;            // 1 = top half of a double-height character, 2 = bottom half
    bool flash = false;
    bool conceal = false;      // hidden until the viewer asks to reveal (control code 0x18)
    bool boxed = false;        // inside a start box / end box pair (subtitles and newsflashes show only boxed cells); true everywhere on a page with no box codes
};
using TtxGrid = std::array<std::array<TtxCell, 40>, 25>;

// Hamming 8/4 decode: returns the nibble, or -1 if uncorrectable.
int ttxHamming84(uint8_t b);
uint8_t ttxReverse8(uint8_t b);
// Hamming 24/18 decode of three bytes (as transmitted, bit 1 first): the 18 data bits (D1 in bit 0), or -1 if the word has two errors.
// A single bit error anywhere in the word is corrected.
int ttxHamming2418(const uint8_t* b3);
// Lay a page out as character cells (spacing attributes, mosaics, double height handled).
void ttxRender(const TtxPage& p, TtxGrid& g);
// The characters of one row of a rendered page as UTF-8 (mosaics and hidden cells as spaces)
std::string ttxRowText(const TtxGrid& g, int row);
// The Unicode character for a G0 code (0x20..0x7F) in the Latin national option subset (C12..C14: 0 English, 1 German, 2 Swedish/Finnish/Hungarian,
// 3 Italian, 4 French, 5 Portuguese/Spanish, 6 Czech/Slovak, 7 falls back to English)
uint32_t ttxG0(uint8_t code, int national);

// The pure layer below the decoder (Rust crate rust/onair_dvbsi, teletext_rs.cpp). One teletext data unit decoded: the 44 byte
// payload of a PES data unit (field / line, framing code, 2 address bytes, 40 data bytes, bit reversed) as a header, row or Fastext record.
struct TtxUnit {
    enum Kind { Ignore = 0, Header = 1, Row = 2, Fastext = 3, Other = 4 };   // Ignore: bad framing code or address (not counted); Other: counted, no effect
    int kind = Ignore;
    bool serial = false;       // header: C11 serial transmission
    bool pageOk = false;       // header: decimal page number (not a time filler): a page is opened
    bool erase = false, newsflash = false, subtitle = false, inhibit = false;   // header: C4, C5, C6, C10
    int linkAct[6] = {0, 0, 0, 0, 0, 0};   // Fastext, per link: 0 unchanged, 1 cleared (no link), 2 set to linkPage / linkSub
    int mag = 0, row = 0;      // magazine 1..8, packet address row
    int number = 0, subcode = 0, national = 0;   // header: page number 100..899, 13 bit subcode (0 if a nibble was bad), C12..C14
    int linkPage[6] = {0, 0, 0, 0, 0, 0}, linkSub[6] = {0, 0, 0, 0, 0, 0};
    uint8_t head[32] = {};     // header: the 32 characters of the header text (7 bit)
    uint8_t rowData[40] = {};  // row: the 40 characters (7 bit)
};
TtxUnit ttxDecodeUnit(const uint8_t* u44);
// Offsets (into the PES) of the 44 byte data units with id 0x02 / 0x03 in a teletext PES packet (EN 300 472); returns how many there are (also past `cap`).
size_t ttxPesUnits(const uint8_t* pes, size_t n, uint32_t* off, size_t cap);

class TeletextDecoder {
public:
    void setPid(int pid);                 // -1 disables
    int pid() const;
    void reset();
    void feedTs(const uint8_t* pkt188);   // cheap PID check; reassembles PES packets
    void feedPes(const uint8_t* pes, size_t n);   // exposed for tests
    // The newest completed copy of a page (a partial one while none has completed); subcode -1 = whichever subpage arrived last.
    bool page(int number, TtxPage& out, int subcode = -1) const;
    std::vector<int> pages() const;               // page numbers seen so far
    std::vector<int> subpages(int number) const;  // subcodes held for a page number, ascending
    uint64_t packets() const;

private:
    void dataUnit(const uint8_t* d44);
    mutable std::mutex mu_;
    int pid_ = -1;
    std::vector<uint8_t> pes_;
    bool inPes_ = false;
    size_t pesLen_ = 0;
    int cc_ = -1;
    struct Entry { TtxPage live, done; bool haveDone = false; };
    std::map<uint32_t, Entry> pages_;     // key = page number << 16 | subcode: the page cache
    int64_t cur_[9];                      // current page key per magazine (index 1..8), -1 none
    void closePage(int64_t key);
    bool curInit_ = false;
    uint64_t version_ = 0, packets_ = 0;
};

} // namespace dect2
