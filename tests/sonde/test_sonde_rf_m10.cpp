// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// M10 through the whole receiver (see sonde_rftest.h)
#include "dect2/sonde/sonde_rftest.h"
int main() {
    const int r = dect2::sondetest::runTypeRf(1);
    if (!r) printf("ok\n");
    return r;
}
