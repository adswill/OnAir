// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DMR burst layout and message formats, see dmr_proto.h.
#include "dect2/dmr/dmr_proto.h"
#include "dect2/rs/dmr.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace dmr {

// ---------------------------------------------------------------------------------------------------- sync patterns

const uint64_t kSyncWords[kSyncCount] = {
    0x755FD7DF75F7ULL,   // BS sourced voice
    0xDFF57D75DF5DULL,   // BS sourced data
    0x7F7D5DD57DFDULL,   // MS sourced voice
    0xD5D7F77FD757ULL,   // MS sourced data
    0x77D55F7DFD77ULL,   // standalone reverse channel
    0x5D577F7757FFULL,   // TDMA direct mode slot 1 voice
    0xF7FDD5DDFD55ULL,   // slot 1 data
    0x7DFFD5F55D5FULL,   // slot 2 voice
    0xD7557F5FF7F5ULL};  // slot 2 data

// the three burst slicers are plain vector copies and stay C++ in every build
Bits burstCentre(const Bits& burst) { return Bits(burst.begin() + 108, burst.begin() + 156); }

void burstDataPayload(const Bits& burst, Bits& p) {
    p.assign(burst.begin(), burst.begin() + 98);
    p.insert(p.end(), burst.begin() + 166, burst.begin() + 264);
}

void burstVoicePayload(const Bits& burst, Bits& vs) {
    vs.assign(burst.begin(), burst.begin() + 108);
    vs.insert(vs.end(), burst.begin() + 156, burst.begin() + 264);
}



// two byte helpers used everywhere (also by the Rust-backed build)
uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
void putBe24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }



// the test signal's alias PDU builder stays C++ in every build
static int aliasPieces(int format, int length) { return onair_dmr_alias_pieces(format, length); }
int talkerAliasPdus(const std::string& alias, int format, FullLc out[4]) {
    std::vector<uint8_t> bytes;
    int length = 0;
    if (format == 3) {
        for (unsigned char c : alias) { bytes.push_back(0); bytes.push_back(c); }
        length = (int)alias.size();
    } else {
        bytes.assign(alias.begin(), alias.end());
        length = (int)alias.size();
    }
    length = std::min(length, format == 3 ? 13 : 27);
    bytes.resize(28, 0);
    for (int i = 0; i < 4; i++) { out[i] = FullLc(); out[i].flco = kFlcoTalkerAliasHeader + i; }
    uint8_t* h = out[0].raw;
    h[0] = (uint8_t)kFlcoTalkerAliasHeader;
    h[2] = (uint8_t)((format << 6) | (length << 1));
    for (int i = 0; i < 6; i++) h[3 + i] = bytes[i];
    for (int b = 1; b < 4; b++) {
        out[b].raw[0] = (uint8_t)(kFlcoTalkerAliasHeader + b);
        for (int i = 0; i < 7; i++) out[b].raw[2 + i] = bytes[6 + 7 * (b - 1) + i];
    }
    return aliasPieces(format, length);
}

} // namespace dmr
} // namespace dect2
