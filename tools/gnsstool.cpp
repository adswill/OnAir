// gnsstool: write the GNSS test signal to a file, run the receiver over a recording, or both in memory.
//   gnsstool gen <file.cs8|.cf32> <seconds> [options]     write the simulated sky (8 bit signed pairs, or 32 bit float pairs)
//   gnsstool rx <file> --rate <Hz> [--format cs8|cu8|cf32] [--center <MHz>] [--secs <s>] [--quiet]   run the receiver over a recording
//   gnsstool run <seconds> [options]                        generator straight into the receiver (no file)
// Generator options: --rate Hz (default 4e6)  --cn0 dBHz (zenith, default 44)  --cfo Hz  --sro ppm  --sats N  --cold (start 13 s into the frame)
//   --seed N  --lat deg --lon deg --height m  --jammer  --dc X  --noise X (rms per component, default 0.14)
// The simulated satellites are synthetic (see docs/modes/gnss.md): nothing here is a real signal.
#include "dect2/gnss_rx.h"
#include "dect2/gnss_sim.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
using namespace dect2;

struct Opt {
    double rate = 4e6, cn0 = 44, cfo = 0, sro = 0, lat = 25.2, lon = 55.36, height = 10, dc = 0, noise = 0.14, center = 1575.42, secs = 0;
    int sats = 0;
    unsigned seed = 1;
    bool cold = false, jammer = false, quiet = false;
    std::string format = "cs8";
};

static GnssSimConfig simConfig(const Opt& o) {
    GnssSimConfig c;
    c.cn0Top = o.cn0; c.cfoHz = o.cfo; c.sroPpm = o.sro; c.maxSats = o.sats; c.warmStart = !o.cold; c.seed = o.seed;
    c.latDeg = o.lat; c.lonDeg = o.lon; c.heightM = o.height; c.jammer = o.jammer; c.dcOffset = o.dc; c.noiseRms = o.noise;
    return c;
}

static bool parseOpts(int argc, char** argv, int from, Opt& o) {
    for (int i = from; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](double& v) { if (i + 1 >= argc) return false; v = atof(argv[++i]); return true; };
        double v = 0;
        if (a == "--rate" && next(v)) o.rate = v;
        else if (a == "--cn0" && next(v)) o.cn0 = v;
        else if (a == "--cfo" && next(v)) o.cfo = v;
        else if (a == "--sro" && next(v)) o.sro = v;
        else if (a == "--lat" && next(v)) o.lat = v;
        else if (a == "--lon" && next(v)) o.lon = v;
        else if (a == "--height" && next(v)) o.height = v;
        else if (a == "--dc" && next(v)) o.dc = v;
        else if (a == "--noise" && next(v)) o.noise = v;
        else if (a == "--center" && next(v)) o.center = v;
        else if (a == "--secs" && next(v)) o.secs = v;
        else if (a == "--sats" && next(v)) o.sats = (int)v;
        else if (a == "--seed" && next(v)) o.seed = (unsigned)v;
        else if (a == "--cold") o.cold = true;
        else if (a == "--jammer") o.jammer = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--format" && i + 1 < argc) o.format = argv[++i];
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

static void printTable(const GnssTelemetry& t) {
    printf("  t=%6.1f s  %s | level %.1f dBFS, clip %.2f%%, cfo %+.0f Hz | search %c%02d round %u | frames ok %llu bad %llu\n", t.signalSecs, gnssSummary(t).c_str(), t.levelDbfs, t.clipPercent, t.cfoHz,
           gnssSystemLetter(t.searchSys), t.searchPrn, t.searchRounds, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    for (auto& c : t.channels) {
        printf("    %c%02d  %-10s  C/N0 %4.1f  dopp %+8.1f Hz  code %7.2f", gnssSystemLetter(c.sys), c.prn, gnssChStateName(c.state), c.cn0, c.dopplerHz, c.codePhase);
        if (c.hasAzEl) printf("  az %5.1f el %4.1f", c.azDeg, c.elDeg);
        printf("  %s  frames %u/%u", c.used ? "USED" : "    ", c.framesOk, c.framesBad);
        if (c.used) printf("  res %+.1f m", c.residualM);
        printf("\n");
    }
}

static void printFix(const GnssTelemetry& t) {
    const GnssFix& f = t.fix;
    if (!f.valid) { printf("no position fix\n"); return; }
    printf("fix: %s\n  lat %.6f  lon %.6f  height %.1f m  (ECEF %.1f %.1f %.1f)\n  HDOP %.2f VDOP %.2f PDOP %.2f  est. horizontal error %.1f m, residual rms %.2f m\n", f.type.c_str(), f.latDeg, f.lonDeg, f.heightM, f.ecef[0], f.ecef[1], f.ecef[2], f.hdop,
           f.vdop, f.pdop, f.hErrM, f.residualRmsM);
    if (f.timeValid) printf("  GPS week %d tow %.3f  =  %04d-%02d-%02d %02d:%02d:%06.3f %s\n", f.gpsWeek, f.gpsTow, f.year, f.month, f.day, f.hour, f.minute, f.second, f.leapSeconds >= 0 ? "UTC" : "(UTC assumed, leap seconds not received)");
    printf("  first fix after %.1f s of signal, %u fixes, receiver clock error %.1f m (%.1f ns)\n", f.firstFixSecs, f.fixCount, f.clockBiasM[0], f.clockBiasM[0] / 0.299792458);
}

static int cmdRun(const Opt& o, double secs, bool toFile, FILE* fo, bool cs8) {
    GnssSim sim(simConfig(o), o.rate);
    GnssReceiver rx;
    if (!toFile) { rx.configure(o.rate); rx.setCenterMhz(o.center); rx.setLogCallback([](const std::string& s) { printf("    [log] %s\n", s.c_str()); }); }
    const size_t block = 65536;
    std::vector<cf32> buf(block);
    const size_t total = (size_t)(secs * o.rate);
    double nextPrint = 5;
    uint64_t seq = 0;
    GnssTelemetry t;
    const auto t0 = std::chrono::steady_clock::now();
    double cpuRx = 0;
    for (size_t done = 0; done < total; done += block) {
        const size_t n = std::min(block, total - done);
        sim.generate(buf.data(), n);
        if (toFile) {
            if (cs8) {
                std::vector<int8_t> q(2 * n);
                for (size_t i = 0; i < n; i++) {
                    q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].real() * 127.f)));
                    q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].imag() * 127.f)));
                }
                fwrite(q.data(), 1, q.size(), fo);
            } else fwrite(buf.data(), sizeof(cf32), n, fo);
        } else {
            // the 8 bit quantisation of a radio
            for (size_t i = 0; i < n; i++) buf[i] = cf32(std::round(buf[i].real() * 127.f) / 127.f, std::round(buf[i].imag() * 127.f) / 127.f);
            timespec a, b;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
            rx.feed(buf.data(), n);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
            cpuRx += (double)(b.tv_sec - a.tv_sec) + 1e-9 * (double)(b.tv_nsec - a.tv_nsec);
            if (rx.telemetry(t, seq)) {
                seq = t.seq;
                if (!o.quiet && t.signalSecs >= nextPrint) { printTable(t); nextPrint += 5; }
            }
        }
    }
    if (!toFile) {
        printTable(t);
        printFix(t);
        printf("receiver: %.2f s of CPU for %.1f s of signal at %.3f Msps: %.1fx real time; acquisition FFTs %llu\n", cpuRx, secs, o.rate / 1e6, secs / cpuRx, (unsigned long long)rx.workUnitsUsed());
        double cf, ct, ca;
        rx.cpuBreakdown(&cf, &ct, &ca);
        printf("  of which front end %.2f s, channels %.2f s, search %.2f s\n", cf, ct, ca);
        const double* r = sim.receiverEcef();
        if (t.fix.valid) {
            double d[3] = {t.fix.ecef[0] - r[0], t.fix.ecef[1] - r[1], t.fix.ecef[2] - r[2]};
            printf("true position error (ECEF distance): %.2f m\n", std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
    }
    (void)t0;
    return 0;
}

static int cmdRx(const std::string& path, const Opt& o) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    GnssReceiver rx;
    rx.configure(o.rate);
    rx.setCenterMhz(o.center);
    rx.setLogCallback([](const std::string& s) { printf("    [log] %s\n", s.c_str()); });
    const size_t block = 65536;
    std::vector<cf32> buf(block);
    std::vector<uint8_t> raw(block * 8);
    const size_t bytes = o.format == "cf32" ? 8 : 2;
    double nextPrint = 5, done = 0;
    uint64_t seq = 0;
    GnssTelemetry t;
    for (;;) {
        const size_t got = fread(raw.data(), bytes, block, f);
        if (got == 0) break;
        for (size_t i = 0; i < got; i++) {
            if (o.format == "cf32") { float v[2]; memcpy(v, &raw[8 * i], 8); buf[i] = cf32(v[0], v[1]); }
            else if (o.format == "cu8") buf[i] = cf32(((float)raw[2 * i] - 127.5f) / 127.5f, ((float)raw[2 * i + 1] - 127.5f) / 127.5f);
            else buf[i] = cf32((float)(int8_t)raw[2 * i] / 128.f, (float)(int8_t)raw[2 * i + 1] / 128.f);
        }
        rx.feed(buf.data(), got);
        done += (double)got / o.rate;
        if (rx.telemetry(t, seq)) { seq = t.seq; if (!o.quiet && t.signalSecs >= nextPrint) { printTable(t); nextPrint += 5; } }
        if (o.secs > 0 && done >= o.secs) break;
    }
    fclose(f);
    printTable(t);
    printFix(t);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: gnsstool gen|rx|run ... (see the top of tools/gnsstool.cpp)\n"); return 1; }
    const std::string cmd = argv[1];
    Opt o;
    if (cmd == "gen" && argc >= 4) {
        if (!parseOpts(argc, argv, 4, o)) return 1;
        const std::string path = argv[2];
        const bool cs8 = path.size() > 4 && path.substr(path.size() - 4) == ".cs8";
        FILE* fo = fopen(path.c_str(), "wb");
        if (!fo) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        const int r = cmdRun(o, atof(argv[3]), true, fo, cs8);
        fclose(fo);
        return r;
    }
    if (cmd == "run" && argc >= 3) {
        if (!parseOpts(argc, argv, 3, o)) return 1;
        return cmdRun(o, atof(argv[2]), false, nullptr, false);
    }
    if (cmd == "rx" && argc >= 3) {
        if (!parseOpts(argc, argv, 3, o)) return 1;
        return cmdRx(argv[2], o);
    }
    printf("usage: gnsstool gen <file> <seconds> | rx <file> --rate <Hz> | run <seconds>\n");
    return 1;
}
