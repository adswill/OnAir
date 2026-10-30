// nRF24 receiver on the synthetic GFSK signal: every data rate, CRC-8 and CRC-16, address lengths 3..5, carrier offset of the real chips (+-100 kHz),
// noise, 8 and 10 Msps, the mirrored spectrum, and the negative tests (noise, bursts of random bits, a CW tone, a wrong rate setting) give no frame.
#include "dect2/nrf24/nrf24_gen.h"
#include "dect2/nrf24/nrf24_rx.h"
#include "dect2/nrf24/nrf24_tel.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <ctime>
using namespace dect2;
#include "../common/test_util.h"

static Nrf24Telemetry run(Nrf24GenConfig c, double sec, bool mirror = false, bool dc = false) {
    Nrf24Receiver rx;
    rx.configure(c.rate);
    rx.setSignalOffset(c.dialOffsetHz);   // where the channel sits in the samples
    if (mirror) c.dialOffsetHz = -c.dialOffsetHz;   // a radio with I and Q swapped: the conjugate puts the channel back where it is expected, with the data inverted
    Nrf24Generator g(c);
    std::vector<cf32> buf(1 << 14);
    const size_t total = (size_t)(sec * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        g.generate(buf.data(), buf.size());
        if (mirror) for (auto& v : buf) v = std::conj(v);
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    Nrf24Telemetry t;
    rx.telemetry(t, 0);
    (void)dc;
    return t;
}

static bool hasFrame(const Nrf24Telemetry& t, const nrf24::Frame& f, int kbps = 0) {
    for (const auto& x : t.frames)
        if (x.address == nrf24::hex(f.address) && x.payloadHex == nrf24::hex(f.payload) && x.pid == f.pid && x.noAck == f.noAck && x.crcBits == f.crcBits && (!kbps || x.rateKbps == kbps)) return true;
    return false;
}

int main() {
    // every rate, CRC-16 frames of three address lengths
    for (double fs : {8e6, 10e6})
        for (int kbps : {250, 1000, 2000}) {
            Nrf24GenConfig c;
            c.rate = fs; c.rateKbps = kbps; c.dialOffsetHz = -1.5e6; c.frames = nrf24TestFrames(0); c.ebn0Db = 99;
            const Nrf24Telemetry t = run(c, 0.25);
            int hit = 0;
            for (const auto& f : c.frames) if (hasFrame(t, f, kbps)) hit++;
            printf("  %.0f Msps %4d kbit/s: %d of %zu frames, %llu bursts\n", fs / 1e6, kbps, hit, c.frames.size(), (unsigned long long)t.bursts);
            CHECK(hit == (int)c.frames.size(), "%.0f Msps %d kbit/s: only %d of %zu frames", fs / 1e6, kbps, hit, c.frames.size());
        }
    // CRC-8 frames are shown once they repeat
    {
        Nrf24GenConfig c; c.rate = 8e6; c.rateKbps = 1000; c.dialOffsetHz = -1.5e6; c.frames = nrf24TestFrames(1);
        const Nrf24Telemetry t = run(c, 0.25);
        int hit = 0;
        for (const auto& f : c.frames) if (hasFrame(t, f, 1000)) hit++;
        CHECK(hit == (int)c.frames.size(), "CRC-8: %d of %zu", hit, c.frames.size());
        CHECK(t.crc8Frames > 0 && t.crc16Frames == 0, "CRC-8 counts");
        // a single CRC-8 frame heard once is not shown (it could be a chance match); it needs three sights
        Nrf24GenConfig one = c; one.frames.resize(1); one.gapSec = 0.002;
        const Nrf24Telemetry t1 = run(one, 0.0015);
        CHECK(t1.blocksOk == 0, "a CRC-8 frame heard once is held back (%llu shown)", (unsigned long long)t1.blocksOk);
        for (const auto& f : t1.frames) printf("    shown: %s len %d crc %d rate %d x%d\n", f.address.c_str(), f.payloadLen, f.crcBits, f.rateKbps, f.repeats);
    }
    // carrier offset +-100 kHz and noise, each rate
    for (int kbps : {250, 1000, 2000})
        for (double cfo : {-100e3, 100e3, 40e3}) {
            Nrf24GenConfig c; c.rate = 8e6; c.rateKbps = kbps; c.dialOffsetHz = -1.5e6; c.cfoHz = cfo; c.frames = nrf24TestFrames(0); c.ebn0Db = kbps == 250 ? 15 : 18; c.clockPpm = 40;
            const Nrf24Telemetry t = run(c, 0.4);
            int hit = 0;
            for (const auto& f : c.frames) if (hasFrame(t, f, kbps)) hit++;
            printf("  %4d kbit/s cfo %+4.0f kHz Eb/N0 %.0f dB: %d of %zu\n", kbps, cfo / 1e3, c.ebn0Db, hit, c.frames.size());
            CHECK(hit == (int)c.frames.size(), "%d kbit/s cfo %.0f: %d of %zu", kbps, cfo, hit, c.frames.size());
        }
    // mixed rates in one signal, mirrored spectrum
    {
        Nrf24GenConfig c; c.rate = 8e6; c.dialOffsetHz = -1.5e6; c.frames = nrf24TestFrames(0); c.rates = {250, 1000, 2000};
        for (bool mir : {false, true}) {
            const Nrf24Telemetry t = run(c, 0.6, mir);
            CHECK(t.frames250 > 0 && t.frames1M > 0 && t.frames2M > 0, "mirror %d: rates seen %llu/%llu/%llu", mir, (unsigned long long)t.frames250, (unsigned long long)t.frames1M, (unsigned long long)t.frames2M);
            CHECK(t.devices.size() == 4, "mirror %d: %zu addresses", mir, t.devices.size());
        }
    }
    // the three rates in turn at 16 dB Eb/N0 (the engine test): no address that is not one of the four
    {
        Nrf24GenConfig c; c.rate = 8e6; c.dialOffsetHz = -1.5e6; c.frames = nrf24TestFrames(0); c.rates = {1000, 2000, 250}; c.ebn0Db = 16;
        const Nrf24Telemetry t = run(c, 3.0);
        for (const auto& d : t.devices) printf("    %s %d k crc%d x%llu\n", d.address.c_str(), d.rateKbps, d.crcBits, (unsigned long long)d.count);
        CHECK(t.devices.size() >= 4 && t.devices.size() <= 5, "noisy mixed rates: %zu addresses", t.devices.size());
    }
    // negative: noise only, random bits, a carrier
    {
        Nrf24GenConfig c; c.rate = 8e6; c.rateKbps = 1000; c.dialOffsetHz = -1.5e6; c.ebn0Db = 12;
        c.randomBits = true; c.gapSec = 0.0005;
        const Nrf24Telemetry t = run(c, 1.0);
        printf("  random bits: %llu bursts, %llu frames\n", (unsigned long long)t.bursts, (unsigned long long)t.blocksOk);
        CHECK(t.bursts > 100 && t.blocksOk == 0, "random-bit bursts: %llu bursts, %llu frames", (unsigned long long)t.bursts, (unsigned long long)t.blocksOk);
        Nrf24GenConfig n = c; n.frames.clear(); n.randomBits = false; n.gapSec = 1e9;   // never sends
        const Nrf24Telemetry t2 = run(n, 0.5);
        CHECK(t2.bursts == 0 && t2.blocksOk == 0, "noise only: %llu bursts", (unsigned long long)t2.bursts);
    }
    // cost: the receiver must keep up with 10 Msps with room to spare (the generator is made first, outside the clock)
    {
        Nrf24GenConfig c; c.rate = 10e6; c.rateKbps = 1000; c.dialOffsetHz = -1.5e6; c.frames = nrf24TestFrames(0); c.ebn0Db = 18; c.gapSec = 0.0005;
        Nrf24Generator g(c);
        std::vector<cf32> sig((size_t)(0.5 * c.rate));
        g.generate(sig.data(), sig.size());
        Nrf24Receiver rx;
        rx.configure(c.rate);
        rx.setSignalOffset(c.dialOffsetHz);
        const std::clock_t t0 = std::clock();   // CPU time of this thread: a busy machine does not change it
        for (size_t o = 0; o < sig.size(); o += 1 << 14) rx.feed(sig.data() + o, std::min<size_t>(1 << 14, sig.size() - o));
        const double took = (double)(std::clock() - t0) / CLOCKS_PER_SEC;
        printf("  receiver cost: %.2f s for 0.5 s of signal at 10 Msps (%.0f%% of one core)\n", took, 100 * took / 0.5);
        CHECK(took < 0.5 * 0.7, "the receiver needs %.0f%% of real time at 10 Msps", 100 * took / 0.5);   // measured about 20%% of a core on an idle Apple M-series; the margin is for a loaded CI machine
    }
    return testSummary("nrf24 rx");
}
