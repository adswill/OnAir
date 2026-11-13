// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench on the command line, no screen: make a test recording with known bits, measure one, demodulate one.
//   wbtool gen <out.cf32|out.wav|out.sigmf> --kind fsk2|fsk4|ask|afsk|bpsk|qpsk|psk8|msk|gmsk|am|fm --rate R --baud B [--bits N] [--snr dB]
//                [--offset Hz] [--lead s] [--tail s] [--diff] [--bitsfile f.txt]
//   wbtool analyze <file> [--rate R] [--format cs8|cu8|cf32|ci16|cu16] [--stereo-iq] [--band f0 f1] [--t0 s] [--t1 s]
//   wbtool demod <file> --kind K --baud B [--band f0 f1] [--t0 s] [--t1 s] [--bits f.txt] [--audio f.wav] [--diff] [other analyze options]
#include "dect2/wb/wb_demod.h"
#include "dect2/wb/wb_export.h"
#include "dect2/wb/wb_gen.h"
#include "dect2/wb/wb_measure.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace dect2;
using namespace dect2::wb;

static int kindOf(const std::string& n) {
    static const char* names[] = {"am", "fm", "usb", "lsb", "cw", "ask", "fsk2", "fsk4", "afsk", "bpsk", "qpsk", "psk8", "msk", "gmsk"};
    for (int i = 0; i < KindCount; i++) if (n == names[i]) return i;
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: wbtool gen|analyze|demod <file> [options]; see the top of tools/wbtool.cpp\n"); return 2; }
    const std::string cmd = argv[1], file = argv[2];
    GenSpec g;
    DemodParams dp;
    LoadOptions lo;
    double t0 = 0, t1 = 1e18, f0 = 0, f1 = 0;
    std::string bitsFile, audioFile;
    int kind = -1;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (const char*)"0"; };
        if (a == "--kind") kind = kindOf(next());
        else if (a == "--rate") { g.rate = atof(next()); lo.sampleRate = g.rate; }
        else if (a == "--baud") { g.baud = atof(next()); dp.baud = g.baud; }
        else if (a == "--bits") g.nbits = (size_t)atol(next());
        else if (a == "--snr") g.snrDb = atof(next());
        else if (a == "--offset") g.offsetHz = atof(next());
        else if (a == "--lead") g.leadSec = atof(next());
        else if (a == "--tail") g.tailSec = atof(next());
        else if (a == "--diff") { g.differential = true; dp.differential = true; }
        else if (a == "--bitsfile" || a == "--bits-out") bitsFile = next();
        else if (a == "--format") { const std::string f = next(); lo.format = f == "cs8" ? 0 : f == "cu8" ? 1 : f == "cf32" ? 2 : f == "ci16" ? 3 : 4; }
        else if (a == "--stereo-iq") lo.stereoIq = true;
        else if (a == "--center") lo.centerHz = atof(next());
        else if (a == "--band") { f0 = atof(next()); f1 = atof(next()); }
        else if (a == "--t0") t0 = atof(next());
        else if (a == "--t1") t1 = atof(next());
        else if (a == "--audio") audioFile = next();
        else if (a == "--noclock") dp.clock = 0;
        else if (a == "--nocarrier") dp.carrierRecovery = false;
        else if (a == "--rolloff") dp.rolloff = g.rolloff = atof(next());
        else if (a == "--mark") dp.markHz = g.markHz = atof(next());
        else if (a == "--space") dp.spaceHz = g.spaceHz = atof(next());
        else if (a == "--fm-first") { dp.fmFirst = true; g.afskOnFm = true; }
    }
    std::string err;
    if (cmd == "gen") {
        if (kind < 0) { fprintf(stderr, "gen needs --kind\n"); return 2; }
        g.kind = kind;
        GenResult r = generate(g);
        auto s = Source::fromSamples(r.x, r.rate, r.real, 0, "generated");
        int fmt = ExCf32;
        if (file.size() > 4 && file.substr(file.size() - 4) == ".wav") fmt = ExWav;
        else if (file.find("sigmf") != std::string::npos) fmt = ExSigmf;
        if (!exportSelection(*s, 0, s->length(), 0, 0, file, fmt, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
        if (!bitsFile.empty()) writeBitsText(bitsFile, r.bits, err);
        printf("wrote %zu samples at %.0f Hz (%zu bits)\n", r.x.size(), r.rate, r.bits.size());
        return 0;
    }
    auto src = Source::open(file, lo, err);
    if (!src) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    printf("%s: %s\n", src->name().c_str(), src->description().c_str());
    const uint64_t a = (uint64_t)(t0 * src->rate()), b = (uint64_t)std::min(t1 * src->rate(), (double)src->length());
    if (cmd == "analyze") {
        Measurement m;
        MeasureOptions o;
        if (!measure(*src, a, b, f0, f1, o, m)) { fprintf(stderr, "%s\n", m.why.c_str()); return 1; }
        printf("band %.1f .. %.1f Hz, floor %.1f dB/bin, peak %.1f dB, power %.1f dBFS, SNR %.1f dB\n", m.f0, m.f1, m.noiseFloorDb, m.peakDb, m.bandPowerDb, m.snrDb);
        printf("bandwidth: -3 dB %.1f Hz, -20 dB %.1f Hz%s, 99%% %.1f Hz (%.1f .. %.1f), centre %.1f Hz, carrier %.2f Hz (offset %+.2f)\n", m.bw3Hz, m.bw20Hz, m.bw20Limited ? " (noise limited)" : "",
               m.obwHz, m.obwLoHz, m.obwHiHz, m.centreHz, m.carrierHz, m.carrierOffsetHz);
        for (const Peak& p : m.peaks) printf("  peak %12.2f Hz  %7.2f dB  width %9.2f Hz\n", p.freqHz, p.levelDb, p.widthHz);
        if (m.sym.baud > 0) printf("symbol rate %.2f Bd (%s, %.1f dB)\n", m.sym.baud, m.sym.method.c_str(), m.sym.strengthDb);
        if (m.identValid) printf("identifier: %s\n", m.ident.summary.c_str());
        printf("duty %.2f, %zu burst%s\n", m.duty, m.bursts.size(), m.bursts.size() == 1 ? "" : "s");
        for (const Burst& x : m.bursts) printf("  burst at %.4f s, %.4f s long, peak %.1f dB\n", x.startSec, x.lenSec, x.peakDb);
        return 0;
    }
    if (cmd == "demod") {
        if (kind < 0) { fprintf(stderr, "demod needs --kind\n"); return 2; }
        dp.kind = kind;
        if (f1 > f0) { dp.centreHz = 0.5 * (f0 + f1); dp.bwHz = f1 - f0; }
        DemodResult r = demodulate(*src, a, b, dp);
        if (!r.ok) { fprintf(stderr, "demodulation failed: %s\n", r.msg.c_str()); return 1; }
        printf("baseband %.0f Hz, %zu symbols at %.2f Bd, %d bit/symbol, SNR %.1f dB, offset %.1f Hz, deviation %.1f Hz\n", r.rate, r.symbols, r.baud, r.bitsPerSymbol, r.snrDb, r.cfoHz, r.devHz);
        printf("%s\n", bitsToString(r.bits, 0, 128).c_str());
        if (!bitsFile.empty()) writeBitsText(bitsFile, r.bits, err);
        if (!audioFile.empty() && !r.audio.empty()) writeWavMono16(audioFile, r.audio, 48000, err);
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
