// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Waterfall in fast mode (the CW preset): rates from 28 rows a second up are "fast", the time per row is exact, rows are written one by one
// and not averaged down the rows, and a batch of rows (several per screen frame) keeps its order and count.
#include "../../app/gfx/waterfall.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    Waterfall wf;
    wf.lv.assign((size_t)wf.cols * Waterfall::H, 0);
    wf.palette();
    check(!wf.fast() && !wf.slow(), "rate 0: every frame, neither fast nor slow");
    wf.setRate(15); check(wf.slow() && !wf.fast() && std::fabs(wf.rowDt - 1.0 / 15) < 1e-12, "15 rows a second: slow");
    wf.setRate(100); check(wf.fast() && !wf.slow() && std::fabs(wf.rowDt - 0.01) < 1e-12 && wf.timeKnown(), "100 rows a second: fast, 10 ms a row, time known at once");
    wf.setRate(500); check(wf.fast() && std::fabs(wf.rowDt - 0.002) < 1e-12, "500 rows a second: 2 ms a row");
    wf.setRate(28); check(wf.fast(), "28 and up is fast"); wf.setRate(27.9f); check(!wf.fast() && wf.slow(), "just under 28 is slow");

    wf.setRate(100);
    wf.minDb = -100; wf.maxDb = -32; wf.rowTau = 0.15f;   // (the row time constant of the ordinary path must not touch fast rows)
    std::vector<float> flat(4096, -100.f), spike(4096, -100.f);
    spike[1000] = -32.f;
    check(wf.offerFast(spike.data(), spike.size()), "a row is taken");
    const int col = 1000 * wf.cols / 4096;
    check(wf.rows == 1 && wf.filled == 1, "one row written");
    check(wf.lv[(size_t)wf.writeRow * wf.cols + col] >= 250, "the spike is at full level");
    check(wf.offerFast(flat.data(), flat.size()), "the next row");
    check(wf.lv[(size_t)wf.writeRow * wf.cols + col] == 0, "no trace of the spike in the next row (not averaged down the rows)");
    check(wf.lv[(size_t)((wf.writeRow + 1) % Waterfall::H) * wf.cols + col] >= 250, "the spike row is the one before, in order");
    // a batch
    for (int i = 0; i < 300; i++) { std::vector<float> r(4096, -100.f); r[i * 10] = -40.f; wf.offerFast(r.data(), r.size()); }
    check(wf.rows == 302 && wf.filled == 302, "a batch of 300 rows: all written");
    const int c299 = 2990 * wf.cols / 4096;
    check(wf.lv[(size_t)wf.writeRow * wf.cols + c299] > 200, "the newest row of the batch is the last one offered");
    check(wf.lv[(size_t)((wf.writeRow + 299) % Waterfall::H) * wf.cols + 0] > 200, "the oldest of the batch is 299 rows back");
    for (int i = 0; i < 300; i++) wf.offerFast(flat.data(), flat.size());
    check(wf.filled == Waterfall::H, "the history holds 512 rows");
    // paused: nothing taken
    wf.paused = true;
    const long long before = wf.rows;
    check(!wf.offerFast(spike.data(), spike.size()) && wf.rows == before, "paused: no new rows");
    wf.paused = false;
    check(!wf.offerFast(spike.data(), 0), "an empty row is refused");
    printf(fails ? "%d FAILED\n" : "wf_fast: all passed\n", fails);
    return fails ? 1 : 0;
}
