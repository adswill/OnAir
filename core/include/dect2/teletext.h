// EBU teletext (ETS 300 706) carried in DVB PES packets (EN 300 472): extraction, page assembly and a character-cell
// renderer (colours, mosaics, double height) that a UI can draw directly.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
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
};

struct TtxCell {
    uint32_t ch = ' ';         // character (Unicode) when !mosaic
    bool mosaic = false;
    uint8_t sextants = 0;      // bit0 TL, 1 TR, 2 ML, 3 MR, 4 BL, 5 BR
    bool separated = false;
    uint8_t fg = 7, bg = 0;    // 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white
    uint8_t dh = 0;            // 1 = top half of a double-height character, 2 = bottom half
    bool flash = false;
};
using TtxGrid = std::array<std::array<TtxCell, 40>, 25>;

// Hamming 8/4 decode: returns the nibble, or -1 if uncorrectable.
int ttxHamming84(uint8_t b);
uint8_t ttxReverse8(uint8_t b);
// Lay a page out as character cells (spacing attributes, mosaics, double height handled).
void ttxRender(const TtxPage& p, TtxGrid& g);

class TeletextDecoder {
public:
    void setPid(int pid);                 // -1 disables
    int pid() const;
    void reset();
    void feedTs(const uint8_t* pkt188);   // cheap PID check; reassembles PES packets
    void feedPes(const uint8_t* pes, size_t n);   // exposed for tests
    bool page(int number, TtxPage& out) const;    // latest copy of a page
    std::vector<int> pages() const;               // page numbers seen so far
    uint64_t packets() const;

private:
    void dataUnit(const uint8_t* d44);
    mutable std::mutex mu_;
    int pid_ = -1;
    std::vector<uint8_t> pes_;
    bool inPes_ = false;
    size_t pesLen_ = 0;
    int cc_ = -1;
    std::map<int, TtxPage> pages_;
    int cur_[9];                          // current page key per magazine (index 1..8), -1 none
    bool curInit_ = false;
    uint64_t version_ = 0, packets_ = 0;
};

} // namespace dect2
