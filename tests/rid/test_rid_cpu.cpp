// Remote ID CPU work (3.9, docs/ADSB-REMOTEID-3.9.md): the burst gate in front of the Wi-Fi demodulator, the block front ends of the BLE and Wi-Fi
// receivers, the narrowband trigger check of the OFDM receiver and the decode of a finished 802.11b frame inside feed(). On the generator's signal:
//   1. the gate on and off decode the same Remote ID messages (BLE and Wi-Fi) at 30, 15 and 10 dB, with carrier offsets of +-40 kHz
//   2. noise alone: the gate hands the Wi-Fi demodulator only a small part of it
//   3. the Wi-Fi receiver finds the same frames whatever the size of the blocks it is handed
//   4. a Bluetooth packet (narrowband) no longer starts the OFDM receiver's long-training search
//   5. CPU seconds per second of signal, gate on and off (printed, not checked: the machine may be busy)
// Simulated signals only. ONAIR_TEST_LONG=1 adds more SNRs and offsets.
#include "dect2/rid/rid_gen.h"
#include "dect2/rid/rid_rx.h"
#include "dect2/rid/rid_tel.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/common/cpu_meter.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<cf32> gen(double secs, double snr, double cfo, bool ble, bool wifi, uint32_t seed = 1) {
    RidGenConfig c;
    c.snrDb = snr; c.cfoHz = cfo; c.ble = ble; c.wifi = wifi; c.seed = seed;
    RidGenerator g(c);
    std::vector<cf32> x((size_t)(secs * 20e6));
    g.generate(x.data(), x.size());
    return x;
}

struct Run { RidTelemetry tel; double cpu = 0; };
static Run run(const std::vector<cf32>& x, bool gate, size_t chunk = 262144) {
    RidReceiver rx;
    rx.configure(20e6);
    rx.setCenterHz(2432e6);
    rx.setGate(gate);
    const double t0 = (double)cpu::processNs() * 1e-9;
    for (size_t i = 0; i < x.size(); i += chunk) rx.feed(x.data() + i, std::min(chunk, x.size() - i));
    Run r;
    r.cpu = ((double)cpu::processNs() * 1e-9 - t0) / ((double)x.size() / 20e6);
    rx.telemetry(r.tel, 0);
    return r;
}

static WifiTelemetry wifiRun(const std::vector<cf32>& x, size_t chunk) {
    WifiReceiver w;
    w.configure(20e6);
    w.setCenterHz(2432e6);
    w.setSignalOffset(5e6);   // channel 6 from a radio at 2432 MHz (ridPlan)
    for (size_t i = 0; i < x.size(); i += chunk) w.feed(x.data() + i, std::min(chunk, x.size() - i));
    WifiTelemetry t;
    w.telemetry(t, 0);
    return t;
}

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    const double secs = longRun ? 2.0 : 0.8;

    // 1. gate on and off
    std::vector<std::pair<double, double>> cases = {{30, 0}, {15, 40e3}, {10, -40e3}};
    if (longRun) for (double s : {8.0, 12.0, 20.0}) for (double f : {-80e3, 0.0, 80e3}) cases.push_back({s, f});
    for (const auto& k : cases) {
        const auto x = gen(secs, k.first, k.second, true, true);
        const Run off = run(x, false), on = run(x, true);
        printf("%2.0f dB %+3.0f kHz: messages %llu / %llu (BLE %llu / %llu, Wi-Fi %llu / %llu), Wi-Fi fed %.2f, CPU %.3f -> %.3f s/s\n", k.first, k.second / 1e3,
               (unsigned long long)off.tel.blocksOk, (unsigned long long)on.tel.blocksOk, (unsigned long long)off.tel.bleRid, (unsigned long long)on.tel.bleRid,
               (unsigned long long)off.tel.wifiRid, (unsigned long long)on.tel.wifiRid, on.tel.wifiFed, off.cpu, on.cpu);
        CHECK(on.tel.bleRid == off.tel.bleRid, "%.0f dB: BLE %llu -> %llu", k.first, (unsigned long long)off.tel.bleRid, (unsigned long long)on.tel.bleRid);
        CHECK(on.tel.wifiRid + 1 >= off.tel.wifiRid, "%.0f dB: Wi-Fi %llu -> %llu", k.first, (unsigned long long)off.tel.wifiRid, (unsigned long long)on.tel.wifiRid);
        if (k.first >= 15) CHECK(on.tel.drones.size() == 2, "%.0f dB: %zu drones", k.first, on.tel.drones.size());
        CHECK(std::fabs(on.tel.levelDb - off.tel.levelDb) < 0.5, "level %.1f -> %.1f dB", off.tel.levelDb, on.tel.levelDb);
    }

    // 2. noise alone: the Wi-Fi demodulator gets a small part of it
    {
        const auto x = gen(0.5, 30, 0, false, false);
        const Run on = run(x, true);
        printf("noise only: Wi-Fi fed %.2f, CPU %.3f s/s, messages %llu\n", on.tel.wifiFed, on.cpu, (unsigned long long)on.tel.blocksOk);
        CHECK(on.tel.wifiFed < 0.2f, "noise only: Wi-Fi fed %.2f", on.tel.wifiFed);
        CHECK(on.tel.blocksOk == 0, "noise only: %llu messages", (unsigned long long)on.tel.blocksOk);
    }

    // 3. the 802.11b frames do not depend on the block size (a finished frame is decoded inside feed(), not at its end)
    {
        const auto x = gen(secs, 30, 0, false, true);
        const WifiTelemetry a = wifiRun(x, 4096), b = wifiRun(x, x.size());
        printf("Wi-Fi blocks of 4096 / all at once: headers %llu / %llu, frames %llu / %llu\n", (unsigned long long)a.headersOk, (unsigned long long)b.headersOk,
               (unsigned long long)a.blocksOk, (unsigned long long)b.blocksOk);
        CHECK(a.headersOk == b.headersOk && a.blocksOk == b.blocksOk, "block size changes the frames: %llu / %llu", (unsigned long long)a.blocksOk, (unsigned long long)b.blocksOk);
        CHECK(a.blocksOk >= 10, "only %llu frames", (unsigned long long)a.blocksOk);
    }

    // 4. Bluetooth alone: few OFDM triggers (each one was a long-training search)
    {
        const auto x = gen(secs, 30, 0, true, false);
        const WifiTelemetry t = wifiRun(x, 65536);
        const double perSec = (double)t.ofdmTriggers / secs;
        printf("BLE only: %.0f OFDM triggers a second\n", perSec);
        CHECK(perSec < 1500, "%.0f OFDM triggers a second on BLE alone", perSec);
    }
    printf(fails ? "FAILED (%d)\n" : "rid cpu: all passed\n", fails);
    return fails ? 1 : 0;
}
