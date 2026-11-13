// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared bits of the Signal workbench tests.
#pragma once
#include "dect2/wb/wb_bits.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../common/test_util.h"

// How well rx matches tx at the best alignment (shift within +-maxShift bits, inverted or not): the number of errors in the compared stretch
// (the first and last `trim` bits of tx are left out: filter start-up). Returns the errors; *len the bits compared, *shift and *inv the match.
static size_t bitErrors(const dect2::wb::Bits& tx, const dect2::wb::Bits& rx, int maxShift = 48, size_t trim = 24, size_t* len = nullptr, int* shift = nullptr, bool* inv = nullptr, bool allowInv = true) {
    size_t best = (size_t)-1, bl = 0;
    int bs = 0; bool bi = false;
    for (int s = -maxShift; s <= maxShift; s++)
        for (int iv = 0; iv < (allowInv ? 2 : 1); iv++) {
            size_t err = 0, n = 0;
            for (size_t i = trim; i + trim < tx.size(); i++) {
                const long j = (long)i + s;
                if (j < 0 || j >= (long)rx.size()) continue;
                n++;
                if ((tx[i] ^ rx[(size_t)j] ^ (uint8_t)iv) & 1) err++;
            }
            if (n > tx.size() / 2 && err < best) { best = err; bl = n; bs = s; bi = iv; }
        }
    if (len) *len = bl;
    if (shift) *shift = bs;
    if (inv) *inv = bi;
    return best;
}
