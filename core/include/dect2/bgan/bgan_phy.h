// Inmarsat BGAN (ETSI TS 102 744 "Family SL") forward bearers, physical layer: bearer parameters, frame layout, unique words, constellations,
// scrambler, turbo code. TS 102 744-2-1 clauses 5.2 and 5.3, tables from its Annex C (generated from the published .TXT files, tools/bgan_tables.py).
// Forward bearers only, and only the bearers whose tables are published: F80T1Q-1B is defined in the standard but has no interleaver tables,
// it can be recognised (frame structure) but not decoded.
// Not validated on a real BGAN signal: see docs/BGAN-3.7.md.
#pragma once
#include "dect2/common/ring.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace dect2::bgan {

enum class Mod { Q4, Q16, Q32, Q64 };   // QPSK, 16-, 32-, 64-QAM
int bitsPerSymbol(Mod m);
const cf32* constellation(Mod m, int& points);                  // indexed by the value of (b[n-1] ... b0), mean power 1
// max-log LLR (log P(0) / P(1)) for each bit, written in table column order (b[n-1] first), nvar = complex noise variance
void softDemap(Mod m, cf32 y, float nvar, float* llr);

constexpr int kFamilyNone = -1;
struct Bearer {
    const char* name;      // "F80T0.25Q-1B"
    int fbearerType;       // the FBearerType value that names it in signalling (TS 102 744-3-1 5.7.31.2), 0 = none assigned (F80T0.25Q-1B)
    int symRate;           // symbols per second
    double rolloff;
    Mod mod;
    int mos, uw, ps, ds;   // modulator output symbols, unique word, pilots per frame, data symbols between pilots
    int teo, blocks;       // encoded symbols per FEC block, FEC blocks per frame
    bool outer;            // 80 ms outer (block) interleaver: FR80T2.5 / FR80T5
    bool dynamic;          // X4/16: the first block is QPSK, later blocks may be QPSK or 16-QAM (ForwardBearerCodeRate AVP)
    int family;            // index into the published tables, kFamilyNone = no tables published
    int family16;          // X4/16 only: table family of the 16-QAM blocks
    float uwAxis;          // |I| = |Q| of the unique word and pilot symbols
    bool uwZeroIsPlus;     // Q bearers: UW bit 0 = + (upper right); all others: bit 1 = +
};
int bearerCount();
const Bearer& bearer(int i);

constexpr int kSubtypes = 15;                                     // L8 .. L1, R, H1 .. H6
const char* subtypeName(int s);                                   // 0 = L8, 7 = L1, 8 = R, 9 = H1, 14 = H6
const uint64_t* uniqueWords();                                    // kSubtypes 40-bit words, first symbol in bit 39 (Figure 5.15)
int bearerUwIndexFromName(const char* name);                      // for tests, -1 unknown

// One FEC block's tables (Annex C). Index: bits: N = data + 4 flush. Null when not published (for example QPSK subtype H3).
struct Table {
    int family = 0, subtype = 0;
    int N = 0;              // d length (user data + 4 flush bits)
    int symbols = 0, bps = 0;
    const uint16_t* perm = nullptr;     // interleaver, q-index -> data index
    const uint16_t* map = nullptr;      // symbols * bps entries, per symbol: b[n-1], ... b0, indices into d[N], p[N], q[N]
    int dataBits() const { return N - 4; }
    int dataBytes() const { return (N - 4) / 8; }
};
const Table* table(int family, int subtype);
int tableCount();

// frame layout: the symbol index in the frame of every data symbol, and of every pilot; the data symbols are in transmission order
struct FrameLayout {
    std::vector<int> dataPos, pilotPos;
};
FrameLayout frameLayout(const Bearer& b);

// which FEC block and which symbol of it the d-th data symbol of a frame is (handles the outer interleaver)
struct DataSym { int block, sym; };
inline DataSym dataSymbolOf(const Bearer& b, int d) {
    if (b.outer) return { d % b.blocks, d / b.blocks };
    return { d / b.teo, d % b.teo };
}

// the scrambler over `bits` bits (one bit per entry), initial state 0x6959
void scramble(uint8_t* bits, int n);
// encode one FEC block: `bytes` = dataBytes() octets, MSB first; `syms` receives t.symbols constellation points
void encodeBlock(const Table& t, Mod m, const uint8_t* bytes, cf32* syms);

// turbo decode one FEC block from the channel LLRs (t.symbols * t.bps, table column order)
class TurboDecoder {
public:
    // iterations: up to maxIter (full iterations, both constituents). out receives dataBytes() octets, already descrambled.
    // ok, if given, is tested after every iteration on the current hard decision; decoding stops early when it returns true
    // Returns iterations used.
    int decode(const Table& t, const float* llr, std::vector<uint8_t>& out, int maxIter, const std::function<bool(const std::vector<uint8_t>&)>& ok = {});
private:
    std::vector<float> a_, b_, ld_, lp_, lq_, ext1_, ext2_;
    std::vector<uint8_t> hard_;
};

} // namespace dect2::bgan
