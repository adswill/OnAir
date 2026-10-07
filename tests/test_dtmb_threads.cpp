// DTMB receiver as the engine uses it: feed() runs on one thread while another reads telemetry, resets the receiver and changes the number of decoder
// threads. Meant for a thread sanitiser build as much as for the check itself: the signal must still lock before the reset and again after it, the telemetry
// sequence must only grow, and the packets must stay right.
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_rx.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace dect2;
using namespace dect2::dtmb;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main() {
    SignalConfig sc;
    sc.rate = 10e6; sc.snrDb = 30;
    sc.tx.header = Header::Pn420; sc.tx.profile.map = Mapping::Qam16; sc.tx.profile.rate = Rate::R06;
    Signal sig(sc, testPacketSource(1));
    const size_t total = (size_t)(1.6 * sc.rate);
    std::vector<cf32> x(total);
    sig.generate(x.data(), total);
    for (auto& v : x) v = cf32(std::round(v.real() * 128.f) / 128.f, std::round(v.imag() * 128.f) / 128.f);

    DtmbReceiver rx;
    rx.configure(sc.rate);
    rx.setDecoderThreads(2);
    std::atomic<uint64_t> good{0}, wrong{0}, logs{0};
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            uint32_t num = 0xFFFFFFFFu;
            if (checkTestPacket(p + i * 188, 1, &num)) good++;
            else wrong++;
        }
    });
    rx.setLogCallback([&](const std::string&) { logs++; });

    std::atomic<bool> feeding{true};
    std::atomic<size_t> fed{0};
    std::thread feeder([&] {
        static const size_t sizes[] = {16384, 4096, 65536, 7, 1000, 12345};
        size_t pos = 0, k = 0;
        while (pos < total) {
            const size_t n = std::min(sizes[k++ % 6], total - pos);
            rx.feed(&x[pos], n);
            pos += n;
            fed = pos;
        }
        feeding = false;
    });
    uint64_t last = 0, seqBack = 0, polls = 0;
    bool resetDone = false;
    uint64_t goodBeforeReset = 0;
    DtmbTelemetry t;
    while (feeding) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        if (rx.telemetry(t, last)) { if (t.seq < last) seqBack++; last = t.seq; }
        polls++;
        if (polls % 7 == 0) rx.setDecoderThreads((int)(polls / 7) % 4);
        if (!resetDone && fed > total * 45 / 100) { goodBeforeReset = good; rx.reset(); resetDone = true; }
    }
    feeder.join();
    rx.flush();
    rx.telemetry(t, 0);
    printf("  polls %llu, telemetry up to %llu, good %llu, wrong %llu, before the reset %llu, log lines %llu\n", (unsigned long long)polls, (unsigned long long)last, (unsigned long long)good.load(),
           (unsigned long long)wrong.load(), (unsigned long long)goodBeforeReset, (unsigned long long)logs.load());
    CHECK(seqBack == 0, "telemetry sequence went back");
    CHECK(wrong == 0, "%llu wrong packets", (unsigned long long)wrong.load());
    CHECK(goodBeforeReset > 100, "no lock before the reset (%llu packets)", (unsigned long long)goodBeforeReset);
    CHECK(good > goodBeforeReset + 100, "no lock after the reset (%llu packets in all)", (unsigned long long)good.load());
    printf(failures ? "dtmb_threads: %d FAILED\n" : "dtmb_threads: all passed\n", failures);
    return failures ? 1 : 0;
}
