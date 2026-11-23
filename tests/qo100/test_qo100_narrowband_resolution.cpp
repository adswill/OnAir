// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The narrowband spectrum detail setting (the 4.5 report: "the resolution of the spectrogram is still very low"): normal, fine (2x) and
// finest (4x) halve the bin width each, give 2x and 4x the bins, still lock on the beacons, and show a carrier further over the floor.
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_gen.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct R { double bin = 0, snr = 0, err = 1e9; size_t n = 0; int state = 0; };

static R run(int detail) {
    const double fs = 2e6, lo = 9750e6, secs = 12;
    SynthConfig sc;
    Qo100Receiver rx;
    rx.configure(fs);
    rx.setLo(lo);
    rx.setCenterHz(qo100::ifOf(qo100::kNarrowCentreHz, lo));
    if (detail) rx.setNbDetail(detail);
    auto syn = makeQo100Synth(sc, fs);
    std::vector<cf32> buf(20000);
    uint64_t seq = 0, done = 0;
    Qo100Telemetry last;
    while (done < (uint64_t)(secs * fs)) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        done += buf.size();
        Qo100Telemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    R r;
    r.bin = last.nbBinHz; r.n = last.nb.size(); r.state = last.state;
    float m = -200;
    for (size_t k = 0; k < last.nb.size(); k++) {
        const double f = last.nbF0Hz + k * last.nbBinHz;
        if (std::fabs(f - kQo100TestCarrierHz) < 300 && last.nb[k] > m) { m = last.nb[k]; }
    }
    r.snr = m - last.nbFloorDb;
    r.err = last.lnbErrHz - qo100SynthErrHz(sc, secs - 0.5);
    return r;
}

int main() {
    const R a = run(0), b = run(2), c = run(4), d = run(1);
    printf("  bins %.1f / %.1f / %.1f Hz, carrier over the floor %.1f / %.1f / %.1f dB\n", a.bin, b.bin, c.bin, a.snr, b.snr, c.snr);
    CHECK(a.bin > 0 && std::fabs(a.bin - d.bin) < 1e-6, "the default is the normal detail: %.2f vs %.2f Hz", a.bin, d.bin);
    CHECK(std::fabs(b.bin * 2 - a.bin) < 0.02 * a.bin, "fine: %.2f Hz bins against %.2f", b.bin, a.bin);
    CHECK(std::fabs(c.bin * 4 - a.bin) < 0.02 * a.bin, "finest: %.2f Hz bins against %.2f", c.bin, a.bin);
    CHECK(c.bin < 20 && b.bin < 40, "finest %.1f Hz and fine %.1f Hz bins are not fine enough", c.bin, b.bin);
    CHECK(b.n > a.n * 3 / 2 && c.n > a.n * 3, "bins %zu / %zu / %zu", a.n, b.n, c.n);
    CHECK(c.snr > a.snr + 3, "the test carrier stands %.1f dB over the floor at the finest detail, %.1f at normal", c.snr, a.snr);
    for (const R* r : {&a, &b, &c}) CHECK(r->state == 2 && std::fabs(r->err) < 10, "state %d, LNB error %.1f Hz off", r->state, r->err);
    return testSummary("qo100 narrowband resolution");
}
