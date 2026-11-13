// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Weather fax as it comes off the air: the fax audio as an upper sideband signal 1.9 kHz (the carrier) above the dial of a real
// station (DWD 7880 kHz, 4583 kHz, Northwood 8040 kHz), tuned 50 Hz off, 15 dB SNR in 3 kHz, a 50 ppm clock error between the
// station and the receiver, and joined late: no start tone (part way into the phasing), or no start tone and no phasing at all
// (half way down the picture, where only the picture repeating line after line says where the lines are).
#include "dect2/marine/marine_rx.h"
#include "dect2/common/stable_random.h"
#include "../data/marine/fax/fax_util.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
#include "dect2/common/mathconst.h"
using namespace dect2;

#include "../common/test_util.h"

namespace {
using dect2::math::kPi;
constexpr double kAudio = 12000, kRate = 250000;
constexpr double kOffset = -20000;           // the channel sits tuneOffsetHz below the radio's centre, as in the engine

// The station's audio, `skip` seconds into the transmission, `secs` long, its clock `ppm` fast
std::vector<float> stationAudio(double skip, double secs, double ppm) {
    FaxAudioSource src(kAudio * (1.0 + ppm * 1e-6), 576, 120, 800, 5);
    src.setTiming(5, 30, 5, 10);
    std::vector<float> a((size_t)((skip + secs) * kAudio));
    src.generate(a.data(), a.size());
    a.erase(a.begin(), a.begin() + (long)(skip * kAudio));
    return a;
}

// Real USB audio -> the complex signal of the sideband (positive frequencies only): mix the 1900 Hz centre to 0, low-pass, mix back
std::vector<std::complex<float>> analytic(const std::vector<float>& a) {
    const int N = 161, h = N / 2;
    std::vector<double> t(N);
    double sum = 0;
    for (int i = 0; i < N; i++) {
        const double m = i - h, fc = 1300.0 / kAudio;
        const double s = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        t[i] = s * (0.42 - 0.5 * std::cos(2 * kPi * i / (N - 1)) + 0.08 * std::cos(4 * kPi * i / (N - 1)));
        sum += t[i];
    }
    for (double& v : t) v /= sum;
    std::vector<std::complex<double>> b(a.size());
    for (size_t n = 0; n < a.size(); n++) b[n] = std::polar(1.0, -2 * kPi * 1900.0 * n / kAudio) * (double)a[n];
    std::vector<std::complex<float>> out(a.size());
    for (size_t n = 0; n < a.size(); n++) {
        std::complex<double> y = 0;
        for (int i = 0; i < N; i++) { const long k = (long)n - i + h; if (k >= 0 && k < (long)a.size()) y += t[i] * b[(size_t)k]; }
        out[n] = std::complex<float>(std::polar(2.0, 2 * kPi * 1900.0 * n / kAudio) * y);
    }
    return out;
}

struct Result { FaxStatus st; FaxImage img; MarineTelemetry tel; };

// The sideband at the radio's rate: the dial at kOffset, the signal `mistuneHz` above where it should be, white noise for `snrDb` in 3 kHz
Result receive(const std::vector<std::complex<float>>& u, double dialHz, int service, double mistuneHz, double snrDb) {
    MarineReceiver rx;
    rx.configure(kRate);
    rx.setSignalOffset(kOffset);
    rx.setService(service);
    rx.setFrequencyHz(dialHz);
    std::mt19937 rng(11);
    dect2::normal_distribution<float> g(0.f, 1.f);
    double pw = 0;
    for (size_t i = 0; i < u.size(); i += 7) pw += std::norm(u[i]);
    pw /= (double)(u.size() / 7);
    const float amp = (float)(0.2 / std::sqrt(pw));
    const double noisePow = 0.04 * kRate / 3000.0 / std::pow(10.0, snrDb / 10.0);
    const float sigma = (float)std::sqrt(noisePow / 2);
    const double step = kAudio / kRate;
    const size_t total = (size_t)((double)(u.size() - 3) / step);
    std::vector<cf32> buf(65536);
    double ph = 0;
    const double dph = 2 * kPi * (kOffset + mistuneHz) / kRate;
    size_t done = 0;
    while (done < total) {
        const size_t n = std::min(buf.size(), total - done);
        for (size_t i = 0; i < n; i++) {
            const double pos = (double)(done + i) * step;
            const size_t k = (size_t)pos;
            const float fr = (float)(pos - (double)k);
            const std::complex<float> v = u[k] + fr * (u[k + 1] - u[k]);
            const std::complex<float> lo((float)std::cos(ph), (float)std::sin(ph));
            ph += dph; if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
            buf[i] = amp * v * lo + cf32{sigma * g(rng), sigma * g(rng)};
        }
        rx.feed(buf.data(), n);
        done += n;
    }
    Result r;
    r.st = rx.faxStatus();
    uint64_t seq = 0;
    rx.latestImage(r.img, seq);
    rx.telemetry(r.tel, 0);
    return r;
}

// Where the decoded picture belongs in the chart: the chart row of decoded row 0 and the sideways shift (the line start is not known
// without the phasing). Then the quality of the whole picture, rolled into place.
faxt::Quality locate(const FaxImage& img, int rowGuess, int& row0, int& shift) {
    const int W = img.width;
    const FaxImage ref = faxTestChart(W, 800, 5);
    double best = -1;
    row0 = rowGuess; shift = 0;
    const int probe = std::min(img.height - 1, 40);
    for (int r0 = rowGuess - 12; r0 <= rowGuess + 12; r0++) {
        if (r0 + probe >= 800 || r0 < 0) continue;
        // a cyclic correlation of a few rows over every shift
        for (int s = 0; s < W; s += 1) {
            double acc = 0;
            for (int pr : {probe / 4, probe / 2, probe}) {
                const uint8_t* d = &img.pix[(size_t)pr * W];
                const uint8_t* q = &ref.pix[(size_t)(r0 + pr) * W];
                for (int x = 0; x < W; x += 3) acc += (d[(x + s) % W] - 128.0) * (q[x] - 128.0);
            }
            if (acc > best) { best = acc; row0 = r0; shift = s; }
        }
    }
    FaxImage rolled = img;
    for (int r = 0; r < img.height; r++)
        for (int x = 0; x < W; x++) rolled.pix[(size_t)r * W + x] = img.pix[(size_t)r * W + (x + shift) % W];
    const int rows = std::min(img.height - 2, 800 - row0);
    FaxImage cut; cut.width = W; cut.height = rows;
    cut.pix.assign(ref.pix.begin() + (long)row0 * W, ref.pix.begin() + (long)(row0 + rows) * W);
    return faxt::compare(rolled, cut, 0);
}

void station(const char* name, double carrierKhz, int service, double mist, bool phasing) {
    const double dial = carrierKhz * 1e3 - 1900.0;
    // no start tone: 12 s into the phasing (5 s tone + 12 s), or 100 lines into the chart (5 + 30 + 0.5 + 50 s) a third of a line in
    const double skip = phasing ? 17.0 : 85.5 + 0.17;
    const double secs = phasing ? 18 + 70 : 75;
    const auto u = analytic(stationAudio(skip, secs, 50.0));
    Result r = receive(u, dial, service, mist, 15.0);
    if (phasing && r.img.height > 1) {      // the first row is the white line after the phasing
        r.img.pix.erase(r.img.pix.begin(), r.img.pix.begin() + r.img.width);
        r.img.height--;
    }
    int row0 = 0, shift = 0;
    const int guess = phasing ? 0 : 100;
    faxt::Quality q;
    if (r.img.width > 0 && r.img.height > 50) q = locate(r.img, guess, row0, shift);
    printf("%-34s state %d lpm %d lines %d phasing lines %d slant %+.1f ppm, black %.0f Hz; picture %dx%d from chart row %d shifted %d px: correlation %.3f drift %.2f px, service %d\n",
           name, r.st.state, r.st.lpm, r.st.lines, r.st.phasingLines, r.st.slantPpm, r.st.blackHz, r.img.width, r.img.height, row0, shift, q.meanCorr, q.drift, r.tel.serviceActive);
    CHECK(r.st.state == 2 && r.st.lpm == 120 && r.img.width == 1809, "%s: state %d lpm %d width %d", name, r.st.state, r.st.lpm, r.img.width);
    CHECK(r.st.lines >= (phasing ? 120 : 130), "%s: %d lines", name, r.st.lines);
    CHECK(std::fabs(r.st.slantPpm - 50.0) < 8.0, "%s: slant %+.1f ppm, the clock is 50 ppm off", name, r.st.slantPpm);
    CHECK(q.ok && q.meanCorr > 0.7, "%s: correlation %.3f", name, q.meanCorr);
    CHECK(std::fabs(q.drift) < 2.0, "%s: drift %.2f px (slant left over)", name, q.drift);
    CHECK(shift <= 3 || shift >= 1809 - 3, "%s: the line start is %d px off", name, shift);
    CHECK(r.tel.serviceActive == 3, "%s: service %d", name, r.tel.serviceActive);
}
} // namespace

int main() {
    station("DWD 7880 kHz, phasing, +50 Hz", 7880, 3, +50, true);
    station("DWD 4583 kHz, phasing, -50 Hz, auto", 4583, 0, -50, true);
    station("Northwood 8040 kHz, mid-picture, +50 Hz", 8040, 3, +50, false);
    station("DWD 7880 kHz, mid-picture, -50 Hz, auto", 7880, 0, -50, false);
    if (fails || faxt::fails) { printf("%d FAILED\n", fails + faxt::fails); return 1; }
    printf("ok\n");
    return 0;
}
