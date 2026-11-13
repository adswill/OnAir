// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FANS-1/A over HFDL: an ACARS downlink carrying a real CPDLC message (libacars examples, also in test_aero_cpdlc) and one carrying an ADS-C
// basic report, each sent as an HFDL burst through the receiver with a tuning error and noise. The CPDLC one must be labelled CPDLC with its
// text decoded, the ADS-C one ADS-C with its position; a damaged CPDLC text (one character changed) must say the CRC failed.
// Simulated RF only (the CPDLC text itself is a real downlink).
#include "dect2/aero/aero_adsc.h"
#include "dect2/hfdl/hfdl_gen.h"
#include "dect2/hfdl/hfdl_rx.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 250e3, kDial = 8.942e6, kCentre = 8.98e6;
// a downlink text starts with the message number and the flight id (ARINC 618), then the application
static const std::string kCpdlc = "M01AKE0907/SOUCAYA.AT1.HL8251243F880C3D903BB412903604FE326C2479F4A64F7F62528B1A9CF8382738186AC28B16668E013DF464D8A7F0";

// the detail of the one good ACARS message on the dial, "" when there is none
static std::string one(const std::string& label, const std::string& text, double cfo, double snrDb, uint32_t seed) {
    HfdlReceiver rx;
    rx.setParallel(false);
    rx.configure(kRate);
    rx.setCenterHz(kCentre);
    const std::vector<uint8_t> pdu = hfdlMpduDown(1, 23, {hfdlUnnumbered(hfdlAcarsHfnpdu("HL8251", label, '2', text, true))});
    int m1 = 7;
    for (int k = 0; k < 8; k++) if (hfdlPayloadOctets(k) >= (int)pdu.size() && hfdlRate(k).bitsPerSym <= 2) { m1 = k; break; }
    HfdlWave w = hfdlMakeWave(m1, pdu, {}, seed);
    w.start = 0.2 * kRate; w.carHz = kDial + kHfdlCarrierHz - kCentre + cfo; w.amp = 0.1f; w.phase = 0.3;
    const double sigma = std::sqrt(0.01 / std::pow(10.0, snrDb / 10) * (kRate / kHfdlBaud) / 2);
    PortableRng rng(seed);
    const size_t N = 32768;
    std::vector<cf32> buf(N);
    const int64_t total = (int64_t)((0.2 + hfdlBurstSeconds(m1) + 0.5) * kRate);
    for (int64_t pos = 0; pos < total; pos += (int64_t)N) {
        for (size_t i = 0; i < N; i++) buf[i] = cf32((float)rng.gauss(sigma), (float)rng.gauss(sigma));
        hfdlRender(w, buf.data(), pos, N, kRate);
        rx.feed(buf.data(), N);
    }
    HfdlTelemetry t; rx.telemetry(t, ~0ull);
    for (const auto& m : t.messages) if (m.acars && m.acarsCrcOk && std::fabs(m.freqHz - kDial) < 1) return m.detail;
    return "";
}

static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main() {
    std::vector<uint8_t> g = aeroAdscBasicGroup(51.47, -12.25, 37000, 1234.5, 6);
    const auto fid = aeroAdscFlightIdGroup("KAL907");
    g.insert(g.end(), fid.begin(), fid.end());
    const std::string adsc = "M02AKE0907" + aeroBuildAdscText("SOUCAYA", "HL8251", g);
    std::string bad = kCpdlc;
    bad[40] = bad[40] == 'A' ? 'B' : 'A';
    struct Case { double cfo, snr; uint32_t seed; };
    for (const Case c : {Case{0, 30, 1}, Case{+180, 12, 2}, Case{-150, 10, 3}}) {
        const std::string d1 = one("H1", kCpdlc, c.cfo, c.snr, c.seed);
        const std::string d2 = one("B6", adsc, c.cfo, c.snr, c.seed + 10);
        printf("  cfo %+4.0f Hz, %2.0f dB: CPDLC %s, ADS-C %s\n", c.cfo, c.snr, d1.empty() ? "not received" : has(d1, "CPDLC (FANS-1/A):") ? "labelled" : "unlabelled",
               d2.empty() ? "not received" : has(d2, "ADS-C (FANS-1/A):") ? "labelled" : "unlabelled");
        CHECK(has(d1, "CPDLC (FANS-1/A):") && has(d1, "FANS-1/A CPDLC message (AT1)") && !has(d1, "CRC FAILED") && !has(d1, "did not decode"),
              "cfo %.0f snr %.0f: CPDLC detail:\n%s", c.cfo, c.snr, d1.c_str());
        CHECK(has(d2, "ADS-C (FANS-1/A):") && !has(d2, "CPDLC"), "cfo %.0f snr %.0f: ADS-C detail:\n%s", c.cfo, c.snr, d2.c_str());
    }
    const std::string d3 = one("H1", bad, 60, 25, 9);
    CHECK(has(d3, "CPDLC (FANS-1/A):") && has(d3, "CRC FAILED"), "damaged CPDLC must say the CRC failed:\n%s", d3.c_str());
    return testSummary("hfdl fans");
}
