// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sigfox uplinks in the ISM sensors mode: generated RC1 frames (0, 4 and 12 byte payloads, three repetitions each on random frequencies
// within the 192 kHz span) through the ISM receiver. Must hold: ID, sequence and payload decode, the three repetitions merge into one
// message, +-1 kHz radio offset, 12 dB in the 100 Hz channel, noise gives nothing.
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_sigfox.h"
#include "dect2/ism/ism_tel.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static std::string hex(const std::vector<uint8_t>& v) { std::string s; char b[4]; for (uint8_t x : v) { snprintf(b, sizeof b, "%02X", x); s += b; } return s; }

static std::vector<SigfoxTx> scene(uint32_t seed, std::vector<std::vector<uint8_t>>* payloads = nullptr) {
    PortableRng rng(seed);   // the same scene on every platform
    std::vector<SigfoxTx> v;
    const size_t lens[3] = {0, 4, 12};
    for (int m = 0; m < 3; m++) {
        std::vector<uint8_t> pl(lens[m]);
        for (auto& b : pl) b = (uint8_t)(rng() & 0xFF);
        if (payloads) payloads->push_back(pl);
        const uint32_t id = 0x003A0000u + (uint32_t)(rng() & 0xFFFF), seq = (uint32_t)(rng() & 0xFFF);
        for (int r = 0; r < 3; r++) {
            SigfoxTx t;
            t.startSec = 0.3 + m * 0.7 + r * 2.8;   // the three messages overlap in time, on different frequencies
            // (at least 2 kHz apart while on the air together: two carriers on one frequency are a collision, not what is tested here)
            auto clash = [&] { for (const auto& o : v) if (std::fabs(o.startSec - t.startSec) < 2.7 && std::fabs(o.offsetHz - t.offsetHz) < 2e3) return true; return false; };
            do t.offsetHz = rng.uniform(-90e3, 90e3); while (clash());
            t.id = id; t.seq = seq; t.payload = pl;
            v.push_back(t);
        }
    }
    return v;
}

static IsmTelemetry run(const std::vector<cf32>& x, double rate, double dialOff) {
    IsmReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(dialOff);
    const size_t chunk = 65536;
    for (size_t i = 0; i < x.size(); i += chunk) rx.feed(x.data() + i, std::min(chunk, x.size() - i));
    IsmTelemetry t;
    rx.telemetry(t, 0);
    return t;
}

static void check(const char* name, double rate, double dialOff, double snr, double cfo, uint32_t seed) {
    std::vector<std::vector<uint8_t>> pls;
    const auto txs = scene(seed, &pls);
    auto x = sigfoxSynth(rate, dialOff, 10.5, snr, cfo, txs, seed + 100);
    const IsmTelemetry t = run(x, rate, dialOff);
    const auto& ms = t.sigfox.messages;
    int good = 0;
    for (int m = 0; m < 3; m++) {
        const SigfoxTx& tx = txs[m * 3];
        char id[16]; snprintf(id, sizeof id, "%08X", tx.id);
        const SigfoxMessage* f = nullptr;
        for (const auto& q : ms) if (q.id == id) f = &q;
        CHECK(f != nullptr, "%s: message %d (ID %s) not decoded", name, m, id);
        if (!f) continue;
        CHECK(f->seq == tx.seq && f->payload == hex(pls[m]) && f->len == (int)pls[m].size(), "%s: ID %s seq %u (sent %u) payload %s (sent %s)", name, id, f->seq, tx.seq, f->payload.c_str(), hex(pls[m]).c_str());
        CHECK(f->reps == 3, "%s: ID %s: %d repetitions merged, expected 3", name, id, f->reps);
        good += f->seq == tx.seq && f->payload == hex(pls[m]) && f->reps == 3;
    }
    CHECK(ms.size() == 3, "%s: %zu messages, expected 3", name, ms.size());
    printf("  %-34s %d/3 messages complete, %zu in the list, %llu carriers followed\n", name, good, ms.size(), (unsigned long long)t.sigfox.carriers);
}

int main() {
    check("clean, 2 Msps, dial -250 kHz", 2e6, -250e3, 25, 0, 1);
    check("12 dB in 100 Hz, 1 Msps", 1e6, 0, 12, 0, 2);
    check("radio offset +1 kHz", 1e6, 0, 14, 1000, 3);
    check("radio offset -1 kHz, 2 Msps", 2e6, -250e3, 14, -1000, 4);
    {   // noise only, and a continuous carrier
        auto x = sigfoxSynth(1e6, 0, 8.0, 12, 0, {}, 9);
        const IsmTelemetry t = run(x, 1e6, 0);
        CHECK(t.sigfox.messages.empty(), "noise: %zu messages", t.sigfox.messages.size());
        for (size_t i = 0; i < x.size(); i++) x[i] += cf32(0.02f * (float)std::cos(2 * M_PI * 30e3 * i / 1e6), 0.02f * (float)std::sin(2 * M_PI * 30e3 * i / 1e6));
        const IsmTelemetry t2 = run(x, 1e6, 0);
        CHECK(t2.sigfox.messages.empty(), "noise and a carrier: %zu messages", t2.sigfox.messages.size());
        printf("  noise only: %zu messages, %llu carriers\n", t.sigfox.messages.size(), (unsigned long long)t.sigfox.carriers);
    }
    printf(fails ? "ism sigfox: %d FAILED\n" : "ism sigfox: all passed\n", fails);
    return fails ? 1 : 0;
}
