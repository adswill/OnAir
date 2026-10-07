// dvbstool: DVB-S, DVB-S2 and DVB-S2X test signals and offline reception.
//
//   dvbstool gen out.cs8 [--secs 10] [--rate 10] [--std s|s2] [--mod qpsk|8psk|16apsk|32apsk] [--fec 2/3] [--rs 5] [--roll 0.35] [--short] [--pilots]
//                        [--inv] [--snr 30] [--cfo 0] [--ppm 0] [--vcm] [--pn 0|1|2|3] [--dc 0] [--iq-db 0] [--iq-deg 0] [--seed 1]
//                        [--ts in.ts] [--format cs8|cf32]
//       Writes the L-band IF of a satellite transponder as complex baseband: `--rate` is the sample rate in Msps, `--rs` the symbol rate in Msym/s,
//       `--snr` is Es/N0 in dB. The payload is the built-in test programme, or the packets of `--ts` (repeated). Nothing is transmitted.
//       --pn 1..3 adds LNB phase noise (EN 302 307-1 H.8 typical and critical masks, M.2 non DTH mask), --dc and --iq-* receiver faults.
//   dvbstool rx recording [--rate 10] [--format cs8|cu8|cf32] [--rs 0] [--std 0|1|2] [--roll 0] [--isi -1] [--pls -1] [--ts out.ts] [--secs 0]
//       Runs the receiver on a recording (`--rate` in Msps) and prints what it found. `--rs` forces the symbol rate (Msym/s, 0 = from the spectrum),
//       `--std` 1 or 2 limits the search to DVB-S or DVB-S2.
//   dvbstool bench [gen options]
//       Makes the test signal in memory, runs the receiver on it the way the engine does (frames are dropped when the decoder threads are behind) and
//       reports the CPU time of the receiver thread and of all threads per second of signal.
//   dvbstool ranges
//       The symbol rates the receiver accepts at each input sample rate.
#include "dect2/dvbs_gen.h"
#include "dect2/dvbs_rx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <map>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include <time.h>

using namespace dect2;
using namespace dect2::dvbs;

namespace {

int parseRateDvbs(const std::string& s) {
    static const char* n[5] = {"1/2", "2/3", "3/4", "5/6", "7/8"};
    for (int i = 0; i < 5; i++) if (s == n[i]) return i;
    return -1;
}
int parseRateDvbs2(const std::string& s) {
    for (int i = 0; i < kS2Rates; i++) if (s == s2RateName(i)) return i;
    return -1;
}

void writeSamples(std::ofstream& f, const cf32* x, size_t n, bool cf32out) {
    if (cf32out) { f.write((const char*)x, (std::streamsize)(n * sizeof(cf32))); return; }
    std::vector<int8_t> b(2 * n);
    for (size_t i = 0; i < n; i++) {
        b[2 * i] = (int8_t)std::max(-128.f, std::min(127.f, std::round(x[i].real() * 128.f)));
        b[2 * i + 1] = (int8_t)std::max(-128.f, std::min(127.f, std::round(x[i].imag() * 128.f)));
    }
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

struct GenOptions {
    std::string out, tsIn, format;
    double secs = 10, rate = 10, rs = 0;
    DvbsSignalConfig c;
    std::string fecName = "2/3";
};

// returns 0 when the options are fine
int parseGen(int argc, char** argv, int first, GenOptions& g, bool needOut) {
    DvbsSignalConfig& c = g.c;
    c.tx.standard = 2;
    for (int i = first; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--secs") g.secs = atof(next().c_str());
        else if (a == "--rate") g.rate = atof(next().c_str());
        else if (a == "--std") { const std::string v = next(); c.tx.standard = v == "s" || v == "1" ? 1 : 2; }
        else if (a == "--mod") { const std::string v = next(); c.tx.mod = v == "8psk" ? k8psk : v == "16apsk" ? k16apsk : v == "32apsk" ? k32apsk : kQpsk; }
        else if (a == "--fec") g.fecName = next();
        else if (a == "--rs") g.rs = atof(next().c_str());
        else if (a == "--roll") c.tx.rollOff = atof(next().c_str());
        else if (a == "--short") c.tx.shortFrame = true;
        else if (a == "--pilots") c.tx.pilots = true;
        else if (a == "--inv") c.inverted = true;
        else if (a == "--snr") c.snrDb = atof(next().c_str());
        else if (a == "--cfo") c.cfoHz = atof(next().c_str());
        else if (a == "--ppm") c.clockPpm = atof(next().c_str());
        else if (a == "--vcm") c.tx.vcm = true;
        else if (a == "--pn") c.phaseNoise = atoi(next().c_str());
        else if (a == "--dc") c.dcOffset = atof(next().c_str());
        else if (a == "--iq-db") c.iqGainDb = atof(next().c_str());
        else if (a == "--iq-deg") c.iqPhaseDeg = atof(next().c_str());
        else if (a == "--seed") c.seed = (uint32_t)atoi(next().c_str());
        else if (a == "--ts") g.tsIn = next();
        else if (a == "--format") g.format = next();
        else if (g.out.empty() && a[0] != '-' && needOut) g.out = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    const int fec = c.tx.standard == 1 ? parseRateDvbs(g.fecName) : parseRateDvbs2(g.fecName);
    if (fec < 0) { fprintf(stderr, "code rate %s does not exist in this standard\n", g.fecName.c_str()); return 1; }
    c.tx.rate = fec;
    if (c.tx.standard == 1) c.tx.mod = kQpsk;
    c.sampleRate = g.rate * 1e6;
    c.tx.symbolRate = g.rs > 0 ? g.rs * 1e6 : std::min(5e6, dvbsMaxSymbolRate(c.sampleRate, c.tx.rollOff));
    if (c.tx.standard == 2 && !s2Dims(c.tx.mod, c.tx.rate, c.tx.shortFrame).ok) { fprintf(stderr, "%s %s does not exist as a %s frame\n", s2ModName(c.tx.mod), g.fecName.c_str(), c.tx.shortFrame ? "short" : "normal"); return 1; }
    if (c.tx.symbolRate > dvbsMaxSymbolRate(c.sampleRate, c.tx.rollOff) * 1.001)
        fprintf(stderr, "warning: %.3f Msym/s needs more than %.1f Msps (the receiver takes up to %.3f Msym/s there)\n", c.tx.symbolRate / 1e6, g.rate, dvbsMaxSymbolRate(c.sampleRate, c.tx.rollOff) / 1e6);
    return 0;
}

double threadCpu() { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
#ifdef _WIN32
double processCpu() {   // user plus kernel time of the whole process, in 100 ns units
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto s = [](const FILETIME& f) { return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) * 1e-7; };
    return s(k) + s(u);
}
#else
double processCpu() { rusage r; getrusage(RUSAGE_SELF, &r); return (double)r.ru_utime.tv_sec + 1e-6 * (double)r.ru_utime.tv_usec + (double)r.ru_stime.tv_sec + 1e-6 * (double)r.ru_stime.tv_usec; }
#endif

int cmdBench(int argc, char** argv) {
    GenOptions g;
    g.secs = 4;
    if (parseGen(argc, argv, 2, g, false)) return 1;
    DvbsSignal sig(g.c);
    const size_t total = (size_t)(g.secs * g.c.sampleRate);
    std::vector<cf32> x(total);
    for (size_t i = 0; i < total; i += 65536) sig.generate(x.data() + i, std::min<size_t>(65536, total - i));
    for (auto& v : x) { v = cf32(std::round(std::min(127.f, std::max(-128.f, v.real() * 128.f))) / 128.f, std::round(std::min(127.f, std::max(-128.f, v.imag() * 128.f))) / 128.f); }
    DvbsReceiver rx;
    rx.configure(g.c.sampleRate);
    rx.setBlocking(false);
    uint64_t packets = 0;
    rx.setPacketCallback([&](const uint8_t*, size_t n, double) { packets += n; });
    const double c0 = processCpu(), t0 = threadCpu();
    const auto w0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < total; i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, total - i));
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const double tc = threadCpu() - t0;
    rx.flush();                                  // the decoder threads may still be working on the last frames
    const double pc = processCpu() - c0;
    DvbsTelemetry t;
    rx.telemetry(t, 0);
    printf("%s\n", dvbsSummary(t).c_str());
    printf("%.1f s of signal at %.1f Msps, %.3f Msym/s: receiver thread %.2f s CPU (%.1fx real time), all threads %.2f s CPU (%.1fx), wall %.2f s; frames/packets ok %llu bad %llu; %llu transport stream packets\n", g.secs, g.rate, g.c.tx.symbolRate / 1e6, tc, g.secs / tc,
           pc, g.secs / pc, wall, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)packets);
    return 0;
}

int cmdGen(int argc, char** argv) {
    GenOptions g;
    if (parseGen(argc, argv, 2, g, true)) return 1;
    if (g.out.empty()) { fprintf(stderr, "usage: dvbstool gen out.cs8 [options] (see the top of tools/dvbstool.cpp)\n"); return 1; }
    DvbsSignalConfig& c = g.c;
    const std::string& out = g.out;
    const std::string& tsIn = g.tsIn;
    const double secs = g.secs, rate = g.rate;
    const std::string& fecName = g.fecName;
    std::string format = g.format;
    if (format.empty()) format = out.size() > 5 && out.substr(out.size() - 5) == ".cf32" ? "cf32" : "cs8";
    std::vector<uint8_t> tsData;
    if (!tsIn.empty()) {
        std::ifstream t(tsIn, std::ios::binary);
        if (!t) { fprintf(stderr, "cannot open %s\n", tsIn.c_str()); return 1; }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        for (size_t o = 0; o + 188 <= raw.size(); o += 188) if (raw[o] == 0x47) tsData.insert(tsData.end(), raw.begin() + (std::ptrdiff_t)o, raw.begin() + (std::ptrdiff_t)o + 188);
        if (tsData.empty()) { fprintf(stderr, "%s holds no transport stream packets\n", tsIn.c_str()); return 1; }
        size_t pos = 0;
        c.ts = [&tsData, pos](uint8_t* p) mutable { memcpy(p, &tsData[pos], 188); pos = (pos + 188) % tsData.size(); };
    }
    DvbsSignal sig(c);
    std::ofstream f(out, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
    const size_t total = (size_t)(secs * c.sampleRate);
    std::vector<cf32> buf(1 << 16);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t done = 0; done < total; done += buf.size()) {
        const size_t m = std::min(buf.size(), total - done);
        sig.generate(buf.data(), m);
        writeSamples(f, buf.data(), m, format == "cf32");
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (c.tx.standard == 1) printf("DVB-S QPSK %s", fecName.c_str());
    else printf("DVB-S2 %s %s%s%s", s2ModName(c.tx.mod), fecName.c_str(), c.tx.shortFrame ? " short" : "", c.tx.pilots ? " pilots" : "");
    printf(", %.3f Msym/s, roll-off %.2f, %.1f Msps, Es/N0 %.1f dB%s%s: %.1f s written to %s (%s, %.1fx real time), net bit rate %.3f Mbit/s\n", c.tx.symbolRate / 1e6, c.tx.rollOff, rate, c.snrDb,
           c.inverted ? ", spectrum inverted" : "", c.tx.vcm ? ", VCM" : "", secs, out.c_str(), format.c_str(), secs / std::max(1e-9, dt), dvbsNetBitrate(c.tx) / 1e6);
    return 0;
}

int cmdRx(int argc, char** argv) {
    std::string path, tsOut, format;
    double rate = 10, rs = 0, roll = 0, secsMax = 0;
    int stdHint = 0, isi = -1, pls = -1;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--rate") rate = atof(next().c_str());
        else if (a == "--format") format = next();
        else if (a == "--rs") rs = atof(next().c_str());
        else if (a == "--std") stdHint = atoi(next().c_str());
        else if (a == "--roll") roll = atof(next().c_str());
        else if (a == "--isi") isi = atoi(next().c_str());
        else if (a == "--pls") pls = atoi(next().c_str());
        else if (a == "--ts") tsOut = next();
        else if (a == "--secs") secsMax = atof(next().c_str());
        else if (path.empty() && a[0] != '-') path = a;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (path.empty()) { fprintf(stderr, "usage: dvbstool rx recording [--rate 10] [--format cs8|cu8|cf32] ... (see the top of tools/dvbstool.cpp)\n"); return 1; }
    if (format.empty()) {
        const size_t d = path.rfind('.');
        const std::string ext = d == std::string::npos ? "" : path.substr(d + 1);
        format = ext == "cf32" ? "cf32" : ext == "cu8" ? "cu8" : "cs8";
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    DvbsReceiver rx;
    rx.configure(rate * 1e6);
    if (!rx.ready()) { fprintf(stderr, "%.2f Msps is below the %.1f Msps this mode needs\n", rate, dvbsTuning().minSampleRate / 1e6); return 1; }
    rx.setBlocking(true);                     // a recording is not in a hurry: wait for the decoder threads instead of dropping frames
    if (rs > 0) rx.setSymbolRate(rs * 1e6);
    if (stdHint) rx.setStandardHint(stdHint);
    if (roll > 0) rx.setRollOff(roll);
    if (isi >= 0) rx.setIsi(isi);
    if (pls >= 0) rx.setPlScrambling(pls);
    std::vector<uint8_t> ts;
    uint64_t packets = 0, bad = 0, nulls = 0, ccErrors = 0;
    std::map<int, uint64_t> pids;
    std::map<int, int> lastCc;
    double signalSecs = 0;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double secs) {
        signalSecs += secs;
        for (size_t i = 0; i < n; i++, p += 188) {
            packets++;
            const int pid = (p[1] & 0x1F) << 8 | p[2];
            if (p[1] & 0x80) { bad++; continue; }
            pids[pid]++;
            if (pid == 0x1FFF) { nulls++; continue; }
            const int cc = p[3] & 15, af = (p[3] >> 4) & 3;
            auto it = lastCc.find(pid);
            if ((af & 1) && it != lastCc.end() && cc != (it->second + 1) % 16 && !(p[3] & 0x20 && af == 2)) ccErrors++;
            if (af & 1) lastCc[pid] = cc;
        }
        if (!tsOut.empty()) ts.insert(ts.end(), p - n * 188, p);
    });
    std::string lastLine;
    rx.setLogCallback([&](const std::string& s) { printf("[%7.2f s] %s\n", signalSecs, s.c_str()); });
    const size_t bps = format == "cf32" ? 8 : 2, block = 1 << 16;
    std::vector<unsigned char> raw(block * bps);
    std::vector<cf32> x(block);
    uint64_t total = 0, seq = 0, nextReport = (uint64_t)(rate * 1e6);
    DvbsTelemetry tel;
    const auto t0 = std::chrono::steady_clock::now();
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        const size_t got = (size_t)f.gcount() / bps;
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (format == "cf32") { float re, im; memcpy(&re, &raw[i * 8], 4); memcpy(&im, &raw[i * 8 + 4], 4); x[i] = cf32(re, im); }
            else if (format == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        }
        rx.feed(x.data(), got);
        total += got;
        DvbsTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; tel = t; }
        if (total >= nextReport) { nextReport += (uint64_t)(rate * 1e6); printf("[%7.2f s] %s\n", (double)total / (rate * 1e6), dvbsSummary(tel).c_str()); }
        if (secsMax > 0 && (double)total / (rate * 1e6) >= secsMax) break;
    }
    rx.flush();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    DvbsTelemetry t;
    if (rx.telemetry(t, 0)) tel = t;
    printf("\nread %.2f s of signal in %.1f s (%.1fx real time)\n", (double)total / (rate * 1e6), wall, (double)total / (rate * 1e6) / std::max(1e-9, wall));
    printf("%s\n", dvbsSummary(tel).c_str());
    if (tel.standard) {
        printf("  symbol rate %.4f Msym/s (spectrum %.4f), roll-off %.2f (%s), carrier %+.1f kHz, spectrum %s\n", tel.symbolRate / 1e6, tel.symbolRateSpectrum / 1e6, tel.rollOff,
               tel.rollOffSource == 2 ? "signalled" : tel.rollOffSource == 1 ? "measured" : "assumed", tel.cfoHz / 1e3, tel.inverted ? "inverted" : "normal");
        printf("  Es/N0 %.1f dB (MER)  pre-FEC BER %s  frames/packets ok %llu bad %llu", tel.merDb, tel.preFecBer < 0 ? "?" : (std::to_string(tel.preFecBer)).c_str(), (unsigned long long)tel.blocksOk, (unsigned long long)tel.blocksBad);
        if (tel.standard >= 2) printf("  LDPC iterations %.1f  BCH bad %llu  user packet CRC errors %llu  dummy frames %llu", tel.ldpcIterAvg, (unsigned long long)tel.bchBad, (unsigned long long)tel.crcErrors, (unsigned long long)tel.framesDummy);
        else printf("  RS clean %llu corrected %llu failed %llu", (unsigned long long)tel.rsClean, (unsigned long long)tel.rsCorrected, (unsigned long long)tel.rsFailed);
        printf("\n");
        if (!tel.signalNote.empty()) printf("  note: %s\n", tel.signalNote.c_str());
    }
    printf("transport stream: %llu packets (%llu with the error indicator, %llu null, %llu continuity errors), signal time %.2f s", (unsigned long long)packets, (unsigned long long)bad, (unsigned long long)nulls, (unsigned long long)ccErrors, signalSecs);
    if (signalSecs > 0) printf(", %.3f Mbit/s", (double)packets * 188 * 8 / signalSecs / 1e6);
    printf("\n");
    int shown = 0;
    for (const auto& kv : pids) { if (shown++ >= 8) break; printf("  PID %4d (0x%04X): %llu packets\n", kv.first, kv.first, (unsigned long long)kv.second); }
    if (!tsOut.empty()) { std::ofstream o(tsOut, std::ios::binary); o.write((const char*)ts.data(), (std::streamsize)ts.size()); printf("written to %s\n", tsOut.c_str()); }
    return packets == 0 ? 2 : 0;
}

int cmdRanges() {
    printf("symbol rates the receiver takes (about 1.3 samples per symbol times (1 + roll-off) of bandwidth; the automatic search needs about 40 spectrum bins across the carrier):\n");
    printf("  %8s  %22s  %22s\n", "Msps", "roll-off 0.35: min auto .. max", "roll-off 0.05: min auto .. max");
    for (double fs : {2.0, 4.0, 8.0, 10.0, 12.5, 16.0, 20.0})
        printf("  %8.1f  %9.3f .. %-8.3f Msym/s  %9.3f .. %-8.3f Msym/s\n", fs, dvbsMinAutoSymbolRate(fs * 1e6) / 1e6, dvbsMaxSymbolRate(fs * 1e6, 0.35) / 1e6, dvbsMinAutoSymbolRate(fs * 1e6) / 1e6, dvbsMaxSymbolRate(fs * 1e6, 0.05) / 1e6);
    printf("a manual symbol rate (--rs) works down to about 0.03 Msym/s at any sample rate\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "gen")) return cmdGen(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "rx")) return cmdRx(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "bench")) return cmdBench(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "ranges")) return cmdRanges();
    fprintf(stderr, "usage: dvbstool gen out.cs8 [options] | dvbstool rx recording --rate Msps [options] | dvbstool bench [gen options] | dvbstool ranges (see the top of tools/dvbstool.cpp)\n");
    return 1;
}
