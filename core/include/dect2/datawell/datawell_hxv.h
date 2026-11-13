// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Datawell HXV real-time format (DWR-MkIII, DWR-G, WR-SG buoys, 2FSK at 81.92 bit/s), decoded from the published description:
//  - Datawell Waverider Reference Manual DWR-MkIII, section 5.7 "Data format" (cdip.ucsd.edu/m/documents/_downloads/ce99796d384cd0194a745160409da9ba/mk3.pdf)
//  - Datawell RX-C4 manual, appendix B (the HXV text lines the receiver prints; its examples were used to check the parity code).
//
// A 64-bit vector every 0.78125 s (1.28 Hz), most significant bit first:  cyclic data 16 | heave 12 | north 12 | west 12 | parity 12.
// Displacements are 12 bit sign-magnitude (bit 11 = 1: negative), in cm. 18 vectors make a block: the first carries the sync word 0x7FFF,
// the second a system file word (4 bit word number + 12 bit word), the other 16 four spectrum words for 4 frequencies each; 16 blocks give a full
// system file (sea state numbers, temperatures, position, battery) and a 64 band directional spectrum.
//
// The parity: the manual only says "a Galois code table encodes 63 bits of a vector". It was found from the vectors of a real recording and of the
// RX-C4 manual's examples (27 of 27 vectors agree): a BCH(63,51) code, generator x^12+x^10+x^8+x^5+x^4+x^3+1 (octal 12471), over the 63 bits
// of the vector without bit 27 (the least significant bit of the heave); it corrects up to 2 bit errors. That is a finding, not a published fact.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::datawell {

struct HxvVector {
    uint16_t cyc = 0;
    int h = 0, n = 0, w = 0;   // cm
    uint16_t parity = 0;
    int fixed = 0;             // bits repaired (0 .. 2)
    bool wrsg = false;         // the third field is the 0xAAA dummy of the WR-SG: h and n are two vertical samples, w is not a displacement
};

uint64_t hxvEncode(uint16_t cyc, int h, int n, int w);        // the 64 bit vector with its parity
int hxvCorrect(uint64_t& v);                                  // repairs up to 2 bit errors in place: the number repaired, -1 when it cannot
HxvVector hxvDecodeVector(uint64_t v, int fixed = 0);
int hxvSignMag(unsigned v12);                                 // 12 bit sign-magnitude to int

struct HxvSpecPoint {
    bool valid = false;
    int n = 0;
    double hz = 0, dirDeg = 0, spreadDeg = 0, m2 = 0, n2 = 0, k = 0, rpsd = 0;   // rpsd: relative to the peak density (PSDmax)
    double psd = 0;            // m^2/Hz, when PSDmax is known (0 otherwise)
};

struct HxvSystem {
    bool have[16] = {};
    unsigned word[16] = {};
    // decoded (valid when the word is there)
    int tn = 0; bool gpsFix = false, gpsModuleOk = false;
    double hrms = 0, hs = 0, fz = 0, tz = 0, psdMax = 0, tr = 0, tw = 0;
    int tolWeeks = 0, battery = 0;
    double av0 = 0, ax0 = 0, ay0 = 0;
    bool havePos = false; double lat = 0, lon = 0;
    double orientDeg = 0, inclDeg = 0;
};

class HxvDecoder {
public:
    HxvDecoder();
    void reset();
    void pushBit(int bit);
    // state
    bool locked() const { return locked_; }
    bool inverted() const { return inv_; }
    int phase() const { return phase_; }
    uint64_t vectors() const { return nVec_; }
    uint64_t clean() const { return nClean_; }
    uint64_t repaired() const { return nFixed_; }
    uint64_t lost() const { return nLost_; }
    uint64_t blocks() const { return nBlocks_; }
    const std::vector<HxvVector>& recent() const { return recent_; }     // oldest first, the last 128 good vectors
    const HxvSystem& system() const { return sys_; }
    const HxvSpecPoint* spectrum() const { return spec_; }               // 64 points
    int lastBlock() const { return blockNo_; }
    static HxvSystem decodeSystem(const HxvSystem& in);
private:
    void vector(uint64_t v);
    bool accept(uint64_t v, int fx);
    void word(const HxvVector& vec, bool ok);
    uint64_t win_ = 0;
    uint64_t old_ = 0;                // the 64 bits before win_
    int64_t searchUntil_ = -1;        // after a lost vector: look for the alignment again within a few bits of the old one (a bit slip)
    int lastH_ = 0; bool haveH_ = false;
    uint64_t nBits_ = 0;
    int cnt_[2][64] = {};
    bool locked_ = false, inv_ = false;
    int phase_ = 0, badRun_ = 0;
    uint64_t nVec_ = 0, nClean_ = 0, nFixed_ = 0, nLost_ = 0, nBlocks_ = 0;
    int pos_ = -1, blockNo_ = -1;
    unsigned cur_[4] = {};
    bool curOk_[4] = {};
    std::vector<HxvVector> recent_;
    HxvSystem sys_;
    HxvSpecPoint spec_[64];
};

} // namespace dect2::datawell
