// Satellite beacon receiver parts: the line finder (frequency, level, C/N0), the frame length finder, the test generator's golden frames, and the whole chain
// (carrier, drift, subcarrier tones, bi-phase bit clock, sync word, frame length, counter byte, frames equal to the generator's) on the generated signal, then the
// real-world impairments: carrier offsets over the capture, weak carrier, clock errors, other bit rates and subcarriers, NRZ data, no modulation.
#include "dect2/satbeacon/satbeacon_dsp.h"
#include "dect2/satbeacon/satbeacon_gen.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

static std::string hexBytes(const std::vector<uint8_t>& v, size_t n = 0) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < v.size() && (n == 0 || i < n); i++) { snprintf(b, sizeof b, "%02X", v[i]); s += b; }
    return s;
}

static void run(sb::Beacon& b, SatbeaconGenerator& g, double rate, double seconds) {
    std::vector<cf32> x(65536);
    for (int64_t n = 0; n < (int64_t)(seconds * rate); n += (int64_t)x.size()) {
        g.generate(x.data(), x.size());
        b.process(x.data(), x.size());
    }
}

// frames the receiver lists that equal one of the generator's frames (the counter byte repeats every 32 frames: try the candidates)
static int framesMatching(const SbTelemetry& t, uint32_t seed, size_t counterByte = 4) {
    int ok = 0;
    for (const auto& f : t.frames) {
        if (f.bytes.size() != (size_t)kSbGenFrameBytes || f.bytes[counterByte] > 31) continue;
        for (uint32_t n = f.bytes[counterByte]; n < 4000; n += 32)
            if (satbeaconFrameBytes(seed, n) == f.bytes) { ok++; break; }
    }
    return ok;
}

int main() {
    // the generator: golden frames
    {
        CHECK(hexBytes(satbeaconFrameBytes(1, 0), 48) == "FAF320FE0000001EA32B607B23FB0011FBFBFE2600609060D5038FFFA27D0000FF600000FF0065F8EE00009E005A3BD4", "frame 0 golden");
        CHECK(hexBytes(satbeaconFrameBytes(1, 1), 48) == "FAF320FE0100001EA32B60B14AFB0011FBFB72D800603560688B1EFFA7330000FF600000FF0059100B0000D9009B02DA", "frame 1 golden");
        CHECK(hexBytes(satbeaconFrameBytes(1, 31), 48) == "FAF320FE1F00001EA32B60F85FFB001EFBFB03F400607B60A08CE5FF427D0000FF600000FF007D6773000058006C731E", "frame 31 golden");
        CHECK(satbeaconFrameBytes(1, 32)[4] == 0 && satbeaconFrameBytes(1, 32) != satbeaconFrameBytes(1, 0), "counter wraps at 32, payload changes");
        CHECK(satbeaconFrameBytes(2, 0) != satbeaconFrameBytes(1, 0), "the seed changes the payload");
    }
    // frame length of a bit stream: 2048 bits found by the repetition, nothing in random bits
    {
        std::vector<uint8_t> bits;
        for (uint32_t f = 0; f < 40; f++) for (uint8_t v : satbeaconFrameBytes(5, f)) for (int j = 7; j >= 0; j--) bits.push_back((v >> j) & 1);
        float agr = 0;
        const int L = sb::frameLengthOf(bits, &agr);
        CHECK(L == 2048, "frame length 2048, got %d", L);
        std::vector<uint8_t> rnd(40000);
        uint32_t s = 12345;
        for (auto& v : rnd) { s = s * 1664525u + 1013904223u; v = (s >> 20) & 1; }
        CHECK(sb::frameLengthOf(rnd, &agr) == 0, "random bits have no frame");
        std::vector<uint8_t> unbal(40000);
        for (auto& v : unbal) { s = s * 1664525u + 1013904223u; v = ((s >> 20) & 7) == 0; }   // a stream with few ones
        CHECK(sb::frameLengthOf(unbal, &agr) == 0, "an unbalanced random stream has no frame either");
    }
    // the line finder: a carrier of known level in noise of known density
    {
        const double rate = 1e6;
        SatbeaconGenConfig c; c.rate = rate; c.telemetry = false; c.reference = false; c.carrierHz = 123456.7; c.driftHzPerSec = 0; c.snrDb = 30; c.amp = 0.25;
        SatbeaconGenerator g(c);
        sb::LineFinder lf;
        lf.configure(rate);
        std::vector<cf32> x(65536);
        bool ready = false;
        for (int i = 0; i < 40 && !ready; i++) { g.generate(x.data(), x.size()); ready = lf.push(x.data(), x.size()); }
        CHECK(ready, "a spectrum after about a second");
        CHECK(!lf.lines().empty(), "a line");
        if (!lf.lines().empty()) {
            const auto& l = lf.lines()[0];
            CHECK(std::fabs(l.hz - 123456.7) < 2.0, "line at %.2f Hz", l.hz);
            CHECK(std::fabs(l.peakDb - 20 * std::log10(0.25)) < 1.0, "line level %.2f dBFS, expected %.2f", l.peakDb, 20 * std::log10(0.25));
            // noise: the carrier is 30 dB over the noise in 1 kHz: noise density = carrier dBFS - 30 - 30 dB in 1 Hz
            const double want = 20 * std::log10(0.25) - 30 - 30;
            CHECK(std::fabs(lf.noiseDbHz() - want) < 1.0, "noise %.2f dBFS/Hz, expected %.2f", lf.noiseDbHz(), want);
        }
    }
    // the whole chain on the default signal: 4800 bit/s bi-phase on 48 kHz
    {
        const double rate = 1e6;
        SatbeaconGenConfig c; c.rate = rate; c.snrDb = 30; c.reference = false;
        SatbeaconGenerator g(c);
        sb::Beacon b; b.configure(rate);
        run(b, g, rate, 40);
        SbTelemetry t; b.fill(t);
        printf("  state %d cfo %.1f cn0 %.1f drift %.1f tones %d sub %.0f baud %.2f coding %d L %d sync %s counter %d/%d frames %llu/%llu\n", t.state, t.cfoHz, (double)t.snrDb, t.driftHzPerMin, t.tones, t.subcarrierHz, t.baud, t.coding, t.frameBits, t.syncName.c_str(), t.counterByte, t.counterMod, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.state == 3, "state %d", t.state);
        CHECK(std::fabs(t.cfoHz - (236100 - 0.4 * t.timeSec)) < 3.0, "carrier %.1f Hz expected %.1f", t.cfoHz, 236100 - 0.4 * t.timeSec);
        CHECK(std::fabs(t.driftHzPerMin + 24.0) < 3.0, "drift %.1f Hz/min", t.driftHzPerMin);
        // 30 dB in 1 kHz is 60 dB-Hz; the phase modulation (1.2 rad) takes about 3.5 dB of the carrier
        CHECK(std::fabs(t.snrDb - 56.5) < 2.0, "C/N0 %.1f dB-Hz, expected about 56.5", (double)t.snrDb);
        CHECK(t.carrierLocked, "carrier locked");
        CHECK(t.tones == 2, "tones %d", t.tones);
        CHECK(std::fabs(t.subcarrierHz - 48000) < 100, "subcarrier %.0f Hz", t.subcarrierHz);
        CHECK(std::fabs(t.baud - 4800) < 2.0, "bit rate %.2f (the data clock is 7 ppm off, so 4800.03)", t.baud);
        CHECK(t.coding == 1, "bi-phase expected, coding %d", t.coding);
        CHECK(t.frameBits == 2048, "frame length %d", t.frameBits);
        CHECK(t.syncName == "FAF320 (IRIG 106)", "sync word '%s'", t.syncName.c_str());
        CHECK(t.counterByte == 4 && t.counterMod == 32, "counter byte %d modulo %d", t.counterByte, t.counterMod);
        CHECK(t.templateBytes.size() == 256 && hexBytes(t.templateBytes, 4) == "FAF320FE", "template starts %s", hexBytes(t.templateBytes, 4).c_str());
        CHECK(t.blocksOk >= 60 && t.blocksBad == 0, "frames %llu good %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(framesMatching(t, 1) >= 40, "frames equal to the generator's: %d of %zu", framesMatching(t, 1), t.frames.size());
        bool ctrOk = true;
        for (const auto& f : t.frames) if (f.counter != f.bytes[4]) ctrOk = false;
        CHECK(ctrOk, "every listed frame carries its counter");
        CHECK(t.lines.size() >= 1 && std::fabs(t.lines[0].offsetHz - t.cfoHz) < 5.0, "the first line is the carrier");
    }
    // a second carrier: listed, with its spacing; at 2 Msps the real file's geometry (carrier at +236 kHz, reference 545.6 kHz above)
    {
        const double rate = 2e6;
        SatbeaconGenConfig c; c.rate = rate; c.snrDb = 30; c.telemetry = false;
        SatbeaconGenerator g(c);
        sb::Beacon b; b.configure(rate);
        run(b, g, rate, 6);
        SbTelemetry t; b.fill(t);
        CHECK(t.state == 1, "carrier only: state %d", t.state);
        CHECK(t.tones == 0 && t.baud == 0, "no telemetry found in an unmodulated carrier (tones %d)", t.tones);
        CHECK(std::fabs(t.lineSpacingHz - 545642) < 30, "reference carrier %.1f Hz away", t.lineSpacingHz);
        bool found = false;
        for (const auto& l : t.lines) if (!l.side && std::fabs(l.relDb + 8.0) < 1.0) found = true;
        CHECK(found, "the reference carrier is 8 dB below the main one");
    }
    // real-world impairments: sweep
    struct Case { const char* name; double rate, cfo, snr, bitRate, sub, ppm, secs; bool biphase; };
    const Case cases[] = {
        {"carrier at -300 kHz",        1e6,  -300000, 30, 4800, 48000,   7, 40, true},
        {"carrier at +10 kHz",         1e6,    10000, 30, 4800, 48000,   7, 40, true},
        {"weak carrier (24 dB / kHz)", 1e6,   236100, 24, 4800, 48000,   7, 50, true},
        {"data clock -20 ppm",         1e6,   236100, 30, 4800, 48000, -20, 40, true},
        {"2400 bit/s on 24 kHz",       1e6,   236100, 30, 2400, 24000,   7, 60, true},
        {"1200 bit/s NRZ on 12 kHz",   1e6,   236100, 30, 1200, 12000,   7, 90, false},
        {"250 ksps capture",           250e3,  80000, 30, 4800, 48000,   7, 40, true},
    };
    for (const auto& k : cases) {
        SatbeaconGenConfig c; c.rate = k.rate; c.snrDb = k.snr; c.carrierHz = k.cfo; c.bitRate = k.bitRate; c.subcarrierHz = k.sub; c.clockPpm = k.ppm; c.biphase = k.biphase; c.reference = false;
        SatbeaconGenerator g(c);
        sb::Beacon b; b.configure(k.rate);
        // the 4800 bit/s cases at 30 dB lock in well under 15 s (the default chain gets 60 of about 90 frames in 40 s): 25 s by default, the long run as listed
        const double secs = (!testLong() && k.bitRate == 4800 && k.snr >= 30) ? 25 : k.secs;
        run(b, g, k.rate, secs);
        SbTelemetry t; b.fill(t);
        printf("  %s: state %d cfo %.1f baud %.2f coding %d L %d sync %s frames %llu/%llu\n", k.name, t.state, t.cfoHz, t.baud, t.coding, t.frameBits, t.syncName.c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.state == 3, "%s: state %d", k.name, t.state);
        CHECK(std::fabs(t.cfoHz - (k.cfo - 0.4 * t.timeSec)) < 4.0, "%s: carrier %.1f Hz", k.name, t.cfoHz);
        CHECK(std::fabs(t.baud - k.bitRate) < 0.005 * k.bitRate, "%s: bit rate %.2f", k.name, t.baud);
        CHECK(t.coding == (k.biphase ? 1 : 0), "%s: coding %d", k.name, t.coding);
        CHECK(t.frameBits == 2048 && t.syncName == "FAF320 (IRIG 106)", "%s: frame length %d sync '%s'", k.name, t.frameBits, t.syncName.c_str());
        CHECK(framesMatching(t, 1) >= 5 && t.blocksBad == 0, "%s: frames equal to the generator's: %d of %zu, %llu bad", k.name, framesMatching(t, 1), t.frames.size(), (unsigned long long)t.blocksBad);
    }
    // CCSDS codeblocks on the same subcarrier: the sync marker is found, the layout and the transfer frames are read from the receiver's own frames
    {
        SatbeaconGenConfig c; c.rate = 1e6; c.snrDb = 30; c.reference = false; c.ccsds = true;
        SatbeaconGenerator g(c);
        sb::Beacon b; b.configure(1e6);
        run(b, g, 1e6, 45);
        SbTelemetry t; b.fill(t);
        b.analyzer().fill(t, 0);
        printf("  CCSDS: state %d L %d sync '%s' frames %llu/%llu\n", t.state, t.frameBits, t.syncName.c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.state == 3 && t.frameBits == kSbGenCcsdsBytes * 8 && t.syncName == "1ACFFC1D (CCSDS)", "CCSDS frames found: state %d L %d sync '%s'", t.state, t.frameBits, t.syncName.c_str());
        bool layout = false, sc = false;
        for (const auto& i : t.info) { if (i.label == "CCSDS layout" && i.value.find("RS(255,223) I = 1") == 0) layout = true; if (i.label == "Transfer frame" && i.value.find("spacecraft 0x2DF") != std::string::npos) sc = true; }
        CHECK(layout && sc, "layout and spacecraft read");
    }
    return testSummary("satbeacon dsp");
}
