// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) SIG fields: S1G_1M SIG (36 bits, 6 BPSK symbols, 2x repeated) and the S1G_SHORT / S1G_LONG SIG(-A) (52 bits, 2 symbols), with the
// CRC-4 (IEEE 802.11-2020 clause 23.3.9 SIG fields, tables 23-? as remembered; bit positions UNVERIFIED against the standard text, they follow the
// public comment-resolution documents of the TGah task group, 802.11-14/0371, -16/0002). The data bits of a field are sent least significant first.
#include "dect2/halow/halow_phy.h"
#include "dect2/common/bits.h"

namespace dect2::halow {

namespace {
void putBits(std::vector<uint8_t>& v, size_t at, uint32_t x, int n) { bits::putLsb(v.data(), at, x, n); }
uint32_t getBits(const uint8_t* v, size_t at, int n) { return (uint32_t)bits::getLsb(v, at, n); }
}

// Long training fields for Nsts space-time streams (clause 23.3.9.5): 1, 2, 4, 4
int ltfCount(int nsts) { return nsts <= 0 ? 1 : nsts == 1 ? 2 : 4; }

// Symbols with the full guard interval that sit between the SIG field and the data: the further LTFs (S1G_SHORT, S1G_1M), or D-STF + D-LTFs + SIG-B (S1G_LONG)
int extraSyms(int pre, int nsts) { return pre == PreLong ? 2 + ltfCount(nsts) : ltfCount(nsts) - 1; }

// S1G_1M SIG (23.3.9.4): Nsts(2) SGI Coding LDPC-extra STBC reserved MCS(4) Aggregation Length(9) Response(2) Smoothing TravelingPilots NDP CRC(4) tail(6).
// S1G_SHORT / LONG SIG(-A) (23.3.9.3): B0 reserved (SU/MU in the long preamble), STBC, Uplink, BW(2: 2/4/8/16 MHz), Nsts(2), partial AID(9), SGI, Coding,
// LDPC-extra, MCS(4), Smoothing, Aggregation, Length(9), Response(2), TravelingPilots, NDP, beamformed (author's choice for the long preamble), reserved, CRC(4), tail(6).
std::vector<uint8_t> sigBits(const Sig& s) {
    const WidthInfo& wi = info(s.w);
    std::vector<uint8_t> b((size_t)wi.sigInfoBits, 0);
    size_t infoLen;
    if (s.w == W1) {
        putBits(b, 0, (uint32_t)s.nsts, 2); putBits(b, 2, s.sgi, 1); putBits(b, 3, s.ldpc, 1); putBits(b, 4, s.ldpcExtra, 1); putBits(b, 5, s.stbc, 1);
        putBits(b, 6, 1, 1); putBits(b, 7, (uint32_t)s.mcs, 4); putBits(b, 11, s.agg, 1); putBits(b, 12, (uint32_t)s.len, 9);
        putBits(b, 21, (uint32_t)s.resp, 2); putBits(b, 23, s.smoothing, 1); putBits(b, 24, s.travelPilots, 1); putBits(b, 25, s.ndp, 1);
        infoLen = 26;
    } else {
        putBits(b, 0, s.pre == PreLong ? 0 : 1, 1); putBits(b, 1, s.stbc, 1); putBits(b, 2, s.ul, 1); putBits(b, 3, (uint32_t)(s.w - 1), 2);
        putBits(b, 5, (uint32_t)s.nsts, 2); putBits(b, 7, (uint32_t)s.id, 9); putBits(b, 16, s.sgi, 1); putBits(b, 17, s.ldpc, 1);
        putBits(b, 18, s.ldpcExtra, 1); putBits(b, 19, (uint32_t)s.mcs, 4); putBits(b, 23, s.smoothing, 1); putBits(b, 24, s.agg, 1);
        putBits(b, 25, (uint32_t)s.len, 9); putBits(b, 34, (uint32_t)s.resp, 2); putBits(b, 36, s.travelPilots, 1); putBits(b, 37, s.ndp, 1);
        putBits(b, 38, s.beamformed, 1); putBits(b, 39, 7, 3);
        infoLen = 42;
    }
    const uint8_t c = crc4(b.data(), infoLen);
    for (int i = 0; i < 4; i++) b[infoLen + (size_t)i] = (uint8_t)(c >> (3 - i) & 1);
    return b;   // the six tail bits stay 0
}

// Reads a decoded SIG field; false when the CRC, the tail, the bandwidth field or the MCS / stream combination is wrong. pre: PreShort or PreLong at 2 MHz and up.
bool parseSig(const uint8_t* bits, int w, Sig& s, int pre) {
    const WidthInfo& wi = info(w);
    const size_t infoLen = w == W1 ? 26 : 42;
    const uint8_t c = crc4(bits, infoLen);
    uint8_t r = 0;
    for (int i = 0; i < 4; i++) r = (uint8_t)(r << 1 | (bits[infoLen + (size_t)i] & 1));
    if (r != c) return false;
    for (size_t i = infoLen + 4; i < (size_t)wi.sigInfoBits; i++) if (bits[i] & 1) return false;   // the tail
    s = Sig();
    s.w = w;
    if (w == W1) {
        s.pre = Pre1M;
        s.nsts = (int)getBits(bits, 0, 2); s.sgi = getBits(bits, 2, 1); s.ldpc = getBits(bits, 3, 1); s.ldpcExtra = getBits(bits, 4, 1); s.stbc = getBits(bits, 5, 1);
        s.mcs = (int)getBits(bits, 7, 4); s.agg = getBits(bits, 11, 1); s.len = (int)getBits(bits, 12, 9); s.resp = (int)getBits(bits, 21, 2);
        s.smoothing = getBits(bits, 23, 1); s.travelPilots = getBits(bits, 24, 1); s.ndp = getBits(bits, 25, 1);
        if (!getBits(bits, 6, 1)) return false;   // the reserved bit is sent as 1 (a noise decode passes it half of the time)
    } else {
        s.pre = pre == PreLong ? PreLong : PreShort;
        s.stbc = getBits(bits, 1, 1); s.ul = getBits(bits, 2, 1);
        if ((int)getBits(bits, 3, 2) != w - 1) return false;   // the bandwidth field disagrees with where it was heard
        s.nsts = (int)getBits(bits, 5, 2); s.id = (int)getBits(bits, 7, 9); s.sgi = getBits(bits, 16, 1); s.ldpc = getBits(bits, 17, 1);
        s.ldpcExtra = getBits(bits, 18, 1); s.mcs = (int)getBits(bits, 19, 4); s.smoothing = getBits(bits, 23, 1); s.agg = getBits(bits, 24, 1);
        s.len = (int)getBits(bits, 25, 9); s.resp = (int)getBits(bits, 34, 2); s.travelPilots = getBits(bits, 36, 1); s.ndp = getBits(bits, 37, 1);
        s.beamformed = getBits(bits, 38, 1);
        if (getBits(bits, 39, 3) != 7) return false;   // reserved bits are sent as 1
        if (s.pre == PreLong && getBits(bits, 0, 1)) return false;   // the long preamble's SU / MU bit: only single user is handled
    }
    if (s.stbc && (s.nsts & 1) == 0) return false;               // STBC doubles the streams: Nsts is even-numbered streams
    if (s.mcs > 10 || (!s.ndp && !mcsValid(w, s.mcs, s.nss()))) return false;
    return true;
}

} // namespace dect2::halow
