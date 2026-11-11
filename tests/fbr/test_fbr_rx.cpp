// Fleet Broadcast monitor (UHF SATCOM, 250.450 / 250.550 / 250.650 MHz): the SIMULATED test signal through the receiver (fbr_gen.h, fbr_rx.h).
// The signal is built from the public description only (differential BPSK, 1200 Bd, 16 channels of one bit per frame, channel 0 for sync); its sync
// pattern is our own 31-bit m-sequence because the real one is not published. So this checks the modem, the 1200 Bd clock, the frame finder and the
// channel classifier under impairments: carrier offset (satellite Doppler), Doppler drift, symbol clock error, noise, slow fading, a radio tuning
// offset, polarity, other sample rates; and that noise alone, a plain carrier and unsynchronised data never claim a frame. Nothing is decoded or decrypted.
// ONAIR_TEST_LONG=1 adds more offsets, clock errors, lower SNRs and a 1 Msps run.
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/fbr/fbr_gen.h"
#include "dect2/fbr/fbr_rx.h"
#include "dect2/fbr/fbr_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static FbrTelemetry run(FbrGenConfig c, double secs, double nominalOffsetHz = 0, bool rxInvert = false, double centerHz = 0) {
    FbrGenerator g(c);
    FbrReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(nominalOffsetHz);
    r.setCenterHz(centerHz);
    r.setInvert(rxInvert);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    FbrTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static void show(const char* name, const FbrTelemetry& t) {
    printf("  %s: state %d framed %d amb %d off %+.1f Hz snr %.1f eye %.2f ppm %+.0f tone %.0f dB sync %d (%d%%) frames %llu lost %llu %s\n", name, t.state, t.framed, t.ambiguous,
           t.offsetHz, t.snrDb, t.eye, t.clockPpm, t.toneDb, t.syncPeriod, t.syncQuality, (unsigned long long)t.frames, (unsigned long long)t.blocksBad, t.status.c_str());
}

static const std::string kMixed = std::string("[Sriir") + std::string(11, 'e') + "]";

// a run of scenario 0 that must be framed with the right channel classes
static void expectMixed(const char* name, const FbrTelemetry& t, double offset, int minFrames = 250) {
    show(name, t);
    CHECK(t.state == 3 && t.framed && t.dataValid, "%s: state %d framed %d", name, t.state, t.framed);
    CHECK(t.status == kMixed, "%s: channel classes %s (want %s)", name, t.status.c_str(), kMixed.c_str());
    CHECK(t.syncPeriod == kFbrTestSyncPeriod, "%s: sync repeat %d frames (want %d)", name, t.syncPeriod, kFbrTestSyncPeriod);
    CHECK(std::fabs(t.offsetHz - offset) < 20, "%s: carrier %+.1f Hz (want %+.0f)", name, t.offsetHz, offset);
    CHECK((int)t.frames >= minFrames, "%s: %llu frames", name, (unsigned long long)t.frames);
}

int main() {
    const bool lng = std::getenv("ONAIR_TEST_LONG") != nullptr;
    FbrGenConfig c;
    c.rate = 250e3; c.snrDb = 20; c.offsetHz = 1500;

    {   // clean: every channel, the frame words and the hex of the idle channels
        const FbrTelemetry t = run(c, 14);
        expectMixed("clean", t, 1500);
        CHECK(t.ch[2].cls == 'i' && t.ch[2].hex == "ffffffffffffffff", "ch2 hex %s", t.ch[2].hex.c_str());
        CHECK(t.ch[3].cls == 'i' && t.ch[3].hex == "0000000000000000", "ch3 hex %s", t.ch[3].hex.c_str());
        CHECK(t.ch[1].cls == 'r' && t.ch[1].activity > 0.95f, "ch1 activity %.2f", t.ch[1].activity);
        CHECK(t.ch[0].cls == 'S' && t.ch[0].period == 31 && t.syncQuality >= 95, "ch0 %c period %d quality %d", t.ch[0].cls, t.ch[0].period, t.syncQuality);
        CHECK(std::fabs(t.baud - 1200.0) < 1e-6 && std::fabs(t.clockPpm) < 30, "baud %.3f ppm %+.0f", t.baud, t.clockPpm);
        CHECK(t.frameLog.size() == kFbrFramesShown, "%zu frames in the log", t.frameLog.size());
        // the frame words: channel 0 is the top bit and follows the 31-bit sequence from some start; channel 2 is 1, channel 3 is 0, channel 1 alternates
        bool pnOk = false;
        for (int rot = 0; rot < 31 && !pnOk; rot++) {
            bool all = true;
            for (size_t f = 0; f < t.frameLog.size(); f++) {
                const unsigned w = (unsigned)std::strtoul(t.frameLog[f].c_str(), nullptr, 16);
                if (((w >> 15) & 1) != (unsigned)fbrTestBit(0, 0, (uint64_t)(rot + (int)f))) all = false;
            }
            pnOk = all;
        }
        CHECK(pnOk, "channel 0 of the frame words does not follow the sync sequence");
        bool wordsOk = true;
        for (size_t f = 0; f < t.frameLog.size(); f++) {
            const unsigned w = (unsigned)std::strtoul(t.frameLog[f].c_str(), nullptr, 16);
            if (((w >> 13) & 1) != 1 || ((w >> 12) & 1) != 0) wordsOk = false;
            if (f && ((w >> 14) & 1) == ((((unsigned)std::strtoul(t.frameLog[f - 1].c_str(), nullptr, 16)) >> 14) & 1)) wordsOk = false;
        }
        CHECK(wordsOk, "channels 1 to 3 of the frame words are wrong");
        CHECK(!t.log.empty() && fbrSummary(t).find("framed") != std::string::npos, "log / summary");
    }
    {   // a carrier offset (satellite Doppler is a few kHz), both signs, and the radio's own tuning offset
        std::vector<double> offs = {-4000, -2500, 0, 3000, 4000};
        if (lng) offs = {-4400, -4000, -3000, -2000, -1000, -300, 0, 300, 1000, 2000, 3000, 4000, 4400};
        for (double o : offs) {
            FbrGenConfig d = c; d.offsetHz = o;
            char nm[40]; snprintf(nm, sizeof nm, "offset %+.0f", o);
            expectMixed(nm, run(d, 14), o);
        }
        FbrGenConfig d = c; d.offsetHz = -30000 + 1200;
        expectMixed("radio offset -30 kHz", run(d, 14, -30000), -30000 + 1200);
    }
    {   // symbol clock error
        std::vector<double> ppm = {-100, 100};
        if (lng) ppm = {-250, -100, -20, 20, 100, 250};
        for (double p : ppm) {
            FbrGenConfig d = c; d.clockPpm = p;
            char nm[40]; snprintf(nm, sizeof nm, "clock %+.0f ppm", p);
            const FbrTelemetry t = run(d, 16);
            expectMixed(nm, t, 1500);
            CHECK(std::fabs(t.clockPpm - p) < 60, "%s: measured %+.0f ppm", nm, t.clockPpm);
        }
    }
    {   // Doppler drift: 20 Hz per second over 14 s
        FbrGenConfig d = c; d.dopplerHzPerS = 20;
        const FbrTelemetry t = run(d, 14);
        show("doppler 20 Hz/s", t);
        CHECK(t.framed && t.status == kMixed, "doppler: framed %d %s", t.framed, t.status.c_str());
        CHECK(std::fabs(t.offsetHz - (1500 + 20 * 14.0)) < 60, "doppler: carrier %+.1f Hz", t.offsetHz);
    }
    {   // noise and fading
        std::vector<double> snr = {10, 8};
        if (lng) snr = {14, 10, 8, 6, 4};
        for (double s : snr) {
            FbrGenConfig d = c; d.snrDb = s;
            char nm[40]; snprintf(nm, sizeof nm, "snr %.0f dB", s);
            const FbrTelemetry t = run(d, 16);
            show(nm, t);
            CHECK(t.framed && t.status == kMixed, "%s: framed %d %s", nm, t.framed, t.status.c_str());
        }
        FbrGenConfig d = c; d.snrDb = 15; d.fadeDepthDb = 12;
        const FbrTelemetry t = run(d, 16);
        show("fading 12 dB, snr 15", t);
        CHECK(t.framed && t.status == kMixed, "fading: framed %d %s", t.framed, t.status.c_str());
    }
    {   // idle-heavy content: the sync channel against channels with constant bits
        FbrGenConfig d = c; d.scenario = 1;
        const FbrTelemetry t = run(d, 14);
        show("sync + constant channels", t);
        CHECK(t.framed && t.status.size() == 18 && t.status[2] == 'i' && t.status[1] == 'S' && t.syncPeriod == 31, "scenario 1: %s", t.status.c_str());
    }
    {   // never claim a frame without one
        FbrGenConfig d = c; d.scenario = 2;
        const FbrTelemetry n = run(d, 20);
        show("noise only", n);
        CHECK(!n.framed && n.state < 2 && n.symbols == 0, "noise: framed %d state %d symbols %llu", n.framed, n.state, (unsigned long long)n.symbols);
        d.scenario = 3;
        const FbrTelemetry k = run(d, 20);
        show("plain carrier", k);
        CHECK(!k.framed && k.symbols == 0, "carrier: framed %d symbols %llu", k.framed, (unsigned long long)k.symbols);
        d.scenario = 4;
        const FbrTelemetry r = run(d, 14);
        show("random on every channel", r);
        int e = 0;
        for (int i = 0; i < 16; i++) e += r.ch[i].cls == 'e';
        CHECK(!r.framed && r.state == 2 && e >= 14 && r.status.find('S') == std::string::npos, "random: framed %d state %d e %d %s", r.framed, r.state, e, r.status.c_str());
    }
    {   // polarity: an inverted stream swaps the idle channels until the switch is set
        FbrGenConfig d = c; d.invert = true;
        const FbrTelemetry t = run(d, 14);
        show("inverted stream, switch off", t);
        CHECK(t.framed && t.ch[2].hex == "0000000000000000" && t.ch[3].hex == "ffffffffffffffff", "inverted: ch2 %s ch3 %s", t.ch[2].hex.c_str(), t.ch[3].hex.c_str());
        const FbrTelemetry u = run(d, 14, 0, true);
        show("inverted stream, switch on", u);
        CHECK(u.framed && u.inverted && u.ch[2].hex == "ffffffffffffffff" && u.ch[3].hex == "0000000000000000", "inverted + switch: ch2 %s ch3 %s", u.ch[2].hex.c_str(), u.ch[3].hex.c_str());
    }
    {   // other sample rates
        std::vector<double> rates = {48e3};
        if (lng) rates = {48e3, 96e3, 1e6};
        for (double fs : rates) {
            FbrGenConfig d = c; d.rate = fs; d.offsetHz = -2200;
            char nm[40]; snprintf(nm, sizeof nm, "%.0f ksps", fs / 1e3);
            expectMixed(nm, run(d, 14), -2200);
        }
    }
    {   // the carrier names and the mode tables
        int n = 0;
        const fbr::Carrier* cs = fbr::carriers(n);
        CHECK(n == 3 && std::fabs(cs[0].mhz - 250.450) < 1e-9 && std::fabs(cs[1].mhz - 250.550) < 1e-9 && std::fabs(cs[2].mhz - 250.650) < 1e-9, "carriers");
        CHECK(fbr::carrierNear(250.4521) == "250.450 MHz" && fbr::carrierNear(250.6) .empty(), "carrier names");
        const ModeTuning* mt = modeTuning(872);
        CHECK(mt && std::string(mt->id) == "fbr" && std::fabs(mt->defMhz - 250.450) < 1e-9, "tuning registry");
        SynthConfig sc; sc.mode = 872; sc.snrDb = 20;
        std::unique_ptr<ModeSynth> sy = makeModeSynth(872, sc, 250e3);
        CHECK(sy != nullptr, "the engine's test signal for standard 872");
        if (sy) {
            FbrReceiver r; r.configure(250e3);
            std::vector<cf32> buf(16384);
            for (size_t done = 0; done < (size_t)(12 * 250e3); done += buf.size()) { sy->generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
            FbrTelemetry t; r.telemetry(t, 0);
            show("engine test signal", t);
            CHECK(t.framed && t.status == kMixed, "engine test signal: framed %d %s", t.framed, t.status.c_str());
        }
    }
    return testSummary("fbr rx");
}
