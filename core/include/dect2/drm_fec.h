// DRM channel coding (ETSI ES 201 980 clause 7 and Annex D): CRC, energy dispersal, multilevel coding with the punctured convolutional code,
// bit and cell interleavers, constellations. The encoder is used by the test signal generator, the soft decoder by the receiver.
#pragma once
#include "ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 { namespace drm {

// ---- Annex D. Bits are one per byte (0 or 1), most significant first. The result is the value that is transmitted (the register, inverted).
uint32_t crc8(const uint8_t* bits, size_t n);      // G8 = x^8 + x^4 + x^3 + x^2 + 1
uint32_t crc16(const uint8_t* bits, size_t n);     // G16 = x^16 + x^12 + x^5 + 1
uint32_t crc8Bytes(const uint8_t* bytes, size_t n);
uint32_t crc16Bytes(const uint8_t* bytes, size_t n);

// ---- clause 7.2.2: the energy dispersal sequence (Table 26 shows the first 16 bits)
void prbs(uint8_t* out, size_t n);

// ---- clauses 7.3.3 and 7.6: permutation Pi(i), i < xin, for the multiplier t (13 and 21 for bits, 5 for the MSC cells)
const std::vector<int>& interleavePerm(int xin, int t);   // cached per (xin, t)

// ---- constellations (clause 7.4). levels = bits per cell / 2: 1 (4-QAM), 2 (16-QAM), 3 (64-QAM).
// Cell bits in transmission order: 4-QAM (i0 q0), 16-QAM (i0 i1 q0 q1), 64-QAM (i0 i1 i2 q0 q1 q2); i bits give the real part, q bits the imaginary part.
float qamNorm(int levels);                                  // a = 1/sqrt(2), 1/sqrt(10), 1/sqrt(42)
int railValue(int levels, int b0, int b1, int b2);          // odd integer amplitude of one rail, units of a
cf32 qamMap(int levels, const uint8_t* bits);               // bits: 2 * levels values
void qamDemapHard(int levels, cf32 z, uint8_t* bits);

// ---- multilevel coding of one channel (FAC, SDC or MSC multiplex frame)
struct MlcParams {
    int levels = 1;                 // 1, 2 or 3
    int rxA[3] = {1, 1, 1}, ryA[3] = {2, 2, 2};    // code rate of each level in the higher protected part (part A)
    int rxB[3] = {1, 1, 1}, ryB[3] = {2, 2, 2};    // ... and in the lower protected part (part B)
    int n1 = 0;                     // cells in part A
    int n2 = 0;                     // cells in part B, tail bits included (SDC and MSC: part B only has the tail)
    bool fac = false;               // FAC: one part, no separate tail pattern, all 6 tail bits are punctured like the data (clause 7.3.2)
};

class MlcCode {
public:
    explicit MlcCode(const MlcParams& p);
    const MlcParams& params() const { return p_; }
    int cells() const { return p_.n1 + p_.n2; }
    int infoBits() const { return l1_ + l2_; }          // L1 + L2: input bits of the encoder (u)
    int infoBitsA() const { return l1_; }               // bits the higher protected part carries (can exceed the nominal size, see the note in clause 7.3.1.1)
    // Encode u (infoBits() bits, one per byte, scrambled already) to cells(); the cells have unit average power
    void encode(const uint8_t* u, cf32* cells) const;
    // Soft decode. z: cells (unit average power constellation, equalised), w: per cell scale of the metric (|H|^2 / noise power), iterations >= 1.
    // Returns the decoded u. The hard decisions of the levels already decoded are used for the next ones; later iterations use all of them.
    void decode(const cf32* z, const float* w, uint8_t* u, int iterations = 2) const;

public:
    struct Impl;
private:
    MlcParams p_;
    int l1_ = 0, l2_ = 0;
    std::shared_ptr<Impl> d_;
};

// Number of information bits per part and level (clause 7.3.1.1): M_p,1 = 2 N1 R_p and M_p,2 = RX_p floor((2 N2 - 12) / RY_p)
int mlcBitsA(int n1, int rx, int ry);
int mlcBitsB(int n2, int rx, int ry);

// Convolutional mother code (rate 1/6, constraint length 7, octal 133 171 145 133 171 145): encode steps = n + 6 steps, 6 outputs each
void convEncode(const uint8_t* in, int n, uint8_t* out /* (n + 6) * 6 */);
// Terminated soft Viterbi decoder: soft[(n + 6) * 6] (positive favours 0, 0 = erased), returns the n information bits
void viterbiDecode(const float* soft, int n, uint8_t* out);

}} // namespace dect2::drm
