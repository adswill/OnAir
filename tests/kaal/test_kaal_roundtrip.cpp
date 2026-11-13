// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL codec, decoder against the Python reference: the Python frames decode to the Python PCM (SNR above 60 dB, in practice
// identical), also with a reserved-type frame and a wrong-shape frame concealed; and the C++ encode -> decode chain follows the Python's.
#include "dect2/kaal/kaal_codec.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"
#include "kaal_test_common.h"

int main() {
    KaalCodec c;
    const size_t n = (size_t)kGoldFrames * 320;
    std::vector<std::vector<uint8_t>> fr;
    for (int f = 0; f < kGoldFrames; f++) fr.push_back(kaalGoldBits(f));
    {
        const auto y = c.decode(fr);
        CHECK(y.size() == n, "size %zu", y.size());
        const double snr = kaalSnrDb(kGoldPcmGood, y.data(), n);
        printf("decode of the Python frames: SNR %.1f dB\n", snr);
        CHECK(snr > 60.0, "snr %.1f", snr);
        int maxd = 0;
        for (size_t i = 0; i < n; i++) maxd = std::max(maxd, std::abs((int)y[i] - kGoldPcmGood[i]));
        CHECK(maxd <= 1, "max sample difference %d", maxd);
    }
    {
        auto d = fr;
        d[5] = std::vector<uint8_t>(320, 1);
        d[8] = std::vector<uint8_t>(7, 0);
        const auto y = c.decode(d);
        const double snr = kaalSnrDb(kGoldPcmDamaged, y.data(), n);
        printf("concealment against the Python: SNR %.1f dB\n", snr);
        CHECK(y.size() == n && snr > 60.0, "snr %.1f", snr);
    }
    {   // C++ encoder -> C++ decoder against the Python decode of the Python frames
        const auto x = kaalTestSignal();
        const auto bits = c.encode(x);
        const auto y = c.decode(bits.data(), kGoldFrames);
        const double snr = kaalSnrDb(kGoldPcmGood, y.data(), n);
        printf("C++ encode + decode against the Python: SNR %.1f dB\n", snr);
        CHECK(snr > 60.0, "snr %.1f", snr);
        // the whole second, delayed by DELAY, is a recognisable copy of the input
        const auto yy = c.decode(bits);
        CHECK(yy.size() == 51u * 320u, "full size");
        const double s2 = kaalSnrDb(x.data() + 640, yy.data() + KaalCodec::kDelay + 640, 15000);
        printf("input against output, 1 s: SNR %.1f dB\n", s2);
        CHECK(s2 > 8.0, "codec snr %.1f", s2);
    }
    return testSummary("kaal_roundtrip");
}
