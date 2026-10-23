// DVB-S / S2 / S2X narrow carriers, 10 kSym/s to 2 MSym/s, in one wide capture (2.4 and 8 Msps): the repo's modulators (dvbs_tx) make every carrier,
// they are added with an LNB-like frequency error, a slow drift and white noise, and each carrier is decoded by a receiver of its own the way the
// survey hands it over (blind: symbol rate, roll-off and standard are found by the receiver). The transport stream of every carrier must come out
// bit-exact; the lock times are printed.
//   test_dvbs_narrow              the checks (about a minute on one core)
//   test_dvbs_narrow threshold    the lowest Es/N0 that still decodes cleanly, per symbol rate (a bench, not a check; a few minutes)
#include "dect2/dvbs/dvbs_testkit.h"
#include "dect2/dvbs/dvbs_s2.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dvbs;
using testjobs::jprintf;

static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: "); jprintf(__VA_ARGS__); jprintf("\n"); fails++; } } while (0)

namespace {

struct Car {
    int standard = 1;            // 1 DVB-S, 2 DVB-S2, 3 DVB-S2X
    int mod = kQpsk, rate = 0;   // DVB-S: 0 = 1/2; S2: index of s2RateName (4 = 1/2); S2X: s2xRate()
    bool pilots = true, shortFrame = false;
    double rs = 1e6, offHz = 0, roll = 0.35, esn0 = 6;
    double clockPpm = 10;        // transmitter symbol clock error
};
struct CarOut {
    uint64_t good = 0, bad = 0, gaps = 0, expect = 0;
    bool have = false;
    double first = -1;           // input seconds of the first good packet
    DvbsTelemetry tel;
    std::vector<std::string> log;
};

struct Capture {
    double fs = 2.4e6;
    double lnbHz = 0;            // common frequency error of the down-converter (all carriers move together)
    double driftHzPerSec = 0;    // ... and its drift (an LNB warming up)
    double noiseRms = 0.06;
    double secs = 10;
    bool pick = true;            // each receiver is handed its carrier (offset as the survey shows it); false: one blind receiver per carrier, nothing set
    bool quantise = true;        // 8 bits like a HackRF
    uint32_t seed = 7;
};

std::vector<CarOut> runCapture(const Capture& cap, const std::vector<Car>& cars) {
    const size_t nc = cars.size();
    std::vector<std::unique_ptr<DvbsSignal>> sig(nc);
    std::vector<uint64_t> pn(nc, 0);
    for (size_t i = 0; i < nc; i++) {
        const Car& c = cars[i];
        DvbsSignalConfig s;
        s.tx.standard = c.standard; s.tx.mod = c.mod; s.tx.rate = c.rate; s.tx.pilots = c.pilots; s.tx.shortFrame = c.shortFrame;
        s.tx.rollOff = c.roll; s.tx.symbolRate = c.rs;
        s.sampleRate = cap.fs; s.snrDb = 200; s.cfoHz = c.offHz + cap.lnbHz; s.clockPpm = c.clockPpm;
        // Es / N0 = (P / Rs) / (noise power / fs)
        s.level = std::sqrt(std::pow(10.0, c.esn0 / 10.0) * cap.noiseRms * cap.noiseRms * c.rs / cap.fs);
        s.shaperHalfSpan = c.roll <= 0.15 ? 16 : 8;
        s.seed = cap.seed + (uint32_t)i;
        uint64_t* p = &pn[i];
        s.ts = [p](uint8_t* q) { testPacket((*p)++, q); };
        sig[i] = std::make_unique<DvbsSignal>(s);
    }
    std::vector<CarOut> out(nc);
    std::vector<std::unique_ptr<DvbsReceiver>> rx(nc);
    std::vector<uint64_t> seq(nc, 0);
    std::atomic<double> now{0};   // the packets come from the receivers' threads
    for (size_t i = 0; i < nc; i++) {
        rx[i] = std::make_unique<DvbsReceiver>();
        DvbsReceiver& r = *rx[i];
        r.setBlocking(true);
        r.configure(cap.fs);
        if (cap.pick) r.selectCarrier(true, cars[i].offHz + cap.lnbHz);
        CarOut* o = &out[i];
        r.setPacketCallback([o, &now](const uint8_t* p, size_t n, double) {
            for (size_t k = 0; k < n; k++, p += 188) {
                const uint64_t id = ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
                uint8_t e[188];
                testPacket(id, e);
                if ((p[1] & 0x80) || memcmp(p, e, 188) != 0) { o->bad++; continue; }
                if (!o->have) { o->have = true; o->first = now.load(); }
                else if (id != o->expect) o->gaps++;
                o->expect = id + 1;
                o->good++;
            }
        });
        r.setLogCallback([o](const std::string& s) { if (o->log.size() < 40) o->log.push_back(s); });
    }
    std::mt19937 rng(cap.seed);
    std::normal_distribution<float> nd(0.f, (float)(cap.noiseRms / std::sqrt(2.0)));
    const size_t chunk = 65536, total = (size_t)(cap.secs * cap.fs);
    std::vector<cf32> sum(chunk), one(chunk);
    double ph = 0;
    for (size_t done = 0; done < total; done += chunk) {
        const size_t m = std::min(chunk, total - done);
        for (size_t j = 0; j < m; j++) sum[j] = cf32(nd(rng), nd(rng));
        for (size_t i = 0; i < nc; i++) {
            sig[i]->generate(one.data(), m);
            for (size_t j = 0; j < m; j++) sum[j] += one[j];
        }
        for (size_t j = 0; j < m; j++) {
            if (cap.driftHzPerSec != 0) {
                const double t = (double)(done + j) / cap.fs;
                ph = 3.14159265358979 * cap.driftHzPerSec * t * t;   // the frequency rises by driftHzPerSec every second
                sum[j] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            if (cap.quantise) {
                auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
                sum[j] = cf32(q(sum[j].real()), q(sum[j].imag()));
            }
        }
        now = (double)(done + m) / cap.fs;   // (a packet is stamped with the end of the chunk that brought it)
        for (size_t i = 0; i < nc; i++) {
            rx[i]->feed(sum.data(), m);
            DvbsTelemetry t;
            if (rx[i]->telemetry(t, seq[i])) { seq[i] = t.seq; out[i].tel = t; }
        }
    }
    for (size_t i = 0; i < nc; i++) {
        rx[i]->flush();
        DvbsTelemetry t;
        if (rx[i]->telemetry(t, 0)) out[i].tel = t;
    }
    return out;
}

const char* stdName(int s) { return s == 1 ? "DVB-S" : s == 2 ? "DVB-S2" : "DVB-S2X"; }
std::string rateText(double rs) { char b[32]; if (rs >= 1e6) snprintf(b, sizeof b, "%.2f MSym/s", rs / 1e6); else snprintf(b, sizeof b, "%.0f kSym/s", rs / 1e3); return b; }

// symbols the receiver needs to lock (DVB-S: the hunt window and the sync; S2: three frames for the header search and one to decode) plus a margin
double lockSecs(const Car& c) {
    const double sym = c.standard == 1 ? 140000.0 : 200000.0;
    return std::max(1.5, sym / c.rs + 0.6);
}

void checkAll(const char* name, const Capture& cap, const std::vector<Car>& cars, const std::vector<CarOut>& out) {
    for (size_t i = 0; i < cars.size(); i++) {
        const Car& c = cars[i];
        const CarOut& o = out[i];
        const double after = cap.secs - (o.first < 0 ? cap.secs : o.first);
        const double expectPk = after * dvbsNetBitrate([&] { DvbsTxConfig t; t.standard = c.standard; t.mod = c.mod; t.rate = c.rate; t.pilots = c.pilots; t.shortFrame = c.shortFrame; t.rollOff = c.roll; t.symbolRate = c.rs; return t; }()) / (188.0 * 8.0);
        jprintf("  %-7s %-8s %+9.0f Hz  ro %.2f  Es/N0 %4.1f dB: lock %5.2f s (%5.0f symbols), %llu good, %llu bad, %llu gaps, rx %s %.1f kSym/s ro %.2f%s%s\n",
                stdName(c.standard), rateText(c.rs).c_str(), c.offHz + cap.lnbHz, c.roll, c.esn0, o.first, o.first > 0 ? o.first * c.rs : 0.0,
                (unsigned long long)o.good, (unsigned long long)o.bad, (unsigned long long)o.gaps, o.tel.standard ? stdName(o.tel.standard) : "-",
                o.tel.symbolRate / 1e3, o.tel.rollOff, o.tel.searchStatus.empty() ? "" : "; ", o.tel.searchStatus.c_str());
        CHECK(o.first > 0 && o.first < lockSecs(c), "%s: %s %s at %+.0f Hz locks within %.1f s (got %.2f)", name, stdName(c.standard), rateText(c.rs).c_str(), c.offHz + cap.lnbHz, lockSecs(c), o.first);
        CHECK(o.bad == 0 && o.gaps == 0, "%s: %s %s bit-exact after the lock (%llu bad, %llu gaps)", name, stdName(c.standard), rateText(c.rs).c_str(), (unsigned long long)o.bad, (unsigned long long)o.gaps);
        CHECK(o.first < 0 || (double)o.good > 0.7 * expectPk - 50, "%s: %s %s: %llu packets after the lock, about %.0f sent", name, stdName(c.standard), rateText(c.rs).c_str(), (unsigned long long)o.good, expectPk);
        CHECK(o.tel.standard == (c.standard == 1 ? 1 : c.standard == 3 ? 3 : 2) || o.first < 0, "%s: %s %s found as standard %d", name, stdName(c.standard), rateText(c.rs).c_str(), o.tel.standard);
        CHECK(o.first < 0 || std::fabs(o.tel.symbolRate / c.rs - 1.0) < 0.002, "%s: %s symbol rate measured %.1f Hz", name, rateText(c.rs).c_str(), o.tel.symbolRate);
        if (o.first < 0) for (const auto& l : o.log) jprintf("      log: %s\n", l.c_str());
    }
}

Car carS(double rs, double off, double roll = 0.35, double esn0 = 6.0) { Car c; c.standard = 1; c.rate = 0; c.rs = rs; c.offHz = off; c.roll = roll; c.esn0 = esn0; return c; }
Car carS2(double rs, double off, double roll = 0.35, double esn0 = 3.5) { Car c; c.standard = 2; c.mod = kQpsk; c.rate = 4; c.rs = rs; c.offHz = off; c.roll = roll; c.esn0 = esn0; return c; }
Car carS2x(double rs, double off, double roll = 0.05, double esn0 = 2.5) { Car c; c.standard = 3; c.mod = kQpsk; c.rate = s2xRate(kQpsk, false, 2); c.rs = rs; c.offHz = off; c.roll = roll; c.esn0 = esn0; return c; }

// the lowest Es/N0 (0.5 dB steps) at which a carrier alone in a 2.4 Msps capture locks and decodes cleanly
void threshold() {
    const double rates[] = {10e3, 25e3, 50e3, 100e3, 250e3, 500e3, 1e6};
    for (int st = 1; st <= 2; st++) {
        for (double rs : rates) {
            double best = 99;
            for (double e = st == 1 ? 6.0 : 3.5; e >= (st == 1 ? 2.0 : -0.5); e -= 0.5) {
                Capture cap; cap.fs = 2.4e6; cap.lnbHz = 31e3; cap.pick = false;
                Car c = st == 1 ? carS(rs, 0, 0.35, e) : carS2(rs, 0, 0.35, e);
                cap.secs = lockSecs(c) + std::max(2.0, 400000.0 / rs);
                const auto o = runCapture(cap, {c});
                if (o[0].first > 0 && o[0].bad == 0 && o[0].gaps == 0) best = e; else break;
            }
            printf("threshold %-6s %-12s Es/N0 %.1f dB\n", stdName(st), rateText(rs).c_str(), best);
            fflush(stdout);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "threshold")) { threshold(); return 0; }
    testjobs::Jobs jobs;

    // 1. one narrow carrier at a time, blind (nothing set, not even picked), 2.4 Msps, an LNB error of up to 100 kHz
    jobs.add([] {
        jprintf("single carriers, 2.4 Msps, blind:\n");
        const double rates[] = {10e3, 25e3, 50e3, 100e3, 250e3, 500e3, 1e6};
        const double lnb[] = {-97e3, 64e3, 100e3, -41e3, 12e3, -80e3, 55e3};
        for (int k = 0; k < 7; k++) {
            Capture cap; cap.fs = 2.4e6; cap.lnbHz = lnb[k]; cap.pick = false; cap.seed = 11 + k;
            const Car c = carS(rates[k], 0);
            cap.secs = lockSecs(c) + std::max(1.0, 60000.0 / c.rs);
            checkAll("single DVB-S", cap, {c}, runCapture(cap, {c}));
        }
    });
    jobs.add([] {
        jprintf("single DVB-S2 / S2X carriers, 2.4 Msps, blind:\n");
        struct { double rs; int st; double roll; double lnb; } v[] = {{10e3, 2, 0.20, 88e3}, {50e3, 3, 0.05, -70e3}, {250e3, 2, 0.10, 33e3}};
        for (const auto& q : v) {
            Capture cap; cap.fs = 2.4e6; cap.lnbHz = q.lnb; cap.pick = false; cap.driftHzPerSec = 5;
            const Car c = q.st == 2 ? carS2(q.rs, 0, q.roll) : carS2x(q.rs, 0, q.roll);
            cap.secs = lockSecs(c) + std::max(1.0, 70000.0 / c.rs);
            checkAll("single S2", cap, {c}, runCapture(cap, {c}));
        }
    });

    // 1b. the QO-100 DATV rates (125 and 333 kSym/s), off-centre, an LNB error and drift like an uncorrected PLL LNB, blind and picked
    jobs.add([] {
        jprintf("QO-100 DATV rates, 2.4 Msps:\n");
        struct { double rs; int st; double roll, off, lnb; bool pick; } v[] = {
            {125e3, 2, 0.35, 0.62e6, -38e3, false}, {333e3, 2, 0.25, -0.48e6, 27e3, false},
            {125e3, 1, 0.35, -0.31e6, 44e3, true}, {333e3, 1, 0.35, 0.40e6, -61e3, true}, {333e3, 3, 0.10, 0.0, 15e3, false}};
        for (const auto& q : v) {
            Capture cap; cap.fs = 2.4e6; cap.lnbHz = q.lnb; cap.pick = q.pick; cap.driftHzPerSec = 8; cap.seed = 31 + (uint32_t)q.rs;
            const Car c = q.st == 1 ? carS(q.rs, q.off, q.roll) : q.st == 2 ? carS2(q.rs, q.off, q.roll) : carS2x(q.rs, q.off, q.roll);
            cap.secs = lockSecs(c) + std::max(1.0, 70000.0 / c.rs);
            checkAll("QO-100", cap, {c}, runCapture(cap, {c}));
        }
    });

    // 2. eight carriers from 10 kSym/s to 2 MSym/s in one 8 Msps capture, the six roll-offs, LNB +73 kHz drifting 10 Hz/s
    jobs.add([] {
        jprintf("eight carriers in one 8 Msps capture:\n");
        Capture cap; cap.fs = 8e6; cap.lnbHz = 73e3; cap.driftHzPerSec = 10; cap.secs = 11.5; cap.noiseRms = 0.05;
        const std::vector<Car> cars = {
            carS2(2e6, -2.2e6, 0.35), carS(1e6, 2.6e6, 0.25), carS2x(500e3, 1.3e6, 0.20, 3.0), carS2(250e3, 0.5e6, 0.15),
            carS(100e3, -0.45e6, 0.10), carS2(50e3, 0.1e6, 0.05), carS(25e3, -0.15e6, 0.35), carS(10e3, 0.25e6, 0.20)};
        checkAll("8 Msps", cap, cars, runCapture(cap, cars));
    });
    // 3. four carriers in one 2.4 Msps capture, the LNB 5 ppm of 10.6 GHz off (53 kHz)
    jobs.add([] {
        jprintf("four carriers in one 2.4 Msps capture:\n");
        Capture cap; cap.fs = 2.4e6; cap.lnbHz = -53e3; cap.secs = 15;
        const std::vector<Car> cars = {carS2(500e3, -0.6e6, 0.20), carS(100e3, 0.3e6, 0.35), carS2(25e3, 0.55e6, 0.25), carS2(10e3, -0.15e6, 0.35)};
        checkAll("2.4 Msps", cap, cars, runCapture(cap, cars));
    });
    jobs.run(2);
    if (fails) { printf("%d FAILED\n", fails.load()); return 1; }
    printf("all passed\n");
    return 0;
}
