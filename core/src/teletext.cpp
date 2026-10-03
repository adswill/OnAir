#include "dect2/teletext.h"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dect2 {

// ------------------------------------------------------------------ hamming
namespace {
uint8_t hammEncode(int n) {
    const int d1 = n & 1, d2 = (n >> 1) & 1, d3 = (n >> 2) & 1, d4 = (n >> 3) & 1;
    const int p1 = 1 ^ d1 ^ d3 ^ d4, p2 = 1 ^ d1 ^ d2 ^ d4, p3 = 1 ^ d1 ^ d2 ^ d3;
    const int x7 = p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
    const int p4 = 1 ^ x7;
    return (uint8_t)(p1 | d1 << 1 | p2 << 2 | d2 << 3 | p3 << 4 | d3 << 5 | p4 << 6 | d4 << 7);
}
struct HammTab {
    int8_t dec[256];
    HammTab() {
        for (int b = 0; b < 256; b++) {
            int best = -1, bd = 9;
            for (int n = 0; n < 16; n++) {
                int d = __builtin_popcount((unsigned)(b ^ hammEncode(n)));
                if (d < bd) { bd = d; best = n; }
            }
            dec[b] = bd <= 1 ? (int8_t)best : (int8_t)-1;
        }
    }
};
const HammTab& hamm() { static HammTab t; return t; }
} // namespace

int ttxHamming84(uint8_t b) { return hamm().dec[b]; }
uint8_t ttxReverse8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
    return b;
}

// ------------------------------------------------------------------ decoder
void TeletextDecoder::setPid(int pid) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pid == pid_) return;
    pid_ = pid;
    pes_.clear(); inPes_ = false; cc_ = -1;
    pages_.clear();
    curInit_ = false;
}
int TeletextDecoder::pid() const { std::lock_guard<std::mutex> lk(mu_); return pid_; }
void TeletextDecoder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    pes_.clear(); inPes_ = false; cc_ = -1; pages_.clear(); curInit_ = false; packets_ = 0;
}
uint64_t TeletextDecoder::packets() const { std::lock_guard<std::mutex> lk(mu_); return packets_; }

void TeletextDecoder::feedTs(const uint8_t* p) {
    if (p[0] != 0x47) return;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pid != pid_) return;
    }
    const bool pusi = (p[1] & 0x40) != 0;
    const int afc = (p[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) off += 1 + p[4];
    if (!(afc & 1) || off >= 188) return;
    std::vector<uint8_t> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int cc = p[3] & 15;
        if (cc_ >= 0 && ((cc_ + 1) & 15) != cc && !pusi) { pes_.clear(); inPes_ = false; } // lost packets: drop the partial PES
        cc_ = cc;
        if (pusi) {
            if (inPes_ && !pes_.empty()) done.swap(pes_);
            pes_.clear();
            inPes_ = true;
            pesLen_ = 0;
        }
        if (inPes_) {
            pes_.insert(pes_.end(), p + off, p + 188);
            if (pes_.size() >= 6 && pesLen_ == 0 && pes_[0] == 0 && pes_[1] == 0 && pes_[2] == 1) {
                const size_t l = ((size_t)pes_[4] << 8) | pes_[5];
                if (l) pesLen_ = l + 6;
            }
            if (pesLen_ && pes_.size() >= pesLen_ && done.empty()) { done.swap(pes_); inPes_ = false; pesLen_ = 0; }
        }
    }
    if (!done.empty()) feedPes(done.data(), done.size());
}

void TeletextDecoder::feedPes(const uint8_t* pes, size_t n) {
    if (n < 10 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1) return;
    if (pes[3] != 0xBD) return; // private stream 1
    const size_t hdr = 9 + pes[8];
    if (hdr + 1 > n) return;
    const uint8_t* d = pes + hdr;
    size_t left = n - hdr;
    if (d[0] < 0x10 || d[0] > 0x1F) return; // data_identifier
    d++; left--;
    while (left >= 2) {
        const uint8_t id = d[0], len = d[1];
        if (2u + len > left) break;
        if ((id == 0x02 || id == 0x03) && len == 44) dataUnit(d + 2);
        d += 2 + len; left -= 2 + len;
    }
}

void TeletextDecoder::dataUnit(const uint8_t* u) {
    // u[0] field/line, u[1] framing code, u[2..43] = 2 address bytes + 40 data bytes (bit-reversed in the PES)
    if (u[1] != 0xE4 && u[1] != 0x27) return;
    uint8_t b[42];
    for (int i = 0; i < 42; i++) b[i] = ttxReverse8(u[2 + i]);
    const int a0 = ttxHamming84(b[0]), a1 = ttxHamming84(b[1]);
    if (a0 < 0 || a1 < 0) return;
    const int addr = a0 | a1 << 4;
    int mag = addr & 7;
    if (mag == 0) mag = 8;
    const int row = addr >> 3;
    std::lock_guard<std::mutex> lk(mu_);
    if (!curInit_) { for (int i = 0; i < 9; i++) cur_[i] = -1; curInit_ = true; }
    packets_++;
    if (row == 0) {
        const int un = ttxHamming84(b[2]), tn = ttxHamming84(b[3]);
        if (un < 0 || tn < 0) { cur_[mag] = -1; return; }
        if (un > 9 || tn > 9) { cur_[mag] = -1; return; } // time filler (0xFF) and non-decimal pages
        const int number = mag * 100 + tn * 10 + un;
        int nib[6];
        for (int i = 0; i < 6; i++) nib[i] = ttxHamming84(b[4 + i]);
        TtxPage& p = pages_[number];
        const bool erase = nib[1] >= 0 && (nib[1] & 8);
        if (erase || p.number == 0) { p.rows = {}; p.rowMask = 0; }
        p.number = number;
        if (nib[0] >= 0 && nib[1] >= 0 && nib[2] >= 0 && nib[3] >= 0) p.subcode = nib[0] | (nib[1] & 7) << 4 | nib[2] << 8 | (nib[3] & 3) << 12;
        p.newsflash = nib[3] >= 0 && (nib[3] & 4);
        p.subtitle = nib[3] >= 0 && (nib[3] & 8);
        p.inhibit = nib[4] >= 0 && (nib[4] & 8);
        p.national = nib[5] >= 0 ? ((nib[5] >> 1) & 7) : 0;
        for (int i = 0; i < 8; i++) p.rows[0][i] = ' ';
        for (int i = 0; i < 32; i++) p.rows[0][8 + i] = b[10 + i] & 0x7F;
        p.rowMask |= 1;
        p.version = ++version_;
        cur_[mag] = number;
    } else if (row >= 1 && row <= 24 && cur_[mag] >= 0) {
        TtxPage& p = pages_[cur_[mag]];
        for (int i = 0; i < 40; i++) p.rows[row][i] = b[2 + i] & 0x7F;
        p.rowMask |= 1u << row;
        p.version = ++version_;
    }
}

bool TeletextDecoder::page(int number, TtxPage& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pages_.find(number);
    if (it == pages_.end()) return false;
    out = it->second;
    return true;
}

std::vector<int> TeletextDecoder::pages() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto& kv : pages_) v.push_back(kv.first);
    return v;
}

// ------------------------------------------------------------------ renderer
namespace {
uint32_t g0(uint8_t c, int national) {
    (void)national; // only the English subset is implemented; other subsets fall back to it
    switch (c) {
    case 0x23: return 0x00A3;            // pound
    case 0x24: return '$';
    case 0x40: return '@';
    case 0x5B: return 0x2190;            // left arrow
    case 0x5C: return 0x00BD;            // one half
    case 0x5D: return 0x2192;            // right arrow
    case 0x5E: return 0x2191;            // up arrow
    case 0x5F: return '#';
    case 0x60: return 0x2014;
    case 0x7B: return 0x00BC;
    case 0x7C: return 0x2016;
    case 0x7D: return 0x00BE;
    case 0x7E: return 0x00F7;
    case 0x7F: return 0x25A0;
    default: return c;
    }
}
} // namespace

void ttxRender(const TtxPage& p, TtxGrid& g) {
    for (auto& r : g) for (auto& c : r) c = TtxCell();
    bool skipNext = false;
    for (int r = 0; r < 25; r++) {
        if (skipNext) { skipNext = false; continue; }   // lower half of a double-height row was filled in below
        if (!(p.rowMask & (1u << r))) continue;
        uint8_t fg = 7, bg = 0;
        bool mosaic = false, sep = false, dbl = false, flash = false, hold = false;
        uint8_t held = 0;
        bool rowDouble = false;
        for (int c = 0; c < 40; c++) {
            uint8_t code = p.rows[r][c] & 0x7F;
            TtxCell cell;
            if (r == 0 && c < 8) { cell.ch = ' '; g[r][c] = cell; continue; }
            if (code < 0x20) {
                // spacing attribute: takes effect from the next cell; this cell shows a space (or the held mosaic)
                switch (code) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: fg = code; mosaic = false; hold = false; break;
                case 0x08: flash = true; break;
                case 0x09: flash = false; break;
                case 0x0C: dbl = false; break;
                case 0x0D: case 0x0F: dbl = true; rowDouble = true; break;
                case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: fg = code & 7; mosaic = true; break;
                case 0x19: sep = false; break;
                case 0x1A: sep = true; break;
                case 0x1C: bg = 0; break;
                case 0x1D: bg = fg; break;
                case 0x1E: hold = true; break;
                case 0x1F: hold = false; break;
                default: break;
                }
                cell.fg = fg; cell.bg = bg; cell.flash = flash; cell.dh = dbl ? 1 : 0;
                if (hold && mosaic && held) { cell.mosaic = true; cell.sextants = held; cell.separated = sep; }
                else cell.ch = ' ';
                g[r][c] = cell;
                continue;
            }
            cell.fg = fg; cell.bg = bg; cell.flash = flash; cell.dh = dbl ? 1 : 0;
            if (mosaic && (code < 0x40 || code >= 0x60)) {
                cell.mosaic = true;
                cell.separated = sep;
                cell.sextants = (uint8_t)((code & 0x1F) | ((code & 0x40) ? 0x20 : 0));
                held = cell.sextants;
            } else cell.ch = g0(code, p.national);
            g[r][c] = cell;
        }
        if (rowDouble && r < 24) {
            for (int c = 0; c < 40; c++) {
                TtxCell lo = g[r][c];
                if (lo.dh == 1) lo.dh = 2; else { lo = TtxCell(); lo.bg = g[r][c].bg; lo.fg = g[r][c].fg; lo.ch = ' '; }
                g[r + 1][c] = lo;
            }
            skipNext = true;
        }
    }
}

} // namespace dect2
