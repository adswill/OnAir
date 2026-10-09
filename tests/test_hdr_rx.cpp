// HD Radio: the test signal (FM hybrid MP1, AM hybrid MA1, FM extended hybrid MP3) through noise and a carrier offset into the real
// receiver, without the engine. Everything the signal carries must come out exactly: the station information, the program list, the program
// service data of every program and the picture, byte for byte. FM and AM at 15 dB SNR on the (primary) digital subcarriers, offsets of
// +-1 kHz; MP3's third program comes on the P3 channel of the extended partitions.
#include "dect2/hdr_gen.h"
#include "dect2/hdr_l2.h"
#include "dect2/hdr_rx.h"
#include "dect2/mode_synth.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const HdrProgram* findProg(const HdrTelemetry& t, int n) {
    for (const auto& p : t.programs) if (p.number == n) return &p;
    return nullptr;
}

// Runs the signal until everything is in (or the time is up); returns the signal seconds it took
static double run(const char* name, int mode, double snr, double cfo, double maxSec, uint64_t* p3 = nullptr) {
    const bool am = mode == 1;
    const HdrTestContent& tc0 = hdrTestContent();
    std::vector<HdrTestContent::Prog> wantList = tc0.programs;
    if (mode == 2) wantList.push_back(tc0.extended);
    const double rate = 2e6;
    SynthConfig sc;
    sc.mode = 23; sc.modeOpt[0] = mode; sc.snrDb = snr; sc.cfoHz = cfo;
    std::unique_ptr<ModeSynth> syn = makeHdrSynth(sc, rate);
    HdrReceiver rx;
    rx.configure(rate);
    std::vector<std::string> log;
    rx.setLogCallback([&](const std::string& s) { log.push_back(s); });
    const HdrTestContent& tc = hdrTestContent();
    std::vector<cf32> buf(20000);
    HdrTelemetry t;
    uint64_t seq = 0;
    double sec = 0, doneAt = -1;
    const auto w0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> img;
    while (sec < maxSec) {
        syn->generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        sec += (double)buf.size() / rate;
        if (rx.telemetry(t, seq)) {
            seq = t.seq;
            bool progs = true;
            for (const auto& w : wantList) { const HdrProgram* p = findProg(t, w.number); progs = progs && p && p->psdCount && !p->name.empty(); }
            const bool all = !t.callSign.empty() && !t.stationName.empty() && !t.slogan.empty() && !t.message.empty() && !t.longName.empty() && t.haveLocation &&
                             progs && rx.lotBytes(tc.artPort, tc.artLot, img);
            if (all && doneAt < 0) { doneAt = sec; break; }
        }
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    printf("%s: %s after %.1f s of signal (%.1f s wall, %.0f%% of real time): state %d %s, MER %.1f/%.1f dB, CFO %+.1f Hz, BER %.4f, P1 %llu ok %llu bad, P3 %llu ok %llu bad, PIDS %llu/%llu\n",
           name, doneAt > 0 ? "complete" : "INCOMPLETE", doneAt > 0 ? doneAt : sec, wall, 100 * wall / sec, t.state, t.modeName.c_str(), t.merLower, t.merUpper, t.cfoHz, t.ber,
           (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.p3Ok, (unsigned long long)t.p3Bad, (unsigned long long)t.pidsOk, (unsigned long long)t.pidsBad);
    CHECK(doneAt > 0, "%s: not everything decoded in %.0f s", name, maxSec);
    CHECK(t.band == (am ? 2 : 1) && t.modeName == (am ? "MA1" : mode == 2 ? "MP3" : "MP1"), "%s: band %d mode %s", name, t.band, t.modeName.c_str());
    CHECK(t.callSign == tc.callSign, "%s: call sign '%s'", name, t.callSign.c_str());
    CHECK(t.stationName == tc.name, "%s: name '%s'", name, t.stationName.c_str());
    CHECK(t.longName == tc.longName, "%s: long name '%s'", name, t.longName.c_str());
    CHECK(t.slogan == tc.slogan, "%s: slogan '%s'", name, t.slogan.c_str());
    CHECK(t.message == tc.message, "%s: message '%s'", name, t.message.c_str());
    CHECK(t.countryCode == tc.country && t.facilityId == tc.facilityId, "%s: station id %s %d", name, t.countryCode.c_str(), t.facilityId);
    CHECK(t.haveLocation && std::fabs(t.latitude - tc.lat) < 2e-4 && std::fabs(t.longitude - tc.lon) < 2e-4 && t.altitudeM == tc.altM, "%s: location %.5f %.5f %d m", name,
          t.latitude, t.longitude, t.altitudeM);
    CHECK(t.programs.size() == wantList.size(), "%s: %zu programs", name, t.programs.size());
    for (const auto& want : wantList) {
        const HdrProgram* p = findProg(t, want.number);
        CHECK(p, "%s: HD%d missing", name, want.number + 1);
        if (!p) continue;
        CHECK(p->name == want.sigName && p->type == want.type && p->title == want.title && p->artist == want.artist && p->album == want.album && p->genre == want.genre,
              "%s: HD%d '%s' type %d '%s' / '%s' / '%s' / '%s'", name, want.number + 1, p->name.c_str(), p->type, p->title.c_str(), p->artist.c_str(), p->album.c_str(), p->genre.c_str());
        CHECK(p->inSis && p->inSig && p->onAir && p->codecMode == (am ? 1 : 0) && p->packetsOk > 0 && p->packetsBad == 0, "%s: HD%d listing / audio packets %llu ok %llu bad", name, want.number + 1,
              (unsigned long long)p->packetsOk, (unsigned long long)p->packetsBad);
        printf("  HD%d %s: %s, codec mode %d, %.1f kbit/s of HDC audio (not decoded), packets %llu ok %llu bad, PSD %llu\n", want.number + 1, p->name.c_str(),
               hdrProgramTypeName(p->type), p->codecMode, p->kbps, (unsigned long long)p->packetsOk, (unsigned long long)p->packetsBad, (unsigned long long)p->psdCount);
    }
    const HdrProgram* p0 = findProg(t, 0);
    CHECK(p0 && p0->xhdrLot == tc.artLot && p0->artPort == tc.artPort, "%s: album art link", name);
    CHECK(img == hdrTestLogoPng(), "%s: picture %zu bytes, %zu sent", name, img.size(), hdrTestLogoPng().size());
    bool lotListed = false, traffic = false;
    for (const auto& l : t.lots) if (l.port == tc.artPort && l.lot == tc.artLot && l.complete && l.name == tc.artName && l.mime == hdr::kMimePng) lotListed = true;
    for (const auto& d : t.dataServices) if (d.fromSig && d.port == tc.trafficPort && d.name == tc.trafficName && d.aasType == 3) traffic = true;
    CHECK(lotListed, "%s: LOT object not listed", name);
    CHECK(traffic, "%s: traffic data service not listed", name);
    CHECK(std::fabs(t.cfoHz - cfo) < 10, "%s: CFO %.1f Hz, sent %.1f", name, t.cfoHz, cfo);
    CHECK(t.blocksBad == 0, "%s: %llu bad transfer frames", name, (unsigned long long)t.blocksBad);
    if (mode == 2) CHECK(t.p3Ok > 0 && t.p3Bad == 0, "%s: P3 %llu ok %llu bad", name, (unsigned long long)t.p3Ok, (unsigned long long)t.p3Bad);
    if (p3) *p3 = t.p3Ok;
    return doneAt;
}

int main() {
    run("FM MP1, 15 dB, +1 kHz", 0, 15, 1000, 30);
    run("FM MP1, 15 dB, -1 kHz", 0, 15, -1000, 30);
    run("AM MA1, 15 dB, +1 kHz", 1, 15, 1000, 40);
    run("AM MA1, 15 dB, -1 kHz", 1, 15, -1000, 40);
    // the P3 channel of AM rides on the secondary and tertiary sidebands, 13 to 14 dB below the primary: it needs a strong signal
    uint64_t p3 = 0;
    run("AM MA1, 32 dB", 1, 32, 300, 40, &p3);
    CHECK(p3 >= 2, "AM P3: %llu frames", (unsigned long long)p3);
    run("FM MP3, 15 dB, +600 Hz", 2, 15, 600, 30);
    printf("hdr rx: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
