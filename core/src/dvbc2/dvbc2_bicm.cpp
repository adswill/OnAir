// DVB-C2 bit-interleaved coding and modulation of the PLPs (EN 302 769 clause 6): the FEC dimensions (tables 3(a) / 3(b)), the LDPC codes
// (DVB-T2's tables for 2/3 .. 5/6, DVB-S2's for 8/9 and 9/10: the annexes A and B of EN 302 769 list the same accumulator addresses), the
// BCH codes, the bit interleaver with the demultiplexer (tables 6, 7 and 10) and the QAM mapping (DVB-T2's unrotated constellations).
#include "dect2/dvbc2/dvbc2.h"
#include "dect2/t2/t2fec_tables.h"
#include "dect2/dsp/ldpc.h"
#include "../dvbs/dvbs_ldpc_tables.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace dect2 {
namespace dvbc2 {

namespace {
// kLdpc, kBch, t per PLP_COD 1 .. 5
const int kNormalK[6] = {0, 43200, 48600, 51840, 54000, 58320};
const int kNormalKbch[6] = {0, 43040, 48408, 51648, 53840, 58192};
const int kNormalT[6] = {0, 10, 12, 12, 10, 8};
const int kShortK[6] = {0, 10800, 11880, 12600, 13320, 14400};
const int kShortKbch[6] = {0, 10632, 11712, 12432, 13152, 14232};
const int kXfecShort[8] = {900, 4050, 2700, 2025, 1620, 1350, 1158, 1013};
const int kXfecNormal[8] = {900, 16200, 10800, 8100, 6480, 5400, 4629, 4050};
} // namespace

Dims dims(const Fec& f) {
    Dims d;
    if (f.mod < 1 || f.mod > 3 || f.cod < 1 || f.cod > 5) return d;
    if (f.fecType) { d.nLdpc = 64800; d.kLdpc = kNormalK[f.cod]; d.kBch = kNormalKbch[f.cod]; d.t = kNormalT[f.cod]; }
    else { d.nLdpc = 16200; d.kLdpc = kShortK[f.cod]; d.kBch = kShortKbch[f.cod]; d.t = 12; }
    d.q = (d.nLdpc - d.kLdpc) / 360;
    d.bitsPerCell = 2 * (f.mod + 1);
    d.cells = d.nLdpc / d.bitsPerCell;
    d.ok = true;
    return d;
}

bool modCodValid(const Fec& f) {
    // tables 11(a) and 11(b); 1024-QAM and 4096-QAM are not implemented here
    if (f.mod < 1 || f.mod > 3) return false;
    switch (f.cod) {
    case 1: return f.mod == 2;                   // 2/3: 64QAM
    case 2: return f.mod == 3;                   // 3/4: 256QAM (and 1024)
    case 3: return f.mod == 1 || f.mod == 2;     // 4/5: 16QAM, 64QAM (and 4096)
    case 4: return f.mod == 3;                   // 5/6: 256QAM (and 1024, 4096)
    case 5: return true;                         // 8/9 and 9/10: all
    default: return false;
    }
}

const char* modName(int mod) {
    static const char* const n[] = {"reserved", "16-QAM", "64-QAM", "256-QAM", "1024-QAM", "4096-QAM", "reserved", "reserved"};
    return mod >= 0 && mod < 8 ? n[mod] : "?";
}
const char* codName(int fecType, int cod) {
    static const char* const n[] = {"reserved", "2/3", "3/4", "4/5", "5/6", "9/10", "reserved", "reserved"};
    if (cod == 5 && !fecType) return "8/9";
    return cod >= 0 && cod < 8 ? n[cod] : "?";
}
int xfecFrameCells(int fecType, int mod) {
    if (mod < 0 || mod > 7) return 0;
    return fecType ? kXfecNormal[mod] : kXfecShort[mod];
}

namespace {
const LdpcCode& ldpcCode(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<LdpcCode>> cache;
    if (f.cod >= 1 && f.cod <= 4) {   // DVB-T2's tables: PLP_COD 2 .. 5 there
        PlpFec t; t.shortFrame = !f.fecType; t.rate = f.cod + 1; t.mod = f.mod;
        return ldpcFor(t);
    }
    std::lock_guard<std::mutex> lk(mu);
    const int key = f.fecType;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    const Dims d = dims(f);
    const dvbs::S2LdpcTable& tab = dvbs::kS2LdpcTables[f.fecType ? 0 : 1][f.fecType ? 10 : 9];   // 9/10 normal, 8/9 short
    std::vector<std::vector<int>> rows;
    const uint16_t* p = tab.data;
    for (int r = 0; r < tab.rows; r++) {
        const int n = *p++;
        rows.emplace_back(p, p + n);
        p += n;
    }
    auto code = std::make_unique<LdpcCode>(d.kLdpc, d.nLdpc, rows);
    const LdpcCode& ref = *code;
    cache[key] = std::move(code);
    return ref;
}
const BchCode& bchCode(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<BchCode>> cache;
    const Dims d = dims(f);
    const int key = (f.fecType ? 100 : 0) + d.t;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto c = std::make_unique<BchCode>(!f.fecType, d.t);
    const BchCode& ref = *c;
    cache[key] = std::move(c);
    return ref;
}
} // namespace

const std::vector<int>& bitMap(const Fec& f) {
    static std::mutex mu;
    static std::map<int, std::unique_ptr<std::vector<int>>> cache;
    std::lock_guard<std::mutex> lk(mu);
    const int key = f.fecType * 100 + f.cod * 10 + f.mod;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    const Dims d = dims(f);
    auto mapp = std::make_unique<std::vector<int>>(d.nLdpc);
    std::vector<int>& out = *mapp;
    const int N = d.nLdpc, nbch = d.kLdpc, q = d.q;
    // parity interleaver (6.1.3)
    std::vector<int> tu(N);
    for (int k = 0; k < nbch; k++) tu[k] = k;
    for (int t = 0; t < q; t++) for (int s = 0; s < 360; s++) tu[nbch + 360 * t + s] = nbch + q * s + t;
    // column twist (tables 6 and 7) and the demultiplexer (table 10)
    const int* twist; const int* mux; int cols;
    const bool sh = !f.fecType;
    if (f.mod == 1) { twist = sh ? k_twist16s : k_twist16n; cols = 8; mux = k_mux16; }
    else if (f.mod == 2) { twist = sh ? k_twist64s : k_twist64n; cols = 12; mux = k_mux64; }
    else if (!sh) { twist = k_twist256n; cols = 16; mux = f.cod == 1 ? k_mux256_23 : k_mux256; }
    else { twist = k_twist256s; cols = 8; mux = k_mux256s; }
    const int rows = N / cols;
    std::vector<int> tv(N);
    int idx = 0;
    for (int col = 0; col < cols; col++) {
        int offset = twist[col];
        for (int row = 0; row < rows; row++) { tv[offset + rows * col] = tu[idx++]; offset = (offset + 1) % rows; }
    }
    idx = 0;
    for (int j = 0; j < rows; j++) for (int c = 0; c < cols; c++) tu[idx++] = tv[rows * c + j];
    int outp = 0;
    for (int g = 0; g < N / cols; g++) for (int e = 0; e < cols; e++) out[outp++] = tu[g * cols + mux[e]];
    const std::vector<int>& ref = out;
    cache[key] = std::move(mapp);
    return ref;
}

void encodeBlock(const Fec& f, const std::vector<uint8_t>& bb, std::vector<cf32>& cells) {
    const Dims d = dims(f);
    cells.clear();
    if (!d.ok || (int)bb.size() < d.kBch) return;
    std::vector<uint8_t> bits(bb.begin(), bb.begin() + d.kBch);
    const uint8_t* rnd = bbRandomiser();
    for (int i = 0; i < d.kBch; i++) bits[i] ^= rnd[i];
    bchCode(f).encode(bits, d.kBch);
    ldpcCode(f).encode(bits);
    const auto& map = bitMap(f);
    std::vector<uint16_t> lab((size_t)d.cells);
    for (int c = 0; c < d.cells; c++) {
        unsigned l = 0;
        for (int k = 0; k < d.bitsPerCell; k++) l = (l << 1) | bits[(size_t)map[(size_t)c * d.bitsPerCell + k]];
        lab[(size_t)c] = (uint16_t)l;
    }
    PlpFec t; t.mod = f.mod; t.rotation = false;
    qamMapBlock(t, lab, cells);
}

BlockResult decodeBlock(const Fec& f, const cf32* cells, const float* n0, std::vector<uint8_t>& bb, int maxIter) {
    BlockResult r;
    const Dims d = dims(f);
    bb.clear();
    if (!d.ok) return r;
    PlpFec t; t.mod = f.mod; t.rotation = false;
    std::vector<float> lab((size_t)d.nLdpc);
    qamDemapBlock(t, cells, n0, d.cells, lab.data());   // label order
    const auto& map = bitMap(f);
    std::vector<float> llr((size_t)d.nLdpc);
    for (int p = 0; p < d.nLdpc; p++) llr[(size_t)map[(size_t)p]] = lab[(size_t)p];
    std::vector<uint8_t> hard;
    const LdpcCode& code = ldpcCode(f);
    t2LdpcDecode(code, llr, maxIter, hard, &r.ldpcIters);
    std::vector<uint8_t> word(hard.begin(), hard.begin() + d.kLdpc);
    int corr = bchCode(f).decode(word);
    if (corr < 0) {   // second chance with more iterations and another normalisation
        int it2 = 0;
        std::vector<uint8_t> h2;
        t2LdpcDecode(code, llr, 3 * maxIter, h2, &it2, 0.66f);
        std::vector<uint8_t> w2(h2.begin(), h2.begin() + d.kLdpc);
        const int c2 = bchCode(f).decode(w2);
        if (c2 >= 0) { corr = c2; word = std::move(w2); hard = std::move(h2); r.ldpcIters = it2; }
    }
    if (corr < 0) return r;
    r.ok = true;
    r.bchCorrected = corr;
    long errs = 0;
    for (int p = 0; p < d.nLdpc; p++) errs += (llr[(size_t)p] < 0) != (hard[(size_t)p] != 0);
    r.preBer = (double)errs / d.nLdpc;
    bb.assign(word.begin(), word.begin() + d.kBch);
    const uint8_t* rnd = bbRandomiser();
    for (int i = 0; i < d.kBch; i++) bb[(size_t)i] ^= rnd[i];
    return r;
}

double blockSeconds(const Fec& f, int dataCellsPerSymbol, int gi) {
    const Dims d = dims(f);
    if (!d.ok || dataCellsPerSymbol <= 0) return 0;
    return (double)d.cells / dataCellsPerSymbol * symbolSeconds(gi);
}

double netBitrate(const Fec& f, double cellsPerSecond) {
    const Dims d = dims(f);
    if (!d.ok) return 0;
    const int npk = (d.kBch - 80) / 1504;
    return cellsPerSecond / d.cells * npk * 188 * 8;
}

} // namespace dvbc2
} // namespace dect2
