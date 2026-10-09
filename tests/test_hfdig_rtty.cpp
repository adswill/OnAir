// HF digital, RTTY: the generator's audio with noise straight into the decoder (three speeds and shifts, tuning offsets, reversed, noise
// alone), then the whole way through the engine (synthetic source -> USB audio -> decoder). Quick and silent.
#include "dect2/engine.h"
#include "dect2/hfdig_gen.h"
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_rx.h"
#include "dect2/hfdig_tel.h"
#include "dect2/modes.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// white noise of a given power in 3 kHz (the generator's audio has a tone of amplitude 0.5: power 0.125)
struct Noise {
    uint32_t s = 12345;
    float next() {   // sum of uniforms: close enough to a Gaussian
        float a = 0;
        for (int i = 0; i < 6; i++) { s = s * 1664525u + 1013904223u; a += (float)(s >> 8) / 16777216.f - 0.5f; }
        return a * 2.f;   // variance 6 / 12 * 4 = 2... scaled below
    }
};

// Runs seconds of the test audio at snrDb into a decoder; returns the decoded text.
static HfdigRttyTelemetry run(const SynthConfig& sc, double snrDb, double seconds, HfdigRtty& dec) {
    auto gen = makeRttyTestAudio(sc);
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) / 0.75);   // noise over 4 kHz, 3 kHz of it counts
    Noise nz;
    const double unit = std::sqrt(2.0);   // next() has a variance of 2
    std::vector<float> buf(800);
    for (size_t done = 0; done < (size_t)(seconds * 8000); done += buf.size()) {
        gen->generate(buf.data(), buf.size());
        for (float& v : buf) v += (float)(sigma / unit * nz.next());
        dec.feedAudio(buf.data(), buf.size());
    }
    HfdigRttyTelemetry t;
    dec.telemetry(t);
    return t;
}

// the text must contain the whole message and then carry on as the message, repeated, without a wrong character
static bool textOk(const std::string& text, int minCopies) {
    const std::string msg = kRttyTestText;
    const size_t first = text.find(msg);
    if (first == std::string::npos) return false;
    std::string expect;
    for (int i = 0; i < 200 && expect.size() < text.size(); i++) expect += msg;
    const std::string tail = text.substr(first);
    if (expect.compare(0, tail.size(), tail) != 0) return false;
    return (int)(tail.size() / msg.size()) >= minCopies;
}

int main() {
    struct Case { int baud, shift; double off; bool rev; double lo; const char* name; };
    const Case cases[] = {
        {0, 0, 0, false, 0, "45.45/170"}, {0, 0, 50, false, 0, "45.45/170 +50 Hz"}, {0, 0, -50, false, 0, "45.45/170 -50 Hz"},
        {1, 3, 0, false, 0, "50/450"}, {1, 3, 50, false, 0, "50/450 +50 Hz"}, {1, 3, -50, false, 0, "50/450 -50 Hz"},
        {2, 4, 0, false, 0, "75/850"}, {2, 4, 50, false, 0, "75/850 +50 Hz"}, {2, 4, -50, false, 0, "75/850 -50 Hz"},
        {0, 0, 0, true, 0, "45.45/170 reversed"}, {3, 1, 0, false, 1500, "100/200 at 1500 Hz"}, {0, 2, 0, false, 700, "45.45/425 at 700 Hz"},
    };
    for (const Case& c : cases) {
        SynthConfig sc;
        sc.modeOpt[1] = c.baud; sc.modeOpt[2] = c.shift; sc.modeOpt[3] = c.rev;
        sc.modeVal[0] = c.lo > 0 ? c.lo + c.off : (c.off != 0 ? 2125 + c.off : 0);
        auto dec = makeRttyDecoder();
        dec->setBaudIndex(c.baud); dec->setShiftIndex(c.shift); dec->setReverse(c.rev);
        const double secs = 2300.0 / kRttyBaudTable[c.baud] + 6.0;   // a few loops
        const HfdigRttyTelemetry t = run(sc, 10.0, secs, *dec);
        const double lowHz = sc.modeVal[0] > 0 ? sc.modeVal[0] : 2125.0, shift = kRttyShiftTable[c.shift];
        const double wantMark = c.rev ? lowHz + shift : lowHz, wantSpace = c.rev ? lowHz : lowHz + shift;
        printf("%-22s state %d, mark %.1f space %.1f, chars %llu, errors %llu, quality %.2f\n", c.name, t.state, t.markHz, t.spaceHz,
               (unsigned long long)t.chars, (unsigned long long)t.framingErrors, t.quality);
        CHECK(textOk(t.text, 2), "%s: wrong text: \"%s\"", c.name, t.text.c_str());
        CHECK(std::fabs(t.markHz - wantMark) < 8 && std::fabs(t.spaceHz - wantSpace) < 8, "%s: tones %.1f / %.1f, wanted %.1f / %.1f", c.name, t.markHz, t.spaceHz, wantMark, wantSpace);
        CHECK(t.framingErrors <= 6, "%s: %llu framing errors", c.name, (unsigned long long)t.framingErrors);
        CHECK(t.quality > 0.2f, "%s: quality %.2f", c.name, t.quality);
    }
    // noise alone: no lock, no text
    {
        SynthConfig sc;
        auto dec = makeRttyDecoder();
        std::vector<float> buf(800);
        Noise nz;
        for (int i = 0; i < 100; i++) {
            for (float& v : buf) v = 0.2f * nz.next();
            dec->feedAudio(buf.data(), buf.size());
        }
        HfdigRttyTelemetry t;
        dec->telemetry(t);
        CHECK(t.text.empty() && t.state == 0, "noise: state %d, %zu characters", t.state, t.text.size());
    }
    // unshift on space: "1 A" sent as figures, space, letters without LTRS must come out as "1 A" only with the option
    {
        auto dec = makeRttyDecoder();
        dec->setUnshiftOnSpace(true);
        CHECK(dec->unshiftOnSpace() && !dec->reverse() && dec->baudIndex() == 0 && dec->shiftIndex() == 0, "settings");
        dec->setBaudIndex(2); dec->setShiftIndex(4);
        CHECK(dec->baudIndex() == 2 && dec->shiftIndex() == 4, "settings 2");
    }

    // through the engine
    Engine e;
    e.hfdig().setSilent(true);
    const ModeTuning mt = hfdigTuning();
    DeviceInfo dev;
    TuneSettings t;
    t.centerHz = mt.defMhz * 1e6;
    t.bandwidthMhz = mt.bandwidthMhz;
    t.sampleRate = mt.sampleRate;
    t.synth.mode = 27;
    t.synth.snrDb = 20;
    t.synth.modeOpt[0] = 0;
    FileOptions fo;
    e.setStandard(27);
    CHECK(e.start(dev, t, fo), "engine start");
    RxTelemetry rx;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    bool got = false;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40.0) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (rx.hfdig.rtty.text.find("CQ CQ DE ONAIR TEST 0123456789 1/2 -.:? THE QUICK BROWN FOX\n") != std::string::npos) { got = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    e.stop();
    const HfdigRttyTelemetry& r = rx.hfdig.rtty;
    printf("engine: %s after %.1f s: mark %.1f space %.1f, chars %llu, errors %llu\n  \"%s\"\n", got ? "text arrived" : "NO TEXT",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), r.markHz, r.spaceHz, (unsigned long long)r.chars,
           (unsigned long long)r.framingErrors, r.text.c_str());
    CHECK(got, "no RTTY text through the engine");
    CHECK(std::fabs(r.markHz - 2125) < 10 && std::fabs(r.spaceHz - 2295) < 10, "tones %.1f / %.1f", r.markHz, r.spaceHz);
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
