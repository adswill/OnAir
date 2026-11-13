// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared forward error correction: byte-wise Reed-Solomon over GF(256), the DVB-T2 binary BCH codes and the LDPC parity-check tables, implemented by the Rust crate
// onair_fec through core/src/fec/fec_proto_rs.cpp. The K = 7 Viterbi decoder is C++ (core/src/fec/fec_proto.cpp, NEON / SSE2 / AVX2 / portable): it is the fastest one;
// the Rust NEON kernel is opt-in (cmake -DDECT2_RUST_VITERBI=ON). dvbt.cpp, atsc.cpp, dab_fec.cpp and t2fec.cpp call these functions; tests/fec/test_fec_proto_rs.cpp
// is the golden-vector test.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dect2::fecp {

// Reed-Solomon, field polynomial 0x11D, generator roots alpha^0 .. alpha^(R-1). Encoders read k bytes and write the whole n byte codeword. Decoders correct
// the n byte word in place and return the number of corrected bytes (0 = clean) or -1 when it cannot be corrected (the DAB one leaves the word as it was).
void rs204Encode(const uint8_t* in188, uint8_t* out204);   // DVB-T / ISDB-T RS(204,188), t = 8
int rs204Decode(uint8_t* word204);
void rs207Encode(const uint8_t* in187, uint8_t* out207);   // ATSC RS(207,187), t = 10
int rs207Decode(uint8_t* word207);
void rs120Encode(const uint8_t* in110, uint8_t* out120);   // DAB RS(120,110), t = 5
int rs120Decode(uint8_t* word120);

// DVB-T / ISDB-T Viterbi, the 64-state K = 7 code (171, 133 octal): `steps` trellis steps from 2 * steps signed soft bytes (positive = bit 0, 0 = punctured),
// start state unknown, trace-back from the best end state into bits[steps] (the input bit of each step). *metricOut = the final path metric (sum of the best path's
// branch metrics). Identical results from the NEON, SSE2, AVX2, portable and Rust implementations.
void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut);

// The same code with 8-bit saturating path metrics (16 states per vector) and two independent runs stepped side by side (their chains of
// dependent operations interleave): about twice the speed of viterbiRange on NEON. Not the same results: the soft values must be at most
// +-31 (kViterbi8Max; renormalised every second step, so no metric of a surviving path saturates) and a hopeless path's metric saturates at
// the floor. With soft values scaled to a mean of about 14 its bit error rate matches viterbiRange's within the statistics of 16 million bits
// (tests/fec/test_viterbi8_ber.cpp). b may be null (one run). Only where viterbi8x2Available() (NEON builds); DVB-T uses it, ISDB-T keeps the
// bit-exact decoder.
constexpr int kViterbi8Max = 31;
bool viterbi8x2Available();
void viterbiRange8x2(const int8_t* a, size_t na, const int8_t* b, size_t nb, std::vector<uint8_t>& bitsA, std::vector<uint8_t>& bitsB, long* metricA, long* metricB);

// DVB-T2 BCH (EN 302 755 section 6.1.1): normal frames GF(2^16), short frames GF(2^14), t = 1..12. Bits are one per byte (0 or 1), first bit = highest power.
class Bch {
public:
    Bch(bool shortFrame, int t);
    int parityBits() const { return deg_; }
    // bits[0..k) hold the message; parity is appended so that bits has k + parityBits() entries (k is clamped to the vector size)
    void encode(std::vector<uint8_t>& bits, int k) const;
    // Corrects up to t errors in place. Returns the number of corrected bits, or -1 if uncorrectable.
    int decode(std::vector<uint8_t>& bits) const;

private:
    int deg_;                              // number of parity bits (m * t)
    std::vector<uint64_t> rsctx_;          // the Rust context (field tables, generator, remainder table), 8-byte aligned
};

// Parity-check tables of the DVB-T2 / DVB-S2 style IRA LDPC codes (dsp/ldpc.h builds its decoder structures from them). `rows[g]` lists the addresses of the 360
// information bits of group g: bit c of the group joins check (a + c * q) mod m (m = n - k, q = m / 360); parity bits form the zig-zag. Returns false (tables
// empty) for n <= k, m < 360 or a negative address.
struct LdpcTables {
    int m = 0, q = 0;
    std::vector<int> chkStart, chkVar;           // check -> variables (CSR)
    std::vector<int> encP, encD;                 // information edges: parity index, information bit
    std::vector<int> layerStart, layerGroup, layerShift;   // layer a mod q -> (group, a / q) in table order (CSR)
};
bool ldpcBuild(int k, int n, const std::vector<std::vector<int>>& rows, LdpcTables& t);

} // namespace dect2::fecp
