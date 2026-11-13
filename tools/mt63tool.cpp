// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MT63 tool: write the test signal, or decode an IQ recording.
//   mt63tool gen <out.cf32> [--rate Hz] [--secs s] [--bw 500|1000|2000] [--long] [--centre Hz] [--snr dB] [--cfo Hz] [--text "..."]
//   mt63tool rx <file.cf32> [--rate Hz] [--offset Hz] [--bw 500|1000|2000] [--intl 1|2]
// The files are raw complex float32. --offset is where the dial frequency sits in the file (negative: below its centre); the signal is looked for
// 100 .. 3900 Hz above the dial. rx prints the receiver's log, then the text.
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_rx.h"
#include "dect2/mt63/mt63_tel.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mt63tool gen <out.cf32> [--rate Hz] [--secs s] [--bw 500|1000|2000] [--long] [--centre Hz] [--snr dB] [--cfo Hz] [--text s]\n       mt63tool rx <file.cf32> [--rate Hz] [--offset Hz] [--bw Hz] [--intl 1|2]\n"); return 2; }
    const std::string cmd = argv[1], path = argv[2];
    double rate = 250000, secs = 60, snr = 15, cfo = 0, centre = 1500, offsetHz = -20000;
    int bw = 1000, intl = 0, forceBw = 0;
    bool lng = false;
    std::string text = "CQ CQ CQ DE LY2ABC LY2ABC PSE K\n";
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--secs" && i + 1 < argc) secs = atof(argv[++i]);
        else if (a == "--snr" && i + 1 < argc) snr = atof(argv[++i]);
        else if (a == "--cfo" && i + 1 < argc) cfo = atof(argv[++i]);
        else if (a == "--centre" && i + 1 < argc) centre = atof(argv[++i]);
        else if (a == "--offset" && i + 1 < argc) offsetHz = atof(argv[++i]);
        else if (a == "--bw" && i + 1 < argc) { bw = atoi(argv[++i]); forceBw = cmd == "rx" ? bw : 0; }
        else if (a == "--intl" && i + 1 < argc) intl = atoi(argv[++i]);
        else if (a == "--text" && i + 1 < argc) text = argv[++i];
        else if (a == "--long") lng = true;
    }
    if (cmd == "gen") {
        Mt63GenConfig c;
        c.rate = rate; c.cfoHz = cfo; c.dialOffsetHz = offsetHz;
        Mt63GenSignal s;
        s.bandwidth = bw; s.longInterleave = lng; s.centreHz = centre; s.snrDb = snr; s.text = text;
        c.signals.push_back(s);
        Mt63Generator g(c);
        std::ofstream o(path, std::ios::binary);
        std::vector<cf32> buf(65536);
        for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { g.generate(buf.data(), buf.size()); o.write((const char*)buf.data(), (long)(buf.size() * sizeof(cf32))); }
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
    Mt63Receiver rx;
    rx.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    rx.configure(rate);
    rx.setSignalOffset(offsetHz);
    rx.setBandwidth(forceBw);
    rx.setInterleave(intl);
    std::vector<cf32> blk(25000);
    while (f.read((char*)blk.data(), (long)(blk.size() * sizeof(cf32))) || f.gcount() > 0) {
        rx.feed(blk.data(), (size_t)f.gcount() / sizeof(cf32));
        if (f.gcount() < (long)(blk.size() * sizeof(cf32))) break;
    }
    Mt63Telemetry t;
    rx.telemetry(t, 0);
    printf("%s\n", mt63Summary(t).c_str());
    printf("quality %.1f (short %.1f, long %.1f), SNR %.1f dB, AFC %+.2f Hz, %llu characters\n---\n%s\n", t.quality, t.qualityShort, t.qualityLong, t.snrDb, t.cfoHz, (unsigned long long)t.chars, t.text.c_str());
    return 0;
}
