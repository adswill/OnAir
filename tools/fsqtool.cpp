// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSQ tool: write the test signal, or decode an IQ recording.
//   fsqtool gen <out.cf32> [--rate Hz] [--secs s] [--speed n] [--centre Hz] [--snr dB] [--cfo Hz] [--text "..."] [--call CALL] [--body "..."]
//   fsqtool rx <file.cf32> [--rate Hz] [--offset Hz] [--speed n]
// Speed index 0..4 = 1.5 2 3 4.5 6 baud. The files are raw complex float32. --offset is where the dial frequency sits in the file (negative: below its centre);
// the signal is looked for 100 .. 3900 Hz above the dial. rx prints the receiver's log, then the text.
#include "dect2/fsq/fsq_rx.h"
#include "dect2/fsq/kbd33_gen.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: fsqtool gen <out.cf32> [--rate Hz] [--secs s] [--speed n] [--centre Hz] [--snr dB] [--cfo Hz] [--text s]\n       fsqtool rx <file.cf32> [--rate Hz] [--offset Hz] [--speed n]\n"); return 2; }
    const std::string cmd = argv[1], path = argv[2];
    double rate = 250000, secs = 60, snr = 15, cfo = 0, centre = 1500, offsetHz = -20000;
    int speed = -1;
    std::string text, call = "LY2ABC", body = "LY2XYZ? snr please";
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--secs" && i + 1 < argc) secs = atof(argv[++i]);
        else if (a == "--snr" && i + 1 < argc) snr = atof(argv[++i]);
        else if (a == "--cfo" && i + 1 < argc) cfo = atof(argv[++i]);
        else if (a == "--centre" && i + 1 < argc) centre = atof(argv[++i]);
        else if (a == "--offset" && i + 1 < argc) offsetHz = atof(argv[++i]);
        else if (a == "--speed" && i + 1 < argc) speed = atoi(argv[++i]);
        else if (a == "--text" && i + 1 < argc) text = argv[++i];
        else if (a == "--call" && i + 1 < argc) call = argv[++i];
        else if (a == "--body" && i + 1 < argc) body = argv[++i];
    }
    if (cmd == "gen") {
        Kbd33GenConfig c;
        c.rate = rate; c.cfoHz = cfo; c.dialOffsetHz = offsetHz;
        Kbd33GenSignal s;
        s.ifkp = false; s.speed = speed < 0 ? 2 : speed; s.centreHz = centre; s.snrDb = snr;
        s.text = text.empty() ? fsqFrame(call, body, true) : text;
        c.signals.push_back(s);
        Kbd33Generator g(c);
        std::ofstream o(path, std::ios::binary);
        std::vector<cf32> buf(65536);
        for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) { g.generate(buf.data(), buf.size()); o.write((const char*)buf.data(), (long)(buf.size() * sizeof(cf32))); }
        return 0;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot read %s\n", path.c_str()); return 1; }
    FsqReceiver rx;
    rx.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    rx.configure(rate);
    rx.setSignalOffset(offsetHz);
    rx.setSpeed(speed);
    std::vector<cf32> blk(25000);
    while (f.read((char*)blk.data(), (long)(blk.size() * sizeof(cf32))) || f.gcount() > 0) {
        rx.feed(blk.data(), (size_t)f.gcount() / sizeof(cf32));
        if (f.gcount() < (long)(blk.size() * sizeof(cf32))) break;
    }
    FsqTelemetry t;
    rx.telemetry(t, 0);
    printf("%s\n", fsqSummary(t).c_str());
    printf("quality %.2f, SNR %.1f dB, AFC %+.2f Hz, %llu characters\n", t.quality, t.snrDb, t.cfoHz, (unsigned long long)t.chars);
    for (const auto& m : t.msgs) printf("message: from %s%s to %s trigger [%c] text \"%s\"\n", m.from.c_str(), m.crcOk ? " (crc ok)" : "", m.to.c_str(), m.trigger, m.text.c_str());
    printf("---\n%s\n", t.text.c_str());
    return 0;
}
