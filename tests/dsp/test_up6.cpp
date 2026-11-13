// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The voice upsampler 8 -> 48 kHz (dsp/up6.h): speech tones pass at unit gain, their images above 4 kHz are gone. The straight-line
// interpolation it replaced kept a 3 kHz tone's image at 5 kHz only about 13 dB down (sibilant, metallic voice).
#include "dect2/dsp/up6.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

// level (dB re a full-scale sine) of the f Hz component of x at rate fs, by correlation over the whole buffer
static double levelDb(const std::vector<float>& x, double f, double fs) {
    std::complex<double> acc = 0;
    for (size_t i = 0; i < x.size(); i++) acc += (double)x[i] * std::polar(1.0, -2 * 3.14159265358979323846 * f * (double)i / fs);
    return 20 * std::log10(std::max(1e-12, 2 * std::abs(acc) / (double)x.size()));
}

int main() {
    for (double f : {300.0, 1000.0, 2000.0, 3000.0, 3400.0}) {
        Up6 up;
        std::vector<float> out;
        for (int i = 0; i < 8000; i++) up.push((float)std::sin(2 * 3.14159265358979323846 * f * i / 8000.0), [&](float v) { out.push_back(v); });
        out.erase(out.begin(), out.begin() + 480);   // the filter's start
        const double pass = levelDb(out, f, 48000);
        double worstImage = -300;
        for (int k = 1; k <= 3; k++)
            for (double img : {8000.0 * k - f, 8000.0 * k + f}) worstImage = std::max(worstImage, levelDb(out, img, 48000));
        // up to 3 kHz the passband; 3.4 kHz is in the filter's transition (XMC_SDR's design: 3.8 kHz, 96 taps), where speech has little left
        CHECK(std::fabs(pass) < (f <= 3000 ? 0.5 : 2.5), "%.0f Hz passes at %.2f dB", f, pass);
        CHECK(worstImage < (f <= 3000 ? -50 : -25), "%.0f Hz: an image at %.1f dB", f, worstImage);
        printf("%5.0f Hz: passband %+.2f dB, worst image %.1f dB\n", f, pass, worstImage);
    }
    return testSummary("up6");
}
