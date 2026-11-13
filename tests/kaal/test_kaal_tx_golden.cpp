// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL transmitter against the Python kaal_tx.py: a fixed 1 s tone (tests/kaal/gen_golden_tx.py) gives the same IQ, streaming in any chunk
// size gives identical samples, and a different output rate keeps length and power.
#include "dect2/kaal/kaal_tx.h"
#include "kaal_golden_tx.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>
using namespace dect2::kaal;
#include "../common/test_util.h"

static std::vector<int16_t> tone() {
    std::vector<int16_t> x(16000);
    for (int i = 0; i < 16000; i++) x[i] = (int16_t)(8000.0 * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 16000.0));
    return x;
}
static std::vector<std::complex<float>> run(double rate, size_t chunk, const std::vector<int16_t>& x, size_t popChunk = 5000) {
    VoiceTx tx;
    tx.configure(rate);
    std::vector<std::complex<float>> all, buf(popChunk);
    auto drain = [&] { for (size_t n; (n = tx.popIq(buf.data(), buf.size())) > 0;) all.insert(all.end(), buf.begin(), buf.begin() + n); };
    for (size_t i = 0; i < x.size(); i += chunk) { tx.pushAudio(&x[i], std::min(chunk, x.size() - i)); drain(); }
    tx.flush();
    drain();
    return all;
}

int main() {
    const auto x = tone();
    const auto y = run(48000, 777, x);
    CHECK(y.size() == G_TX_LEN, "length %zu, Python %zu", y.size(), G_TX_LEN);
    // The sweep is relative to the fixed data power here and to the measured one in the Python (3 % apart for this tone).
    const double tol = 0.004;
    double worst = 0;
    for (int i = 0; i < 8 && (size_t)i < y.size(); i++) {
        const double dr = y[i].real() - kTxGain * G_TX_HEAD[i][0], di = y[i].imag() - kTxGain * G_TX_HEAD[i][1];
        worst = std::max(worst, std::max(std::fabs(dr), std::fabs(di)));
    }
    CHECK(worst < tol, "first 8 samples differ by %.5f", worst);
    double worstL = 0;
    for (size_t k = 0; k < sizeof G_TX_IDX / sizeof G_TX_IDX[0]; k++) {
        if (G_TX_IDX[k] >= y.size()) continue;
        const double dr = y[G_TX_IDX[k]].real() - kTxGain * G_TX_LATER[k][0], di = y[G_TX_IDX[k]].imag() - kTxGain * G_TX_LATER[k][1];
        worstL = std::max(worstL, std::max(std::fabs(dr), std::fabs(di)));
    }
    CHECK(worstL < tol, "later samples differ by %.5f", worstL);
    printf("worst difference: head %.5f, later %.5f\n", worst, worstL);

    double peak = 0, pw = 0;
    for (const auto& s : y) { peak = std::max(peak, (double)std::max(std::fabs(s.real()), std::fabs(s.imag()))); pw += std::norm(s); }
    pw /= y.size();
    CHECK(peak > 0.25 && peak < 0.8, "peak %.3f", peak);

    // chunking must not change a single sample
    const auto y2 = run(48000, 16000, x, 70000);
    const auto y3 = run(48000, 1, std::vector<int16_t>(x.begin(), x.begin() + 3200), 7);
    const auto y4 = run(48000, 3200, std::vector<int16_t>(x.begin(), x.begin() + 3200));
    bool same = y2.size() == y.size();
    for (size_t i = 0; same && i < y.size(); i++) same = y2[i] == y[i];
    CHECK(same, "chunk size changes the output");
    CHECK(y3 == y4 && !y3.empty(), "one-sample pushes differ");

    // 24 kHz output: half the length (the filter ringing adds a few samples), same mean power
    const auto z = run(24000, 777, x);
    CHECK(z.size() > y.size() / 2 - 40 && z.size() < y.size() / 2 + 60, "24 kHz length %zu", z.size());
    double pz = 0;
    for (const auto& s : z) pz += std::norm(s);
    pz /= z.size();
    CHECK(std::fabs(pz / pw - 1) < 0.06, "24 kHz power ratio %.3f", pz / pw);

    // nothing pushed: nothing out; a flush with no audio makes nothing either
    VoiceTx tx;
    tx.configure(48000);
    std::complex<float> b[10];
    CHECK(tx.popIq(b, 10) == 0, "output without input");
    tx.flush();
    CHECK(tx.popIq(b, 10) == 0, "output from an empty flush");
    return testSummary("kaal_tx_golden");
}
