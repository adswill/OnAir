// Mercury OFDM loopback through OnAir's own modulator: every mode decodes through noise, a frequency offset, a timing offset and a
// sample clock error; a seeded CRC comes back with its seed; the waveform is what Mercury's code computes (carrier layout, unique word
// positions, preamble); noise alone decodes nothing. Verified against this implementation's own modulator only (see docs/mercury.md).
#include "dect2/mercury/mercury_phy.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/dsp/portable_rng.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

namespace {

std::vector<OfdmFrame> run(const std::vector<cf>& x) {
    OfdmReceiver rx;
    // in uneven chunks, as a radio delivers them
    size_t at = 0, k = 0;
    while (at < x.size()) {
        const size_t n = std::min(x.size() - at, (size_t)(300 + 977 * (k++ % 5)));
        rx.pushSideband(x.data() + at, n);
        at += n;
    }
    rx.flush();
    return rx.take();
}

std::vector<uint8_t> randomPayload(int mode, uint64_t seed) {
    PortableRng r(seed);
    std::vector<uint8_t> p((size_t)geometry(mode).payloadBytes);
    for (auto& b : p) b = (uint8_t)r();
    return p;
}

} // namespace

int main() {
    // the geometry Mercury's ofdm_create computes (hand-checked from modem/freedv/ofdm.c and ofdm_mode.c)
    {
        const Geometry& g16 = geometry(kDatac16);
        CHECK(g16.M == 128 && g16.Ncp == 48 && g16.nlower == 22, "DATAC16 M %d Ncp %d nlower %d", g16.M, g16.Ncp, g16.nlower);
        CHECK(g16.uwSym.size() == 16 && g16.uwSym[0] == 2 && g16.uwSym[1] == 4 && g16.uwSym[15] == 32, "DATAC16 unique word at %d %d .. %d", g16.uwSym[0], g16.uwSym[1], g16.uwSym.back());
        const Geometry& g1 = geometry(kDatac1);
        CHECK(g1.nlower == 10 && g1.interleaverB == interleaverStep(4096), "DATAC1 nlower %d", g1.nlower);
        const Geometry& gq = geometry(kQam16c2);
        CHECK(gq.Ncp == 32 && gq.nuwsyms == 42 && gq.uwSym[0] == 8 && gq.uwSym[1] == 17, "QAM16C2 Ncp %d uw %d %d", gq.Ncp, gq.uwSym[0], gq.uwSym[1]);
        const Geometry& g14 = geometry(kDatac14);
        CHECK(g14.M == 144 && g14.Ncp == 40 && g14.nlower == 24, "DATAC14 M %d Ncp %d nlower %d", g14.M, g14.Ncp, g14.nlower);
        // codec2 ofdm_rand: the first values of seed 2 (the preamble), computed by hand from 1103515245 x + 12345 mod 32768
        uint16_t r[3];
        ofdmRand(r, 3, 2);
        CHECK(r[0] == (uint16_t)((1103515245ull * 2 + 12345) % 32768), "ofdm_rand");
        // golden prime step: floor(n / 1.62), then the next prime above it
        CHECK(interleaverStep(320) == 199 && interleaverStep(1024) == 641, "interleaver steps %d %d", interleaverStep(320), interleaverStep(1024));
    }

    // every mode through noise (well above its threshold), +17.3 Hz, a random start and 60 ppm of sample clock
    const double snr[kNumOfdmModes] = {5, 10, 5, 0, -2, 0, -2, -2, 12, 22};
    for (int m = 0; m < kNumOfdmModes; m++) {
        const std::vector<uint8_t> pay = randomPayload(m, 100 + (uint64_t)m);
        std::vector<GenBurst> sc(1);
        sc[0].mode = m;
        sc[0].frame = makeFrame(m, pay.data(), pay.size(), m == kDatac3 ? 0xBEEF : 0);
        sc[0].gapSec = 0.8;
        ChannelConfig ch;
        ch.snrDb = snr[m]; ch.cfoHz = 17.3; ch.sroPpm = 60; ch.seed = 3 + (uint64_t)m;
        const std::vector<cf> x = renderScript(sc, ch, 0.37 + 0.011 * m);
        const std::vector<OfdmFrame> f = run(x);
        int ok = 0;
        for (const OfdmFrame& fr : f) {
            if (fr.mode != m || !fr.ldpcOk) continue;
            ok += fr.payload == pay;
            printf("  %-8s %+5.1f dB: SNR estimate %+5.1f dB, offset %+6.2f Hz, %d LDPC iterations, %d channel bit errors, uw errors %d, start %lld\n",
                   ofdmMode(m).name, snr[m], fr.snrDb, fr.cfoHz, fr.ldpcIters, fr.rawErrors, fr.uwErrors, (long long)fr.startSample);
            CHECK(std::fabs(fr.cfoHz - 17.3) < 0.6, "%s: offset %.2f", ofdmMode(m).name, fr.cfoHz);
            if (m == kDatac3) CHECK(!fr.crcPlain && (uint16_t)(fr.crcRx ^ fr.crcCalc) == 0xBEEF, "DATAC3: seed %04X", fr.crcRx ^ fr.crcCalc);
            else CHECK(fr.crcPlain, "%s: CRC", ofdmMode(m).name);
        }
        CHECK(ok == 1, "%s: %d frames decoded right of %zu reported", ofdmMode(m).name, ok, f.size());
    }

    // the whole demo script, no noise: every burst once, in order
    {
        const std::vector<GenBurst> sc = demoScript(0);
        ChannelConfig ch;
        ch.cfoHz = -8;
        const std::vector<OfdmFrame> f = run(renderScript(sc, ch));
        size_t ok = 0;
        for (const OfdmFrame& fr : f) ok += fr.ldpcOk;
        printf("  demo script: %zu bursts sent, %zu decoded, %zu reported\n", sc.size(), ok, f.size());
        CHECK(ok == sc.size() && f.size() == sc.size(), "demo script: %zu of %zu", ok, sc.size());
        for (size_t i = 0; i < f.size() && i < sc.size(); i++) CHECK(f[i].mode == sc[i].mode, "burst %zu: mode %d, sent %d", i, f[i].mode, sc[i].mode);
    }

    // the real 8 kHz audio a Mercury station puts out (the real part), through AudioToBaseband: the path of a sound card or a WAV file
    {
        std::vector<GenBurst> sc(2);
        for (int i = 0; i < 2; i++) {
            const int m = i ? kDatac1 : kDatac16;
            const std::vector<uint8_t> pay = randomPayload(m, 7 + (uint64_t)i);
            sc[(size_t)i].mode = m; sc[(size_t)i].frame = makeFrame(m, pay.data(), pay.size()); sc[(size_t)i].gapSec = 0.5;
        }
        ChannelConfig ch;
        ch.snrDb = 12; ch.cfoHz = -9;
        const std::vector<cf> x = renderScript(sc, ch, 0.4);
        std::vector<float> audio(x.size());
        for (size_t i = 0; i < x.size(); i++) audio[i] = x[i].real();
        AudioToBaseband ab;
        std::vector<cf> bb;
        ab.process(audio.data(), audio.size(), bb);
        OfdmReceiver rx;
        rx.push(bb.data(), bb.size());
        rx.flush();
        int ok = 0;
        for (const OfdmFrame& fr : rx.take()) ok += fr.ldpcOk && fr.crcPlain;
        CHECK(ok == 2, "audio path: %d of 2", ok);
    }

    // noise alone (60 s): nothing decodes
    {
        std::vector<cf> x((size_t)(60 * 8000));
        PortableRng r(99);
        for (cf& v : x) v = cf((float)r.gauss(0.1), (float)r.gauss(0.1));
        OfdmReceiver rx;
        rx.pushSideband(x.data(), x.size());
        rx.flush();
        int dec = 0;
        for (const OfdmFrame& fr : rx.take()) dec += fr.ldpcOk;
        printf("  60 s of noise: %llu bursts tried, %d decoded\n", (unsigned long long)rx.detections(), dec);
        CHECK(dec == 0, "noise decoded %d frames", dec);
        CHECK(rx.detections() <= 3, "noise: %llu decode attempts", (unsigned long long)rx.detections());
    }
    return testSummary("mercury phy");
}
