// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal identifier (sig_ident.h) on the test signals of the modes: DMR, NXDN96 / NXDN48, P25, dPMR, TETRA, AIS, POCSAG, LoRa, DAB (OFDM),
// FM broadcast, an AM carrier and plain noise, off the centre and with a carrier error; plus a capture through the Capture thread.
#include "dect2/ais/ais_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dab/dab_gen.h"
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dpmr/dpmr_gen.h"
#include "dect2/fm/fm_gen.h"
#include "dect2/mesh/mesh_lora.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/p25/p25_gen.h"
#include "dect2/pager/pager_gen.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/source/source.h"
#include "dect2/tetra/tetra_gen.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::sigid;
#include "../common/test_util.h"

static void shift(std::vector<cf32>& x, double fs, double hz) {
    double ph = 0; const double d = 2 * M_PI * hz / fs;
    for (auto& v : x) { v *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph = std::fmod(ph + d, 2 * M_PI); }
}

static Result run(const char* name, const std::vector<cf32>& x, double fs, double rfHz, const char* expect, int mod, double rate = 0,
                  int minSync = 0, Options o = Options()) {
    o.rfHz = rfHz;
    o.usePriors = false;   // the signal alone (the frequency database knows 868.1 MHz as the LoRaWAN channel; the prior has its own test in test_specx_idfeatures)
    const auto t0 = std::chrono::steady_clock::now();
    Result r = identify(x.data(), x.size(), fs, o);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const Features& f = r.f;
    printf("%-10s %s | kurt %.2f carrier %.0f dB lines %.0f/%.0f/%.0f/%.0f dB at %.0f/%.0f/%.0f/%.0f Hz | %.0f ms\n", name, r.summary.c_str(), f.envKurt,
           f.carrierDb, f.lineDb[0], f.lineDb[1], f.lineDb[2], f.lineDb[3], f.lineHz[0], f.lineHz[1], f.lineHz[2], f.lineHz[3], ms);
    for (size_t i = 0; i < r.cands.size() && i < 3; i++)
        printf("           %d. %s %.0f%s\n", (int)i + 1, r.cands[i].name.c_str(), r.cands[i].score,
               r.cands[i].syncTried ? (" sync " + std::to_string(r.cands[i].syncHits)).c_str() : "");
    CHECK(f.valid, "%s: not valid (%s)", name, f.why.c_str());
    CHECK(f.mod == mod, "%s: modulation %s, expected %s", name, modName(f.mod), modName(mod));
    if (rate > 0) CHECK(std::fabs(f.symRate / rate - 1) < 0.02, "%s: symbol rate %.1f, expected %.1f", name, f.symRate, rate);
    if (expect) {
        CHECK(!r.cands.empty() && r.cands[0].name == expect, "%s: top candidate %s, expected %s", name, r.cands.empty() ? "none" : r.cands[0].name.c_str(), expect);
        if (minSync && !r.cands.empty()) CHECK(r.cands[0].syncHits >= minSync, "%s: %d frame syncs", name, r.cands[0].syncHits);
    }
    return r;
}

static std::vector<cf32> drop(std::vector<cf32> x, double fs, double sec) { x.erase(x.begin(), x.begin() + (long)(fs * sec)); return x; }

int main() {
    // ---- DMR base station, 1.2 kHz carrier error, 120 kHz off the centre
    {
        const double fs = 1e6;
        DmrGenConfig c; c.rate = fs; c.cfoHz = 1200; c.snrDb = 30;
        DmrSignal g(c);
        std::vector<cf32> x((size_t)(fs * 1.0));
        g.generate(x.data(), x.size());
        shift(x, fs, 120e3);
        const Result r = run("DMR", x, fs, 446e6, "DMR", ModFsk4, 4800, 2);
        const double inner = (r.f.devHz[2] - r.f.devHz[1]) / 2, outer = (r.f.devHz[3] - r.f.devHz[0]) / 2;
        CHECK(std::fabs(inner / 648 - 1) < 0.2 && std::fabs(outer / 1944 - 1) < 0.2, "DMR levels +-%.0f / +-%.0f", inner, outer);
        CHECK(std::fabs(r.f.centreHz - 121.2e3) < 300, "DMR centre %.0f", r.f.centreHz);
    }
    // ---- DMR at weaker signals and carrier errors, and at 10 Msps (the explorer's rate) 2 MHz off the centre
    for (double snr : {14.0, 20.0})
        for (double cfo : {-2500.0, 2000.0}) {
            const double fs = 1e6;
            DmrGenConfig c; c.rate = fs; c.cfoHz = cfo; c.snrDb = snr; c.seed = 7;
            DmrSignal g(c);
            std::vector<cf32> x((size_t)(fs * 0.8));
            g.generate(x.data(), x.size());
            char nm[32]; snprintf(nm, sizeof nm, "DMR%.0f/%+.0f", snr, cfo);
            run(nm, x, fs, 0, "DMR", ModFsk4, 4800, 2);
        }
    {
        const double fs = 10e6;
        DmrGenConfig c; c.rate = fs; c.snrDb = 30;
        DmrSignal g(c);
        std::vector<cf32> x((size_t)(fs * 0.4));
        g.generate(x.data(), x.size());
        shift(x, fs, -2.1e6);
        Options o; o.loHz = -2.1e6 - 10e3; o.hiHz = -2.1e6 + 10e3;
        run("DMR10M", x, fs, 446e6, "DMR", ModFsk4, 4800, 2, o);
    }
    // ---- NXDN96 and NXDN48
    for (int baud : {4800, 2400}) {
        const double fs = 500e3;
        NxdnGenConfig c; c.rate = fs; c.snrDb = 30; c.cfoHz = -600;
        NxdnGenTx t; t.baud = baud; t.durSec = 4; c.txs.push_back(t);
        NxdnGenerator g(c);
        std::vector<cf32> x((size_t)(fs * 1.6));
        g.generate(x.data(), x.size());
        x = drop(x, fs, 0.45);
        const Result r = run(baud == 4800 ? "NXDN96" : "NXDN48", x, fs, 450e6, baud == 4800 ? "NXDN96" : "NXDN48", ModFsk4, baud, 2);
        const double outer = (r.f.devHz[3] - r.f.devHz[0]) / 2;
        CHECK(std::fabs(outer / (baud == 4800 ? 2400 : 1050) - 1) < 0.2, "NXDN%d outer +-%.0f", baud == 4800 ? 96 : 48, outer);
    }
    // ---- P25 (C4FM)
    {
        const double fs = 500e3;
        P25GenConfig c; c.rate = fs; c.snrDb = 30; c.dialOffsetHz = -60e3;
        P25GenTx t; t.durSec = 4; c.txs.push_back(t);
        P25Generator g(c);
        std::vector<cf32> x((size_t)(fs * 1.5));
        g.generate(x.data(), x.size());
        x = drop(x, fs, 0.4);
        run("P25", x, fs, 851e6, "P25 Phase 1", ModFsk4, 4800, 2);
    }
    // ---- dPMR
    {
        const double fs = 500e3;
        DpmrGenConfig c; c.rate = fs; c.snrDb = 30;
        DpmrGenTx t; t.durSec = 4; c.txs.push_back(t);
        DpmrGenerator g(c);
        std::vector<cf32> x((size_t)(fs * 1.6));
        g.generate(x.data(), x.size());
        x = drop(x, fs, 0.4);
        run("dPMR", x, fs, 446.1e6, "dPMR", ModFsk4, 2400, 2);
    }
    // ---- TETRA downlink: one cell 87.5 kHz below the centre
    {
        const double fs = 1e6;
        SynthConfig sc; sc.mode = 48; sc.snrDb = 30;
        auto g = makeTetraSynth(sc, fs);
        std::vector<cf32> x((size_t)(fs * 0.5));
        g->generate(x.data(), x.size());
        run("TETRA", x, fs, kTetraTestCentreHz, "TETRA", ModPi4Dqpsk, 18000);
    }
    // ---- TETRA with a carrier error
    for (double cfo : {-1800.0, 2600.0}) {
        const double fs = 1e6;
        SynthConfig sc; sc.mode = 48; sc.snrDb = 20; sc.cfoHz = cfo;
        auto g = makeTetraSynth(sc, fs);
        std::vector<cf32> x((size_t)(fs * 0.5));
        g->generate(x.data(), x.size());
        char nm[32]; snprintf(nm, sizeof nm, "TETRA%+.0f", cfo);
        run(nm, x, fs, kTetraTestCentreHz, "TETRA", ModPi4Dqpsk, 18000);
    }
    // ---- AIS bursts on channel A, about 30 per second
    {
        const double fs = 500e3;
        std::mt19937 rng(5);
        std::vector<AisBurstSpec> b;
        for (int i = 0; i < 30; i++) {
            AisBurstSpec s; s.channel = 'A'; s.startSec = 0.02 + i * 0.033;
            s.payload.resize(168);
            for (auto& v : s.payload) v = (uint8_t)(rng() & 1);
            b.push_back(s);
        }
        AisRenderConfig rc; rc.rate = fs; rc.durationSec = 1.0; rc.snrDb = 30;
        const auto x = renderAisBursts(b, rc);
        run("AIS", x, fs, 162e6, "AIS", ModGmsk, 9600, 2);
    }
    // ---- POCSAG 1200 only, 50 kHz below the centre
    {
        const double fs = 250e3;
        SynthConfig sc; sc.mode = 25; sc.snrDb = 30; sc.modeOpt[0] = 1; sc.modeOpt[2] = 2;
        auto g = makePagerSynth(sc, fs);
        std::vector<cf32> x((size_t)(fs * 3));
        g->generate(x.data(), x.size());
        Options o; o.loHz = -65e3; o.hiHz = -35e3;
        run("POCSAG", drop(x, fs, 0.5), fs, 466.075e6, "POCSAG", ModFsk2, 1200, 1, o);
    }
    // ---- LoRa SF9 125 kHz frames back to back
    {
        const double fs = 1e6;
        lora::Params p; p.sf = 9; p.bwHz = 125e3; p.preamble = 8;
        std::vector<cf32> x((size_t)(fs * 0.5), cf32(0, 0));
        std::mt19937 rng(3);
        double t = 0.01;
        while (t < 0.5) {
            std::vector<uint8_t> pay(24);
            for (auto& v : pay) v = (uint8_t)rng();
            lora::TxFrame f; f.p = p; f.data = lora::encode(p, pay.data(), pay.size()); f.startSec = t; f.freqHz = -150e3; f.amp = 0.3f;
            f.render(x.data(), x.size(), 0, fs);
            t = f.endSec() + 0.005;
        }
        dect2::normal_distribution<float> nd(0, 0.3f * std::pow(10.f, -25 / 20.f) * std::sqrt(fs / 125e3f / 2));
        for (auto& v : x) v += cf32{nd(rng), nd(rng)};
        const Result r = run("LoRa", x, fs, 868.1e6, "LoRa", ModChirp);
        CHECK(r.f.loraSf == 9, "LoRa SF %d", r.f.loraSf);
    }
    // ---- DAB (OFDM, Tu 1 ms)
    {
        const double fs = 2.048e6;
        SynthConfig sc; sc.dab = true; sc.snrDb = 25;
        auto g = makeDabSynth(sc, fs);
        std::vector<cf32> x((size_t)(fs * 0.3));
        g->generate(x.data(), x.size());
        const Result r = run("DAB", x, fs, 227.36e6, "DAB / DAB+", ModOfdm);
        CHECK(std::fabs(r.f.ofdmTuSec - 1e-3) < 2e-5 && std::fabs(r.f.ofdmTgSec - 246e-6) < 30e-6, "DAB Tu %.1f us Tg %.1f us", r.f.ofdmTuSec * 1e6, r.f.ofdmTgSec * 1e6);
    }
    // ---- FM broadcast with the stereo pilot
    {
        const double fs = 1e6;
        FmGenConfig c; c.rate = fs; c.cnrDb = 40;
        FmGenerator g(c);
        std::vector<cf32> x;
        g.generate((size_t)(fs * 0.4), x);
        const Result r = run("FM", x, fs, 98.5e6, "FM broadcast", ModFm);
        CHECK(r.f.fmPilot, "FM: no pilot");
    }
    // ---- AM: a carrier with 30-60 % voice-like modulation in an airband channel
    {
        const double fs = 250e3;
        std::vector<cf32> x((size_t)(fs * 0.5));
        std::mt19937 rng(9);
        dect2::normal_distribution<float> nd(0, 0.002f);
        for (size_t i = 0; i < x.size(); i++) {
            const double t = i / fs;
            const double a = 0.2 * (1 + 0.35 * std::sin(2 * M_PI * 700 * t) + 0.25 * std::sin(2 * M_PI * 1900 * t + 1));
            x[i] = cf32((float)(a * std::cos(2 * M_PI * 30e3 * t)), (float)(a * std::sin(2 * M_PI * 30e3 * t))) + cf32{nd(rng), nd(rng)};
        }
        run("AM", x, fs, 124.5e6, "Airband voice", ModAm);
    }
    // ---- noise only
    {
        std::vector<cf32> x(400000);
        std::mt19937 rng(1);
        dect2::normal_distribution<float> nd(0, 0.01f);
        for (auto& v : x) v = cf32{nd(rng), nd(rng)};
        const Result r = identify(x.data(), x.size(), 1e6);
        CHECK(!r.f.valid && r.f.mod == ModNoise, "noise taken for %s", modName(r.f.mod));
    }
    // ---- the capture thread: DMR pushed in pieces, the region given
    {
        const double fs = 1e6;
        DmrGenConfig c; c.rate = fs;
        DmrSignal g(c);
        Capture cap;
        cap.start(fs, 0, 20e3, 440e6, 0.8);
        std::vector<cf32> x(65536);
        while (cap.armed()) { g.generate(x.data(), x.size()); cap.push(x.data(), x.size()); }
        Result r; uint64_t seq = 0;
        for (int i = 0; i < 400 && !cap.result(r, seq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(25));
        CHECK(seq == 1 && !r.cands.empty() && r.cands[0].name == "DMR", "capture: %s", r.cands.empty() ? "nothing" : r.cands[0].name.c_str());
        CHECK(cap.state() == 3, "capture state %d", cap.state());
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("specx_ident: all passed\n");
    return 0;
}
