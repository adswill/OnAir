// dtmbtool: the DTMB test signal and the receiver on files.
//   dtmbtool gen out.cs8 [signal options] [--seconds 2] [--rate 10e6] [--format cs8|cf32]
//   dtmbtool rx recording [--rate 10e6] [--format cs8|cu8|cf32] [--ts out.ts] [--check] [--threads n] [--chunk n] [--verbose]
//   dtmbtool sweep [signal options] --from 10 --to 24 --step 2 [--seconds 3]     (generator -> receiver in memory, packets checked)
// Signal options: --carriers 3780|1 (1 forces PN595)  --header 945|595|420  --mod 64|32|16|4|4nr  --code 0.4|0.6|0.8  --mode 1|2 (interleaver)  --snr dB  --cfo Hz  --ppm ppm
//   --echo dB:samples (repeatable; negative samples arrive before the main path)  --fixed-phase  --payload demo|test  --seed n  --level rms
// --check on rx: the packets are the numbered test packets of `gen --payload test`; counts good, wrong and missing ones.
#include "dect2/dtmb_gen.h"
#include "dect2/dtmb_rx.h"
#include "dect2/demo_ts.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::dtmb;

namespace {

struct Args {
    int argc; char** argv; int i;
    bool more() const { return i < argc; }
    std::string next() { return argv[i++]; }
};

struct SigOpts {
    SignalConfig sc;
    bool testPayload = false;
    double seconds = 2.0;
    std::string format = "cs8";
    uint32_t seed = 1;
};

bool parseMapping(const std::string& v, Mapping& m) {
    if (v == "64") m = Mapping::Qam64; else if (v == "32") m = Mapping::Qam32; else if (v == "16") m = Mapping::Qam16;
    else if (v == "4") m = Mapping::Qam4; else if (v == "4nr") m = Mapping::Qam4Nr; else return false;
    return true;
}

// Returns 1 when the option was a signal option and was consumed, 0 when it is not one, -1 on a bad value
int signalOption(const std::string& a, Args& g, SigOpts& o) {
    SignalConfig& sc = o.sc;
    auto val = [&]() -> std::string { return g.more() ? g.next() : std::string(); };
    if (a == "--header") {
        const std::string v = val();
        if (v == "945") sc.tx.header = Header::Pn945; else if (v == "595") sc.tx.header = Header::Pn595; else if (v == "420") sc.tx.header = Header::Pn420; else return -1;
    } else if (a == "--carriers") {
        const std::string v = val();
        if (v == "1") { sc.tx.carriers = 1; sc.tx.header = Header::Pn595; sc.tx.phaseRotate = false; } else if (v == "3780") sc.tx.carriers = 3780; else return -1;
    } else if (a == "--mod") { if (!parseMapping(val(), sc.tx.profile.map)) return -1; }
    else if (a == "--code") {
        const std::string v = val();
        if (v == "0.4") sc.tx.profile.rate = Rate::R04; else if (v == "0.6") sc.tx.profile.rate = Rate::R06; else if (v == "0.8") sc.tx.profile.rate = Rate::R08; else return -1;
    } else if (a == "--mode") { const std::string v = val(); if (v != "1" && v != "2") return -1; sc.tx.profile.mode2 = v == "2"; }
    else if (a == "--snr") sc.snrDb = atof(val().c_str());
    else if (a == "--cfo") sc.cfoHz = atof(val().c_str());
    else if (a == "--ppm") sc.sroPpm = atof(val().c_str());
    else if (a == "--echo") {
        const std::string v = val();
        const size_t c = v.find(':');
        if (c == std::string::npos) return -1;
        sc.echoes.push_back({atof(v.substr(0, c).c_str()), atof(v.substr(c + 1).c_str())});
    } else if (a == "--fixed-phase") sc.tx.phaseRotate = false;
    else if (a == "--payload") { const std::string v = val(); if (v == "test") o.testPayload = true; else if (v == "demo") o.testPayload = false; else return -1; }
    else if (a == "--seed") { o.seed = (uint32_t)atol(val().c_str()); sc.seed = o.seed; }
    else if (a == "--level") sc.rms = (float)atof(val().c_str());
    else if (a == "--seconds") o.seconds = atof(val().c_str());
    else if (a == "--rate") sc.rate = atof(val().c_str());
    else if (a == "--format") o.format = val();
    else return 0;
    return 1;
}

FrameTx::TsSource makeSource(const SigOpts& o) {
    if (o.testPayload) return testPacketSource(1);
    return demoTsSource(netBitrate(o.sc.tx.header, o.sc.tx.profile));
}

double threadCpu() { timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

void describe(const SigOpts& o) {
    const TxConfig& t = o.sc.tx;
    printf("signal: C=%d %s %s rate %s interleaver mode %d, %.3f Mbit/s, C/N %.1f dB, cfo %.0f Hz, clock %.1f ppm, %zu echo(es), %s, %.4g Msps\n", t.carriers, headerInfo(t.header).name, mappingName(t.profile.map),
           rateName(t.profile.rate), t.profile.mode2 ? 2 : 1, netBitrate(t.header, t.profile) / 1e6, o.sc.snrDb, o.sc.cfoHz, o.sc.sroPpm, o.sc.echoes.size(), o.testPayload ? "numbered test packets" : "test programme",
           o.sc.rate / 1e6);
}

int cmdGen(Args& g) {
    SigOpts o;
    std::string path;
    while (g.more()) {
        const std::string a = g.next();
        const int r = signalOption(a, g, o);
        if (r < 0) { fprintf(stderr, "bad value for %s\n", a.c_str()); return 1; }
        if (r == 0) { if (a[0] == '-' || !path.empty()) { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; } path = a; }
    }
    if (path.empty()) { fprintf(stderr, "usage: dtmbtool gen out.cs8 [options]\n"); return 1; }
    if (!profileValid(o.sc.tx.profile)) { fprintf(stderr, "this modulation exists at code rate 0.8 only\n"); return 1; }
    if (o.sc.rate < 8e6) { fprintf(stderr, "the signal is 7.56 MHz wide: --rate 8e6 or more\n"); return 1; }
    describe(o);
    Signal sig(o.sc, makeSource(o));
    std::ofstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
    const size_t total = (size_t)(o.seconds * o.sc.rate), block = 1 << 16;
    std::vector<cf32> x(block);
    std::vector<int8_t> b8;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(block, total - done);
        sig.generate(x.data(), n);
        if (o.format == "cf32") f.write((const char*)x.data(), (std::streamsize)(n * sizeof(cf32)));
        else {
            b8.resize(n * 2);
            for (size_t i = 0; i < n; i++) {
                b8[2 * i] = (int8_t)std::lround(std::max(-1.f, std::min(127.f / 128.f, x[i].real())) * 128.f);
                b8[2 * i + 1] = (int8_t)std::lround(std::max(-1.f, std::min(127.f / 128.f, x[i].imag())) * 128.f);
            }
            f.write((const char*)b8.data(), (std::streamsize)b8.size());
        }
        done += n;
    }
    printf("wrote %.2f s (%zu samples, %s) to %s\n", o.seconds, total, o.format.c_str(), path.c_str());
    return 0;
}

// Packet bookkeeping for --check
struct Checker {
    bool on = false;
    uint64_t good = 0, wrong = 0, missing = 0, backwards = 0, total = 0;
    uint32_t expect = 0xFFFFFFFFu;
    std::vector<uint8_t> ts;
    bool keep = false;
    void packets(const uint8_t* p, size_t n) {
        total += n;
        if (keep) ts.insert(ts.end(), p, p + n * 188);
        if (!on) return;
        for (size_t i = 0; i < n; i++) {
            uint32_t num = 0xFFFFFFFFu;
            if (checkTestPacket(p + i * 188, 1, &num)) {
                good++;
                if (expect != 0xFFFFFFFFu) { if (num < expect) backwards++; else missing += num - expect; }
                expect = num + 1;
            } else wrong++;
        }
    }
};

void printTelemetry(const DtmbTelemetry& t) { printf("  %s\n", dtmbSummary(t).c_str()); }

int cmdRx(Args& g) {
    std::string path, out, format = "cs8";
    double rate = 10e6;
    int threads = -1;
    size_t chunk = 16384;
    bool verbose = false;
    Checker chk;
    while (g.more()) {
        const std::string a = g.next();
        if (a == "--rate" && g.more()) rate = atof(g.next().c_str());
        else if (a == "--format" && g.more()) format = g.next();
        else if (a == "--ts" && g.more()) out = g.next();
        else if (a == "--threads" && g.more()) threads = atoi(g.next().c_str());
        else if (a == "--chunk" && g.more()) chunk = (size_t)std::max(1L, atol(g.next().c_str()));
        else if (a == "--check") chk.on = true;
        else if (a == "--verbose") verbose = true;
        else if (a[0] != '-' && path.empty()) path = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (path.empty()) { fprintf(stderr, "usage: dtmbtool rx recording [--rate 10e6] [--format cs8|cu8|cf32] [--ts out.ts] [--check]\n"); return 1; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    chk.keep = !out.empty();
    DtmbReceiver rx;
    rx.configure(rate);
    if (!rx.ready()) { fprintf(stderr, "the sample rate is too low: the signal needs 8 Msps or more\n"); return 1; }
    if (threads >= 0) rx.setDecoderThreads(threads);
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { chk.packets(p, n); });
    rx.setLogCallback([](const std::string& s) { printf("  [log] %s\n", s.c_str()); });
    const size_t bps = format == "cf32" ? 8 : format == "cs16" ? 4 : 2, block = 1 << 18;
    std::vector<unsigned char> raw(block * bps);
    std::vector<cf32> x(block);
    uint64_t total = 0, lastSeq = 0;
    double cpu = 0, lastPrint = 0;
    const auto t0 = std::chrono::steady_clock::now();
    DtmbTelemetry t;
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        const size_t got = (size_t)f.gcount() / bps;
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (format == "cf32") { float re, im; memcpy(&re, &raw[i * 8], 4); memcpy(&im, &raw[i * 8 + 4], 4); x[i] = cf32(re, im); }
            else if (format == "cs16") { int16_t re, im; memcpy(&re, &raw[i * 4], 2); memcpy(&im, &raw[i * 4 + 2], 2); x[i] = cf32(re / 32768.f, im / 32768.f); }
            else if (format == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        }
        for (size_t at = 0; at < got; at += chunk) {
            const size_t n = std::min(chunk, got - at);
            const double c0 = threadCpu();
            rx.feed(x.data() + at, n);
            cpu += threadCpu() - c0;
            total += n;
            if (rx.telemetry(t, lastSeq)) {
                lastSeq = t.seq;
                const double now = (double)total / rate;
                if (verbose && now - lastPrint >= 1.0) { lastPrint = now; printf("t=%.1f s", now); printTelemetry(t); }
            }
        }
    }
    rx.flush();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.telemetry(t, 0);
    printf("read %.2f s of signal in %.2f s (feed() on one thread: %.1fx real time, whole run %.1fx)\n", (double)total / rate, wall, (double)total / rate / std::max(cpu, 1e-9), (double)total / rate / std::max(wall, 1e-9));
    printTelemetry(t);
    if (t.header >= 0 && t.siOk) {
        static const char* mp[5] = {"4QAM-NR", "4QAM", "16QAM", "32QAM", "64QAM"};
        printf("  level %.1f dBFS, C/N (PN) %.1f dB, MER %.1f dB, clock %.1f ppm, echo span %.1f us, LDPC %.1f iterations/codeword, codewords ok %llu bad %llu dropped %llu, BCH corrections %llu, net %.2f Mbit/s (%s)\n",
               t.levelDbfs, t.snrPnDb, t.merDb, t.clockPpm, t.echoSpanUs, t.ldpcIter, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.cwDropped, (unsigned long long)t.bchCorrected, t.netMbps, mp[t.mapping]);
    }
    printf("transport stream: %llu packets", (unsigned long long)chk.total);
    if (chk.on) printf(" (%llu good, %llu wrong, %llu missing between good ones, %llu out of order)", (unsigned long long)chk.good, (unsigned long long)chk.wrong, (unsigned long long)chk.missing, (unsigned long long)chk.backwards);
    printf("\n");
    if (!out.empty()) { std::ofstream o(out, std::ios::binary); o.write((const char*)chk.ts.data(), (std::streamsize)chk.ts.size()); printf("written to %s\n", out.c_str()); }
    if (chk.on) return chk.good > 0 && chk.wrong == 0 ? 0 : 2;
    return chk.total ? 0 : 2;
}

// Generator -> receiver in memory over a range of C/N values: packets are checked, so a number here is a measured result
int cmdSweep(Args& g) {
    SigOpts o;
    o.testPayload = true;
    o.seconds = 3.0;
    double from = 10, to = 24, step = 2;
    int threads = -1;
    while (g.more()) {
        const std::string a = g.next();
        if (a == "--from" && g.more()) from = atof(g.next().c_str());
        else if (a == "--to" && g.more()) to = atof(g.next().c_str());
        else if (a == "--step" && g.more()) step = atof(g.next().c_str());
        else if (a == "--threads" && g.more()) threads = atoi(g.next().c_str());
        else {
            const int r = signalOption(a, g, o);
            if (r <= 0) { fprintf(stderr, "bad option %s\n", a.c_str()); return 1; }
        }
    }
    if (!profileValid(o.sc.tx.profile)) { fprintf(stderr, "this modulation exists at code rate 0.8 only\n"); return 1; }
    o.testPayload = true;
    describe(o);
    printf("   C/N   first pkt   packets good  wrong  missing   codewords ok/bad  LDPC it   C/N(PN)   MER   state\n");
    for (double snr = from; snr <= to + 1e-9; snr += step) {
        SigOpts s = o;
        s.sc.snrDb = snr;
        Signal sig(s.sc, makeSource(s));
        DtmbReceiver rx;
        rx.configure(s.sc.rate);
        if (threads >= 0) rx.setDecoderThreads(threads);
        Checker chk; chk.on = true;
        double first = -1, now = 0;
        rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { chk.packets(p, n); if (first < 0 && chk.good) first = now; });
        const size_t total = (size_t)(s.seconds * s.sc.rate), block = 16384;
        std::vector<cf32> x(block);
        for (size_t done = 0; done < total;) {
            const size_t n = std::min(block, total - done);
            sig.generate(x.data(), n);
            for (auto& v : x) v = cf32(std::round(v.real() * 128.f) / 128.f, std::round(v.imag() * 128.f) / 128.f);
            now = (double)done / s.sc.rate;
            rx.feed(x.data(), n);
            done += n;
        }
        rx.flush();
        DtmbTelemetry t;
        rx.telemetry(t, 0);
        printf("  %5.1f   %8.2f   %9llu  %5llu  %7llu   %8llu/%-6llu  %7.1f   %7.1f  %5.1f   %d\n", snr, first, (unsigned long long)chk.good, (unsigned long long)chk.wrong, (unsigned long long)chk.missing,
               (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.ldpcIter, t.snrPnDb, t.merDb, t.state);
        fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dtmbtool gen|rx|sweep ... (see the top of tools/dtmbtool.cpp)\n");
        return 1;
    }
    Args g{argc, argv, 2};
    const std::string cmd = argv[1];
    if (cmd == "gen") return cmdGen(g);
    if (cmd == "rx") return cmdRx(g);
    if (cmd == "sweep") return cmdSweep(g);
    fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 1;
}
