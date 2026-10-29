// KAPP in the engine's wrapper and the transmit session's generator: the transmit generator (the TX path) makes the one-way signal from a message,
// KappLiveReceiver (what the engine feeds) finds the bursts, assembles the message and lists it in the telemetry. Rates other than the internal one,
// a carrier offset, noise, a message of several bursts, the repeat that is ignored, and a TxSession writing an IQ file that the receiver decodes.
// Also: the mode is registered (tuning, transmit table, family numbers).
#include "dect2/kapp/kapp_live.h"
#include "dect2/kapp/kapp_tel.h"
#include "dect2/kapp/kapp_txgen.h"
#include "dect2/kapp/kapp_frame.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include "dect2/source/iq_file.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>
#include <vector>
using namespace dect2;
namespace fs = std::filesystem;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

static uint64_t fnv(const std::vector<uint8_t>& m) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : m) { h ^= b; h *= 1099511628211ull; }
    return h;
}

struct Result {
    KappTelemetry t;
    double secSignal = 0, secWork = 0;
};

// Pushes the signal into the receiver (with a carrier offset and noise when asked for) and returns the last telemetry.
static Result receive(const std::vector<cf32>& sig, double rate, double cfoHz, double esn0Db) {
    KappLiveReceiver rx;
    rx.configure(rate);
    Result r;
    uint64_t seq = 0;
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double pSig = 0.35 * 0.35;   // the generator's burst power
    const float sigma = std::isfinite(esn0Db) ? (float)std::sqrt(pSig * rate / (kapp::kSymRate * std::pow(10.0, esn0Db / 10.0)) / 2.0) : 0.f;
    std::vector<cf32> blk;
    const double t0 = processCpuSecs();
    double ph = 0;
    const double dph = 2 * 3.141592653589793 * cfoHz / rate;
    for (size_t pos = 0; pos < sig.size() + (size_t)(2 * rate); pos += 4096) {   // two seconds of silence at the end make the receiver report
        blk.assign(4096, cf32(0.f, 0.f));
        for (size_t i = 0; i < 4096; i++) {
            if (pos + i < sig.size()) blk[i] = sig[pos + i];
            if (cfoHz != 0) { blk[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; }
            if (sigma > 0) blk[i] += cf32(sigma * nd(rng), sigma * nd(rng));
        }
        rx.feed(blk.data(), blk.size());
        KappTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.t = t; }
    }
    r.secWork = (processCpuSecs() - t0);   // CPU seconds of this process
    r.secSignal = (double)sig.size() / rate;
    return r;
}

static std::vector<cf32> generate(const TxRequest& req, double rate, std::string& err) {
    auto g = makeKappTxGenerator(req, rate, err);
    std::vector<cf32> all;
    if (!g) return all;
    std::vector<cf32> blk(8192);
    while (!g->finished() && all.size() < (size_t)(rate * 600)) {
        g->generate(blk.data(), blk.size());
        all.insert(all.end(), blk.begin(), blk.end());
    }
    return all;
}

int main() {
    // ---- registration: tuning, the transmit table, family numbers
    {
        const ModeTuning* t = modeTuning(126);
        CHECK(t && t->id == std::string("kapp") && t->name == std::string("KAPP") && t->stdMode == 126, "tuning of engine standard 126");
        if (t) CHECK(t->minSampleRate == 125000 && std::fabs(t->bandwidthMhz - 0.1) < 1e-9, "KAPP is 100 kHz wide and needs 125 ksps");
        CHECK(modeTuningById("kapp") == t, "modeTuningById");
        std::set<std::string> ids;
        std::set<int> stds;
        for (int s = 0; s < 400; s++) {
            const ModeTuning* m = modeTuning(s);
            if (!m || m->stdMode != s) continue;
            CHECK(ids.insert(m->id).second, "mode id %s is unique", m->id);
            CHECK(stds.insert(m->stdMode).second, "engine standard %d is unique", m->stdMode);
        }
        const TxModeSpec* m = txModeById("kapp");
        CHECK(m && m->stdMode == 126 && txPayloadKind("kapp") == TxPayloadKind::Kapp, "transmit table row");
        CHECK(txModeForList(124, "") == m, "family 124 maps to the KAPP transmit row");
        CHECK(m && txModeFinite(*m), "KAPP is a finite transmission");
    }

    const std::string text = "KAPP says hello from the engine test: selective repeat, one way, twice.";
    TxRequest base;
    base.modeId = "kapp";
    base.callsign = "ly2abc";
    base.text = text;
    base.opt["kapp.profile"] = 2;
    base.opt["kapp.repeat"] = 2;
    const std::vector<uint8_t> want(text.begin(), text.end());

    // ---- a message, at several radio rates, clean
    for (double rate : {250000.0, 500000.0, 1e6}) {
        std::string err;
        const std::vector<cf32> sig = generate(base, rate, err);
        CHECK(!sig.empty(), "generator at %.0f: %s", rate, err.c_str());
        const Result r = receive(sig, rate, 0, INFINITY);
        printf("rate %.0f: state %d, bursts %llu, blocks %llu/%llu, messages %llu, repeats %llu, CPU %.2f s for %.2f s of signal\n", rate, r.t.state, (unsigned long long)r.t.bursts,
               (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, (unsigned long long)r.t.messages, (unsigned long long)r.t.repeats, r.secWork, r.secSignal);
        CHECK(r.t.messages == 1 && r.t.repeats == 1, "rate %.0f: %llu messages, %llu repeats", rate, (unsigned long long)r.t.messages, (unsigned long long)r.t.repeats);
        CHECK(!r.t.packets.empty() && r.t.packets[0].preview == text && r.t.packets[0].callsign == "LY2ABC" && r.t.packets[0].bytes == text.size(), "rate %.0f: packet", rate);
        CHECK(!r.t.packets.empty() && r.t.packets[0].contentHash == fnv(want), "rate %.0f: content hash", rate);
        CHECK(r.t.blocksBad == 0 && r.t.profile == 2, "rate %.0f: %llu bad blocks, profile %d", rate, (unsigned long long)r.t.blocksBad, r.t.profile);
        CHECK(!r.t.constellation.empty() && r.t.constellationBits == 2, "rate %.0f: constellation %zu points, %d bits", rate, r.t.constellation.size(), r.t.constellationBits);
    }

    // ---- impairments: carrier offsets (up to +-2.5 kHz: 5 ppm at 440 MHz, found from blocks of 8 preamble symbols) and noise, several profiles. Es/N0 of P0 is 1 dB,
    // 2 dB above the measured -1 dB threshold.
    struct Case { int profile; double cfo, esn0; };
    for (const Case c : {Case{2, 0, 9}, Case{2, 200, 9}, Case{2, -250, 9}, Case{0, 150, 3}, Case{5, -200, 16}, Case{8, 100, 33},
                         Case{2, 2000, 9}, Case{2, -2000, 9}, Case{2, 2500, 9}, Case{0, -2000, 1.0}, Case{0, 1500, 1.0}, Case{5, -2400, 16}, Case{8, 2000, 33}}) {
        TxRequest rq = base;
        rq.opt["kapp.profile"] = c.profile;
        std::string err;
        const double rate = 250000;
        const std::vector<cf32> sig = generate(rq, rate, err);
        const Result r = receive(sig, rate, c.cfo, c.esn0);
        const bool got = !r.t.packets.empty() && r.t.packets[0].preview == text;
        printf("profile %d, cfo %+.0f Hz, Es/N0 %.1f dB: messages %llu, snr measured %.1f dB, blocks %llu/%llu\n", c.profile, c.cfo, c.esn0, (unsigned long long)r.t.messages, (double)r.t.snrDb,
               (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
        CHECK(got, "profile %d with %+.0f Hz and %.1f dB: message not delivered (%llu messages)", c.profile, c.cfo, c.esn0, (unsigned long long)r.t.messages);
    }

    // ---- a message of several bursts (40 kB of pseudo-random bytes sent from a file)
    {
        const fs::path dir = fs::temp_directory_path() / "onair_kapp_engine_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::vector<uint8_t> big(40000);
        std::mt19937 rng(3);
        for (auto& b : big) b = (uint8_t)rng();
        const std::string path = (dir / "big.bin").string();
        { std::ofstream o(path, std::ios::binary); o.write((const char*)big.data(), (std::streamsize)big.size()); }
        TxRequest rq = base;
        rq.file = path;
        rq.opt["kapp.repeat"] = 1;
        std::string err;
        const std::vector<cf32> sig = generate(rq, 250000, err);
        const Result r = receive(sig, 250000, 200, 12);
        printf("40 kB: bursts %llu, messages %llu, bytes %llu, throughput %.1f kbit/s, airtime estimate %.1f s, made %.1f s\n", (unsigned long long)r.t.bursts, (unsigned long long)r.t.messages,
               (unsigned long long)r.t.bytesDelivered, r.t.throughputBps / 1000, kappAirtimeSeconds(big.size(), 2, 1), r.secSignal);
        CHECK(r.t.bursts >= 3, "several bursts (%llu)", (unsigned long long)r.t.bursts);
        CHECK(r.t.messages == 1 && !r.t.packets.empty() && r.t.packets[0].bytes == big.size() && r.t.packets[0].contentHash == fnv(big), "the 40 kB message arrives intact");
        CHECK(std::fabs(kappAirtimeSeconds(big.size(), 2, 1) - (r.secSignal - 2 * 0.0)) < 0.1 * r.secSignal, "airtime estimate %.1f s against %.1f s made", kappAirtimeSeconds(big.size(), 2, 1), r.secSignal);

        // ---- a TxSession writing an IQ file (the destination of the TX panel), read back and decoded
        IqFileTxConfig c;
        c.format = IqFileFormat::SigmfCf32;
        c.paced = false;
        c.centerHz = 433.92e6;
        c.path = iqFilePath(dir.string(), "kapp", c.format);
        c.counters = std::make_shared<IqFileTxCounters>();
        TxRequest tr = base;
        tr.loop = true;   // endless unless the generator ends it
        tr.tune.centerHz = 433.92e6;
        tr.tune.sampleRate = 250000;
        tr.opt["kapp.repeat"] = 1;
        TxSession s;
        const TxModeSpec* m = txModeById("kapp");
        CHECK(m != nullptr, "kapp transmit row");
        if (m) {
            CHECK(s.start(makeIqFileTxSink(c), tr, {}, err), "session: %s", err.c_str());
            const auto t0 = std::chrono::steady_clock::now();
            while (s.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const bool ended = !s.running();
            s.stop();
            CHECK(ended, "the session ended by itself after one pass");
            IqFileInfo info;
            CHECK(probeIqFile(c.path, info, err) && std::fabs(info.sampleRate - 250000) < 1 && std::fabs(info.centerHz - 433.92e6) < 1, "probe: %s", err.c_str());
            std::vector<cf32> iq;
            {
                std::ifstream f(info.dataPath, std::ios::binary);
                f.seekg(0, std::ios::end);
                const size_t n = (size_t)f.tellg() / sizeof(cf32);
                f.seekg(0);
                iq.resize(n);
                f.read((char*)iq.data(), (std::streamsize)(n * sizeof(cf32)));
            }
            const Result r2 = receive(iq, 250000, 0, INFINITY);
            printf("IQ file: %zu samples (%.1f s), messages %llu\n", iq.size(), (double)iq.size() / 250000, (unsigned long long)r2.t.messages);
            CHECK(iq.size() > 100000 && r2.t.messages == 1 && !r2.t.packets.empty() && r2.t.packets[0].preview == text, "the IQ file decodes to the message");
        }
        fs::remove_all(dir);
    }

    // ---- refusals in plain words
    {
        std::string err;
        TxRequest bad = base;
        bad.callsign = "TOOLONGCALL";
        CHECK(!makeKappTxGenerator(bad, 250000, err) && !err.empty(), "a callsign of more than 7 characters is refused");
        bad = base;
        bad.opt["kapp.profile"] = 9;
        CHECK(!makeKappTxGenerator(bad, 250000, err), "profile 9 is refused");
        CHECK(!makeKappTxGenerator(base, 100000, err), "a rate below 125 kHz is refused");
        bad = base;
        bad.file = "/nonexistent/none.bin";
        CHECK(!makeKappTxGenerator(bad, 250000, err), "a missing file is refused");
    }
    return testSummary("kapp_engine");
}
