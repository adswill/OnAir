// Iridium command line tool: write the test signal to a file, decode a recording, show the simulated sky.
//   iridiumtool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cn0 dBHz] [--sats n] [--seed n] [--cfo Hz] [--sro ppm]
//                   [--center MHz] [--no-voice] [--no-doppler]
//   iridiumtool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center MHz] [--raw] [--threshold dB]
//       --raw prints every burst with a unique word as a gr-iridium "RAW:" line (iridium-toolkit's iridium-parser.py reads them)
//   iridiumtool sky [secs] [--seed n] [--sats n]
#include "dect2/iridium_gen.h"
#include "dect2/iridium_rx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
using namespace dect2;

static void usage() {
    printf("usage:\n"
           "  iridiumtool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cn0 dBHz] [--sats n] [--seed n] [--cfo Hz] [--sro ppm] [--center MHz] [--no-voice] [--no-doppler]\n"
           "  iridiumtool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center MHz] [--raw] [--threshold dB]\n"
           "  iridiumtool sky [secs] [--seed n] [--sats n]\n");
}

static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int gen(int argc, char** argv) {
    if (argc < 4) { usage(); return 1; }
    const double secs = atof(argv[2]);
    const std::string file = argv[3];
    double rate = 10e6;
    SynthConfig c;
    c.snrDb = 25;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--rate") rate = val();
        else if (a == "--snr") c.snrDb = val();
        else if (a == "--cn0") c.modeVal[0] = val();
        else if (a == "--sats") c.modeOpt[0] = (int)val();
        else if (a == "--seed") c.modeOpt[1] = (int)val();
        else if (a == "--cfo") c.cfoHz = val();
        else if (a == "--sro") c.sroPpm = val();
        else if (a == "--center") c.modeVal[1] = val();
        else if (a == "--no-voice") c.modeOpt[2] = 1;
        else if (a == "--no-doppler") c.modeOpt[3] = 1;
        else { printf("unknown option %s\n", a.c_str()); return 1; }
    }
    c.mode = 21;
    auto s = makeIridiumSynth(c, rate);
    FILE* f = fopen(file.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", file.c_str()); return 1; }
    const bool cs8 = !endsWith(file, ".cf32");
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    std::vector<int8_t> b8(2 * buf.size());
    float peak = 0;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        s->generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) peak = std::max(peak, std::max(std::fabs(buf[i].real()), std::fabs(buf[i].imag())));
        if (cs8) {
            for (size_t i = 0; i < n; i++) {
                b8[2 * i] = (int8_t)std::lround(std::max(-128.f, std::min(127.f, buf[i].real() * 128)));
                b8[2 * i + 1] = (int8_t)std::lround(std::max(-128.f, std::min(127.f, buf[i].imag() * 128)));
            }
            fwrite(b8.data(), 1, 2 * n, f);
        } else {
            fwrite(buf.data(), sizeof(cf32), n, f);
        }
        done += n;
    }
    fclose(f);
    printf("wrote %.1f s at %.3f Msps to %s (%s), peak %.2f\n", secs, rate / 1e6, file.c_str(), cs8 ? "cs8" : "cf32", peak);
    return 0;
}

static std::string bitsStr(const std::vector<uint8_t>& b) {
    std::string s;
    s.reserve(b.size());
    for (auto v : b) s += char('0' + (v & 1));
    return s;
}

static int rx(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    const std::string file = argv[2];
    double rate = 0, center = 1622, thr = 0;
    std::string fmt = endsWith(file, ".cf32") ? "cf32" : endsWith(file, ".cu8") ? "cu8" : "cs8";
    bool raw = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) fmt = argv[++i];
        else if (a == "--center" && i + 1 < argc) center = atof(argv[++i]);
        else if (a == "--threshold" && i + 1 < argc) thr = atof(argv[++i]);
        else if (a == "--raw") raw = true;
        else { printf("unknown option %s\n", a.c_str()); return 1; }
    }
    if (rate <= 0) { printf("--rate is needed\n"); return 1; }
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", file.c_str()); return 1; }
    IridiumReceiver r;
    r.configure(rate);
    r.setCenterMhz(center);
    if (thr > 0) r.setThresholdDb(thr);
    r.setOffline(true);
    r.setLogCallback([](const std::string& s) { printf("    [log] %s\n", s.c_str()); });
    std::mutex m;
    const long startUnix = (long)kIridiumSynthEpoch;
    uint64_t id = 0;
    r.setBurstCallback([&](const IridiumBurstBits& b, const IridiumFrame& fr) {
        std::lock_guard<std::mutex> lk(m);
        if (raw) {
            // gr-iridium's output line: RAW: <file> <ms> <freq> A:OK I:<id> <confidence>% <level> <symbols> <bits incl. unique word>
            const char* uw = b.downlink ? "001100000011000011110011" : "110011000011110011111100";
            printf("RAW: i-%ld-t1 %012.4f %010.0f A:OK I:%011llu %3.0f%% %7.5f %3zu %s%s\n", startUnix, b.timeSec * 1e3, b.freqHz, (unsigned long long)id++,
                   b.confidence, std::pow(10.0, b.levelDb / 20), b.bits.size() / 2, uw, bitsStr(b.bits).c_str());
            return;
        }
        printf("%9.4f s %11.0f Hz %s %3.0f%% %-4s", b.timeSec, b.freqHz, b.downlink ? "DL" : "UL", b.confidence, fr.typeName.empty() ? "?" : fr.typeName.c_str());
        if (fr.type == IridiumType::Voice) { printf(" (voice: not decoded)\n"); return; }
        if (!fr.ok) { printf(" (no check)\n"); return; }
        if (fr.satId >= 0) printf(" sat %3d", fr.satId);
        if (fr.beamId >= 0) printf(" beam %2d", fr.beamId);
        if (fr.hasPosition) printf(" pos %+.2f %+.2f alt %.0f km", fr.lat, fr.lon, fr.altKm);
        if (fr.type == IridiumType::IRA) printf(" paged %d", fr.paged);
        if (fr.hasTime) printf(" time %.3f", fr.unixTime);
        if (fr.ric >= 0) printf(" ric %d seq %d part %d/%d \"%s\"", fr.ric, fr.msgSeq, fr.block, fr.blocks, fr.msgText.c_str());
        printf("\n");
    });
    const bool isF = fmt == "cf32", isU = fmt == "cu8";
    std::vector<cf32> buf(65536);
    std::vector<uint8_t> b8(2 * buf.size());
    double secs = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        size_t n;
        if (isF) n = fread(buf.data(), sizeof(cf32), buf.size(), f);
        else {
            n = fread(b8.data(), 2, buf.size(), f);
            for (size_t i = 0; i < n; i++) {
                if (isU) buf[i] = cf32((b8[2 * i] - 127.5f) / 128.f, (b8[2 * i + 1] - 127.5f) / 128.f);
                else buf[i] = cf32((int8_t)b8[2 * i] / 128.f, (int8_t)b8[2 * i + 1] / 128.f);
            }
        }
        if (n == 0) break;
        r.feed(buf.data(), n);
        secs += n / rate;
    }
    r.flush();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fclose(f);
    IridiumTelemetry t;
    r.telemetry(t, 0);
    printf("%s\n", iridiumSummary(t).c_str());
    printf("%.1f s of signal in %.2f s (%.1fx real time): %llu bursts, %llu with a unique word (%llu DL, %llu UL), %llu dropped, %llu frames ok, %llu failed\n",
           secs, wall, secs / std::max(1e-9, wall), (unsigned long long)t.bursts, (unsigned long long)t.uwOk, (unsigned long long)t.downlink,
           (unsigned long long)t.uplink, (unsigned long long)t.dropped, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    printf("frame types:");
    for (int i = 0; i < kIridiumTypeSlots; i++) if (t.typeCount[i]) printf(" %s %llu", iridiumTypeLabel(i), (unsigned long long)t.typeCount[i]);
    printf("\n");
    for (const auto& s : t.sats) printf("  satellite %3d: %llu frames, %zu beams, offset %+.0f Hz%s\n", s.id, (unsigned long long)s.frames, s.beams.size(), s.freqOffsetHz,
                                        s.hasPos ? "" : " (no position yet)");
    for (const auto& mm : t.messages) printf("  pager %d seq %d%s: %s\n", mm.ric, mm.seq, mm.complete ? "" : " (incomplete)", mm.text.c_str());
    for (const auto& a : t.acars) printf("  ACARS %s %s label %s block %c%s: %s\n", a.downlink ? "to" : "from", a.reg.c_str(), a.label.c_str(), a.blockId ? a.blockId : ' ',
                                         a.more ? " (more)" : "", a.text.c_str());
    if (t.sbdPackets) printf("  short burst data packets: %llu\n", (unsigned long long)t.sbdPackets);
    if (t.hasTime) printf("  Iridium time at the end: %.3f (unix)\n", t.iridiumUtc);
    return 0;
}

static int sky(int argc, char** argv) {
    double secs = 600;
    int seed = 0, sats = 3;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = atoi(argv[++i]);
        else if (a == "--sats" && i + 1 < argc) sats = atoi(argv[++i]);
        else secs = atof(argv[i]);
    }
    const double t0 = iridiumSkyStart((uint32_t)seed, sats);
    printf("model start %.0f s (seed %d, %d satellites)\n", t0, seed, sats);
    for (double t = 0; t <= secs; t += std::max(1.0, secs / 20)) {
        printf("t=%6.0f s:", t);
        for (const auto& s : iridiumSky(t0 + t, 1)) printf("  [%d el %.0f az %.0f dop %+.1f kHz %+.0f Hz/s]", s.id, s.elevDeg, s.azDeg, s.dopplerHz / 1e3, s.dopplerRate);
        printf("\n");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "gen") return gen(argc, argv);
    if (cmd == "rx") return rx(argc, argv);
    if (cmd == "sky") return sky(argc, argv);
    usage();
    return 1;
}
