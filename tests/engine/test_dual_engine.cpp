// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Two modes at once (Engine::setSecondary): ACARS (131.5 to 131.8 MHz) as the first mode and VDL Mode 2 (136.725 to 136.975 MHz) as the
// second, from one 12 Msps capture centred on 132 MHz written by both test generators. Both must decode, the second through latestRx2(),
// and pairs that cannot work must be refused with a reason.
#include "dect2/acars/acars_gen.h"
#include "dect2/vdl2/vdl2_gen.h"
#include "dect2/engine/engine.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const double rate = 12e6, centre = 132e6, secs = 6;
    const std::string path = (std::filesystem::temp_directory_path() / "dect2_dual_acars_vdl2_12Msps.cs8").string();
    {   // the capture: both generators at the same rate and centre, added, as 8 bit samples
        // one noise floor for both: each generator adds white noise over the whole capture, so only VDL2's does (ACARS's would bury
        // the weaker VDL2 aircraft; the ACARS carriers still sit about 30 dB above it in 7 kHz)
        SynthConfig cfg, clean;
        cfg.snrDb = 32;
        clean.snrDb = 60;
        AcarsGenOptions ao; ao.centerHz = centre; ao.rateFactor = 3;
        Vdl2GenOptions vo; vo.centerHz = centre; vo.rateFactor = 3;
        auto ga = makeAcarsSynthEx(ao, clean, rate);
        auto gv = makeVdl2SynthEx(vo, cfg, rate);
        CHECK(ga && gv, "generators");
        if (!ga || !gv) return 1;
        FILE* f = fopen(path.c_str(), "wb");
        CHECK(f, "cannot write %s", path.c_str());
        if (!f) return 1;
        const size_t blk = 1 << 18;
        std::vector<cf32> a(blk), v(blk);
        std::vector<int8_t> q(2 * blk);
        for (size_t done = 0; done < (size_t)(rate * secs); done += blk) {
            ga->generate(a.data(), blk);
            gv->generate(v.data(), blk);
            for (size_t i = 0; i < blk; i++) {
                const cf32 s = (a[i] + v[i]) * 0.5f;
                q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(s.real() * 128)));
                q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(s.imag() * 128)));
            }
            fwrite(q.data(), 1, q.size(), f);
        }
        fclose(f);
    }

    // what can and cannot be paired, without a radio
    CHECK(Engine::secondaryAllowed(29) && Engine::secondaryAllowed(16) && Engine::secondaryAllowed(26) && Engine::secondaryAllowed(47) && Engine::secondaryAllowed(11), "allow-list");
    CHECK(!Engine::secondaryAllowed(14) && !Engine::secondaryAllowed(34) && !Engine::secondaryAllowed(44) && !Engine::secondaryAllowed(1) && !Engine::secondaryAllowed(7), "refused modes");
    CHECK(Engine::secondaryCheck(18, 29, centre, rate, 136.85e6).empty(), "ACARS + VDL2 at 12 Msps: %s", Engine::secondaryCheck(18, 29, centre, rate, 136.85e6).c_str());
    const std::string tooSlow = Engine::secondaryCheck(18, 29, 131.725e6, 2e6, 136.85e6);
    CHECK(tooSlow.find("Msps or more") != std::string::npos, "ACARS + VDL2 at 2 Msps: '%s'", tooSlow.c_str());
    printf("  refused: %s\n", tooSlow.c_str());
    CHECK(Engine::secondaryCheck(47, 11, 446.153125e6, 2e6, 446.18125e6).empty(), "dPMR + DMR at 2 Msps");
    CHECK(Engine::secondaryCheck(16, 26, 162e6, 2e6, 144.8e6).find("Msps or more") != std::string::npos, "AIS + APRS are 17 MHz apart");
    CHECK(Engine::secondaryCheck(44, 26, 144.8e6, 20e6, 144.8e6).find("retunes") != std::string::npos, "beside the spectrum analyser");
    CHECK(Engine::secondaryCheck(18, 18, centre, rate, 131.725e6).find("already") != std::string::npos, "the same mode twice");

    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = true;
    TuneSettings t; t.centerHz = centre; t.sampleRate = rate; t.bandwidthMhz = 2;
    e.setStandard(18);
    CHECK(!e.setSecondaryHz(14, 1575.42e6), "GNSS as a second mode");
    CHECK(e.secondaryNote().find("cannot run as a second mode") != std::string::npos, "note '%s'", e.secondaryNote().c_str());
    CHECK(e.setSecondaryHz(29, 136.85e6) && e.secondaryStandard() == 29, "VDL2 asked for before the start");
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.secondaryRunning() && e.secondaryNote().empty(), "second mode not running: '%s'", e.secondaryNote().c_str());
    CHECK(std::fabs(e.captureCentreHz() - centre) < 1, "capture centre %.0f", e.captureCentreHz());

    RxTelemetry r1, r2;
    uint64_t s1 = 0, s2 = 0;
    int n1 = 0, n2 = 0, wrong = 0;
    const auto t0 = std::chrono::steady_clock::now();
    // a recording waits for both receivers, so a busy machine only plays it slower: up to 60 s, until both have decoded enough
    double el = 0;
    while ((el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()) < 60.0) {
        if (e.latestRx(r1, s1)) { s1 = r1.seq; n1++; wrong += r1.standard != 17; }
        if (e.latestRx2(r2, s2)) { s2 = r2.seq; n2++; wrong += r2.standard != 28; }
        if (el > 10 && r1.acars.blocksOk >= 10 && r2.vdl2.blocksOk >= 20) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    printf("  %.1f s\n", el);
    printf("  first:  %d reports: %s\n", n1, acarsSummary(r1.acars).c_str());
    printf("  second: %d reports: %s, %zu messages\n", n2, vdl2Summary(r2.vdl2).c_str(), r2.vdl2.messages.size());
    CHECK(wrong == 0, "%d reports of the wrong standard", wrong);
    CHECK(n1 > 5 && n2 > 5, "reports: %d first, %d second", n1, n2);
    CHECK(r1.acars.blocksOk >= 10 && r1.acars.messages.size() >= 10, "ACARS: %llu blocks, %zu messages", (unsigned long long)r1.acars.blocksOk, r1.acars.messages.size());
    CHECK(r1.acars.blocksBad <= r1.acars.blocksOk / 10, "ACARS: %llu bad", (unsigned long long)r1.acars.blocksBad);
    CHECK(r2.vdl2.blocksOk >= 20 && !r2.vdl2.messages.empty(), "VDL2: %llu frames, %zu messages", (unsigned long long)r2.vdl2.blocksOk, r2.vdl2.messages.size());
    CHECK(r2.vdl2.blocksBad * 10 <= r2.vdl2.blocksOk, "VDL2: %llu bad bursts", (unsigned long long)r2.vdl2.blocksBad);
    CHECK(e.secondaryDropped() == 0 && e.droppedSamples() == 0, "dropped: %llu second, %llu first", (unsigned long long)e.secondaryDropped(), (unsigned long long)e.droppedSamples());

    // while running: a frequency outside the capture is refused (and the second mode is off), a good one comes back
    CHECK(!e.setSecondaryHz(29, 150e6) && e.secondaryStandard() == 0 && !e.secondaryRunning(), "VDL2 at 150 MHz in a 12 Msps capture on 132 MHz");
    printf("  refused: %s\n", e.secondaryNote().c_str());
    CHECK(e.secondaryNote().find("Msps or more") != std::string::npos, "note '%s'", e.secondaryNote().c_str());
    CHECK(!e.setSecondaryHz(18, 131.725e6), "ACARS twice");
    CHECK(e.setSecondary(29, 136.85e6 - centre) && e.secondaryRunning(), "VDL2 again (as an offset): '%s'", e.secondaryNote().c_str());
    const uint64_t before = r2.vdl2.blocksOk;
    bool again = false;
    for (int i = 0; i < 500 && !(again = e.latestRx2(r2, s2) && r2.standard == 28); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(again, "second report after the restart");
    printf("  second again: %llu frames (%llu before)\n", (unsigned long long)r2.vdl2.blocksOk, (unsigned long long)before);
    // a recording keeps its own centre whatever the VFO does (IQ file playback tunes by offset), so a retune leaves the second mode running
    TuneSettings t2 = t; t2.centerHz = 128e6;
    e.retuneReset(t2);
    CHECK(e.secondaryRunning() && e.secondaryStandard() == 29 && e.secondaryNote().empty(), "after moving the VFO of a recording: '%s'", e.secondaryNote().c_str());
    e.retuneReset(t);
    CHECK(e.secondaryRunning() && e.secondaryNote().empty(), "after moving back: '%s'", e.secondaryNote().c_str());
    e.clearSecondary();
    CHECK(!e.secondaryRunning() && e.secondaryStandard() == 0, "off");
    e.stop();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    printf(fails ? "dual engine: %d FAILED\n" : "dual engine: all passed\n", fails);
    return fails ? 1 : 0;
}
