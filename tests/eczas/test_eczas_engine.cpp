// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// e-Czas through the engine (setStandard(87)): the synthetic source plays the 225 kHz test signal at the computer's clock and the receiver reports
// through latestRx() with a clock offset; then a recording (IQ file, looped) is played, which has no clock to compare. No sound.
#include "dect2/eczas/eczas_gen.h"
#include "../common/tmp_path.h"
#include "dect2/eczas/eczas_live.h"
#include "dect2/eczas/eczas_tel.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool wait(Engine& e, RxTelemetry& rx, double secs, bool (*done)(const RxTelemetry&)) {
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; if (rx.standard == 86 && done(rx)) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main() {
    const ModeTuning mt = eczasTuning();
    CHECK(mt.stdMode == 87 && modeTuningById("eczas") && modeTuningById("eczas")->stdMode == 87 && modeTuning(87) && modeTuning(87)->tuneOffsetHz == 12000, "tuning table");
    CHECK(std::abs(mt.defMhz - 0.225) < 1e-9, "default frequency");
    {   // the test signal, as the app starts it
        Engine e;
        DeviceInfo dev;
        TuneSettings t;
        t.centerHz = mt.defMhz * 1e6; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = mt.sampleRate;
        t.synth.mode = 87; t.synth.snrDb = 30;
        FileOptions fo;
        e.setStandard(87);
        CHECK(e.start(dev, t, fo), "engine start");
        CHECK(e.activeStandard() == 86, "active standard %d", e.activeStandard());
        RxTelemetry rx;
        const bool got = wait(e, rx, 40.0, [](const RxTelemetry& r) { return r.eczas.confirmed && r.eczas.clockValid; });
        const EczasTelemetry& d = rx.eczas;
        printf("  %s, SNR %.1f dB, clock offset %+.1f ms (avg %+.1f), %llu ok / %llu bad, %llu other slots\n", eczasSummary(d).c_str(), d.snrDb, d.clockOffsetMs, d.clockOffsetAvgMs,
               (unsigned long long)d.blocksOk, (unsigned long long)d.blocksBad, (unsigned long long)d.otherType);
        CHECK(got, "no confirmed time with a clock offset within 40 s");
        CHECK(rx.rateOk && rx.dataValid && rx.state == 2, "rate %d valid %d state %d", (int)rx.rateOk, (int)rx.dataValid, rx.state);
        CHECK(d.pkt.tzHours == 1 || d.pkt.tzHours == 2, "tz %d", d.pkt.tzHours);
        // the test signal carries the computer's clock: the offset is the path delay of the receiver position (a placeholder) and the buffering, well under a second
        CHECK(std::fabs(d.clockOffsetAvgMs) < 400, "offset %.1f ms", d.clockOffsetAvgMs);
        CHECK(d.constelI.size() > 50 && d.trace.size() > 500 && !d.levelHist.empty(), "plot data: %zu points, %zu trace, %zu level", d.constelI.size(), d.trace.size(), d.levelHist.size());
        CHECK(e.droppedSamples() == 0, "dropped %llu", (unsigned long long)e.droppedSamples());
        e.stop();
    }
    {   // a recording: IQ at 48 ksps with the carrier on the centre (a file holds the channel as recorded, no offset), looped; no clock to compare
        const std::string path = tmpPath("eczas_engine_test.cf32");
        {
            EczasGenConfig gc;
            gc.rate = 48000; gc.dialOffsetHz = 0.0; gc.snrDb = 30; gc.startUtc = 1786640400; gc.timeSlotStride = 2;
            EczasGenerator g(gc);
            std::vector<cf32> b(48000);
            FILE* f = fopen(path.c_str(), "wb");
            CHECK(f != nullptr, "cannot write %s", path.c_str());
            if (f) { for (int s = 0; s < 36; s++) { g.generate(b.data(), b.size()); fwrite(b.data(), sizeof(cf32), b.size(), f); } fclose(f); }
        }
        Engine e;
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "e-Czas test file";
        TuneSettings t;
        t.centerHz = 225000; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = 48000;
        FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = 48000; fo.loop = true;
        e.setStandard(87);
        CHECK(e.start(dev, t, fo), "engine start (file)");
        RxTelemetry rx;
        const bool got = wait(e, rx, 60.0, [](const RxTelemetry& r) { return r.eczas.timeValid && r.eczas.blocksOk >= 3; });
        const EczasTelemetry& d = rx.eczas;
        printf("  file: %s, %llu ok, clock valid %d\n", eczasSummary(d).c_str(), (unsigned long long)d.blocksOk, (int)d.clockValid);
        CHECK(got, "no time packets from the file");
        CHECK(!d.clockValid, "a file has no clock to compare");
        CHECK(d.pkt.utcSec % 3 == 0 && d.pkt.utcSec >= 1786640400 && d.pkt.utcSec < 1786640400 + 4000, "stamp %lld", (long long)d.pkt.utcSec);
        e.stop();
        std::remove(path.c_str());
    }
    {   // a recording of a receiver's audio (8 kHz, the carrier a 1 kHz tone, I = audio, Q = 0), as a stereo WAV plays: "Audio, not IQ"
        const std::string path = tmpPath("eczas_engine_test_audio.cf32");
        {
            EczasGenConfig gc;
            gc.rate = 8000; gc.dialOffsetHz = 1000; gc.snrDb = 30; gc.startUtc = 1786640400; gc.timeSlotStride = 2;
            EczasGenerator g(gc);
            std::vector<cf32> b(8000);
            FILE* f = fopen(path.c_str(), "wb");
            CHECK(f != nullptr, "cannot write %s", path.c_str());
            if (f) { for (int s = 0; s < 36; s++) { g.generate(b.data(), b.size()); for (auto& x : b) x = cf32(x.real(), 0.f); fwrite(b.data(), sizeof(cf32), b.size(), f); } fclose(f); }
        }
        Engine e;
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "e-Czas audio test file";
        TuneSettings t;
        t.centerHz = 225000; t.bandwidthMhz = mt.bandwidthMhz; t.sampleRate = 8000;
        FileOptions fo; fo.path = path; fo.format = FileFormat::CF32; fo.sampleRate = 8000; fo.loop = true;
        e.setStandard(87);
        e.eczas().setAudioInput(true, 1000);
        CHECK(e.start(dev, t, fo), "engine start (audio file)");
        RxTelemetry rx;
        const bool got = wait(e, rx, 60.0, [](const RxTelemetry& r) { return r.eczas.timeValid && r.eczas.blocksOk >= 3; });
        printf("  audio file: %s, %llu ok, audio input %d\n", eczasSummary(rx.eczas).c_str(), (unsigned long long)rx.eczas.blocksOk, (int)rx.eczas.audioInput);
        CHECK(got && rx.rateOk && rx.eczas.audioInput, "no time packets from the audio file");
        e.stop();
        std::remove(path.c_str());
    }
    return testSummary("eczas engine");
}
