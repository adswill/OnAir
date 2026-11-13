// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL transmitter with the beacon mix: voice plus slice on the 2 s / 10 s schedule; the slice receiver finds the frames in the output.
#include "dect2/kaal/kaal_beacon.h"
#include "dect2/kaal/kaal_tx.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
using namespace dect2::kaal;
#include "../common/test_util.h"

typedef std::complex<float> cf;

int main() {
    // 7 s of speech-like audio (wobbling pitch, slow level change), as in the Python beacon test
    std::vector<int16_t> pcm(16000 * 7);
    for (size_t i = 0; i < pcm.size(); i++) {
        const double t = (double)i / 16000.0;
        pcm[i] = (int16_t)(3000.0 * std::sin(2 * 3.141592653589793 * (150 + 60 * std::sin(2 * 3.141592653589793 * 1.3 * t)) * t) *
                           (0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 3 * t)));
    }
    BeaconFields F;
    F.callsign = "LY2ABC"; F.grid = "KO26ab"; F.lat = 54.687; F.lon = 25.279; F.powerDbm = 20;
    const std::string bio = "Hi, I am testing KAAL, a custom digital voice mode. Tell me what you hear.";

    auto run = [&](bool mix, std::vector<cf>& y) {
        VoiceTx tx;
        tx.configure(48000);
        tx.setBeacon(F, bio);
        tx.setBeaconMix(mix);
        std::vector<cf> buf(4096);
        auto drain = [&] { for (size_t n; (n = tx.popIq(buf.data(), buf.size())) > 0;) y.insert(y.end(), buf.begin(), buf.begin() + n); };
        for (size_t i = 0; i < pcm.size(); i += 1600) { tx.pushAudio(&pcm[i], std::min<size_t>(1600, pcm.size() - i)); drain(); }
        tx.flush();
        drain();
    };
    std::vector<cf> plain, mixed;
    run(false, plain);
    run(true, mixed);
    CHECK(plain.size() == mixed.size() && plain.size() > 48000u * 7, "lengths %zu %zu", plain.size(), mixed.size());

    // the added part is the beacon: its power is about 10 dB below the voice data
    double pp = 0, pd = 0;
    for (size_t i = 0; i < std::min(plain.size(), mixed.size()); i++) { pp += std::norm(plain[i]); pd += std::norm(mixed[i] - plain[i]); }
    CHECK(pd > 0, "no beacon added");
    const double ratio = 10 * std::log10(pd / pp);
    printf("beacon power relative to the transmit signal: %.1f dB (it is on for part of the time)\n", ratio);
    CHECK(ratio < -10.0 && ratio > -20.0, "beacon level %.1f dB", ratio);

    // without the mix the receiver finds nothing; with it, the full frame at 0 s (it lasts 3.4 s, so the 2 s header slot is skipped) and headers at 4 s, 6 s and 8 s: the 7 s of speech take 8.9 s on air
    // because a 20 ms voice frame is 202 symbols at 8000 symbols/s (25.25 ms)
    auto rxAll = [&](const std::vector<cf>& y) {
        BeaconSliceRx rx;
        rx.configure(48000);
        std::vector<std::pair<BeaconFields, std::string>> got;
        for (size_t i = 0; i < y.size(); i += 960) {
            rx.feed(&y[i], std::min<size_t>(960, y.size() - i));
            BeaconFields f; std::string t;
            while (rx.poll(f, t)) got.emplace_back(f, t);
        }
        return got;
    };
    CHECK(rxAll(plain).empty(), "a beacon without the mix");
    const auto got = rxAll(mixed);
    printf("frames decoded from the mixed signal: %zu\n", got.size());
    CHECK(got.size() == 4u, "%zu frames, expected the full one and three headers", got.size());
    if (got.size() >= 1) CHECK(got[0].second == bio && got[0].first.callsign == "LY2ABC", "first frame is not the full beacon");
    for (size_t i = 1; i < got.size(); i++) CHECK(got[i].second.empty() && got[i].first.callsign == "LY2ABC", "frame %zu is not a header", i);
    return testSummary("kaal_tx_mix");
}
