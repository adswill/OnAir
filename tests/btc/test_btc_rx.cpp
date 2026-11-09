// Bluetooth Classic discovery receiver over the air (synthetic): the test scene (two phones' worth of inquiry responses, a scanner, two piconets) at
// 20 / 10 / 8 / 4 Msps through the receiver with the faults of a real band: carrier offsets, noise, a wideband burst (Wi-Fi), cut-off packets, a strong
// neighbour. Every LAP the receiver reports must be one the scene sends (a wrong one fails every case); every response it decodes must carry the
// right address and class of device; the clean cases must find everything.
// Simulated signals only: no recording of Bluetooth Classic was available.
#include "dect2/btc/btc_gen.h"
#include "dect2/btc/btc_rx.h"
#include "dect2/btc/btc_tel.h"
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
using namespace dect2;
#include "../common/test_util.h"

// the channels a capture covers: the whole 1 MHz channel inside the rate (the receiver's rule)
static bool covered(const BtcGenDevice& d, double rate, double centerHz) { return std::fabs(2402e6 + d.channel * 1e6 - centerHz) <= rate / 2 - 0.6e6; }

struct Result { size_t want = 0, wantFhs = 0; std::set<uint32_t> laps; std::set<uint32_t> fhs; int hits = 0, wrong = 0, wrongFhs = 0; BtcTelemetry tel; };

static Result run(const BtcGenConfig& cfg, double sec, double centerHz) {
    Result r;
    std::set<uint32_t> truth;
    for (const auto& d : cfg.devs) truth.insert(d.kind == BtcGenDevice::Response ? d.fhs.lap : d.lap);
    std::set<uint32_t> want;
    for (const auto& d : cfg.devs) if (covered(d, cfg.rate, centerHz)) want.insert(d.kind == BtcGenDevice::Response ? d.fhs.lap : d.lap);
    r.want = want.size();
    for (const auto& d : cfg.devs) if (d.kind == BtcGenDevice::Response && covered(d, cfg.rate, centerHz)) r.wantFhs++;
    BtcReceiver rx;
    rx.configure(cfg.rate);
    rx.setCenterHz(centerHz);
    rx.setHitCallback([&](uint32_t lap, double, int, bool fhs) {
        r.hits++;
        if (truth.count(lap)) { r.laps.insert(lap); if (fhs) r.fhs.insert(lap); } else { r.wrong++; printf("    WRONG LAP %06X\n", lap); }
    });
    BtcGenerator gen(cfg);
    std::vector<cf32> buf(1 << 16);
    const size_t total = (size_t)(sec * cfg.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t n = std::min(buf.size(), total - done);
        gen.generate(buf.data(), n);
        rx.feed(buf.data(), n);
    }
    rx.telemetry(r.tel, 0);
    for (const auto& d : r.tel.devices)
        if (d.hasFhs) {
            bool ok = false;
            for (const auto& g : cfg.devs)
                if (g.kind == BtcGenDevice::Response && g.fhs.lap == d.lap) ok = d.addr == btc::bdAddr(g.fhs) && d.codRaw == g.fhs.cod && d.clk == g.fhs.clk;
            if (!ok) { r.wrongFhs++; printf("    WRONG FHS %s %s\n", d.addr.c_str(), d.cod.c_str()); }
        }
    return r;
}

static BtcGenConfig scene(double rate, double snr = 30) {
    BtcGenConfig c;
    c.rate = rate; c.snrDb = snr; c.devs = btcTestScene();
    return c;
}
static void row(const char* name, const Result& r, size_t n) {
    printf("  %-34s %zu/%zu LAPs, %zu responses decoded, %d hits, %d wrong, bursts %llu (wide %llu), bad %llu\n", name, r.laps.size(), n, r.fhs.size(), r.hits, r.wrong,
           (unsigned long long)r.tel.burstsSeen, (unsigned long long)r.tel.burstsWide, (unsigned long long)r.tel.blocksBad);
}

int main() {
    for (double rate : {20e6, 10e6, 8e6, 4e6}) {
        BtcGenConfig c = scene(rate);
        if (rate < 8e6) { for (auto& d : c.devs) d.channel = 39 + (d.channel % 3) - 1; }   // 4 Msps covers 3 channels
        const Result r = run(c, 3.0, 2441e6);
        char nm[48]; snprintf(nm, sizeof nm, "clean, %.0f Msps", rate / 1e6);
        row(nm, r, r.want);
        CHECK(r.laps.size() >= r.want && r.wrong == 0 && r.wrongFhs == 0, "%s: %zu / %zu LAPs, %d wrong", nm, r.laps.size(), r.want, r.wrong);
        CHECK(r.fhs.size() >= r.wantFhs, "%s: %zu / %zu responses decoded", nm, r.fhs.size(), r.wantFhs);
        if (rate == 20e6) {
            CHECK(r.tel.giacIds > 0 && r.tel.fhsCount > 0 && r.tel.channelsCovered >= 18, "counters: ids %llu fhs %llu channels %d", (unsigned long long)r.tel.giacIds, (unsigned long long)r.tel.fhsCount, r.tel.channelsCovered);
            bool kinds = false;
            for (const auto& d : r.tel.devices) if (d.lap == 0xA1B2C3 && d.hasFhs && d.major == "Phone" && d.cod.find("smartphone") != std::string::npos) kinds = true;
            CHECK(kinds, "the phone's class of device");
        }
    }
    for (double off : {50e3, -50e3, 100e3}) {   // the radio's tuning error on top of each device's own offset
        BtcGenConfig c = scene(20e6); c.cfoHz = off;
        const Result r = run(c, 3.0, 2441e6);
        char nm[48]; snprintf(nm, sizeof nm, "radio offset %+.0f kHz", off / 1e3);
        row(nm, r, r.want);
        CHECK(r.laps.size() >= r.want && r.wrong == 0 && r.wrongFhs == 0, "%s: %zu LAPs, %d wrong", nm, r.laps.size(), r.wrong);
    }
    for (double snr : {22.0, 16.0, 10.0}) {
        const Result r = run(scene(20e6, snr), 6.0, 2441e6);
        char nm[48]; snprintf(nm, sizeof nm, "%.0f dB C/N in 1 MHz", snr);
        row(nm, r, r.want);
        CHECK(r.wrong == 0 && r.wrongFhs == 0, "%s: %d wrong", nm, r.wrong);
        if (snr >= 16) CHECK(r.laps.size() == r.want, "%s: %zu / %zu", nm, r.laps.size(), r.want);
        else CHECK(r.laps.size() >= 1, "%s: nothing heard", nm);
    }
    {   // a 20 MHz wide burst every second: skipped as wideband, nothing wrong
        BtcGenConfig c = scene(20e6); c.wifiAt = 0.37; c.wifiDb = 12;
        const Result r = run(c, 3.0, 2441e6);
        row("Wi-Fi bursts", r, r.want);
        CHECK(r.wrong == 0 && r.laps.size() == r.want, "wifi: %zu LAPs, %d wrong", r.laps.size(), r.wrong);
        CHECK(r.tel.burstsWide > 0, "wideband bursts counted: %llu", (unsigned long long)r.tel.burstsWide);
    }
    {   // cut-off packets: nothing wrong (the access code may still be whole, so a LAP can be heard: it must be a right one)
        BtcGenConfig c = scene(20e6);
        for (auto& d : c.devs) d.truncate = 0.4;
        const Result r = run(c, 3.0, 2441e6);
        row("cut-off packets (40 %)", r, r.want);
        CHECK(r.wrong == 0 && r.wrongFhs == 0, "cut: %d wrong", r.wrong);
    }
    {   // a strong device on the next channel (30 dB louder)
        BtcGenConfig c = scene(20e6);
        BtcGenDevice loud; loud.kind = BtcGenDevice::Piconet; loud.lap = 0x777777; loud.channel = 39; loud.levelDb = 30; loud.period = 0.37; loud.phase = 0.011;
        c.devs.push_back(loud);
        const Result r = run(c, 3.0, 2441e6);
        row("loud neighbour (+30 dB)", r, r.want);
        CHECK(r.wrong == 0 && r.wrongFhs == 0 && r.laps.count(0x777777), "loud: %d wrong, loud heard %d", r.wrong, (int)r.laps.count(0x777777));
    }
    {   // noise and silence
        BtcGenConfig c = scene(20e6, 20); c.devs.clear();
        const Result r = run(c, 3.0, 2441e6);
        CHECK(r.hits == 0, "noise: %d hits", r.hits);
        BtcGenConfig z = scene(20e6, 99); z.devs.clear();
        CHECK(run(z, 0.5, 2441e6).hits == 0, "zeros");
        printf("  noise only: %d hits\n", r.hits);
    }
    {   // the centre need not be 2441: 2437 MHz and an unknown centre (a file: the centre is taken as 2441)
        BtcGenConfig c = scene(10e6); c.centerHz = 2438e6;
        const Result r = run(c, 3.0, 2438e6);
        row("centre 2438 MHz, 10 Msps", r, r.want);
        CHECK(r.laps.size() >= r.want && r.wrong == 0, "centre 2438: %zu / %zu LAPs", r.laps.size(), r.want);
    }
    return testSummary("btc rx");
}
