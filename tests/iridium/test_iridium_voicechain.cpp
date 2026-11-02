// Iridium voice slots through the whole receiver (generator -> impairments -> demodulator -> frame layer -> call grouping), with known
// payloads: every voice field must come out bit-exact. Sweeps: carrier offset, off-centre tuning, satellite Doppler and Doppler rate,
// sample clock error, 8-bit samples, SNR (compared with the theory below). A real recording is not covered: see iridium_voice.h.
// Theory: the burst is 358 differentially decoded QPSK bits; coherent QPSK with differential decoding has Pb = 2 Q(sqrt(Es/N0)); a burst is
// whole with probability (1-Pb)^358, so 90 % whole bursts need Pb = 2.9e-4, i.e. Es/N0 = 11.2 dB.
#include "dect2/iridium/iridium_testkit.h"
#include "dect2/iridium/iridium_voice.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::iridiumtest;
#include "../common/test_util.h"

static std::vector<IridiumTestBurst> voiceScene(const SceneOpts& so, double sigma) {
    auto v = makeScene(so, sigma);
    uint32_t n = 1;
    for (auto& b : v) {
        b.downlink = true;
        b.preamble = iridium::kPreambleShort;
        b.bits = iridiumBuildVoiceLike(1000 + n++);   // random voice field, link control word type 0
    }
    return v;
}

static Match go(const char* name, const SceneOpts& so, const RunOpts& ro, double* calls = nullptr) {
    const auto bursts = voiceScene(so, ro.noiseSigma);
    RunResult r = runScene(bursts, so.rate, so.secs, ro);
    const Match m = match(bursts, r.got, ro.centerMhz * 1e6, 0, 1e9, ro.sroPpm);
    // the same bursts into the call logic: voice slots on one frequency make calls; readable frames are the good ones
    IridiumVoice voice;
    voice.setSilent(true);
    for (const auto& g : r.got) voice.feed(g.bits, decodeIridiumBurst(g.bits));
    voice.closeAll();
    uint32_t frames = 0, readable = 0;
    for (const auto& c : voice.calls()) { frames += c.frames; readable += c.readable; }
    printf("%-34s %3d sent %3d found %3d exact (%5.1f %%), %2d calls %u frames %u readable\n", name, m.sent, m.found, m.exact, 100.0 * m.exact / std::max(1, m.sent),
           (int)voice.calls().size(), frames, readable);
    if (calls) *calls = (double)voice.calls().size();
    return m;
}

int main() {
    SceneOpts base; base.rate = 4e6; base.secs = 1.6; base.perSec = 100; base.esn0Db = 20; base.seed = 91;
    RunOpts ro0;
    const Match m0 = go("clean 20 dB", base, ro0);
    CHECK(m0.sent > 100 && m0.exact == m0.sent, "clean: %d of %d exact", m0.exact, m0.sent);

    for (double cfo : {-8000.0, -3000.0, 3000.0, 8000.0}) {   // carrier offset (radio error, 5 ppm at 1.6 GHz is 8 kHz)
        SceneOpts so = base; so.cfoHz = cfo; char nm[48]; snprintf(nm, sizeof nm, "carrier offset %+.0f Hz", cfo);
        const Match m = go(nm, so, ro0);
        CHECK(m.exact >= 0.98 * m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
    }
    {   // off-centre: the receiver tuned 600 kHz away from the channels (centre frequency differs from the band centre)
        SceneOpts so = base; so.bandHz = 1.2e6; so.cfoHz = 600e3; so.rate = 4e6;
        const Match m = go("off-centre +600 kHz", so, ro0);
        CHECK(m.exact >= 0.95 * m.sent, "off-centre: %d of %d exact", m.exact, m.sent);
    }
    {   // satellite Doppler: +-37.5 kHz, 350 Hz/s
        SceneOpts so = base; so.dopplerMax = 37500; so.dopplerRate = 350;
        const Match m = go("Doppler +-37.5 kHz, 350 Hz/s", so, ro0);
        CHECK(m.exact >= 0.98 * m.sent, "Doppler: %d of %d exact", m.exact, m.sent);
    }
    for (double ppm : {-50.0, 50.0}) {
        RunOpts ro; ro.sroPpm = ppm; char nm[48]; snprintf(nm, sizeof nm, "sample clock %+.0f ppm", ppm);
        const Match m = go(nm, base, ro);
        CHECK(m.exact >= 0.98 * m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
    }
    {   // all together, HackRF-like 8-bit samples, DC
        SceneOpts so = base; so.cfoHz = 4000; so.dopplerMax = 30000; so.dopplerRate = 300;
        RunOpts ro; ro.sroPpm = 20; ro.quant8 = true; ro.dc = cf32(0.02f, -0.01f);
        const Match m = go("combined + 8 bit + DC", so, ro);
        CHECK(m.exact >= 0.95 * m.sent, "combined: %d of %d exact", m.exact, m.sent);
    }
    // sensitivity against theory
    double exact90 = 99;
    for (double snr = 16; snr >= 8; snr -= 1) {
        SceneOpts so = base; so.esn0Db = snr; so.secs = 3.0; so.seed = 300 + (uint32_t)snr;
        char nm[48]; snprintf(nm, sizeof nm, "Es/N0 %4.1f dB", snr);
        const Match m = go(nm, so, ro0);
        if (m.exact >= 0.9 * m.sent) exact90 = snr;
    }
    printf("90 %% whole voice bursts down to Es/N0 %.0f dB (theory 11.2 dB)\n", exact90);
    CHECK(exact90 <= 13, "sensitivity: %.0f dB against 11.2 dB theory", exact90);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
