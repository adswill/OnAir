// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
#include <vector>

namespace dect2 {

// A table-driven quasi-cyclic LDPC code for the generic GPU kernel (DTMB: 127 x 127 circulants). Block row r has the edges rowStart[r] ..
// rowStart[r+1]-1 of col[] / shift[]: check i of the block row contains variable (i + shift) mod z of block column col. A block column may appear
// only once in a block row. The decoder input is the transmitted word: the variables from `punctured` on (the first ones are unknown, LLR 0);
// the output is the variables of the last blockCols - blockRows block columns (the information bits).
struct QcLdpc {
    int z = 0, blockRows = 0, blockCols = 0, punctured = 0;
    std::vector<int> rowStart, col, shift;
    float clip = 1000.f;                 // the totals are limited to +-clip
    int giveUpIter[2] = {0, 0};          // after iteration giveUpIter[i] the word is abandoned when more than blockRows*z/giveUpDiv[i] checks fail (0: never)
    int giveUpDiv[2] = {1, 1};
    int sentBits() const { return blockCols * z - punctured; }
    int infoBits() const { return (blockCols - blockRows) * z; }
};

} // namespace dect2
