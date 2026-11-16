// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The DVB-T2 configuration rules of t2profile.h, transcribed from ETSI EN 302 755 V1.4.1 (tables 58, 59, I.1, I.4, I.5, I.6).
#include "dect2/t2/t2profile.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace dect2 {

namespace {

constexpr int pp(std::initializer_list<int> l) {
    int m = 0;
    for (int p : l) m |= 1 << (p - 1);
    return m;
}

// [FFT row][guard interval in L1 order: 1/32 1/16 1/8 1/4 1/128 19/128 19/256]; rows 1K 2K 4K 8K 16K 32K
using Table = int[6][7];

// Table 58: SISO
const Table kBaseSiso = {
    /* 1K  */ {0, pp({4, 5}), pp({2, 3}), pp({1}), 0, 0, 0},
    /* 2K  */ {pp({7, 4}), pp({4, 5}), pp({2, 3}), pp({1}), 0, 0, 0},
    /* 4K  */ {pp({7, 4}), pp({4, 5}), pp({2, 3}), pp({1}), 0, 0, 0},
    /* 8K  */ {pp({7, 4}), pp({8, 4, 5}), pp({2, 3, 8}), pp({1, 8}), pp({7}), pp({2, 3, 8}), pp({8, 4, 5})},
    /* 16K */ {pp({7, 4, 6}), pp({2, 8, 4, 5}), pp({2, 3, 8}), pp({1, 8}), pp({7}), pp({2, 3, 8}), pp({2, 8, 4, 5})},
    /* 32K */ {pp({4, 6}), pp({2, 8, 4}), pp({2, 8}), 0, pp({7}), pp({2, 8}), pp({2, 8, 4})},
};
// Table 59: MISO
const Table kBaseMiso = {
    /* 1K  */ {0, pp({3}), pp({1}), 0, 0, 0, 0},
    /* 2K  */ {pp({4, 5}), pp({3}), pp({1}), 0, 0, 0, 0},
    /* 4K  */ {pp({4, 5}), pp({3}), pp({1}), 0, 0, 0, 0},
    /* 8K  */ {pp({8, 4, 5}), pp({3, 8}), pp({1, 8}), 0, pp({8, 4, 5}), pp({1, 8}), pp({3, 8})},
    /* 16K */ {pp({8, 4, 5}), pp({3, 8}), pp({1, 8}), 0, pp({8, 4, 5}), pp({1, 8}), pp({3, 8})},
    /* 32K */ {pp({8, 4}), pp({2, 8}), 0, 0, pp({8, 4, 6}), 0, pp({2, 8})},
};
// Table I.5: T2-Lite SISO (no 1K, no 32K)
const Table kLiteSiso = {
    /* 1K  */ {0, 0, 0, 0, 0, 0, 0},
    /* 2K  */ {pp({7, 4}), pp({4, 5}), pp({2, 3}), pp({1}), 0, 0, 0},
    /* 4K  */ {pp({7, 4}), pp({4, 5}), pp({2, 3}), pp({1}), 0, 0, 0},
    /* 8K  */ {pp({7, 4}), pp({4, 5}), pp({2, 3}), pp({1}), pp({7}), pp({2, 3}), pp({4, 5})},
    /* 16K */ {pp({7, 6}), pp({4, 5}), pp({2, 3}), pp({1}), pp({7}), pp({2, 3}), pp({2, 4, 5})},
    /* 32K */ {0, 0, 0, 0, 0, 0, 0},
};
// Table I.6: T2-Lite MISO
const Table kLiteMiso = {
    /* 1K  */ {0, 0, 0, 0, 0, 0, 0},
    /* 2K  */ {pp({4, 5}), pp({3}), pp({1}), 0, 0, 0, 0},
    /* 4K  */ {pp({4, 5}), pp({3}), pp({1}), 0, 0, 0, 0},
    /* 8K  */ {pp({4, 5}), pp({3}), pp({1}), 0, pp({4, 5}), pp({1}), pp({3})},
    /* 16K */ {pp({4}), pp({3}), pp({1}), 0, pp({4, 5}), pp({1}), pp({3})},
    /* 32K */ {0, 0, 0, 0, 0, 0, 0},
};

int fftRow(int fftN) {
    switch (fftN) {
    case 1024: return 0;
    case 2048: return 1;
    case 4096: return 2;
    case 8192: return 3;
    case 16384: return 4;
    case 32768: return 5;
    default: return -1;
    }
}

} // namespace

int t2PilotMask(int fftN, int giIdx, bool miso, bool lite) {
    const int r = fftRow(fftN);
    if (r < 0 || giIdx < 0 || giIdx > 6) return 0;
    const Table& t = lite ? (miso ? kLiteMiso : kLiteSiso) : (miso ? kBaseMiso : kBaseSiso);
    return t[r][giIdx];
}

const char* t2PilotTable(bool miso, bool lite) {
    if (lite) return miso ? "table I.6" : "table I.5";
    return miso ? "table 59" : "table 58";
}

bool t2LiteFft(int fftN) { return fftN == 2048 || fftN == 4096 || fftN == 8192 || fftN == 16384; }

bool t2CodeRateAllowed(int cod, bool lite) {
    if (cod < 0 || cod > 7) return false;
    return lite ? cod != 4 && cod != 5 : cod <= 5;
}

bool t2ModCodAllowed(int mod, int cod, bool lite) {
    if (mod < 0 || mod > 3 || !t2CodeRateAllowed(cod, lite)) return false;
    return !(lite && mod == 3 && (cod == 2 || cod == 3));
}

bool t2RotationAllowed(int mod, bool lite) { return !(lite && mod == 3); }

int t2LiteMaxBlocks(int kBch, int cellsPerBlock, double frameSec, int tiBlocks) {
    if (kBch <= 80 || cellsPerBlock <= 0 || !(frameSec > 0)) return 0;
    long n = (long)std::floor(kT2LiteMaxPlpBps * frameSec / (kBch - 80) + 1e-9);
    if (tiBlocks > 0) n = std::min(n, (long)tiBlocks * (kT2TiCellsLite / cellsPerBlock));
    return (int)std::max(0L, n);
}

} // namespace dect2
