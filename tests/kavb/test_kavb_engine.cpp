// KAVB in the engine's wrapper and the transmit session's generator: the transmit generator (the TX path) makes the signal from a file of bytes or the synthetic
// test stream at the radio's rate, KavbLiveReceiver (what the engine feeds) mixes, resamples, finds the frames, rebuilds the FEC groups and lists the packets in
// the telemetry. Through the radio-rate path with a carrier offset of +-2% of the symbol rate, a sample-rate error, 8-bit samples and noise; a lost frame that the FEC
// restores; the other profiles; and a TxSession writing an IQ file that the receiver decodes. Also: the mode is registered (tuning, transmit table, family numbers).
#include "dect2/kavb/kavb_live.h"
#include "dect2/kavb/kavb_tel.h"
#include "dect2/kavb/kavb_txgen.h"
#include "dect2/kavb/kavb_phy.h"
#include "dect2/kavb/kavb_av.h"
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

struct Impair {
    double cfoHz = 0;
    double ppm = 0;            // the receiver's clock runs fast by this much: the signal is read at 1 + ppm * 1e-6 of the sample spacing
    bool eightBit = false;
    double esn0Db = INFINITY;
    std::pair<double, double> hole{0, 0};   // seconds: samples zeroed (a lost stretch)
};

struct Result {
    KavbTelemetry t;
    double secWork = 0, secSignal = 0;
    std::vector<uint8_t> saved;
};

static std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static cf32 cubic(const std::vector<cf32>& x, double pos) {
    const long i = (long)std::floor(pos);
    const float m = (float)(pos - (double)i);
    auto at = [&](long k) { return (k >= 0 && k < (long)x.size()) ? x[(size_t)k] : cf32(0.f, 0.f); };
    return at(i - 1) * (-m * (m - 1) * (m - 2) / 6.0f) + at(i) * ((m + 1) * (m - 1) * (m - 2) / 2.0f) + at(i + 1) * (-(m + 1) * m * (m - 2) / 2.0f) +
           at(i + 2) * ((m + 1) * m * (m - 1) / 6.0f);
}

// The signal through the impairments (offset, clock error, a hole, 8-bit, noise), pushed into the receiver in blocks of 4096; two seconds of silence at the end.
static Result receive(const std::vector<cf32>& sig0, double rate, const Impair& im, int listen, const std::string& outFile, double symRate) {
    std::vector<cf32> sig = sig0;
    if (im.ppm != 0) {
        std::vector<cf32> y(sig.size());
        for (size_t i = 0; i < y.size(); i++) y[i] = cubic(sig, (double)i * (1.0 + im.ppm * 1e-6));
        sig.swap(y);
    }
    if (im.hole.second > im.hole.first) {
        const size_t a = (size_t)(im.hole.first * rate), b = std::min(sig.size(), (size_t)(im.hole.second * rate));
        for (size_t i = a; i < b; i++) sig[i] = cf32(0.f, 0.f);
    }
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    const double pSig = 0.25 * 0.25;   // the generator's data power
    const float sigma = std::isfinite(im.esn0Db) ? (float)std::sqrt(pSig * rate / (symRate * std::pow(10.0, im.esn0Db / 10.0)) / 2.0) : 0.f;
    float peak = 1e-6f;
    for (const cf32& c : sig) peak = std::max(peak, std::max(std::fabs(c.real()), std::fabs(c.imag())));
    KavbLiveReceiver rx;
    rx.configure(rate);
    rx.setListenProfile(listen);
    if (!outFile.empty()) rx.setOutputFile(outFile);
    Result r;
    uint64_t seq = 0;
    std::vector<cf32> blk;
    double ph = 0;
    const double dph = 2 * 3.141592653589793 * im.cfoHz / rate;
    const double t0 = processCpuSecs();
    for (size_t pos = 0; pos < sig.size() + (size_t)(2 * rate); pos += 4096) {
        blk.assign(4096, cf32(0.f, 0.f));
        for (size_t i = 0; i < 4096; i++) {
            if (pos + i < sig.size()) blk[i] = sig[pos + i];
            if (im.cfoHz != 0) { blk[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += dph; }
            if (sigma > 0) blk[i] += cf32(sigma * nd(rng), sigma * nd(rng));
            if (im.eightBit) {
                const float g = 120.f / (peak * 1.15f);   // an AGC that keeps the peaks of the clean signal just below full scale
                blk[i] = cf32(std::round(std::max(-127.f, std::min(127.f, blk[i].real() * g))), std::round(std::max(-127.f, std::min(127.f, blk[i].imag() * g))));
            }
        }
        rx.feed(blk.data(), blk.size());
        KavbTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.t = t; }
    }
    r.secWork = (processCpuSecs() - t0);
    r.secSignal = (double)sig.size() / rate;
    rx.setOutputFile("");
    if (!outFile.empty()) r.saved = readFile(outFile);
    return r;
}

static std::vector<cf32> generate(const TxRequest& req, double rate, std::string& err) {
    auto g = makeKavbTxGenerator(req, rate, err);
    std::vector<cf32> all;
    if (!g) return all;
    std::vector<cf32> blk(8192);
    while (!g->finished() && all.size() < (size_t)(rate * 120)) {
        g->generate(blk.data(), blk.size());
        all.insert(all.end(), blk.begin(), blk.end());
    }
    return all;
}

static void print(const char* what, const Result& r) {
    printf("%s: state %d, profile %d, frames %llu good / %llu bad, blocks %llu/%llu, groups %llu ok %llu restored %llu lost, packets %llu, payload %llu B, Es/N0 %.1f dB, CFO %+.0f Hz "
           "(coarse %+.0f), CPU %.2f s for %.2f s\n", what, r.t.state, r.t.profile, (unsigned long long)r.t.framesGood, (unsigned long long)r.t.framesBad, (unsigned long long)r.t.blocksOk,
           (unsigned long long)r.t.blocksBad, (unsigned long long)r.t.groupsOk, (unsigned long long)r.t.groupsRecovered, (unsigned long long)r.t.groupsLost, (unsigned long long)r.t.packets,
           (unsigned long long)r.t.payloadBytes, (double)r.t.esn0Db, (double)r.t.cfoHz, (double)r.t.coarseHz, r.secWork, r.secSignal);
}

int main() {
    // ---- registration: tuning, the transmit table, family numbers
    {
        const ModeTuning* t = modeTuning(125);
        CHECK(t && t->id == std::string("kavb") && t->name == std::string("KAVB") && t->stdMode == 125, "tuning of engine standard 125");
        if (t) CHECK(t->minSampleRate == 2e6 && std::fabs(t->bandwidthMhz - 2.0) < 1e-9, "KAVB is 2 MHz wide and needs 2 Msps");
        CHECK(modeTuningById("kavb") == t, "modeTuningById");
        std::set<std::string> ids;
        std::set<int> stds;
        for (int s = 0; s < 400; s++) {
            const ModeTuning* m = modeTuning(s);
            if (!m || m->stdMode != s) continue;
            CHECK(ids.insert(m->id).second, "mode id %s is unique", m->id);
            CHECK(stds.insert(m->stdMode).second, "engine standard %d is unique", m->stdMode);
        }
        const TxModeSpec* m = txModeById("kavb");
        CHECK(m && m->stdMode == 125 && txPayloadKind("kavb") == TxPayloadKind::Kavb, "transmit table row");
        CHECK(txModeForList(123, "") == m, "family 123 maps to the KAVB transmit row");
        CHECK(m && txModeFinite(*m), "KAVB is a finite transmission");
    }

    const fs::path dir = fs::temp_directory_path() / "onair_kavb_engine_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<uint8_t> data(24000);
    {
        std::mt19937 rng(5);
        for (auto& b : data) b = (uint8_t)rng();
    }
    const std::string dataPath = (dir / "data.bin").string();
    { std::ofstream o(dataPath, std::ios::binary); o.write((const char*)data.data(), (std::streamsize)data.size()); }
    const std::string savePath = (dir / "saved.bin").string();

    TxRequest base;
    base.modeId = "kavb";
    base.callsign = "ly2abc";
    base.file = dataPath;
    base.opt["kavb.profile"] = 0;
    base.opt["kavb.fec"] = 1;
    const double kRate = 2.4e6, kSym = 700e3;
    std::string err;
    const std::vector<cf32> sigA = generate(base, kRate, err);
    CHECK(!sigA.empty(), "generator: %s", err.c_str());

    // ---- clean, at two radio rates
    for (double rate : {2.4e6, 3.2e6}) {
        const std::vector<cf32> sig = rate == kRate ? sigA : generate(base, rate, err);
        const Result r = receive(sig, rate, Impair(), 0, savePath, kSym);
        print(rate == kRate ? "A clean 2.4 Msps" : "A clean 3.2 Msps", r);
        CHECK(r.saved == data, "rate %.0f: the saved payload is the file (%zu of %zu bytes)", rate, r.saved.size(), data.size());
        CHECK(r.t.callsign == "LY2ABC", "rate %.0f: station '%s'", rate, r.t.callsign.c_str());
        CHECK(r.t.profile == 0 && !r.t.robust && r.t.framesBad == 0 && r.t.blocksBad == 0, "rate %.0f: profile %d, %llu bad frames", rate, r.t.profile, (unsigned long long)r.t.framesBad);
        CHECK(r.t.groupsLost == 0 && r.t.packetsByType[4] > 0 && r.t.payloadBytes == data.size(), "rate %.0f: groups lost %llu, data packets %llu, payload %llu", rate,
              (unsigned long long)r.t.groupsLost, (unsigned long long)r.t.packetsByType[4], (unsigned long long)r.t.payloadBytes);
        CHECK(!r.t.constellation.empty() && r.t.constellationBits == 4 && !r.t.recent.empty(), "constellation %zu points, %d bits, %zu recent packets", r.t.constellation.size(),
              r.t.constellationBits, r.t.recent.size());
        CHECK(r.t.savedBytes == data.size() && r.t.savePath == savePath, "saved %llu bytes", (unsigned long long)r.t.savedBytes);
    }

    // ---- impairments at profile A: carrier offsets of +-2% of the symbol rate (+-14 kHz), a clock error of +-20 ppm, 8-bit samples and noise (Es/N0 22 dB)
    struct Case { const char* name; Impair im; };
    Impair i1; i1.cfoHz = 14000; i1.eightBit = true; i1.esn0Db = 22; i1.ppm = 20;
    Impair i2; i2.cfoHz = -14000; i2.eightBit = true; i2.esn0Db = 22; i2.ppm = -20;
    Impair i3; i3.cfoHz = 6000; i3.esn0Db = 20;
    Impair i4; i4.cfoHz = -3000; i4.eightBit = true; i4.ppm = 50;
    for (const Case& c : {Case{"A +2% 8-bit +20 ppm 22 dB", i1}, Case{"A -2% 8-bit -20 ppm 22 dB", i2}, Case{"A +6 kHz 20 dB", i3}, Case{"A -3 kHz 8-bit +50 ppm", i4}}) {
        const Result r = receive(sigA, kRate, c.im, 0, savePath, kSym);
        print(c.name, r);
        CHECK(r.saved == data, "%s: the saved payload differs (%zu of %zu bytes)", c.name, r.saved.size(), data.size());
        CHECK(r.t.groupsLost == 0, "%s: %llu groups lost", c.name, (unsigned long long)r.t.groupsLost);
        CHECK(std::fabs(r.t.cfoHz - c.im.cfoHz) < 1500, "%s: carrier offset %.0f Hz against %.0f", c.name, (double)r.t.cfoHz, c.im.cfoHz);
        if (std::isfinite(c.im.esn0Db)) CHECK(std::fabs(r.t.esn0Db - c.im.esn0Db) < 5, "%s: Es/N0 %.1f dB against %.1f", c.name, (double)r.t.esn0Db, c.im.esn0Db);
    }

    // ---- the coarse carrier search: offsets of +-5%, +-8% and +-12% of the symbol rate are outside what the frame receiver takes by itself (+-2.5%). The search steps through
    // hypotheses while no frame is found (0.1 s each, 0.7 s for all), so the signal must last longer than that: a 3 s test stream. The frames before the lock are lost.
    {
        TxRequest rq;
        rq.modeId = "kavb";
        rq.callsign = "LY2ABC";
        rq.opt["kavb.profile"] = 0;
        rq.opt["kavb.fec"] = 1;
        rq.opt["kavb.seconds"] = 3;
        const std::vector<cf32> sig = generate(rq, kRate, err);
        for (double cfo : {35000.0, -35000.0, 56000.0, -56000.0, 84000.0, -84000.0}) {
            Impair im; im.cfoHz = cfo; im.esn0Db = 24;
            const Result r = receive(sig, kRate, im, 0, "", kSym);
            char nm[64];
            snprintf(nm, sizeof nm, "A synthetic %+.0f Hz (%+.0f%%) 24 dB", cfo, cfo / kSym * 100);
            print(nm, r);
            CHECK(r.t.state == 2 || r.t.framesGood > 0, "%s: no lock", nm);
            CHECK(r.t.framesGood > 0.6 * (r.secSignal - 2.0) / kavb::frameSeconds(kavb::profileOf(0, false)), "%s: only %llu good frames", nm, (unsigned long long)r.t.framesGood);
            CHECK(std::fabs(r.t.cfoHz - cfo) < 1500 && std::fabs(r.t.coarseHz) > 20000, "%s: carrier offset %.0f Hz, coarse %.0f", nm, (double)r.t.cfoHz, (double)r.t.coarseHz);
            CHECK(r.t.packetsByType[0] > 0 && r.t.packetsByType[1] > 0 && r.t.callsign == "LY2ABC", "%s: video %llu audio %llu packets", nm, (unsigned long long)r.t.packetsByType[0],
                  (unsigned long long)r.t.packetsByType[1]);
        }
    }

    // ---- the receiver set to another symbol rate hears nothing
    {
        const Result r = receive(sigA, kRate, Impair(), 1, "", kSym);
        CHECK(r.t.framesGood == 0 && r.t.packets == 0 && r.t.state == 1, "listening at 1050 ksym/s: %llu frames, state %d", (unsigned long long)r.t.framesGood, r.t.state);
    }

    // ---- a lost stretch inside one frame: FEC profile 3 (28 + 4) restores the group, so the file still arrives whole
    {
        TxRequest rq = base;
        rq.opt["kavb.fec"] = 3;
        const std::vector<cf32> sig = generate(rq, kRate, err);
        const kavb::Profile p = kavb::profileOf(0, false);
        const double f0 = (96.0 + 8.0 + 2 * p.frameSyms) / p.symRate, fs = p.frameSyms / p.symRate;   // the third frame
        Impair im; im.hole = {f0 + 0.25 * fs, f0 + 0.75 * fs}; im.esn0Db = 24;
        const Result r = receive(sig, kRate, im, 0, savePath, kSym);
        print("A fec 28+4 with one frame cut in the middle", r);
        CHECK(r.saved == data, "the saved payload differs (%zu of %zu bytes)", r.saved.size(), data.size());
        CHECK(r.t.framesBad >= 1 && r.t.groupsRecovered >= 1 && r.t.groupsLost == 0, "bad frames %llu, groups restored %llu, lost %llu", (unsigned long long)r.t.framesBad,
              (unsigned long long)r.t.groupsRecovered, (unsigned long long)r.t.groupsLost);
        // the same cut with no parity (4 + 0) loses the group
        TxRequest r0 = base;
        r0.opt["kavb.fec"] = 0;
        const std::vector<cf32> sig0 = generate(r0, kRate, err);
        const Result q = receive(sig0, kRate, im, 0, savePath, kSym);
        print("A fec 4+0 with one frame cut in the middle", q);
        CHECK(q.saved != data && q.t.groupsLost >= 1, "without parity the cut must cost data (groups lost %llu)", (unsigned long long)q.t.groupsLost);
    }

    // ---- the synthetic test stream at each profile (video and audio sized units); B at 2.4 Msps, C and D at 3.2 Msps (the radio rate must be at least 1.6 symbol rates)
    struct Prof { int id; double rate; double sym; };
    for (const Prof pr : {Prof{0, 2.4e6, 700e3}, Prof{1, 2.4e6, 1050e3}, Prof{2, 3.2e6, 1400e3}, Prof{3, 3.2e6, 1400e3}}) {
        TxRequest rq;
        rq.modeId = "kavb";
        rq.callsign = "LY2XYZ";
        rq.opt["kavb.profile"] = pr.id;
        rq.opt["kavb.fec"] = 1;
        rq.opt["kavb.seconds"] = 1;
        const std::vector<cf32> sig = generate(rq, pr.rate, err);
        CHECK(!sig.empty(), "generator profile %d: %s", pr.id, err.c_str());
        Impair im; im.cfoHz = 0.01 * pr.sym; im.esn0Db = pr.id == 3 ? 30 : 24;
        const Result r = receive(sig, pr.rate, im, pr.id, "", pr.sym);
        char nm[48];
        snprintf(nm, sizeof nm, "profile %c synthetic", 'A' + pr.id);
        print(nm, r);
        CHECK(r.t.profile == pr.id && r.t.callsign == "LY2XYZ", "profile %c: header says %d, station %s", 'A' + pr.id, r.t.profile, r.t.callsign.c_str());
        CHECK(r.t.packetsByType[0] > 0 && r.t.packetsByType[1] > 0 && r.t.groupsLost == 0 && r.t.packetsDropped == 0,
              "profile %c: video %llu audio %llu packets, %llu groups lost, %llu dropped", 'A' + pr.id, (unsigned long long)r.t.packetsByType[0], (unsigned long long)r.t.packetsByType[1],
              (unsigned long long)r.t.groupsLost, (unsigned long long)r.t.packetsDropped);
        CHECK(r.t.framesBad == 0, "profile %c: %llu bad frames", 'A' + pr.id, (unsigned long long)r.t.framesBad);
    }

    // ---- a TxSession writing an IQ file (the destination of the TX panel), read back and decoded
    {
        IqFileTxConfig c;
        c.format = IqFileFormat::SigmfCf32;
        c.paced = false;
        c.centerHz = 1.29e9;
        c.path = iqFilePath(dir.string(), "kavb", c.format);
        c.counters = std::make_shared<IqFileTxCounters>();
        TxRequest tr = base;
        tr.loop = true;   // endless unless the generator ends it
        tr.tune.centerHz = 1.29e9;
        tr.tune.sampleRate = 2.4e6;
        TxSession s;
        CHECK(txModeById("kavb") != nullptr, "kavb transmit row");
        CHECK(s.start(makeIqFileTxSink(c), tr, {}, err), "session: %s", err.c_str());
        const auto t0 = std::chrono::steady_clock::now();
        while (s.running() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(90)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const bool ended = !s.running();
        s.stop();
        CHECK(ended, "the session ended by itself after the file");
        IqFileInfo info;
        CHECK(probeIqFile(c.path, info, err) && std::fabs(info.sampleRate - 2.4e6) < 1 && std::fabs(info.centerHz - 1.29e9) < 1, "probe: %s", err.c_str());
        std::vector<cf32> iq;
        {
            std::ifstream f(info.dataPath, std::ios::binary);
            f.seekg(0, std::ios::end);
            const size_t n = (size_t)f.tellg() / sizeof(cf32);
            f.seekg(0);
            iq.resize(n);
            f.read((char*)iq.data(), (std::streamsize)(n * sizeof(cf32)));
        }
        const Result r = receive(iq, 2.4e6, Impair(), 0, savePath, kSym);
        print("IQ file", r);
        CHECK(iq.size() > 100000 && r.saved == data, "the IQ file decodes to the file (%zu samples, %zu of %zu bytes)", iq.size(), r.saved.size(), data.size());
    }

    // ---- refusals in plain words
    {
        TxRequest bad = base;
        bad.callsign = "TOOLONGCALL";
        CHECK(!makeKavbTxGenerator(bad, 2.4e6, err) && !err.empty(), "a callsign of more than 7 characters is refused");
        bad = base;
        bad.opt["kavb.profile"] = 4;
        CHECK(!makeKavbTxGenerator(bad, 2.4e6, err), "profile 4 is refused");
        bad = base;
        bad.opt["kavb.fec"] = 8;
        CHECK(!makeKavbTxGenerator(bad, 2.4e6, err), "FEC profile 8 is refused");
        bad = base;
        bad.opt["kavb.profile"] = 3;
        CHECK(!makeKavbTxGenerator(bad, 2.0e6, err), "profile D at 2 MHz is refused");
        CHECK(!makeKavbTxGenerator(base, 1.0e6, err), "a rate below 1.6 symbol rates is refused");
        bad = base;
        bad.file = "/nonexistent/none.bin";
        CHECK(!makeKavbTxGenerator(bad, 2.4e6, err), "a missing file is refused");
    }
    fs::remove_all(dir);
    return testSummary("kavb_engine");
}
