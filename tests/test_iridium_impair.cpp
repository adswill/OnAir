// Iridium receiver under impairments (random payloads, bit-exact or counted): SNR sweep to the failure point, carrier offset of
// +-10 ppm at 1.6 GHz plus the satellite Doppler (+-37.5 kHz, 350 Hz/s), sample clock +-50 ppm, 8-bit samples, DC offset, a 20 ms
// gap, reset() in the middle of the stream.
#include "dect2/iridium_testkit.h"
#include "dect2/iridium_frame.h"
#include <cstdio>
using namespace dect2;
using namespace dect2::iridiumtest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static Match go(const char* name, const SceneOpts& so, const RunOpts& ro, RunResult* out = nullptr, double from = 0, double to = 1e9) {
    const auto bursts = makeScene(so, ro.noiseSigma);
    RunResult r = runScene(bursts, so.rate, so.secs, ro);
    const Match m = match(bursts, r.got, ro.centerMhz * 1e6, from, to, ro.sroPpm);
    printf("%-36s %4d sent, %4d found (%5.1f %%), %4d exact (%5.1f %%), %6d bit errors, %d extra, max %4.0f Hz %.1f us\n", name, m.sent, m.found,
           100.0 * m.found / std::max(1, m.sent), m.exact, 100.0 * m.exact / std::max(1, m.sent), m.bitErrors, m.extra, m.maxFreqErr, m.maxTimeErr * 1e6);
    if (out) *out = std::move(r);
    return m;
}

int main() {
    // SNR sweep (Es/N0 of each burst), 4 Msps: detection, unique word and bit-exact rates
    printf("SNR sweep, 4 Msps, 200 bursts per point\n");
    double exact90 = 99, found90 = 99;
    for (double snr = 20; snr >= 4; snr -= 2) {
        SceneOpts so; so.rate = 4e6; so.secs = 1.6; so.perSec = 125; so.esn0Db = snr; so.seed = 500 + (uint32_t)snr;
        RunOpts ro;
        char nm[64]; snprintf(nm, sizeof nm, "Es/N0 %4.1f dB", snr);
        const Match m = go(nm, so, ro);
        if (m.found >= 0.9 * m.sent) found90 = snr;
        if (m.exact >= 0.9 * m.sent) exact90 = snr;
        if (snr >= 14) CHECK(m.exact == m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
        // 12 dB: differential decoding of coherent QPSK loses about 1 burst of 300 bits in 50 (bit error rate near 7e-5)
        if (snr >= 12) CHECK(m.exact >= 0.95 * m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
        CHECK(m.extra <= 2, "%s: %d bursts that were not sent", nm, m.extra);
    }
    printf("found in 90 %% down to %.0f dB, bit-exact in 90 %% down to %.0f dB (Es/N0)\n", found90, exact90);
    CHECK(found90 <= 10 && exact90 <= 12, "sensitivity: found %.0f dB, exact %.0f dB", found90, exact90);

    // the test signal at falling levels: frames whose codes check out (the frame layer corrects bit errors)
    {
        printf("test signal, 10 Msps, 2 s per level: frames decoded\n");
        double ref = 0;
        for (double snr : {25.0, 15.0, 12.0, 10.0, 8.0, 6.0}) {
            SynthConfig c; c.snrDb = snr; c.mode = 21; c.modeOpt[1] = 9;
            auto syn = makeIridiumSynth(c, 10e6);
            IridiumReceiver rx; rx.configure(10e6); rx.setOffline(true);
            std::vector<cf32> buf(50000);
            for (int i = 0; i < 400; i++) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
            rx.flush();
            IridiumTelemetry t; rx.telemetry(t, 0);
            if (snr == 25) ref = (double)t.blocksOk;
            printf("  Es/N0 %4.1f dB (zenith): %4llu frames ok (%5.1f %%), %3llu failed, %3llu IRA, %llu messages, corrected by the codes\n", snr, (unsigned long long)t.blocksOk,
                   100.0 * t.blocksOk / std::max(1.0, ref), (unsigned long long)t.blocksBad, (unsigned long long)t.typeCount[1], (unsigned long long)t.messages.size());
            // the satellites low in the sky arrive up to 4 dB weaker; the codes correct 2 bits in 31, so a frame of 864 bits needs a
            // bit error rate well below 1 %: Es/N0 of about 10 dB
            if (snr >= 15) CHECK(t.blocksOk >= 0.97 * ref && t.messages.size() >= 3, "Es/N0 %.0f: %llu of %.0f frames", snr, (unsigned long long)t.blocksOk, ref);
            if (snr == 12) CHECK(t.blocksOk >= 0.4 * ref, "Es/N0 %.0f: %llu of %.0f frames", snr, (unsigned long long)t.blocksOk, ref);
        }
    }
    // carrier offset: +-16 kHz (10 ppm of 1.6 GHz) and +-81 kHz (50 ppm) on top of the satellite Doppler (+-37.5 kHz, 350 Hz/s)
    for (double cfo : {-81e3, -16e3, 16e3, 81e3}) {
        SceneOpts so; so.rate = 10e6; so.secs = 1.2; so.cfoHz = cfo; so.dopplerMax = 37.5e3; so.dopplerRate = 350; so.seed = 600 + (cfo > 0) + 2 * (std::fabs(cfo) > 20e3);
        RunOpts ro;
        char nm[64]; snprintf(nm, sizeof nm, "offset %+.0f kHz, Doppler 37.5 kHz", cfo / 1e3);
        const Match m = go(nm, so, ro);
        CHECK(m.sent > 80 && m.exact == m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
    }
    // sample clock +-50 and +-100 ppm
    for (double ppm : {-100.0, -50.0, 50.0, 100.0}) {
        SceneOpts so; so.rate = 10e6; so.secs = 1.2; so.seed = 610 + (ppm > 0) + 2 * (std::fabs(ppm) > 60);
        RunOpts ro; ro.sroPpm = ppm;
        char nm[64]; snprintf(nm, sizeof nm, "sample clock %+.0f ppm", ppm);
        const Match m = go(nm, so, ro);
        CHECK(m.sent > 80 && m.exact == m.sent, "%s: %d of %d exact", nm, m.exact, m.sent);
    }
    // 8-bit samples (a HackRF at a low level: noise about 6 LSB rms) and a DC offset
    {
        SceneOpts so; so.rate = 10e6; so.secs = 1.2; so.esn0Db = 18; so.seed = 620;
        RunOpts ro; ro.quant8 = true; ro.noiseSigma = 0.035;
        const Match m = go("8-bit samples, Es/N0 18 dB", so, ro);
        CHECK(m.sent > 80 && m.exact == m.sent, "8 bit: %d of %d exact", m.exact, m.sent);
    }
    {
        SceneOpts so; so.rate = 10e6; so.secs = 1.2; so.seed = 621;
        RunOpts ro; ro.dc = cf32(0.12f, -0.08f);
        const Match m = go("DC offset 0.14", so, ro);
        CHECK(m.sent > 80 && m.exact >= m.sent - 1, "DC: %d of %d exact", m.exact, m.sent);
    }
    // 20 ms of samples lost: only the bursts the gap cuts are lost
    {
        SceneOpts so; so.rate = 10e6; so.secs = 1.5; so.seed = 622;
        RunOpts ro; ro.gapAt = 0.7; ro.gapSecs = 0.02; ro.chunk = 5000;
        RunResult r;
        const Match m = go("20 ms gap", so, ro, &r);
        CHECK(m.sent > 100 && m.exact >= m.sent - 3 && m.extra <= 1, "gap: %d of %d exact", m.exact, m.sent);
    }
    // reset() in the middle: the tables empty, decoding goes on
    {
        SceneOpts so; so.rate = 4e6; so.secs = 1.5; so.seed = 623;
        RunOpts ro; ro.resetAt = 0.75; ro.chunk = 4000;
        RunResult r;
        const Match m = go("reset at 0.75 s", so, ro, &r);
        CHECK(m.sent > 100 && m.exact >= m.sent - 2, "reset: %d of %d exact", m.exact, m.sent);
        CHECK(r.tel.uwOk < (uint64_t)m.sent * 0.6 && r.tel.uwOk > (uint64_t)m.sent * 0.4 && r.tel.timeSec < 0.8, "after the reset: %llu unique words, time %.2f",
              (unsigned long long)r.tel.uwOk, r.tel.timeSec);
    }
    printf(fails ? "iridium impair: %d FAILED\n" : "iridium impair: all passed\n", fails);
    return fails ? 1 : 0;
}
