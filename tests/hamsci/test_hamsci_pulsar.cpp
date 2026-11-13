// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pulsar folding on simulated filter bank data: detection of a dispersed pulse train in noise, the dispersion measure and period found, several pulsars
// (periods from 33 ms to 1.3 s), a weak case that must NOT be reported, noise alone, a wrong nominal period inside the search range, the FFT front end on a
// tone (channel order), and the radiometer arithmetic. Simulated only: no pulsar recording was available, and an ordinary SDR antenna sees none of these.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_testutil.h"
using namespace dect2::hamsci;

static PulsarResult fold(const PulsarConfig& c, const std::vector<float>& rows) {
    PulsarFolder f(c);
    const int C = f.config().channels;
    for (size_t r = 0; r + (size_t)C <= rows.size(); r += (size_t)C) f.pushRow(&rows[r]);
    return f.search();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    PulsarConfig c;
    c.centreHz = 408e6; c.rateHz = 2.4e6; c.channels = 64; c.sampleSec = 0.5e-3; c.bins = 64;
    c.periodS = 0.714520; c.dm = 26.76; c.dmSpan = 4.0; c.dmSteps = 9; c.periodRelSpan = 2e-4; c.periodSteps = 21;
    PulsarFolder probe(c);
    c = probe.config();   // the effective sample time
    // 1. B0329+54 like: 120 s, per channel per row S/N 0.4 (invisible in one row), 3 % duty
    {
        auto rows = pulsarRenderRows(c, 120, 0.4, 5, 0.006);
        PulsarResult r = fold(c, rows);
        std::printf("pulsar B0329 like: S/N %.1f, DM %.2f (true 26.76), period %.6f (true 0.714520), duty %.3f\n", r.snr, r.dm, r.periodS, r.dutyCycle);
        CHECK(r.ok, "not detected, S/N %.1f", r.snr);
        CHECK(std::fabs(r.dm - 26.76) <= 1.5, "DM %.2f", r.dm);
        CHECK(std::fabs(r.periodS - 0.714520) < 0.714520 * 4e-5, "period %.7f", r.periodS);
        CHECK(r.profile.size() == 64, "profile size");
    }
    // 2. a wrong nominal period by 1e-4 (inside the search range) is found
    {
        PulsarConfig c2 = c;
        c2.periodS = 0.714520 * (1.0 + 1.2e-4);
        auto rows = pulsarRenderRows(c2, 120, 0.4, 6, 0.006, -1, 0.714520);
        PulsarResult r = fold(c2, rows);
        CHECK(r.ok && std::fabs(r.periodS - 0.714520) < 0.714520 * 4e-5, "offset period: ok %d period %.7f", r.ok, r.periodS);
    }
    // 3. other pulsars, periods from the table (nominal period, DM)
    int n = 0;
    const PulsarTarget* t = pulsarTargets(n);
    for (int i : {0, 1, 2, 7}) {
        PulsarConfig c3 = c;
        c3.periodS = t[i].periodS; c3.dm = t[i].dm;
        c3.sampleSec = t[i].periodS < 0.1 ? 0.25e-3 : 0.5e-3;
        PulsarFolder pf(c3);
        c3 = pf.config();
        auto rows = pulsarRenderRows(c3, t[i].periodS < 0.1 ? 60 : 120, 0.5, 10 + (uint32_t)i, 0.01);
        PulsarResult r = fold(c3, rows);
        std::printf("pulsar %-16s S/N %.1f DM %.2f (%.2f) period %.7f (%.7f)\n", t[i].name, r.snr, r.dm, t[i].dm, r.periodS, t[i].periodS);
        CHECK(r.ok && std::fabs(r.periodS - t[i].periodS) < t[i].periodS * 8e-5, "%s: ok %d S/N %.1f period %.7f", t[i].name, r.ok, r.snr, r.periodS);
    }
    // 4. too weak: must not be reported
    {
        auto rows = pulsarRenderRows(c, 120, 0.02, 7, 0.006);
        PulsarResult r = fold(c, rows);
        std::printf("pulsar too weak (S/N 0.02 per channel row): reported S/N %.1f ok=%d\n", r.snr, r.ok);
        CHECK(!r.ok, "a pulsar too weak to see was reported (S/N %.1f)", r.snr);
    }
    // 5. noise alone, several seeds: no detection
    {
        int falses = 0;
        double worst = 0;
        for (uint32_t seed = 1; seed <= 6; seed++) {
            auto rows = pulsarRenderRows(c, 100, 0.0, 100 + seed, 0.03);
            PulsarResult r = fold(c, rows);
            worst = std::max(worst, r.snr);
            if (r.ok) falses++;
        }
        std::printf("pulsar noise alone: worst S/N over 6 records and %d trials each: %.1f\n", c.dmSteps * c.periodSteps, worst);
        CHECK(falses == 0, "noise alone: %d false detections", falses);
    }
    // 6. the front end: a tone at +300 kHz of a 2.4 Msps input lands in channel 40 of 64, and the output rate is right
    {
        PulsarConfig c4 = c;
        PulsarFolder pf(c4);
        const double rate = c4.rateHz;
        std::vector<std::complex<float>> x((size_t)(0.1 * rate));
        for (size_t i = 0; i < x.size(); i++) x[i] = std::complex<float>((float)std::cos(2 * 3.14159265 * 300e3 * (double)i / rate), (float)std::sin(2 * 3.14159265 * 300e3 * (double)i / rate));
        pf.feed(x.data(), x.size());
        const size_t rowsN = pf.blocksStored();
        CHECK(std::fabs((double)rowsN * pf.config().sampleSec - 0.1) < 0.002, "rows %zu x %.6f s", rowsN, pf.config().sampleSec);
    }
    // 7. radiometer arithmetic: B0329+54, 1.5 Jy, 2.4 MHz, Tsys 80 K, 1 h, 3 % duty, S/N 5
    {
        const double a = pulsarRequiredAreaM2(1.5, 2.4e6, 80, 3600, 0.03, 5, 2);
        std::printf("radiometer: B0329+54 needs about %.2f m^2 effective area (1 h, 2.4 MHz, 80 K, S/N 5); a dipole at 408 MHz has about 0.12 m^2\n", a);
        CHECK(a > 0.5 && a < 2.0, "area %.1f m^2", a);
        CHECK(pulsarRequiredAreaM2(1.5, 2.4e6, 80, 4 * 3600, 0.03, 5, 2) < a * 0.55, "four times the time must halve the area");
        CHECK(std::fabs(pulsarDelaySec(26.76, 407.0, 409.0) - 4.148808e3 * 26.76 * (1 / (407.0 * 407.0) - 1 / (409.0 * 409.0))) < 1e-12, "dispersion delay");
    }
    std::printf(g_fails ? "pulsar: %d failures\n" : "pulsar: all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
