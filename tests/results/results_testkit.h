// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The decoded-results tests (tests/test_*_results.cpp): a receiver fed with its mode's built-in test signal, then reset, configured
// again, saved, loaded into a fresh receiver and cleared. The lists must live through all but the Clear.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/results/results_io.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace rtk {

inline int fails = 0;
#define RCHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); rtk::fails++; } } while (0)

// secs of the generator into the receiver in blocks of 64k; the newest report in last (true when one came)
template <class Rx, class Tel>
bool feed(Rx& rx, dect2::ModeSynth& syn, double secs, Tel& last, uint64_t& seq) {
    std::vector<dect2::cf32> buf(65536);
    size_t left = (size_t)(secs * syn.sampleRate());
    bool got = false;
    while (left) {
        const size_t n = left < buf.size() ? left : buf.size();
        syn.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        left -= n;
        Tel t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = std::move(t); got = true; }
    }
    return got;
}

// quiet input (zeros): the receiver runs on with nothing new to decode
template <class Rx, class Tel>
bool feedQuiet(Rx& rx, double rate, double secs, Tel& last, uint64_t& seq) {
    std::vector<dect2::cf32> buf(65536, dect2::cf32(0, 0));
    size_t left = (size_t)(secs * rate);
    bool got = false;
    while (left) {
        const size_t n = left < buf.size() ? left : buf.size();
        rx.feed(buf.data(), n);
        left -= n;
        Tel t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = std::move(t); got = true; }
    }
    return got;
}

// The common checks of one receiver: count(tel) is the size of the lists that must survive.
//  1. decode for secs: something is listed
//  2. reset() and configure() (the engine's mode switch, retune and restart): still listed after more quiet signal
//  3. saveResults -> a fresh receiver's loadResults: the same count, at once and after it runs
//  4. a damaged blob and another receiver's blob are refused and change nothing
//  5. clearResults(): empty, and stays empty
template <class Rx, class Tel, class Count>
void checkReceiver(const char* name, double rate, std::unique_ptr<dect2::ModeSynth> syn, double secs, Count count, const std::string& otherBlob,
                   void (*setup)(Rx&) = nullptr) {
    Rx rx;
    rx.configure(rate);
    if (setup) setup(rx);
    Tel t;
    uint64_t seq = 0;
    RCHECK(syn != nullptr, "%s: no test signal", name);
    if (!syn) return;
    feed(rx, *syn, secs, t, seq);
    const size_t n0 = count(t);
    printf("  %s: %zu listed after %.0f s\n", name, n0, secs);
    RCHECK(n0 > 0, "%s: nothing decoded from the test signal", name);

    rx.reset();
    feedQuiet(rx, rate, 1.0, t, seq);
    RCHECK(count(t) >= n0, "%s: %zu listed after reset(), %zu before", name, count(t), n0);
    rx.configure(rate);
    if (setup) setup(rx);
    feedQuiet(rx, rate, 1.0, t, seq);
    const size_t n1 = count(t);
    RCHECK(n1 >= n0, "%s: %zu listed after configure(), %zu before", name, n1, n0);

    const std::string blob = rx.saveResults();
    RCHECK(blob.size() > 16, "%s: empty results blob", name);
    Rx rx2;
    RCHECK(rx2.loadResults(blob), "%s: its own blob refused", name);
    Tel t2;
    uint64_t seq2 = 0;
    RCHECK(rx2.telemetry(t2, seq2) && count(t2) == n1, "%s: loaded receiver reports %zu at once, saved %zu", name, count(t2), n1);
    seq2 = t2.seq;
    rx2.configure(rate);
    if (setup) setup(rx2);
    feedQuiet(rx2, rate, 1.0, t2, seq2);
    RCHECK(count(t2) == n1, "%s: loaded receiver lists %zu after it ran, saved %zu", name, count(t2), n1);
    RCHECK(rx2.saveResults().size() > 16, "%s: the loaded receiver saves nothing", name);

    std::string bad = blob;
    bad.resize(bad.size() / 2);
    RCHECK(!rx2.loadResults(bad), "%s: a cut blob was taken", name);
    RCHECK(!rx2.loadResults(otherBlob), "%s: another receiver's blob was taken", name);
    feedQuiet(rx2, rate, 0.5, t2, seq2);
    RCHECK(count(t2) == n1, "%s: a refused blob changed the lists (%zu, was %zu)", name, count(t2), n1);

    rx.clearResults();
    Tel t3;
    RCHECK(rx.telemetry(t3, seq) && count(t3) == 0, "%s: %zu listed right after clearResults()", name, count(t3));
    feedQuiet(rx, rate, 1.0, t, seq);
    RCHECK(count(t) == 0, "%s: %zu listed after clearResults() and quiet signal", name, count(t));
    Rx rx3;
    RCHECK(rx3.loadResults(rx.saveResults()), "%s: the cleared blob refused", name);
}

} // namespace rtk
