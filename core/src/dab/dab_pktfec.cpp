// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/dab/dab_pktfec.h"
#include "dect2/dvbt/dvbt.h"
#include <cstring>

namespace dect2 {

namespace {
constexpr size_t kData = 2256, kFec = 216;
bool fecHeader(const uint8_t* p, int counter) { return (p[0] >> 6) == 0 && ((p[0] >> 2) & 15) == counter && ((p[0] & 3) << 8 | p[1]) == 1022; }
bool isFecSet(const uint8_t* p) { for (int c = 0; c < 9; c++) if (!fecHeader(p + (size_t)c * 24, c)) return false; return true; }
}

void DabPacketFec::reset() { buf_.clear(); synced_ = false; sets_ = fixed_ = bad_ = 0; }

void DabPacketFec::takeSet(const uint8_t* d, const uint8_t* fec, std::vector<uint8_t>& out) {
    // the RS table in the FEC data fields: row 1 .. 12 of column 1, then column 2 ...
    uint8_t par[192]; size_t q = 0;
    for (int pk = 0; pk < 9; pk++) for (int b = 2; b < 24 && q < 192; b++) par[q++] = fec[(size_t)pk * 24 + (size_t)b];
    std::vector<uint8_t> data(d, d + kData);
    for (int r = 0; r < 12; r++) {
        uint8_t row[204];
        for (int c = 0; c < 188; c++) row[c] = data[(size_t)c * 12 + (size_t)r];
        for (int c = 0; c < 16; c++) row[188 + c] = par[(size_t)c * 12 + (size_t)r];
        const int e = dvbt::rsDecode(row);
        if (e < 0) { bad_++; continue; }
        if (e > 0) { fixed_++; for (int c = 0; c < 188; c++) data[(size_t)c * 12 + (size_t)r] = row[c]; }
    }
    out.insert(out.end(), data.begin(), data.end());
    sets_++;
}

void DabPacketFec::feed(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    buf_.insert(buf_.end(), p, p + n);
    size_t pos = 0;
    for (;;) {
        if (buf_.size() - pos < kData + kFec) break;
        if (synced_) {
            if (isFecSet(buf_.data() + pos + kData)) { takeSet(buf_.data() + pos, buf_.data() + pos + kData, out); pos += kData + kFec; continue; }
            synced_ = false;
        }
        // look for a set of nine FEC packets; the packet set before it is whole only once a whole one has gone by
        bool found = false;
        for (size_t i = pos + kData; i + kFec <= buf_.size(); i++) if (isFecSet(buf_.data() + i)) { pos = i - kData; found = true; break; }
        if (!found) { if (buf_.size() > kFec) pos = buf_.size() - kFec - kData > pos ? buf_.size() - kFec - kData : pos; break; }
        synced_ = true;
    }
    buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)pos);
}

std::vector<uint8_t> dabPacketFecEncode(const uint8_t* data) {
    uint8_t par[192];
    for (int r = 0; r < 12; r++) {
        uint8_t row[188], cw[204];
        for (int c = 0; c < 188; c++) row[c] = data[(size_t)c * 12 + (size_t)r];
        dvbt::rsEncode(row, cw);
        for (int c = 0; c < 16; c++) par[(size_t)c * 12 + (size_t)r] = cw[188 + c];
    }
    std::vector<uint8_t> o(data, data + kData);
    size_t q = 0;
    for (int pk = 0; pk < 9; pk++) { uint8_t h[24] = {}; h[0] = (uint8_t)(pk << 2 | (1022 >> 8)); h[1] = (uint8_t)(1022 & 255); for (int b = 2; b < 24 && q < 192; b++) h[b] = par[q++]; o.insert(o.end(), h, h + 24); }
    return o;
}

}  // namespace dect2
