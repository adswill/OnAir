// GK-2A end to end: LRIT (BPSK 128 ksym/s) and HRIT (QPSK 3 Msym/s) test signals into the receiver at several sample rates, carrier
// offsets, symbol clock offsets, noise levels and carrier phases; every LRIT file must come back byte for byte, the pictures exact,
// the encrypted channel after the key; noise alone gives nothing.
#include "dect2/gk2a/gk2a_gen.h"
#include "dect2/gk2a/gk2a_rx.h"
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
using namespace dect2;
#include "../common/test_util.h"

static void runCase(const char* name, int link, double rate, double snr, double cfo, double ppm, double seconds, uint32_t seed, bool encrypt = false) {
    GoesGenConfig g;
    g.rate = rate; g.symbolRate = gk2aSymbolRate(link); g.qpsk = link == 1; g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.seed = seed; g.scid = 0x68;
    g.fillFrames = link ? 300 : 12;
    g.files = gk2aTestFiles(encrypt);
    GoesGenerator gen(g);
    Gk2aReceiver rx(link);
    rx.configure(rate);
    if (encrypt) rx.importKeys(gk2aTestKeyFile());
    std::vector<std::vector<uint8_t>> files;
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { files.push_back(f); });
    std::vector<cf32> buf(65536);
    Gk2aTelemetry t;
    uint64_t seq = 0;
    double cpu = 0;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        const auto t0 = std::chrono::steady_clock::now();
        rx.feed(buf.data(), buf.size());
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (rx.telemetry(t, seq)) seq = t.seq;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));     // the picture worker
    rx.telemetry(t, 0);
    printf("  %s: %s; cfo %.0f Hz (sent %.0f), sym %.2f ksym/s, Es/N0 %.1f dB, lock %.2f, frames %llu ok %llu bad, %llu files, CPU %.2f s for %.1f s (x%.2f real time)\n",
           name, gk2aSummary(t).c_str(), t.cfoHz, cfo, t.symbolRate / 1e3, t.snrDb, t.lockMetric, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
           (unsigned long long)t.files, cpu, seconds, cpu / seconds);
    CHECK(t.state == 3 && t.carrierLock, "%s: state %d", name, t.state);
    CHECK(std::fabs(t.cfoHz - cfo) < (link ? 2000 : 300), "%s: carrier %.0f", name, t.cfoHz);
    CHECK(t.qpsk == (link == 1), "%s: modulation", name);
    const std::vector<GoesTestFile> tf = gk2aTestFiles(encrypt);
    size_t exact = 0;
    for (const auto& f : files) { bool m = false; for (const auto& x : tf) m |= f == x.file; exact += m; }
    CHECK(files.size() >= tf.size() && exact == files.size(), "%s: %zu of %zu files byte exact (%zu sent)", name, exact, files.size(), tf.size());
    int pw, ph;
    const auto ir = gk2aTestPicture(0, pw, ph), vi = gk2aTestPicture(1, pw, ph);
    int got = 0;
    for (const auto& p : t.products) {
        std::lock_guard<std::mutex> l(p->mu);
        const std::vector<uint16_t>* pic = p->channelIdx == 12 ? &ir : p->channelIdx == 2 ? &vi : nullptr;
        if (!pic || !p->complete) continue;
        bool same = p->pix.size() == pic->size();
        for (size_t i = 0; same && i < pic->size(); i++) same = p->pix[i] == ((*pic)[i] >> 2);
        CHECK(same && !p->errors, "%s: picture of channel %d", name, p->channelIdx);
        got++;
    }
    CHECK(got >= 2, "%s: %d pictures", name, got);
    CHECK(t.crcErrors == 0 && t.fileErrors == 0, "%s: %llu CRC errors, %llu file errors", name, (unsigned long long)t.crcErrors, (unsigned long long)t.fileErrors);
}

int main() {
    runCase("LRIT 2.4 Msps, Es/N0 6 dB, +20 kHz, +25 ppm", 0, 2.4e6, 6, 20e3, 25, 12, 1);
    runCase("LRIT 1.0 Msps, Es/N0 3 dB, -9 kHz", 0, 1.0e6, 3, -9e3, -10, 12, 2);
    runCase("LRIT 20 Msps, Es/N0 5 dB, +60 kHz", 0, 20e6, 5, 60e3, 0, 12, 3);
    runCase("LRIT 2.4 Msps, Es/N0 5 dB, encrypted visible channel with the key", 0, 2.4e6, 5, 5e3, 0, 12, 4, true);
    runCase("HRIT 10 Msps, Es/N0 8 dB, +80 kHz, +15 ppm", 1, 10e6, 8, 80e3, 15, 1.5, 5);
    runCase("HRIT 10 Msps, Es/N0 5 dB, -150 kHz", 1, 10e6, 5, -150e3, -10, 1.5, 6);
    runCase("HRIT 8 Msps, Es/N0 6 dB, +40 kHz, seed 7", 1, 8e6, 6, 40e3, 0, 1.5, 7);
    runCase("HRIT 20 Msps, Es/N0 6 dB, +300 kHz, encrypted visible channel with the key", 1, 20e6, 6, 300e3, 0, 1.5, 8, true);
    {   // noise alone gives nothing
        for (int link = 0; link < 2; link++) {
            Gk2aReceiver rx(link);
            const double rate = link ? 10e6 : 2.4e6;
            rx.configure(rate);
            std::mt19937 rng(1);
            std::normal_distribution<float> n(0.f, 0.2f);
            std::vector<cf32> buf(65536);
            Gk2aTelemetry t;
            for (int i = 0; i < (int)(rate * (link ? 1.0 : 6.0) / 65536); i++) {
                for (auto& v : buf) v = cf32(n(rng), n(rng));
                rx.feed(buf.data(), buf.size());
            }
            rx.telemetry(t, 0);
            CHECK(t.blocksOk == 0 && t.products.empty(), "noise only (%s): %llu frames", gk2aLinkName(link), (unsigned long long)t.blocksOk);
            CHECK(!t.hint.empty(), "noise only: a hint is given");
        }
    }
    {   // the sample rate hint
        Gk2aReceiver rx(1);
        rx.configure(2.4e6);
        Gk2aTelemetry t;
        std::vector<cf32> buf(65536, cf32(0.01f, 0));
        rx.feed(buf.data(), buf.size());
        for (int i = 0; i < 14; i++) rx.feed(buf.data(), buf.size());
        CHECK(rx.telemetry(t, 0), "a report");
        CHECK(!rx.ready(), "HRIT is not ready at 2.4 Msps");
        CHECK(gk2aHint(t, true, 2.4e6).find("too low") != std::string::npos, "the rate hint");
    }
    return testSummary("gk2a rx");
}
