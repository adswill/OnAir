// CDR generator -> noise and carrier offset -> receiver, for every transmission mode, every spectrum mode (with the analogue FM programme
// in the hybrid ones), the three constellations, the four code rates and the three sub-frame allocations: synchronisation, the system
// information, the control tables, the service list and the text must come out exactly, with no failed LDPC code word.
#include "dect2/cdr_gen.h"
#include "dect2/cdr_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Case { int opt[8]; double snr, cfo, seconds; const char* name; };

static void run(const Case& k) {
    SynthConfig sc;
    for (int i = 0; i < 8; i++) sc.modeOpt[i] = k.opt[i];
    sc.snrDb = k.snr;
    sc.cfoHz = k.cfo;
    const double rate = 2e6;
    auto synth = makeCdrSynth(sc, rate);
    const CdrTxConfig cfg = cdrTxConfigFrom(sc);
    CdrTransmitter ref(cfg);                   // the plan of what is sent
    const CdrTxPlan& plan = ref.plan();
    CdrReceiver rx;
    rx.configure(rate);
    std::vector<cf32> x(50000);
    const auto t0 = std::chrono::steady_clock::now();
    for (double t = 0; t < k.seconds; t += x.size() / rate) {
        synth->generate(x.data(), x.size());
        rx.feed(x.data(), x.size());
    }
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CdrTelemetry tl;
    rx.telemetry(tl, 0);
    printf("%-34s mode %d/%2d  SNR %5.1f dB  cfo %+7.1f Hz  SI %llu/%llu  SDC %llu/%llu  mux %llu/%llu  LDPC %llu ok %llu bad (%.1f it)  %zu services  %.1f s cpu\n",
           k.name, tl.tm, tl.sm, tl.snrDb, tl.cfoHz, (unsigned long long)tl.siOk, (unsigned long long)tl.siBad, (unsigned long long)tl.sdcOk,
           (unsigned long long)tl.sdcBad, (unsigned long long)tl.muxOk, (unsigned long long)tl.muxBad, (unsigned long long)tl.blocksOk,
           (unsigned long long)tl.blocksBad, tl.ldpcIterations, tl.services.size(), cpu);
    CHECK(tl.state == 3, "%s: state %d", k.name, tl.state);
    CHECK(tl.tm == cfg.tm && tl.sm == cfg.sm, "%s: detected mode %d/%d", k.name, tl.tm, tl.sm);
    CHECK(std::fabs(tl.cfoHz - k.cfo) < 5, "%s: carrier offset %.1f", k.name, tl.cfoHz);
    CHECK(tl.siValid && tl.alloc == cfg.alloc && tl.sdiMod == cfg.sdiMod && tl.msdMod == cfg.msdMod && tl.rate == cfg.rate && tl.hier == 0 && tl.uniform && !tl.multiFreq,
          "%s: system information", k.name);
    CHECK(tl.nominalKhz == cdr::spectrumMode(cfg.sm)->nominalKhz, "%s: nominal frequency %d", k.name, tl.nominalKhz);
    CHECK(tl.siBad <= 1, "%s: %llu bad SI", k.name, (unsigned long long)tl.siBad);
    CHECK(tl.sdcOk >= 2 && tl.sdcBad == 0 && tl.tablesBad == 0, "%s: service description", k.name);
    CHECK(tl.muxOk >= 2 && tl.muxBad == 0, "%s: multiplex frames", k.name);
    CHECK(tl.blocksOk >= 2u * (uint64_t)tl.codewords && tl.blocksBad == 0, "%s: LDPC %llu ok %llu bad", k.name, (unsigned long long)tl.blocksOk, (unsigned long long)tl.blocksBad);
    CHECK(tl.capacityBytes == plan.capacityBytes, "%s: capacity", k.name);
    CHECK(tl.network == cfg.network && tl.country == "CHN" && tl.networkId == cfg.networkId && tl.freqsMhz.size() == 1 && std::fabs(tl.freqsMhz[0] - 106.1) < 1e-6,
          "%s: network information", k.name);
    const size_t want = cfg.textService ? 3 : 2;
    CHECK(tl.services.size() == want, "%s: %zu services", k.name, tl.services.size());
    if (tl.services.size() == want) {
        const CdrServiceInfo &a = tl.services[0], &b = tl.services[1];
        CHECK(a.id == cfg.serviceA && b.id == cfg.serviceB && a.smfId == 1 && b.smfId == 1, "%s: service ids", k.name);
        CHECK(a.seen && a.audio && !a.data && a.streams.size() == 1 && a.streams[0].bitrate == plan.rateA && a.streams[0].sampleRate == 48000 &&
              a.streams[0].channelsCode == 2 && a.streams[0].language == "chi" && a.streams[0].algo == 0 && a.audioUnits == 30 && a.audioBytes == plan.rateA * 64 / 800,
              "%s: service A", k.name);
        CHECK(b.seen && b.audio && b.streams.size() == 1 && b.streams[0].bitrate == plan.rateB && b.streams[0].sampleRate == 32000 && b.streams[0].channelsCode == 1 &&
              b.audioUnits == 20, "%s: service B", k.name);
        CHECK(a.subBad == 0 && b.subBad == 0, "%s: sub-frame CRCs", k.name);
        if (want == 3) {
            const CdrServiceInfo& c = tl.services[2];
            CHECK(c.id == cfg.serviceText && c.data && !c.audio && c.dataTypes.size() == 1 && c.dataTypes[0] == 160 && c.text == cfg.text, "%s: text service", k.name);
        }
    }
}

int main() {
    // modeOpt: tm, spectrum, MSD constellation, rate (0 3/4, 1 1/4, 2 1/3, 3 1/2), SDI constellation, allocation, FM programme (0 on), text (0 on)
    const Case cases[] = {
        {{0, 0, 0, 0, 0, 0, 0, 0}, 20, 1500, 3.0, "default: 1/1 QPSK 3/4"},
        {{2, 2, 1, 3, 0, 2, 0, 0}, 25, -2500, 4.0, "2/2 16QAM 1/2, allocation 2"},
        {{3, 3, 2, 0, 1, 3, 0, 0}, 30, 300, 6.5, "3/9 64QAM 3/4 + FM, allocation 3"},
        {{1, 4, 0, 2, 2, 1, 0, 1}, 12, -700, 3.0, "1/10 QPSK 1/3 + FM, SDI 64QAM"},
        {{2, 5, 0, 1, 0, 1, 0, 0}, 4, 3000, 3.0, "2/22 QPSK 1/4 + FM at 4 dB"},
        {{3, 6, 1, 4, 0, 2, 0, 0}, 22, -4000, 4.0, "3/23 16QAM 3/4 + FM, allocation 2"},
        {{1, 2, 2, 3, 0, 1, 1, 0}, 24, 50, 3.0, "1/2 64QAM 1/2"},
    };
    for (const Case& k : cases) run(k);
    // noise only: no lock, no services
    {
        SynthConfig sc;
        CdrReceiver rx;
        rx.configure(2e6);
        std::vector<cf32> x(50000);
        uint32_t s = 1;
        for (int i = 0; i < 40; i++) {
            for (auto& v : x) {
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                v = cf32((float)((int)(s & 0xFFFF) - 32768) / 200000.f, (float)((int)(s >> 16) - 32768) / 200000.f);
            }
            rx.feed(x.data(), x.size());
        }
        CdrTelemetry tl;
        rx.telemetry(tl, 0);
        CHECK(tl.state == 0 && tl.services.empty() && tl.siOk == 0, "noise: state %d", tl.state);
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
