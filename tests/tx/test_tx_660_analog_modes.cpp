// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Issue #660: the analog modes (AM, WFM, USB, LSB, CW, DSB, RAW, C-QUAM) of the transmitter all came out as NFM. The General receiver's modulation is a row
// of its own ("general.mod": the first choice follows the receiver's demodulator, the others fix it), the AM broadcast entry's test signal is AM, and the
// analog voice path sends every modulation. Checked for every choice, with the test tone and with analog voice, at several radio rates. In memory: NOTHING
// is transmitted.
#include "dect2/engine/mode_tuning.h"
#include "dect2/general/general_rx.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_params.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<cf32> make(const char* id, const TxRequest& r, double rate, size_t n) {
    std::string err;
    std::vector<cf32> x;
    const TxModeSpec* m = txModeById(id);
    if (!m) { printf("no mode %s\n", id); return x; }
    auto g = makeTxGenerator(*m, r, rate, err);
    if (!g) { printf("no generator for %s at %.0f: %s\n", id, rate, err.c_str()); return x; }
    x.resize(n);
    g->generate(x.data(), n);
    return x;
}
static double powerAt(const std::vector<cf32>& x, double f, double fs, size_t skip) {
    std::complex<double> s = 0;
    for (size_t i = skip; i < x.size(); i++) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * f * (double)i / fs);
    return std::norm(s) / std::pow((double)(x.size() - skip), 2);
}
static double db(double p) { return 10 * std::log10(std::max(p, 1e-30)); }

struct Look { double spread, up, down, car, devRms; };
// off: the carrier's place in the baseband (the mode's tuning offset, negated); the audio tone is 1 kHz
static Look look(const std::vector<cf32>& y, double carrier, double fs, size_t skip) {
    Look l{};
    double lo = 1e9, hi = 0;
    for (size_t i = skip; i < y.size(); i++) { const double e = std::abs(y[i]); lo = std::min(lo, e); hi = std::max(hi, e); }
    l.spread = hi > 0 ? (hi - lo) / hi : 1;
    l.up = db(powerAt(y, carrier + 1000, fs, skip));
    l.down = db(powerAt(y, carrier - 1000, fs, skip));
    l.car = db(powerAt(y, carrier, fs, skip));
    // the instantaneous frequency around the carrier: its RMS deviation
    double sum = 0, sum2 = 0; size_t cnt = 0;
    for (size_t i = skip + 1; i < y.size(); i++) {
        const std::complex<double> d = std::complex<double>(y[i]) * std::conj(std::complex<double>(y[i - 1]));
        const double f = std::arg(d) * fs / (2 * M_PI) - carrier;
        sum += f; sum2 += f * f; cnt++;
    }
    const double mean = sum / (double)cnt;
    l.devRms = std::sqrt(std::max(0.0, sum2 / (double)cnt - mean * mean));
    return l;
}

// pick: GeneralDemod + 1 (row choice); the checks the modulation must pass
static void expect(const char* what, int demod, const Look& l) {
    switch (demod) {
    case GenNfm: CHECK(l.spread < 0.03 && l.devRms < 6000, "%s NFM: envelope %.3f, deviation %.0f Hz", what, l.spread, l.devRms); break;
    case GenWfm: CHECK(l.spread < 0.03 && l.devRms > 15000, "%s WFM: envelope %.3f, deviation %.0f Hz", what, l.spread, l.devRms); break;
    case GenAm: CHECK(l.spread > 0.3 && std::fabs(l.up - l.down) < 1.5 && l.car > l.up, "%s AM: envelope %.3f, sidebands %.1f / %.1f, carrier %.1f dB", what, l.spread, l.up, l.down, l.car); break;
    case GenUsb: CHECK(l.up > l.down + 30, "%s USB: %.1f / %.1f dB", what, l.up, l.down); break;
    case GenLsb: CHECK(l.down > l.up + 30, "%s LSB: %.1f / %.1f dB", what, l.up, l.down); break;
    case GenDsb: CHECK(l.spread > 0.5 && std::fabs(l.up - l.down) < 1.5 && l.car < l.up - 15, "%s DSB: envelope %.3f, sidebands %.1f / %.1f, carrier %.1f dB", what, l.spread, l.up, l.down, l.car); break;
    default: CHECK(l.spread < 0.03 && l.devRms < 1000 && l.up < l.car - 30 && l.down < l.car - 30, "%s CW/RAW: envelope %.3f, deviation %.0f Hz, carrier %.1f, sidebands %.1f / %.1f dB", what, l.spread, l.devRms, l.car, l.up, l.down); break;
    }
}

int main() {
    // the AM broadcast entry's test signal is AM
    const SubMode* am = subModeById("am");
    CHECK(am != nullptr, "the AM broadcast entry exists");
    if (am) CHECK(am->synth[0] == GenAm, "AM broadcast: its generator modulation is %d, not AM (%d)", am->synth[0], (int)GenAm);

    // the row: ten choices, the first follows the receiver, the others set synth.modeOpt[0] to the demodulator number
    const TxModeSpec* gm = txModeById("general");
    CHECK(gm != nullptr, "mode general");
    if (!gm) { printf("fatal\n"); return 1; }
    TxParamCtx ctx;
    const auto& rows = txParams(*gm, ctx);
    const TxParamDef* row = txParamFind(rows, "general.mod");
    CHECK(row != nullptr, "the General receiver has a Modulation row");
    if (row) {
        for (int pick = 0; pick <= (int)GenDemodCount; pick++) {
            TxRequest r; r.synth.modeOpt[0] = 77;
            txParamApply(*row, pick, "", r);
            if (pick == 0) CHECK(r.synth.modeOpt[0] == 77 && r.o("general.mod", -1) == 0, "choice 0 follows the receiver and leaves the generator alone");
            else CHECK(r.synth.modeOpt[0] == pick - 1, "choice %d sets the generator's modulation to %d (got %d)", pick, pick - 1, r.synth.modeOpt[0]);
        }
    }

    // every modulation, with the test tone and with analog voice, at several radio rates
    const double kRates[] = {2e6, 2.4e6, 4e6};
    for (double fs : kRates) {
        const size_t n = (size_t)(fs * 0.12), skip = (size_t)(fs * 0.04);
        const double carrier = -txModeTuneOffsetHz(*gm);
        for (int demod = 0; demod < (int)GenDemodCount; demod++) {
            for (int voice = 0; voice < 2; voice++) {
                TxRequest r; r.modeId = "general"; r.analogVoice = voice == 1; r.toneHz = 1000; r.audioSrc = 2;
                r.opt["audio.hpHz"] = 0; r.opt["audio.lpHz"] = 0;
                r.synth.modeOpt[0] = demod;
                char what[64]; snprintf(what, sizeof what, "general %s at %.1f Msps demod %d", voice ? "voice" : "tone", fs / 1e6, demod);
                auto y = make("general", r, fs, n);
                CHECK(!y.empty(), "%s: a generator", what);
                if (y.empty()) continue;
                expect(what, demod, look(y, carrier, fs, skip));
            }
        }
    }

    // AM stereo (C-QUAM) is AM, not NFM
    if (const TxModeSpec* cq = txModeById("cquam")) {
        const double fs = 2e6, carrier = -txModeTuneOffsetHz(*cq);
        TxRequest r; r.modeId = "cquam"; r.toneHz = 1000; r.audioSrc = 2;
        auto y = make("cquam", r, fs, (size_t)(fs * 0.3));
        CHECK(!y.empty(), "cquam generator");
        if (!y.empty()) {
            const Look l = look(y, carrier, fs, (size_t)(fs * 0.1));
            CHECK(l.spread > 0.05, "C-QUAM is amplitude modulated: envelope %.3f", l.spread);
        }
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
