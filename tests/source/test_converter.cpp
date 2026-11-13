// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The frequency converter (converter.h): the one conversion between the frequency at the antenna and the one the radio is tuned to.
#include "dect2/source/converter.h"
#include <cstdio>
#include <vector>

using namespace dect2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static bool nearBy(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

static Converter make(const char* preset, std::map<std::string, std::string> extra = {}) {
    extra[convKeyPreset()] = preset;
    return converterFromRadio(extra);
}

int main() {
    // no converter: untouched
    CHECK(!converterFromRadio({}).active);
    CHECK(!make("none").active);
    CHECK(!make("bogus").active);
    CHECK(make("none").rfToIf(7.074e6) == 7.074e6);

    // Ham It Up: 7.074 MHz at the antenna, the radio at 132.074 MHz, no mirror
    Converter h = make("hamitup");
    CHECK(h.active && !h.inverts());
    CHECK(nearBy(h.rfToIf(7.074e6), 132.074e6));
    CHECK(nearBy(h.ifToRf(132.074e6), 7.074e6));

    // QO-100 beacon through a 9750 MHz LNB: 739.55 MHz
    Converter q = make("qo100");
    CHECK(q.active && !q.inverts());
    CHECK(nearBy(q.rfToIf(10489.55e6), 739.55e6));
    CHECK(nearBy(q.ifToRf(739.55e6), 10489.55e6));
    CHECK(nearBy(make("ku-high").rfToIf(11.8e9), 1.2e9));

    // Ham It Down: 2400 MHz at the antenna, the radio at 900 MHz, no mirror, ppm field, below the LO unreachable
    Converter d = make("hamitdown");
    CHECK(d.active && !d.inverts() && nearBy(d.effLoHz(), 1500e6));
    CHECK(nearBy(d.rfToIf(2400e6), 900e6));
    CHECK(nearBy(d.ifToRf(900e6), 2400e6));
    CHECK(nearBy(d.ifToRf(d.rfToIf(2403.5e6)), 2403.5e6));
    CHECK(nearBy(make("hamitdown", {{convKeyPpm(), "2"}}).effLoHz(), 1500e6 + 3000));
    CHECK(d.reachable(2400e6) && !d.reachable(1400e6) && !d.reachable(1500e6));
    CHECK(make("hamitdown", {{convKeyInvert(), "1"}}).inverts());
    CHECK(findConverterPreset("hamitdown") != nullptr);

    // ppm: a TCXO LNB 1 ppm fast has its LO 9750 Hz high, so the same antenna frequency lands that much lower at the radio
    Converter b = make("bullseye", {{convKeyPpm(), "1"}});
    CHECK(nearBy(b.effLoHz(), 9750e6 + 9750, 1e-3));
    CHECK(nearBy(b.rfToIf(10489.55e6), 739.55e6 - 9750));
    CHECK(nearBy(b.ifToRf(b.rfToIf(10489.55e6)), 10489.55e6));
    // a preset without a ppm field ignores a stray value
    CHECK(nearBy(make("qo100", {{convKeyPpm(), "5"}}).effLoHz(), 9750e6));
    CHECK(nearBy(make("hamitup", {{convKeyPpm(), "-2.5"}}).effLoHz(), 125e6 * (1 - 2.5e-6)));

    // custom: high-side injection mirrors the spectrum
    Converter c = make("custom", {{convKeyLo(), "1000"}, {convKeyMode(), "high"}});
    CHECK(c.active && c.inverts());
    CHECK(nearBy(c.rfToIf(900e6), 100e6));
    CHECK(nearBy(c.ifToRf(100e6), 900e6));
    CHECK(nearBy(c.ifToRf(c.rfToIf(123.456e6)), 123.456e6));
    // the extra invert flag toggles it: on a mode that mirrors it cancels, on one that does not it mirrors
    CHECK(!make("custom", {{convKeyLo(), "1000"}, {convKeyMode(), "high"}, {convKeyInvert(), "1"}}).inverts());
    CHECK(make("qo100", {{convKeyInvert(), "1"}}).inverts());
    // a custom converter without an LO is no converter
    CHECK(!make("custom").active);
    CHECK(make("custom", {{convKeyLo(), "125"}}).rfToIf(7e6) == 132e6);
    // unreachable: the radio frequency would not be positive
    CHECK(!make("qo100").reachable(5e6));
    CHECK(make("qo100").reachable(10489.55e6));

    // swapping I and Q mirrors the spectrum: a tone at +f comes back at -f (as j * conj)
    const double f = 0.125;
    for (int i = 0; i < 64; i++) {
        std::complex<float> o = std::polar(1.0f, (float)(2 * M_PI * f * i));
        std::complex<float> y = o;
        swapIq(&y, 1);
        CHECK(std::abs(y - std::complex<float>(0, 1) * std::conj(o)) < 1e-5);
    }

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("converter ok\n");
    return 0;
}
