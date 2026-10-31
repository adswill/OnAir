// Inmarsat Aero: a 10500 bit/s P channel in a crowded band. Ten strong narrow carriers (a busy 600 / 1200 bit/s group, or plain CW)
// sit in the capture next to the 10500 bit/s channel, which spreads its power over ~10 kHz and so shows a far lower peak. The receiver
// must still find it, find its rate on its own and decode it, with a carrier offset, at low Eb/N0, and with the carriers
// arriving before it. Also: noise alone gives no channels.
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void run(const char* name, double rate, double ebn0, double cfo, int nTones, double secs) {
    SynthConfig cfg;
    cfg.mode = 20;
    cfg.modeOpt[0] = 4;               // 10500 bit/s only
    cfg.modeOpt[1] = 9;
    cfg.modeVal[0] = ebn0;
    cfg.cfoHz = cfo;
    AeroSynth syn(cfg, rate);
    AeroReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
    const double sig = syn.noiseSigma();
    const double want = syn.channels()[0].offsetHz + cfo;
    std::vector<double> f, ph(nTones, 0.0), amp;
    for (int i = 0; i < nTones; i++) {
        double o = -1.4e6 + i * (2.8e6 / nTones) + 7300;
        if (std::fabs(o - (want - aeroTuning().tuneOffsetHz)) < 60000) o += 120000;
        f.push_back(o);
        amp.push_back(sig * 4.0);     // far stronger per bin than the OQPSK channel
    }
    std::vector<cf32> buf(65536);
    uint64_t t = 0;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        syn.generate(buf.data(), buf.size());
        for (size_t k = 0; k < buf.size(); k++, t++) {
            const double tm = (double)t / rate;
            for (int i = 0; i < nTones; i++) {
                const double p = 2 * M_PI * (f[(size_t)i] * tm + 0.4 * std::sin(0.7 * tm + i));      // slow phase wander
                buf[k] += cf32((float)(amp[(size_t)i] * std::cos(p)), (float)(amp[(size_t)i] * std::sin(p)));
            }
        }
        rx.feed(buf.data(), buf.size());
    }
    AeroTelemetry tel;
    rx.telemetry(tel, 0);
    int got = 0;
    for (const auto& c : tel.channels) if (c.bitRate == 10500 && c.state == 3 && std::fabs(c.offsetHz - (want - aeroTuning().tuneOffsetHz + aeroTuning().tuneOffsetHz)) < 5000) got++;
    printf("%-34s %zu carriers, 10500 bit/s decoded: %d\n", name, tel.channels.size(), got);
    for (const auto& c : tel.channels) if (c.bitRate == 10500) printf("    10500 bit/s %+9.1f Hz Eb/N0 %.1f SUs %llu/%llu\n", c.offsetHz, c.ebn0Db, (unsigned long long)c.susOk, (unsigned long long)c.susBad);
    CHECK(got == 1, "%s: 10500 bit/s channel decoded %d times", name, got);
}

int main() {
    run("2.4 Msps, 10 tones, clean", 2.4e6, 12, 0, 10, 40);
    run("4 Msps, 10 tones, cfo +4.3 kHz", 4e6, 10, 4300, 10, 40);
    run("4 Msps, 14 tones, 6 dB", 4e6, 6, -3100, 14, 50);
    // noise alone: no channel
    {
        SynthConfig cfg; cfg.mode = 20; cfg.modeOpt[0] = 0; cfg.modeOpt[1] = 3;
        AeroReceiver rx; rx.configure(2.4e6); rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
        std::vector<cf32> buf(65536);
        uint64_t s = 12345;
        auto g = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (float)((s >> 11) & 0xFFFFFF) / 16777216.f - 0.5f; };
        for (double done = 0; done < 30 * 2.4e6; done += (double)buf.size()) {
            for (auto& v : buf) v = cf32(g() * 2, g() * 2);
            rx.feed(buf.data(), buf.size());
        }
        AeroTelemetry tel; rx.telemetry(tel, 0);
        size_t synced = 0;
        for (const auto& c : tel.channels) synced += c.state >= 2;
        CHECK(synced == 0, "noise only: %zu channels in sync", synced);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
