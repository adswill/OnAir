// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 4.0 sensitivity (docs/SENSITIVITY-4.0-X2.md): the APT line filter (a local Wiener filter along the line with the noise measured on space A)
// against the same receiver with it off (3.9), same noise. The receiver's own test signal (generator SNR: carrier to noise in 40 kHz) with a
// 2.5 kHz carrier offset, a 30 Hz/s Doppler rate and an 80 ppm clock; the figure is the PSNR of image A and B against the test picture after a
// least-squares gain and offset (the picture's black and white come from the sync pulses). Prints the curve; guard: the filter gains 0.5 dB at 5 dB and
// 1.5 dB at 2 dB, at 30 dB it costs at most 0.3 dB, and the lines found are the same both ways. Simulated signals only; the test picture is smooth
// (ramps and slow waves with one sharp step), so real pictures with more detail gain less.
#include "dect2/wxsat/wxsat_gen.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

struct Out { double psnr = 0; uint64_t lines = 0; };

Out run(double snr, bool filter) {
    WxsatGenConfig g; g.profile = kWxApt; g.rate = 960e3; g.snrDb = snr; g.cfoHz = 2500; g.driftHzPerS = 30; g.clockPpm = 80; g.seed = 11;
    WxsatGenerator gen(g);
    WxsatReceiver rx(kWxApt);
    rx.configure(g.rate);
    rx.setAptNoiseFilter(filter);
    std::vector<cf32> buf(1 << 16);
    WxsatTelemetry t;
    uint64_t seq = 0;
    for (double done = 0; done < 16 * g.rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    rx.flush();
    if (rx.telemetry(t, seq)) seq = t.seq;
    Out o; o.lines = t.lines;
    const WxImage* im = t.image.get();
    if (!im) return o;
    std::vector<const WxLine*> ls;
    for (const auto& l : im->line) if (l && (int)l->px.size() >= 2080) ls.push_back(l.get());
    if (ls.size() < 4) return o;
    ls.erase(ls.begin());   // the first line's black and white are still settling
    auto ref = [](int seg, int x, int y) { return wxsat::wxTestPixel(seg, x, y) / 1023.0; };
    // the line number of the first decoded line: the offset (mod 400) with the best match
    int best = 0; double bestE = 1e300;
    for (int off = 0; off < 64; off++) {
        double e = 0;
        for (size_t k = 0; k < std::min<size_t>(ls.size(), 6); k++)
            for (int x = 0; x < 909; x += 9) { const double d = ls[k]->px[(size_t)(86 + x)] / 255.0 - ref(0, x, ls[k]->idx + off); e += d * d; }
        if (e < bestE) { bestE = e; best = off; }
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, n = 0;
    for (const WxLine* l : ls) for (int seg = 0; seg < 2; seg++) for (int x = 0; x < 909; x++) {
        const double r = ref(seg, x, l->idx + best), y = l->px[(size_t)((seg ? 1126 : 86) + x)];
        sx += r; sy += y; sxx += r * r; sxy += r * y; syy += y * y; n++;
    }
    const double a = (n * sxy - sx * sy) / (n * sxx - sx * sx), b = (sy - a * sx) / n;
    const double res = (syy - 2 * a * sxy - 2 * b * sy + a * a * sxx + 2 * a * b * sx + n * b * b) / n;
    o.psnr = 10 * std::log10(255.0 * 255.0 / std::max(1e-9, res));
    return o;
}

} // namespace

int main() {
    double g5 = 0, g2 = 0, g30 = 0;
    for (double snr : {30.0, 15.0, 10.0, 5.0, 2.0, 0.0, -3.0}) {
        const Out a = run(snr, false), b = run(snr, true);
        printf("APT %5.1f dB: PSNR %5.2f -> %5.2f dB (%+.2f), lines %llu / %llu\n", snr, a.psnr, b.psnr, b.psnr - a.psnr,
               (unsigned long long)a.lines, (unsigned long long)b.lines);
        fflush(stdout);
        CHECK(a.lines == b.lines, "%.0f dB: lines %llu without, %llu with the filter", snr, (unsigned long long)a.lines, (unsigned long long)b.lines);
        if (snr == 5.0) g5 = b.psnr - a.psnr;
        if (snr == 2.0) g2 = b.psnr - a.psnr;
        if (snr == 30.0) g30 = b.psnr - a.psnr;
    }
    CHECK(g5 >= 0.5 && g2 >= 1.5, "gain %.2f dB at 5 dB, %.2f dB at 2 dB", g5, g2);
    CHECK(g30 >= -0.3, "30 dB: gain %.2f dB", g30);
    return testSummary("wxsat_apt40");
}
