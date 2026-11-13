// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital HamDRM: the test JPEG in mode B (16-QAM, 2.5 kHz) at 20 dB (signal to noise in 3 kHz) with a +-20 Hz tuning error decodes byte
// for byte with its name and callsign; segments left out of a transmission are reported missing and filled in by the repeat; the other
// modes and constellations; noise alone gives nothing; and the whole HF digital receiver on the I/Q test signal.
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_hamdrm.h"
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/engine/modes.h"
#include "dect2/source/source.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Noise {
    uint64_t s = 777;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

struct Run {
    HfdigHamdrmTelemetry t;
    std::vector<std::string> log;
    double sec = 0;
};

// feeds the transmitter's audio (or silence when tx is null) with noise to a decoder until `until` says stop or the time is up
template <class F>
static Run run(HfdigTestAudio* tx, double snrDb, double maxSec, F until) {
    Run r;
    auto dec = makeHamdrmDecoder();
    dec->setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    Noise nz;
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) * 4000.0 / 3000.0);
    std::vector<float> buf(800);
    for (int blk = 0; blk < (int)(maxSec * 10); blk++) {
        if (tx) tx->generate(buf.data(), buf.size()); else std::fill(buf.begin(), buf.end(), 0.f);
        for (float& v : buf) v += (float)(sigma * nz.g());
        dec->feedAudio(buf.data(), buf.size());
        r.sec = (blk + 1) * 0.1;
        if (blk % 5 == 4) { dec->telemetry(r.t); if (until(r.t)) break; }
    }
    dec->telemetry(r.t);
    return r;
}

static bool sameFile(const HfdigHamdrmTelemetry& t, const std::vector<uint8_t>& want) {
    return !t.history.empty() && t.history[0]->data == want;
}

int main() {
    const std::vector<uint8_t>& pic = hamdrmTestPicture();
    CHECK(pic.size() > 1000 && pic[0] == 0xFF && pic[1] == 0xD8, "test picture is not a JPEG (%zu bytes)", pic.size());
    {   // the mode B JPEG at 20 dB, +-20 Hz
        for (double off : {+20.0, -20.0}) {
            HamdrmTxConfig c;
            c.offsetHz = off;
            auto tx = makeHamdrmTx(c, pic);
            const Run r = run(tx.get(), 20, 40, [](const HfdigHamdrmTelemetry& t) { return t.filesDone >= 1; });
            const auto& t = r.t;
            printf("mode B %+.0f Hz: %s in %.1f s, mode %s %d-QAM, SNR %.1f dB, offset %+.1f Hz, frames %llu ok %llu bad, %d segments\n", off,
                   t.filesDone ? "done" : "NOT done", r.sec, t.mode >= 0 ? hamdrmModeName(t.mode) : "-", t.qam >= 0 ? hamdrmQam(t.qam) : 0, t.snrDb, t.offsetHz,
                   (unsigned long long)t.framesOk, (unsigned long long)t.framesBad, t.segTotal);
            CHECK(t.filesDone == 1 && sameFile(t, pic), "%+.0f Hz: the file is not byte-exact", off);
            if (!t.history.empty()) {
                CHECK(t.history[0]->name == "onair-test.jpg" && t.history[0]->callsign == "LY0AIR" && t.history[0]->mime == "image/jpeg",
                      "name '%s' callsign '%s' mime '%s'", t.history[0]->name.c_str(), t.history[0]->callsign.c_str(), t.history[0]->mime.c_str());
                CHECK(t.history[0]->transmissions == 1, "%d transmissions", t.history[0]->transmissions);
            }
            CHECK(t.mode == 1 && t.occ == 1 && t.qam == 1, "frame information: mode %d occ %d qam %d", t.mode, t.occ, t.qam);
            CHECK(std::abs(t.offsetHz - off) < 3, "offset %.1f Hz, sent %+.0f", t.offsetHz, off);
            CHECK(t.snrDb > 14, "SNR %.1f dB", t.snrDb);
            CHECK(t.name == "onair-test.jpg" && t.callsign == "LY0AIR" && t.segHave == t.segTotal && t.segTotal == hamdrmBodySegments(c, pic.size()),
                  "progress: %d / %d, name '%s'", t.segHave, t.segTotal, t.name.c_str());
        }
    }
    {   // every 4th segment left out of the first transmission: reported missing after it; the second transmission fills them in
        HamdrmTxConfig c;
        c.dropEvery = 4;
        c.repeats = 1;
        auto tx = makeHamdrmTx(c, pic);
        const int n = hamdrmBodySegments(c, pic.size());
        std::vector<int> want;
        for (int i = 3; i < n; i += 4) want.push_back(i);
        const Run r1 = run(tx.get(), 20, 30, [](const HfdigHamdrmTelemetry& t) { return t.state == 2; });
        printf("dropped: state %d, %d / %d segments, %zu missing, log: %s\n", r1.t.state, r1.t.segHave, r1.t.segTotal, r1.t.missing.size(),
               r1.log.empty() ? "-" : r1.log.back().c_str());
        CHECK(r1.t.state == 2 && r1.t.filesDone == 0, "state %d, files %llu", r1.t.state, (unsigned long long)r1.t.filesDone);
        CHECK(r1.t.missing == want && r1.t.segHave == n - (int)want.size(), "missing list wrong (%zu, want %zu)", r1.t.missing.size(), want.size());
        bool logged = false;
        for (const auto& s : r1.log) logged |= s.find("missing 3, 7") != std::string::npos;
        CHECK(logged, "no 'missing' line in the log");
        // the same decoder must go on: run again from the start with a fresh transmitter would lose the state, so continue with this one
        auto dec = makeHamdrmDecoder();
        auto tx2 = makeHamdrmTx(c, pic);   // round 0 (with gaps), then round 1 (complete)
        Noise nz;
        const double sigma = std::sqrt(0.125 / 100.0 * 4000.0 / 3000.0);
        std::vector<float> buf(800);
        HfdigHamdrmTelemetry t;
        bool sawGap = false;
        for (int blk = 0; blk < 600 && t.filesDone == 0; blk++) {
            tx2->generate(buf.data(), buf.size());
            for (float& v : buf) v += (float)(sigma * nz.g());
            dec->feedAudio(buf.data(), buf.size());
            dec->telemetry(t);
            sawGap |= t.state == 2 && !t.missing.empty();
        }
        printf("repeat: %s, %d transmissions\n", t.filesDone ? "done" : "NOT done", t.history.empty() ? 0 : t.history[0]->transmissions);
        CHECK(sawGap && t.filesDone == 1 && sameFile(t, pic), "the repeat did not complete the file (gap seen %d)", (int)sawGap);
        CHECK(!t.history.empty() && t.history[0]->transmissions == 2, "transmissions %d", t.history.empty() ? 0 : t.history[0]->transmissions);
        CHECK(t.missing.empty(), "%zu still missing", t.missing.size());
    }
    {   // the other modes, occupancies and constellations
        struct Case { int mode, occ, qam, prot; double off; };
        for (const Case& k : {Case{0, 0, 1, 1, 7}, Case{2, 1, 0, 1, -12}, Case{1, 0, 2, 1, 15}, Case{2, 0, 2, 0, 0}, Case{0, 1, 0, 0, -5}}) {
            HamdrmTxConfig c;
            c.mode = k.mode; c.occ = k.occ; c.qam = k.qam; c.prot = k.prot; c.offsetHz = k.off;
            auto tx = makeHamdrmTx(c, pic);
            const Run r = run(tx.get(), k.qam == 2 ? 27 : 22, 90, [](const HfdigHamdrmTelemetry& t) { return t.filesDone >= 1; });
            printf("mode %s %.1f kHz %d-QAM %s: %s in %.1f s, SNR %.1f dB, frames %llu ok %llu bad, %d segments of %d bytes\n", hamdrmModeName(k.mode),
                   hamdrmOccupancyKhz(k.occ), hamdrmQam(k.qam), k.prot ? "normal" : "high", r.t.filesDone ? "done" : "NOT done", r.sec, r.t.snrDb,
                   (unsigned long long)r.t.framesOk, (unsigned long long)r.t.framesBad, r.t.segTotal, hamdrmSegmentBytes(k.mode, k.occ, k.qam, k.prot));
            CHECK(r.t.filesDone == 1 && sameFile(r.t, pic) && r.t.mode == k.mode && r.t.occ == k.occ && r.t.qam == k.qam && r.t.prot == k.prot,
                  "mode %d occ %d qam %d prot %d", k.mode, k.occ, k.qam, k.prot);
        }
    }
    {   // noise alone (and silence): nothing
        const Run r = run(nullptr, 20, 30, [](const HfdigHamdrmTelemetry&) { return false; });
        printf("noise: state %d, frames %llu ok %llu bad, files %llu\n", r.t.state, (unsigned long long)r.t.framesOk, (unsigned long long)r.t.framesBad,
               (unsigned long long)r.t.filesDone);
        CHECK(r.t.framesOk == 0 && r.t.filesDone == 0 && !r.t.haveFile && r.t.history.empty() && r.log.empty(), "noise decoded something");
        auto dec = makeHamdrmDecoder();
        std::vector<float> z(8000, 0.f);
        for (int i = 0; i < 5; i++) dec->feedAudio(z.data(), z.size());
        HfdigHamdrmTelemetry t;
        dec->telemetry(t);
        CHECK(t.framesOk == 0 && t.state == 0, "silence decoded something");
    }
    {   // the whole receiver: the I/Q test signal (HamDRM on the upper sideband) -> HfdigReceiver, HamDRM alone
        SynthConfig sc;
        sc.modeOpt[0] = 7;
        sc.snrDb = 25;
        sc.cfoHz = 10;
        const double rate = hfdigTuning().minSampleRate;
        auto syn = makeModeSynth(27, sc, rate);
        HfdigReceiver rx;
        rx.setSilent(true);
        rx.setDecoders(16);
        rx.configure(rate);
        rx.setSignalOffset(-hfdigTuning().tuneOffsetHz);
        std::vector<cf32> v(100000);
        HfdigTelemetry t;
        uint64_t seq = 0;
        double sec = 0;
        while (sec < 40) {
            syn->generate(v.data(), v.size());
            rx.feed(v.data(), v.size());
            sec += (double)v.size() / rate;
            if (rx.telemetry(t, seq)) { seq = t.seq; if (t.hamdrm.filesDone) break; }
        }
        printf("receiver: %s after %.1f s, summary '%s', offset %+.1f Hz\n", t.hamdrm.filesDone ? "done" : "NOT done", sec, hfdigSummary(t).c_str(), t.hamdrm.offsetHz);
        CHECK(t.hamdrm.filesDone == 1 && sameFile(t.hamdrm, pic), "the receiver did not get the picture");
        CHECK(t.rtty.audioSamples == 0 && t.hamdrm.audioSamples > 0, "decoder mask: rtty %llu hamdrm %llu", (unsigned long long)t.rtty.audioSamples,
              (unsigned long long)t.hamdrm.audioSamples);
    }
    {   // the protocol entry
        const SubMode* sm = subModeById("hamdrm");
        CHECK(sm && sm->stdMode == 27 && sm->decoders == 16 && sm->synth[0] == 7, "sub mode entry");
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
