// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM+ (robustness mode E, 100 kHz, VHF): the generator's mode E signal through the receiver, which must find mode E by itself (the search
// runs for modes A to D and E at once), read the FAC and the SDC (service label, language, time, text message) and decode the AAC audio
// frames (an audio super frame of 200 ms in two logical frames of 100 ms). At the rates the app asks for and the least the DRM+ lane takes,
// 16-QAM and 4-QAM, the short interleaver, +-200 Hz and a few kHz of tuning error, 15 dB SNR, a 20 ppm sample clock; and a DRM30 signal
// at the same rates is still found as DRM30.
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_rx.h"
#include "dect2/engine/modes.h"
#include "dect2/engine/mode_synth.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using namespace dect2;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Case {
    const char* name;
    double rate = 2e6, snrDb = 30, cfoHz = 0, sroPpm = 0, secs = 7;
    int mode = 5;            // SynthConfig::modeOpt[0]: 5 E, 2 B
    int qam4 = 0, shortIl = 0;
};

static void run(const Case& c) {
    SynthConfig sc;
    sc.mode = 12; sc.snrDb = c.snrDb; sc.cfoHz = c.cfoHz; sc.sroPpm = c.sroPpm;
    sc.modeOpt[0] = c.mode; sc.modeOpt[2] = c.qam4; sc.modeOpt[4] = c.shortIl;
    std::unique_ptr<ModeSynth> syn = makeModeSynth(12, sc, c.rate);
    CHECK(syn != nullptr, "%s: no test signal", c.name);
    if (!syn) return;
    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    CHECK(rx.ready(), "%s: not ready", c.name);
    uint64_t frames = 0;
    int lenSeen = 0;
    rx.setStreamCallback([&](int, const uint8_t*, int len, int, const DrmTelemetry&) { frames++; lenSeen = len; });
    std::string log;
    rx.setLogCallback([&](const std::string& s) { log += s + "\n"; });
    std::vector<cf32> buf(50000);
    const size_t total = (size_t)(c.secs * c.rate);
    double firstAudio = -1;
    DrmTelemetry t;
    uint64_t seq = 0;
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        syn->generate(buf.data(), n);
        rx.feed(buf.data(), n);
        DrmTelemetry u;
        if (rx.telemetry(u, seq)) { seq = u.seq; t = u; if (firstAudio < 0 && t.dataValid) firstAudio = (double)(done + n) / c.rate; }
    }
    const bool E = c.mode == 5;
    jprintf("%-40s mode %c %5.1f kHz %2d-QAM  FAC %llu/%llu SDC %llu/%llu  frames %llu (%d bytes)  audio %llu ok %llu bad  SNR %4.1f dB  CFO %+7.1f Hz  first audio %.1f s  '%s'\n",
            c.name, t.modeName ? t.modeName : '-', t.bandwidthKhz, t.mscQam, (unsigned long long)t.facOk, (unsigned long long)t.facBad, (unsigned long long)t.sdcOk,
            (unsigned long long)t.sdcBad, (unsigned long long)frames, lenSeen, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz, firstAudio,
            t.services.empty() ? "" : t.services[0].label.c_str());
    CHECK(t.mode == (E ? 4 : 1) && t.state == 2 && t.dataValid, "%s: mode %d state %d data %d", c.name, t.mode, t.state, (int)t.dataValid);
    if (E) {
        CHECK(t.bandwidthKhz == 100 && t.occupancy == 0 && t.mscQam == (c.qam4 ? 4 : 16) && t.sdcQam == 4, "%s: %.0f kHz, MSC %d-QAM, SDC %d-QAM", c.name, t.bandwidthKhz, t.mscQam, t.sdcQam);
        CHECK(t.longInterleave == (c.shortIl ? 0 : 1), "%s: interleaver", c.name);
    }
    CHECK(t.services.size() == 1, "%s: %zu services", c.name, t.services.size());
    if (t.services.size() == 1) {
        const DrmService& s = t.services[0];
        CHECK(s.label == (E ? "OnAir DRM+" : "OnAir DRM") && s.id == 0xE12345 && s.languageName == "eng" && s.country == "ae", "%s: service '%s' %06X '%s' '%s'", c.name, s.label.c_str(),
              s.id, s.languageName.c_str(), s.country.c_str());
        CHECK(s.audio && s.audioCoding == 0 && s.audioRateHz == 24000, "%s: audio %d %d Hz", c.name, s.audioCoding, s.audioRateHz);
        if (E) CHECK(s.bitrateBps > 30000, "%s: %d bit/s", c.name, s.bitrateBps);
    }
    CHECK(t.textMessage == "OnAir DRM test signal", "%s: text '%s'", c.name, t.textMessage.c_str());
    CHECK(t.timeValid && t.year == 2026 && t.month == 10 && t.day == 6, "%s: time", c.name);
    CHECK(t.facBad == 0 && t.sdcBad == 0, "%s: FAC %llu bad, SDC %llu bad", c.name, (unsigned long long)t.facBad, (unsigned long long)t.sdcBad);
    CHECK(t.blocksOk >= (E ? 40u : 10u) && t.blocksBad == 0 && t.audioSamples > 48000, "%s: audio frames %llu ok %llu bad, %llu samples", c.name, (unsigned long long)t.blocksOk,
          (unsigned long long)t.blocksBad, (unsigned long long)t.audioSamples);
    if (c.cfoHz != 0) CHECK(std::fabs(t.cfoHz - c.cfoHz) < 5, "%s: CFO %.1f Hz, sent %.1f", c.name, t.cfoHz, c.cfoHz);
    CHECK(firstAudio > 0 && firstAudio < (E ? 4.0 : 12.0), "%s: first audio after %.1f s", c.name, firstAudio);
    if (fails) jprintf("%s", log.c_str());
}

int main() {
    // the parameters of mode E (Tables 2 and 47)
    const drm::ModeParams& e = drm::modeParams(drm::kModeE);
    CHECK(e.name == 'E' && std::fabs(e.spacingHz() - 444.444) < 0.01 && std::fabs(e.symbolMs() - 2.5) < 1e-9 && std::fabs(e.frameMs() - 100) < 1e-9 && e.frames == 4,
          "mode E parameters");
    auto L = drm::layout(drm::kModeE, 0);
    CHECK(L && L->kmin == -106 && L->kmax == 106 && L->nFac == 244, "mode E layout");
    const ModeTuning mt = drmTuning();
    CHECK(mt.maxMhz >= 230 && mt.minMhz <= 0.15, "tuning range %.1f .. %.1f MHz", mt.minMhz, mt.maxMhz);

    testjobs::Jobs jobs;
    std::vector<Case> cs;
    { Case c; c.name = "E 16-QAM, 2 Msps, 30 dB"; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 192 ksps"; c.rate = 192000; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 250 ksps"; c.rate = 250000; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 15 dB"; c.snrDb = 15; c.secs = 8; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 15 dB, +200 Hz"; c.snrDb = 15; c.cfoHz = 200; c.secs = 8; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 15 dB, -200 Hz"; c.snrDb = 15; c.cfoHz = -200; c.secs = 8; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, +3.7 kHz"; c.cfoHz = 3700; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, -5.2 kHz, 1 Msps"; c.cfoHz = -5200; c.rate = 1e6; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM, 20 ppm clock"; c.sroPpm = 20; c.cfoHz = 120; c.secs = 9; cs.push_back(c); }
    { Case c; c.name = "E 4-QAM, 2 Msps, 10 dB"; c.qam4 = 1; c.snrDb = 10; c.secs = 8; cs.push_back(c); }
    { Case c; c.name = "E 16-QAM short interleaver"; c.shortIl = 1; cs.push_back(c); }
    { Case c; c.name = "B 10 kHz (DRM30), 2 Msps"; c.mode = 2; c.secs = 14; cs.push_back(c); }
    { Case c; c.name = "B 10 kHz (DRM30), 250 ksps"; c.mode = 2; c.rate = 250000; c.secs = 14; cs.push_back(c); }
    for (const Case& c : cs) jobs.add([c] { run(c); });
    jobs.run();
    if (fails) { printf("%d check(s) failed\n", fails.load()); return 1; }
    printf("OK\n");
    return 0;
}
