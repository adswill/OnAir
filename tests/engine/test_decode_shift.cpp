// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sticky tuning (Engine::setDecodeShiftHz, a click on the spectrum inside the captured band): only the receiver gets the clicked signal at DC.
// The spectrum must keep showing the capture as the radio delivers it, around captureCentreHz(): the app draws its axis around the radio's centre
// and the tuned marker at the click. When the spectrum was taken after the shift, every signal slid by the shift on the screen, the marker sat on
// the wrong one and a second click jumped to the other side (#262, DAB with a wide capture).
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// the frequency (Hz from the centre) of the strongest bin of the spectrum, averaged over a few frames
static bool peakHz(Engine& e, double rate, double& hz) {
    std::vector<double> acc;
    uint64_t seq = 0;
    SpectrumFrame f;
    if (e.latestSpectrum(f, 0)) seq = f.seq;
    int frames = 0;
    enginetest::waitFor(20, [&] {
        if (e.latestSpectrum(f, seq) && !f.dbfs.empty()) {
            seq = f.seq;
            if (acc.size() != f.dbfs.size()) { acc.assign(f.dbfs.size(), 0.0); frames = 0; }
            for (size_t k = 0; k < f.dbfs.size(); k++) acc[k] += std::pow(10.0, f.dbfs[k] / 10.0);
            frames++;
        }
        return frames >= 8;
    });
    if (!frames) return false;
    size_t best = 0;
    for (size_t k = 1; k < acc.size(); k++) if (acc[k] > acc[best]) best = k;
    hz = ((double)best - (double)acc.size() / 2) * rate / (double)acc.size();
    return true;
}

int main() {
    const double rate = 2.048e6, toneHz = 400e3, shiftHz = -600e3;
    const std::string path = "decode_shift_test.cf32";
    {   // a tone 400 kHz above the centre, with a little noise, a second long (the file loops)
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { printf("FAIL: cannot write %s\n", path.c_str()); return 1; }
        std::vector<std::complex<float>> x((size_t)rate);
        uint32_t s = 12345;
        for (size_t i = 0; i < x.size(); i++) {
            s = s * 1664525u + 1013904223u;
            const float nz = ((float)(s >> 8) / 16777216.f - 0.5f) * 0.01f;
            const double ph = 2 * M_PI * toneHz * (double)i / rate;
            x[i] = std::complex<float>(0.3f * (float)std::cos(ph) + nz, 0.3f * (float)std::sin(ph) - nz);
        }
        fwrite(x.data(), sizeof x[0], x.size(), f);
        fclose(f);
    }
    Engine e;
    e.setSpectrumEnabled(true);
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "decode shift test";
    TuneSettings tune; tune.sampleRate = rate; tune.centerHz = 220e6; tune.bandwidthMhz = 1.536;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = rate; fo.loop = true;
    e.setStandard(3);   // DAB
    if (!e.start(dev, tune, fo)) { printf("FAIL: the engine did not start\n"); std::remove(path.c_str()); return 1; }
    const double bin = rate / 1024;   // coarser than any FFT size the analyser picks at this rate
    double hz = 0;
    CHECK(peakHz(e, rate, hz), "no spectrum");
    CHECK(std::fabs(hz - toneHz) < bin, "without a shift the tone is at %+.0f Hz, expected %+.0f", hz, toneHz);
    e.setDecodeShiftHz(shiftHz);   // the receiver decodes 600 kHz below the centre: the spectrum must not move
    const double t0 = enginetest::signalSecs(e);
    enginetest::waitFor(10, [&] { return enginetest::signalSecs(e) > t0 + 0.3; });   // frames from before the shift are gone
    CHECK(peakHz(e, rate, hz), "no spectrum after the shift");
    CHECK(std::fabs(hz - toneHz) < bin, "with a decode shift of %+.0f Hz the tone shows at %+.0f Hz, expected it to stay at %+.0f Hz", shiftHz, hz, toneHz);
    CHECK(std::fabs(e.decodeShiftHz() - shiftHz) < 1, "decodeShiftHz %.0f", e.decodeShiftHz());
    e.stop();
    std::remove(path.c_str());
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("decode shift: the spectrum stays around the radio's centre\n");
    return 0;
}
