// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tuning inside an IQ recording: app/gfx/file_vfo.h (mapping, clamp, edge note, spike, centre from the name) and the DC notch.
#include "../../app/gfx/file_vfo.h"
#include "dect2/dsp/dc_notch.h"
#include <cstdio>
#include <vector>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static bool near(double a, double b, double e = 1e-9) { return std::fabs(a - b) <= e; }
int main() {
    using namespace filevfo;
    // 10 Msps: the usable half span is 4.5 MHz; a 200 kHz passband may sit 4.4 MHz from the centre
    check(near(usableHalfSpanHz(10e6), 4.5e6), "usable half span");
    check(near(maxOffsetHz(10e6, 200e3), 4.4e6), "max offset keeps the passband inside");
    check(maxOffsetHz(10e6, 20e6) == 0, "a passband wider than the span leaves no offset");
    check(near(clampOffsetHz(9e6, 10e6, 200e3), 4.4e6) && near(clampOffsetHz(-9e6, 10e6, 200e3), -4.4e6), "clamp both sides");
    check(near(clampOffsetHz(1e6, 10e6, 200e3), 1e6), "inside: untouched");
    // the VFO frequency is the centre plus the offset
    check(near(vfoMhz(100.0, 4e6), 104.0) && near(offsetHzFromVfo(104.0, 100.0), 4e6), "vfo = centre + offset, both ways");
    // a signal at +40 % of the span (4 MHz of 10): reachable by a 200 kHz passband, not clamped, not at the edge
    Plan p = plan(104.0, 100.0, 10e6, 200e3);
    check(near(p.offsetHz, 4e6) && near(p.vfoMhz, 104.0) && !p.clamped && !p.nearEdge, "+40 % of the span is reachable");
    p = plan(109.0, 100.0, 10e6, 200e3);
    check(p.clamped && near(p.vfoMhz, 104.4) && p.nearEdge, "beyond the band: clamped to the edge with a note");
    p = plan(104.3, 100.0, 10e6, 200e3);
    check(!p.clamped && p.nearEdge, "within 3 % of the edge: the passband touches it");
    p = plan(95.0, 100.0, 2e6, 3e6);
    check(p.tooWide && near(p.offsetHz, 0), "bandwidth wider than the recording: no offset");
    p = plan(100.0, 100.0, 10e6, 200e3);
    check(near(p.offsetHz, 0) && !p.clamped && !p.nearEdge, "on the centre");
    // the spike (the centre) against the passband
    check(spikeInPassband(0, 200e3, 2e3) && spikeInPassband(99e3, 200e3, 2e3), "spike inside the passband");
    check(!spikeInPassband(4e6, 200e3, 2e3), "spike far outside");
    // the centre frequency a name may carry
    check(near(guessCentreHz("/data/onair_dvb_554.000MHz_10Msps_2026.cs8"), 554e6), "MHz in the name");
    check(near(guessCentreHz("C:\\x\\cap-100.5mhz.cu8"), 100.5e6), "lower case, windows path");
    check(near(guessCentreHz("uat_978000khz_2Msps.cs8"), 978e6), "kHz");
    check(near(guessCentreHz("gps_1.57542GHz.cf32"), 1.57542e9), "GHz");
    check(guessCentreHz("capture_10Msps.cs8") == 0, "a rate is not a frequency");
    check(guessCentreHz("/data/554MHz_dir/plain.cs8") == 0, "only the file name counts");
    // the DC notch removes a constant offset and leaves a tone 1 MHz away
    {
        const double fs = 2e6;
        dect2::DcNotch n; n.set(fs);
        std::vector<std::complex<float>> x(200000);
        for (size_t i = 0; i < x.size(); i++) {
            const double ph = 6.283185307 * 250e3 * (double)i / fs;
            x[i] = std::complex<float>(0.5f + 0.3f * (float)std::cos(ph), 0.4f + 0.3f * (float)std::sin(ph));
        }
        n.process(x.data(), x.size());
        double mr = 0, mi = 0, pw = 0; size_t k = 0;
        for (size_t i = x.size() / 2; i < x.size(); i++, k++) { mr += x[i].real(); mi += x[i].imag(); pw += std::norm(x[i]); }
        mr /= (double)k; mi /= (double)k; pw /= (double)k;
        check(std::fabs(mr) < 0.01 && std::fabs(mi) < 0.01, "DC removed");
        check(pw > 0.07 && pw < 0.095, "the tone survives (0.09 power)");
    }
    if (!fails) printf("file_vfo: all passed\n");
    return fails ? 1 : 0;
}
