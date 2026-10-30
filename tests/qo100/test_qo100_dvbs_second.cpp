// DVB-S/S2 as the second mode beside QO-100 (Engine::setSecondaryDvbs): QO-100 is the first mode on a 10 Msps capture of the whole downlink; two
// DVB-S2 carriers sit on the wideband transponder (1 Msym/s at +2.25 MHz, 0.5 Msym/s at -2.5 MHz). The second mode decodes the one that was asked for:
// the report comes through latestRx2() as standard 7 with a transport stream lock and the symbol rate of that carrier, the stream reaches the
// engine's analysis (tsSnapshot(): a video and an audio stream) and the first mode goes on reporting QO-100. Other pairs are refused with a reason.
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/engine/engine.h"
#include "dect2/qo100/qo100_dsp.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static bool hasVideoAudio(Engine& e) {
    for (const auto& s : e.tsSnapshot().services) {
        bool v = false, a = false;
        for (const auto& x : s.streams) { v |= x.kind == "video"; a |= x.kind == "audio"; }
        if (v && a) return true;
    }
    return false;
}

int main() {
    const double rate = 10e6, lo = 9750e6, secs = 4;
    const double centre = qo100::ifOf(qo100::kWholeCentreHz, lo);   // the radio's frequency (IF)
    const double off1 = 2.25e6, off2 = -2.5e6, rs1 = 1e6, rs2 = 0.5e6;
    const std::string path = (std::filesystem::temp_directory_path() / "dect2_qo100_dvbs_10Msps.cs8").string();
    {   // the capture: two DVB-S2 carriers (the second one clean and weaker), as 8 bit samples
        SynthConfig c1, c2;
        c1.snrDb = 18; c1.cfoHz = off1; c1.modeVal[0] = rs1;
        c2.snrDb = 40; c2.cfoHz = off2; c2.modeVal[0] = rs2;
        auto g1 = makeDvbsSynth(c1, rate), g2 = makeDvbsSynth(c2, rate);
        CHECK(g1 && g2, "generators");
        if (!g1 || !g2) return 1;
        FILE* f = fopen(path.c_str(), "wb");
        CHECK(f, "cannot write %s", path.c_str());
        if (!f) return 1;
        const size_t blk = 1 << 18;
        std::vector<cf32> a(blk), b(blk);
        std::vector<int8_t> q(2 * blk);
        for (size_t done = 0; done < (size_t)(rate * secs); done += blk) {
            g1->generate(a.data(), blk);
            g2->generate(b.data(), blk);
            for (size_t i = 0; i < blk; i++) {
                const cf32 s = (a[i] + 0.6f * b[i]) * 1.5f;
                q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(s.real() * 128)));
                q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(s.imag() * 128)));
            }
            fwrite(q.data(), 1, q.size(), f);
        }
        fclose(f);
    }

    // what can and cannot be paired, without a radio
    CHECK(!Engine::secondaryAllowed(8) && Engine::secondaryAllowed(8, 55) && !Engine::secondaryAllowed(8, 18) && !Engine::secondaryAllowed(8, 8), "allow-list");
    CHECK(Engine::secondaryCheck(55, 8, centre, rate, centre + off1, rs1 * 1.4).empty(), "QO-100 + DVB-S: %s", Engine::secondaryCheck(55, 8, centre, rate, centre + off1, rs1 * 1.4).c_str());
    CHECK(Engine::secondaryCheck(18, 8, centre, rate, centre + off1, rs1 * 1.4).find("cannot run as a second mode") != std::string::npos, "ACARS + DVB-S");
    CHECK(Engine::secondaryCheck(1, 8, centre, rate, centre + off1, rs1 * 1.4).find("cannot run as a second mode") != std::string::npos, "DVB-T2 + DVB-S");
    const std::string farBy = Engine::secondaryCheck(55, 8, centre, 2e6, centre + 4e6, rs1 * 1.4);
    CHECK(farBy.find("Msps or more") != std::string::npos, "a carrier outside a 2 Msps capture: '%s'", farBy.c_str());

    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File;
    FileOptions fo; fo.path = path; fo.format = FileFormat::CS8; fo.sampleRate = rate; fo.loop = true;
    TuneSettings t; t.centerHz = centre; t.sampleRate = rate; t.bandwidthMhz = 10;
    e.setStandard(55);
    e.qo100().setLo(lo);
    CHECK(e.start(dev, t, fo), "engine start");
    CHECK(e.activeStandard() == 54, "active standard %d", e.activeStandard());
    CHECK(std::fabs(e.captureCentreHz() - centre) < 1, "capture centre %.0f", e.captureCentreHz());
    CHECK(e.setSecondaryDvbs(centre + off1, rs1) && e.secondaryStandard() == 8, "DVB-S asked for: '%s'", e.secondaryNote().c_str());
    CHECK(e.secondaryRunning() && e.secondaryNote().empty(), "not running: '%s'", e.secondaryNote().c_str());
    CHECK(e.secondaryRate() >= 2e6 && e.secondaryRate() <= 5e6, "second receiver at %.3f Msps", e.secondaryRate() / 1e6);

    // a recording waits for both receivers, so a busy machine only plays it slower: up to 90 s
    auto run = [&](double rs, double db, const char* what) {
        RxTelemetry r1, r2;
        uint64_t s1 = 0, s2 = 0;
        int n1 = 0, n2 = 0, wrong = 0;
        bool lock = false, media = false; DvbsTelemetry lastLocked;
        const auto t0 = std::chrono::steady_clock::now();
        double el = 0;
        while ((el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()) < 90.0) {
            if (e.latestRx(r1, s1)) { s1 = r1.seq; n1++; wrong += r1.standard != 54; }
            if (e.latestRx2(r2, s2)) { s2 = r2.seq; n2++; wrong += r2.standard != 7; lock |= r2.dvbs.tsLock; if (r2.dvbs.tsLock) lastLocked = r2.dvbs; }
            media = hasVideoAudio(e);
            if (lock && media && el > 3) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const DvbsTelemetry& d = lock ? lastLocked : r2.dvbs;   // the report that showed the lock (a later one may be mid-restart on a loaded machine)
        printf("  %s: %.1f s, first %d reports, second %d: %s\n", what, el, n1, n2, dvbsSummary(d).c_str());
        CHECK(wrong == 0, "%s: %d reports of the wrong standard", what, wrong);
        CHECK(n1 > 3 && n2 > 3, "%s: reports: %d first, %d second", what, n1, n2);
        CHECK(lock && d.tsLock && d.standard == 2, "%s: transport stream lock %d, standard %d", what, (int)lock, d.standard);
        CHECK(media, "%s: no video and audio stream in the transport stream analysis", what);
        CHECK(std::fabs(d.symbolRate / rs - 1.0) < 1e-3, "%s: symbol rate %.1f Hz (wanted %.0f)", what, d.symbolRate, rs);
        CHECK(d.snrDb > db - 3.5 && d.snrDb < db + 3.5, "%s: Es/N0 %.1f dB (signal about %.0f dB)", what, d.snrDb, db);
        CHECK(d.packets > 100 && d.packetsBad == 0, "%s: %llu packets, %llu bad", what, (unsigned long long)d.packets, (unsigned long long)d.packetsBad);
        CHECK(e.secondaryDropped() == 0 && e.droppedSamples() == 0, "%s: dropped %llu second, %llu first", what, (unsigned long long)e.secondaryDropped(), (unsigned long long)e.droppedSamples());
    };
    run(rs1, 18, "1 Msym/s carrier");

    // the other carrier: the old stream is gone at once, the new one comes
    CHECK(e.setSecondaryDvbs(centre + off2, rs2) && e.secondaryRunning(), "the other carrier: '%s'", e.secondaryNote().c_str());
    run(rs2, 16.6, "0.5 Msym/s carrier");   // the noise floor is the first carrier's: 0.6 times its amplitude at half the symbol rate, 1.4 dB less Es/N0

    // a recording keeps its own centre whatever the VFO does (IQ file playback tunes by offset): a retune leaves the second mode running
    TuneSettings t2 = t; t2.centerHz = centre + 4e6;
    e.retuneReset(t2);
    CHECK(e.secondaryRunning() && e.secondaryStandard() == 8 && e.secondaryNote().empty(), "after moving the VFO of a recording: '%s'", e.secondaryNote().c_str());
    e.retuneReset(t);
    CHECK(e.secondaryRunning() && e.secondaryNote().empty(), "after moving back: '%s'", e.secondaryNote().c_str());

    // off: the stream and the receiver go
    e.clearSecondary();
    CHECK(!e.secondaryRunning() && e.secondaryStandard() == 0, "off");
    CHECK(e.tsSnapshot().services.empty() && e.player().selected() < 0, "the stream is gone");
    // refused beside any other first mode
    e.stop();
    e.setStandard(18);
    CHECK(!e.setSecondaryDvbs(centre + off1, rs1) && e.secondaryStandard() == 0, "DVB-S beside ACARS");
    CHECK(e.secondaryNote().find("cannot run as a second mode") != std::string::npos, "note '%s'", e.secondaryNote().c_str());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    printf(fails ? "qo100 dvbs second: %d FAILED\n" : "qo100 dvbs second: all passed\n", fails);
    return fails ? 1 : 0;
}
