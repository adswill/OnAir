#include "dect2/atsc3_ti.h"
#include "dect2/atsc3_bb.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace atsc3 {

int ctiRows(int code, bool extended) {
    static const int rows[4] = {512, 724, 887, 1024};   // Table 9.24: 50, 100, 150 and 200 ms
    if (extended) return code == 2 ? 1254 : code == 3 ? 1448 : 0;
    return code >= 0 && code < 4 ? rows[code] : 0;
}

CtiInterleaver::CtiInterleaver(int rows, int eta, const std::vector<cf32>& cons, int startRow) : rows_(rows), row_(startRow % rows), lines_(rows) {
    // initial state: eta * rows * (rows - 1) / 2 PRBS bits mapped to cells, filled left to right and top to bottom starting with line 1
    long cellsNeeded = (long)rows * (rows - 1) / 2;
    auto bits = bbScrambleSequence((int)(cellsNeeded * eta));
    long q = 0;
    for (int k = 1; k < rows; k++)
        for (int e = 0; e < k; e++) {
            int label = 0;
            for (int b = 0; b < eta; b++) label = (label << 1) | bits[q * eta + b];
            // delay elements are numbered from the left; the right-most one is output first, so it is the front of the queue
            lines_[k].push_back(cons[label]);
            q++;
        }
    // the leftmost element (first filled) is the newest: reverse so that front = right-most
    for (int k = 1; k < rows; k++) std::reverse(lines_[k].begin(), lines_[k].end());
}

cf32 CtiInterleaver::push(cf32 in) {
    cf32 out = in;
    if (row_ > 0) {
        out = lines_[row_].front();
        lines_[row_].pop_front();
        lines_[row_].push_back(in);
    }
    row_ = (row_ + 1) % rows_;
    return out;
}

std::vector<cf32> CtiInterleaver::process(const std::vector<cf32>& in) {
    std::vector<cf32> out;
    out.reserve(in.size());
    for (auto& c : in) out.push_back(push(c));
    return out;
}

CtiDeinterleaver::CtiDeinterleaver(int rows, int startRow) : rows_(rows), row_(startRow % rows), lines_(rows) {
    for (int k = 0; k < rows; k++) lines_[k].assign(rows - 1 - k, cf32(0, 0));
}

cf32 CtiDeinterleaver::push(cf32 in) {
    cf32 out = in;
    if (!lines_[row_].empty()) {
        out = lines_[row_].front();
        lines_[row_].pop_front();
        lines_[row_].push_back(in);
    }
    row_ = (row_ + 1) % rows_;
    return out;
}

std::vector<cf32> CtiDeinterleaver::process(const std::vector<cf32>& in) {
    std::vector<cf32> out;
    out.reserve(in.size());
    for (auto& c : in) out.push_back(push(c));
    return out;
}

// ---- HTI

namespace {

int log2ceil(int n) { int b = 0; while ((1 << b) < n) b++; return b; }

}

std::vector<int> htiCellPermutation(int cells, int r) {
    const int nr = log2ceil(cells);
    std::vector<int> l0;
    l0.reserve(cells);
    unsigned lo = 0;   // N_i[nr-2 .. 0]
    for (int i = 0; i < (1 << nr); i++) {
        if (i == 2) lo = 1;
        else if (i > 2) {
            unsigned fb;
            switch (nr) {
            case 11: fb = ((lo >> 0) ^ (lo >> 3)) & 1; break;
            case 12: fb = ((lo >> 0) ^ (lo >> 2)) & 1; break;
            case 13: fb = ((lo >> 0) ^ (lo >> 1) ^ (lo >> 4) ^ (lo >> 6)) & 1; break;
            case 14: fb = ((lo >> 0) ^ (lo >> 1) ^ (lo >> 4) ^ (lo >> 5) ^ (lo >> 9) ^ (lo >> 11)) & 1; break;
            default: fb = ((lo >> 0) ^ (lo >> 1) ^ (lo >> 2) ^ (lo >> 12)) & 1; break;
            }
            lo = (lo >> 1) | (fb << (nr - 2));
        }
        unsigned v = ((unsigned)(i & 1) << (nr - 1)) | lo;
        if ((int)v < cells) l0.push_back((int)v);
    }
    // shift P(r): bit-reversed counter, skipping values that are too large
    int k = 0, p = 0;
    for (int rr = 0; rr <= r; rr++) {
        p = cells;
        while (p >= cells) {
            int v = 0;
            for (int j = 0; j < nr; j++) v |= ((k >> j) & 1) << (nr - 1 - j);
            p = v;
            k++;
        }
    }
    std::vector<int> perm(cells);
    for (int q = 0; q < cells; q++) perm[q] = (l0[q] + p) % cells;
    return perm;
}

std::vector<cf32> htiCellInterleave(const std::vector<cf32>& in, int cells, int nFec) {
    std::vector<cf32> out(in.size());
    for (int r = 0; r < nFec; r++) {
        auto perm = htiCellPermutation(cells, r);
        for (int q = 0; q < cells; q++) out[(size_t)r * cells + q] = in[(size_t)r * cells + perm[q]];   // d(q) = g(L(q))
    }
    return out;
}

std::vector<cf32> htiCellDeinterleave(const std::vector<cf32>& in, int cells, int nFec) {
    std::vector<cf32> out(in.size());
    for (int r = 0; r < nFec; r++) {
        auto perm = htiCellPermutation(cells, r);
        for (int q = 0; q < cells; q++) out[(size_t)r * cells + perm[q]] = in[(size_t)r * cells + q];
    }
    return out;
}

std::vector<int> htiTwistedReadOrder(int nr, int nFec, int nMax) {
    const int nVirtual = nMax - nFec;
    std::vector<int> order;
    order.reserve((size_t)nFec * nr);
    const long total = (long)nr * nMax;
    for (long i = 0; i < total; i++) {
        long R = i % nr, T = R % nMax, C = (T + i / nr) % nMax;
        long theta = nr * C + R;
        if (theta >= (long)nVirtual * nr) order.push_back((int)(theta - (long)nVirtual * nr));   // position among the data cells (virtual columns come first)
    }
    return order;
}

std::vector<cf32> htiBlockInterleave(const std::vector<cf32>& in, int cells, int nFec, int nMax) {
    auto order = htiTwistedReadOrder(cells, nFec, nMax);
    std::vector<cf32> out(order.size());
    for (size_t i = 0; i < order.size(); i++) out[i] = in[order[i]];
    return out;
}

std::vector<cf32> htiBlockDeinterleave(const std::vector<cf32>& in, int cells, int nFec, int nMax) {
    auto order = htiTwistedReadOrder(cells, nFec, nMax);
    std::vector<cf32> out(order.size());
    for (size_t i = 0; i < order.size() && i < in.size(); i++) out[order[i]] = in[i];
    return out;
}

std::vector<cf32> htiInterleave(const std::vector<cf32>& in, int cells, int nFec, int nMax, bool ci) {
    auto x = ci ? htiCellInterleave(in, cells, nFec) : in;
    return htiBlockInterleave(x, cells, nFec, nMax);
}

std::vector<cf32> htiDeinterleave(const std::vector<cf32>& in, int cells, int nFec, int nMax, bool ci) {
    auto x = htiBlockDeinterleave(in, cells, nFec, nMax);
    return ci ? htiCellDeinterleave(x, cells, nFec) : x;
}

int htiBlocksInTiBlock(int nFecIf, int nTi, int s) {
    if (nTi <= 1) return nFecIf;
    int base = nFecIf / nTi, rem = nFecIf % nTi;
    return s < nTi - rem ? base : base + 1;
}

} // namespace atsc3
} // namespace dect2
