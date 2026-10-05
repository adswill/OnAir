#include <cstdint>
#include <map>
#include <mutex>
#include "dect2/t2interleave.h"
#include <utility>
#include <vector>

namespace dect2 {

namespace {
const int bitperm1keven[9] = {8, 7, 6, 5, 0, 1, 2, 3, 4};
const int bitperm1kodd[9] = {6, 8, 7, 4, 1, 0, 5, 2, 3};
const int bitperm2keven[10] = {4, 3, 9, 6, 2, 8, 1, 5, 7, 0};
const int bitperm2kodd[10] = {6, 9, 4, 8, 5, 1, 0, 7, 2, 3};
const int bitperm4keven[11] = {6, 3, 0, 9, 4, 2, 1, 8, 5, 10, 7};
const int bitperm4kodd[11] = {5, 9, 1, 4, 3, 0, 8, 10, 7, 2, 6};
const int bitperm8keven[12] = {7, 1, 4, 2, 9, 6, 8, 10, 0, 3, 11, 5};
const int bitperm8kodd[12] = {11, 4, 9, 3, 1, 2, 5, 0, 6, 7, 10, 8};
const int bitperm16keven[13] = {9, 7, 6, 10, 12, 5, 1, 11, 0, 2, 3, 4, 8};
const int bitperm16kodd[13] = {6, 8, 10, 12, 2, 0, 4, 1, 11, 3, 5, 9, 7};
const int bitperm32k[14] = {7, 13, 3, 4, 9, 2, 12, 11, 1, 8, 10, 0, 5, 6};
const int logic1k[2] = {0, 4}, logic2k[2] = {0, 3}, logic4k[2] = {0, 2}, logic8k[4] = {0, 1, 4, 6},
          logic16k[6] = {0, 1, 4, 5, 9, 11}, logic32k[4] = {0, 1, 2, 12};
} // namespace

static void freqInterleaverSeqCompute(int fftCode, int nCells, bool oddSymbol, std::vector<int>& H) {
    int degree = 0, xorSize = 0;
    const int *logic = logic1k, *permEven = bitperm1keven, *permOdd = bitperm1kodd;
    switch (fftCode) {
    case 3: degree = 9; xorSize = 2; logic = logic1k; permEven = bitperm1keven; permOdd = bitperm1kodd; break;
    case 0: degree = 10; xorSize = 2; logic = logic2k; permEven = bitperm2keven; permOdd = bitperm2kodd; break;
    case 2: degree = 11; xorSize = 2; logic = logic4k; permEven = bitperm4keven; permOdd = bitperm4kodd; break;
    case 1: degree = 12; xorSize = 4; logic = logic8k; permEven = bitperm8keven; permOdd = bitperm8kodd; break;
    case 4: degree = 13; xorSize = 6; logic = logic16k; permEven = bitperm16keven; permOdd = bitperm16kodd; break;
    case 5: degree = 14; xorSize = 4; logic = logic32k; permEven = bitperm32k; permOdd = bitperm32k; break;
    default: H.clear(); return;
    }
    const int maxStates = 1 << (degree + 1);
    const int mask = (1 << degree) - 1;
    std::vector<int> he, ho;
    he.reserve(nCells);
    ho.reserve(nCells);
    int lfsr = 0;
    for (int i = 0; i < maxStates; i++) {
        if (i == 0 || i == 1) lfsr = 0;
        else if (i == 2) lfsr = 1;
        else {
            int result = 0;
            for (int k = 0; k < xorSize; k++) result ^= (lfsr >> logic[k]) & 1;
            lfsr &= mask;
            lfsr >>= 1;
            lfsr |= result << (degree - 1);
        }
        int even = 0, odd = 0;
        for (int n = 0; n < degree; n++) even |= ((lfsr >> n) & 1) << permEven[n];
        for (int n = 0; n < degree; n++) odd |= ((lfsr >> n) & 1) << permOdd[n];
        even += (i % 2) * (maxStates / 2);
        odd += (i % 2) * (maxStates / 2);
        if (even < nCells) he.push_back(even);
        if (odd < nCells) ho.push_back(odd);
    }
    if (fftCode == 5) { // 32K: the even-symbol permutation is the inverse of the odd one
        std::vector<int> inv(nCells);
        for (int j = 0; j < (int)ho.size(); j++) inv[ho[j]] = j;
        he = inv;
    }
    H = oddSymbol ? ho : he;
}

// The sequence only depends on (FFT size, active cells, symbol parity): compute it once.
void freqInterleaverSeq(int fftCode, int nCells, bool oddSymbol, std::vector<int>& H) {
    static std::mutex mu;
    static std::map<int64_t, std::vector<int>> cache;
    const int64_t key = ((int64_t)fftCode << 40) | ((int64_t)nCells << 1) | (oddSymbol ? 1 : 0);
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::vector<int> v;
        freqInterleaverSeqCompute(fftCode, nCells, oddSymbol, v);
        it = cache.emplace(key, std::move(v)).first;
    }
    H = it->second;
}

} // namespace dect2
