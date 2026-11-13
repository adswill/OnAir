// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRa (raw) monitor: the test signal through the receiver (loraraw_gen.h, loraraw_rx.h): the whole schedule on Auto (SF7 - SF12, 125 / 250 / 500 kHz,
// 4/5 - 4/8, CRC on and off, payload as hex and text, SNR / level / carrier offset), carrier offsets, a channel off the centre, the transmitter's
// clock error, noise, implicit header, fixed settings, a wrong sync word, noise alone, and the results file.
#include "dect2/loraraw/loraraw_gen.h"
#include "dect2/loraraw/loraraw_rx.h"
#include "dect2/loraraw/loraraw_tel.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Run {
    LoraRawTelemetry t;
};

static LoraRawTelemetry run(LoraRawGenConfig c, const LoraRawSettings& s, double secs, double chanOffsetHz = 0) {
    c.dialOffsetHz = chanOffsetHz;
    LoraRawGenerator g(c);
    LoraRawReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(0);
    LoraRawSettings ss = s;
    ss.offsetHz = chanOffsetHz;
    r.setSettings(ss);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)((secs + 0.4) * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    LoraRawTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const LrFrame* findText(const LoraRawTelemetry& t, const std::string& text) {
    for (const auto& f : t.log) if (f.ascii.find(text) != std::string::npos) return &f;
    return nullptr;
}

static double scheduleSec(const std::vector<LoraRawGenFrame>& v) {
    double s = 0;
    for (const auto& f : v) s += lora::airSeconds(f.p, f.payload.size()) + f.gapSec;
    return s;
}

// Measurement only: frames decoded against SNR (in 125 kHz) for SF7 - SF12, around Semtech's demodulator limits (SF7 -7.5 dB ... SF12 -20 dB).
// test_loraraw_rx sweep [frames] [rate]
static void sweep(int n, double rate) {
    printf("SF | limit | auto: L+4 L+2 L L-2 | fixed SF: L+4 L+2 L L-2 (percent of %d frames, CRC good)\n", n);
    for (int sf = 7; sf <= 12; sf++) {
        const double L = -7.5 - 2.5 * (sf - 7);
        printf("%2d | %5.1f |", sf, L);
        for (int fixed = 0; fixed < 2; fixed++) {
            for (double d : {4.0, 2.0, 0.0, -2.0}) {
                LoraRawGenConfig c;
                c.rate = rate; c.snrDb = L + d; c.loop = false; c.seed = (uint32_t)(sf * 31 + (int)(d * 7) + 100);
                uint32_t x = c.seed * 2654435761u + 1;
                for (int i = 0; i < n; i++) {
                    LoraRawGenFrame f;
                    f.p.sf = sf; f.p.bwHz = 125e3; f.p.cr = 5; f.p.preamble = 8; f.p.syncWord = 0x12; f.p.crc = true;
                    f.p.ldro = sf >= 11;   // RadioLib: symbols of 16 ms or more
                    f.payload.resize(16);
                    for (auto& b : f.payload) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = (uint8_t)x; }
                    f.gapSec = 0.05 + (double)(1 << sf) / 125e3 * 4;
                    f.cfoHz = (double)((int)(x % 4001) - 2000);
                    c.frames.push_back(f);
                }
                LoraRawSettings st;
                if (fixed) { st.autoSf = false; st.sf = sf; st.autoBw = false; st.bwKhz = 125; }
                const LoraRawTelemetry t = run(c, st, scheduleSec(c.frames) + 0.2);
                printf(" %5.1f", 100.0 * (double)t.blocksOk / n);
                fflush(stdout);
            }
            printf(" |");
        }
        printf("\n");
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "sweep") { sweep(argc > 2 ? atoi(argv[2]) : 30, argc > 3 ? atof(argv[3]) : 500e3); return 0; }
    LoraRawSettings autoSet;   // Auto SF, Auto BW, sync 0x12
    {   // the whole scene, clean
        LoraRawGenConfig c;
        c.snrDb = 30; c.loop = false;
        c.frames = loraRawTestSchedule();
        const LoraRawTelemetry t = run(c, autoSet, scheduleSec(c.frames) + 0.2);
        printf("  scene: %s, %zu in the log\n", loraRawSummary(t).c_str(), t.log.size());
        CHECK(t.blocksOk >= 8 && t.blocksBad == 0, "%llu good, %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        struct W { const char* text; int sf; double bw; int cr; bool crc; };
        const W want[] = {{"OnAir LoRa raw SF7 125k", 7, 125e3, 5, true}, {"cr4/6", 8, 125e3, 6, true}, {"OnAir raw SF9 250k", 9, 250e3, 5, true},
                          {"no crc", 10, 500e3, 8, false}, {"SF7 500k cr4/7", 7, 500e3, 7, true}, {"\"t\":21.5", 11, 125e3, 5, true},
                          {"Hello from SF12", 12, 125e3, 5, true}, {"node=7 bat=3.71V", 9, 125e3, 5, true}, {"binary", 8, 250e3, 6, true}};
        for (const W& w : want) {
            const LrFrame* f = findText(t, w.text);
            CHECK(f != nullptr, "'%s' not decoded", w.text);
            if (!f) continue;
            CHECK(f->sf == w.sf && std::fabs(f->bwHz - w.bw) < 1 && f->cr == w.cr && f->hasCrc == w.crc && (f->crcOk || !w.crc), "'%s': SF%d %.0f 4/%d crc %d ok %d", w.text, f->sf, f->bwHz, f->cr - 4, f->hasCrc, f->crcOk);
            CHECK(!f->hex.empty() && f->hex.size() >= f->payload.size() * 3 - 1 && f->snrDb > 10 && f->levelDb < 0 && f->levelDb > -80, "'%s': hex %zu snr %.1f level %.1f", w.text, f->hex.size(), f->snrDb, f->levelDb);
        }
        const LrFrame* f = findText(t, "OnAir LoRa raw SF7 125k");
        if (f) {
            CHECK(std::fabs(f->cfoHz - 1500) < 150, "carrier offset %.0f Hz (1500 sent)", f->cfoHz);
            CHECK(f->hex.substr(0, 8) == "4F 6E 41", "hex '%s'", f->hex.c_str());
        }
        f = findText(t, "Hello from SF12");
        CHECK(f && std::fabs(f->cfoHz + 900) < 150, "SF12 carrier offset %.0f Hz (-900 sent)", f ? f->cfoHz : 0);
        CHECK(t.perSf[7] >= 2 && t.perSf[12] >= 1 && t.perBw[0] >= 1 && t.perBw[1] >= 1 && t.perBw[2] >= 1, "per SF / BW counters");
    }
    {   // noise: the 125 kHz frames at 0 dB SNR (SF7 needs about -7.5 dB, so it must still work), clock error 20 ppm, carrier offset 5 kHz
        LoraRawGenConfig c;
        c.snrDb = 0; c.loop = false; c.sroPpm = 20; c.cfoHz = 3500;
        c.frames = loraRawTestSchedule();
        const LoraRawTelemetry t = run(c, autoSet, scheduleSec(c.frames) + 0.2);
        printf("  0 dB, 20 ppm, +3.5 kHz: %llu good, %llu bad\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= 5, "only %llu frames decoded at 0 dB", (unsigned long long)t.blocksOk);
        CHECK(findText(t, "Hello from SF12") && findText(t, "OnAir LoRa raw SF7 125k"), "SF12 / SF7 frames lost");
    }
    {   // the channel off the centre of the capture (433.92 MHz LoRa, 300 kHz away from the dial), receiver told the offset
        LoraRawGenConfig c;
        c.snrDb = 25; c.loop = false;
        c.frames = loraRawTestSchedule();
        const LoraRawTelemetry t = run(c, autoSet, scheduleSec(c.frames) + 0.2, 300e3);
        CHECK(t.blocksOk >= 8 && t.blocksBad == 0, "off centre: %llu good", (unsigned long long)t.blocksOk);
    }
    {   // fixed settings: only SF9 / 250 kHz is searched, the others are ignored
        LoraRawGenConfig c;
        c.snrDb = 30; c.loop = false;
        c.frames = loraRawTestSchedule();
        LoraRawSettings s;
        s.autoSf = false; s.sf = 9; s.autoBw = false; s.bwKhz = 250;
        const LoraRawTelemetry t = run(c, s, scheduleSec(c.frames) + 0.2);
        CHECK(findText(t, "OnAir raw SF9 250k") && !findText(t, "SF12") && !findText(t, "SF7 125k"), "fixed SF9 / 250 kHz: %zu frames", t.log.size());
        CHECK(t.blocksOk == 1, "%llu frames", (unsigned long long)t.blocksOk);
    }
    {   // implicit header: SF8, 250 kHz, 4/6, 12 bytes, CRC (agreed beforehand); with the wrong length nothing good comes out
        LoraRawGenConfig c;
        c.snrDb = 30; c.loop = false;
        c.frames = loraRawImplicitSchedule();
        LoraRawSettings s;
        s.autoSf = false; s.sf = 8; s.autoBw = false; s.bwKhz = 250; s.implicit = true; s.cr = 6; s.crc = true; s.length = 12;
        const LoraRawTelemetry t = run(c, s, scheduleSec(c.frames) + 0.2);
        printf("  implicit: %llu good, %llu bad\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk == 3 && t.blocksBad == 0, "implicit header: %llu good", (unsigned long long)t.blocksOk);
        CHECK(findText(t, "implicit #00") && t.log.size() && t.log[0].implicit && t.log[0].len == 12, "implicit frame text");
        s.length = 9;
        const LoraRawTelemetry w = run(c, s, scheduleSec(c.frames) + 0.2);
        CHECK(w.blocksOk == 0, "wrong implicit length gave %llu good frames", (unsigned long long)w.blocksOk);
    }
    {   // a wrong sync word: a 0x34 (LoRaWAN) network is not heard when listening for 0x12, but is on "any"
        LoraRawGenConfig c;
        c.snrDb = 30; c.loop = false;
        c.frames = loraRawTestSchedule(0x34);
        const LoraRawTelemetry t = run(c, autoSet, scheduleSec(c.frames) + 0.2);
        CHECK(t.blocksOk == 0, "sync 0x34 heard on 0x12: %llu", (unsigned long long)t.blocksOk);
        LoraRawSettings s;
        s.sync = -1;
        const LoraRawTelemetry a = run(c, s, scheduleSec(c.frames) + 0.2);
        CHECK(a.blocksOk >= 8, "sync any: %llu good", (unsigned long long)a.blocksOk);
        const LrFrame* f = a.log.empty() ? nullptr : &a.log[0];
        CHECK(f && f->sync == 0x34, "sync word reported %02X", f ? f->sync : 0);
    }
    {   // noise alone: nothing
        LoraRawGenConfig c;
        c.snrDb = 5; c.loop = false;
        const LoraRawTelemetry t = run(c, autoSet, 3.0);
        CHECK(t.blocksOk == 0 && t.log.empty(), "noise alone gave %llu frames", (unsigned long long)t.blocksOk);
    }
    {   // the results file: save, load into a new receiver
        LoraRawGenConfig c;
        c.snrDb = 30; c.loop = false;
        c.frames = loraRawTestSchedule();
        LoraRawGenerator g(c);
        LoraRawReceiver r;
        r.configure(c.rate);
        r.setSettings(autoSet);
        std::vector<cf32> buf(16384);
        for (size_t done = 0; done < (size_t)((scheduleSec(c.frames) + 0.6) * c.rate); done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
        const std::string blob = r.saveResults();
        LoraRawReceiver r2;
        r2.configure(c.rate);
        CHECK(!blob.empty() && r2.loadResults(blob) && !r2.loadResults("junk"), "results file");
    }
    printf(fails ? "loraraw rx: %d FAILED\n" : "loraraw rx: all passed\n", fails);
    return fails ? 1 : 0;
}
