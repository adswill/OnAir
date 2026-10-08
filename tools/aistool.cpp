// AIS command line tool.
//   aistool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--vessels n] [--seed n] [--speed x] [--equal] [--only-vessels]
//   aistool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz] [--quiet]
// rx prints the !AIVDM sentences as they are decoded, then the counters and the station table.
#include "dect2/ais_gen.h"
#include "dect2/ais_rx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static bool endsWith(const std::string& s, const char* e) { const size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int usage() {
    printf("usage: aistool gen <secs> <file.cs8|.cf32> [--rate sps] [--snr dB] [--cfo Hz] [--sro ppm] [--vessels n] [--seed n] [--speed x] [--equal] [--only-vessels]\n"
           "       aistool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz] [--quiet]\n");
    return 1;
}

static int doGen(int argc, char** argv) {
    if (argc < 4) return usage();
    const double secs = atof(argv[2]);
    const std::string path = argv[3];
    double rate = 2e6;
    SynthConfig cfg;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? atof(argv[++i]) : 0.0; };
        if (a == "--rate") rate = val();
        else if (a == "--snr") cfg.snrDb = val();
        else if (a == "--cfo") cfg.cfoHz = val();
        else if (a == "--sro") cfg.sroPpm = val();
        else if (a == "--vessels") cfg.modeOpt[0] = (int)val();
        else if (a == "--seed") cfg.modeOpt[1] = (int)val();
        else if (a == "--speed") cfg.modeVal[0] = val();
        else if (a == "--equal") cfg.modeOpt[2] = 1;
        else if (a == "--only-vessels") cfg.modeOpt[3] = 1;
        else return usage();
    }
    const bool cf32f = endsWith(path, ".cf32");
    if (!cf32f && !endsWith(path, ".cs8")) return usage();
    auto synth = makeAisSynth(cfg, rate);
    if (!synth) { printf("rate %.0f is not supported (200 kS/s to 21 MS/s)\n", rate); return 1; }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", path.c_str()); return 1; }
    const size_t total = (size_t)(secs * rate);
    std::vector<cf32> buf(65536);
    std::vector<int8_t> q;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        synth->generate(buf.data(), n);
        if (cf32f) fwrite(buf.data(), sizeof(cf32), n, f);
        else {
            q.resize(2 * n);
            for (size_t i = 0; i < n; i++) {
                q[2 * i] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].real() * 127.f)));
                q[2 * i + 1] = (int8_t)std::max(-127.f, std::min(127.f, std::round(buf[i].imag() * 127.f)));
            }
            fwrite(q.data(), 1, q.size(), f);
        }
        done += n;
    }
    fclose(f);
    printf("wrote %.1f s at %.0f sps to %s\n", secs, rate, path.c_str());
    return 0;
}

static int doRx(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string path = argv[2];
    double rate = 0, offset = 0;
    std::string fmt = endsWith(path, ".cf32") ? "cf32" : endsWith(path, ".cu8") ? "cu8" : "cs8";
    bool quiet = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) fmt = argv[++i];
        else if (a == "--offset" && i + 1 < argc) offset = atof(argv[++i]);
        else if (a == "--quiet") quiet = true;
        else return usage();
    }
    if (rate <= 0) return usage();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); return 1; }
    AisReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(offset);
    if (!rx.ready()) { printf("rate %.0f is too low (the minimum is 250 kS/s)\n", rate); return 1; }
    AisTelemetry t; uint64_t last = 0;
    std::string lastPrinted;
    const size_t bytesPer = fmt == "cf32" ? 8 : 2;
    std::vector<uint8_t> raw(65536 * bytesPer);
    std::vector<cf32> x(65536);
    while (true) {
        const size_t got = fread(raw.data(), bytesPer, 65536, f);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (fmt == "cf32") { float v[2]; memcpy(v, &raw[8 * i], 8); x[i] = cf32(v[0], v[1]); }
            else if (fmt == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.5f, ((int)raw[2 * i + 1] - 127.5f) / 127.5f);
            else x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
        }
        rx.feed(x.data(), got);
        if (rx.telemetry(t, last)) {
            last = t.seq;
            if (!quiet) {
                size_t from = 0;
                if (!lastPrinted.empty())
                    for (size_t i = t.nmea.size(); i > 0; i--) if (t.nmea[i - 1] == lastPrinted) { from = i; break; }
                for (size_t i = from; i < t.nmea.size(); i++) printf("%s\n", t.nmea[i].c_str());
                if (!t.nmea.empty()) lastPrinted = t.nmea.back();
            }
        }
    }
    fclose(f);
    // one last report: feed a little silence so the last burst is decoded and published
    std::vector<cf32> z((size_t)(rate * 0.5), cf32(0, 0));
    rx.feed(z.data(), z.size());
    if (rx.telemetry(t, last)) {
        if (!quiet) {
            size_t from = 0;
            if (!lastPrinted.empty())
                for (size_t i = t.nmea.size(); i > 0; i--) if (t.nmea[i - 1] == lastPrinted) { from = i; break; }
            for (size_t i = from; i < t.nmea.size(); i++) printf("%s\n", t.nmea[i].c_str());
        }
    }
    printf("\n%s\n", aisSummary(t).c_str());
    printf("bursts %llu, good %llu (AIS 1 %llu, AIS 2 %llu), bad %llu (AIS 1 %llu, AIS 2 %llu), snr %.1f dB, carrier %.0f Hz\n", (unsigned long long)t.bursts,
           (unsigned long long)t.blocksOk, (unsigned long long)t.channelOk[0], (unsigned long long)t.channelOk[1], (unsigned long long)t.blocksBad,
           (unsigned long long)t.channelBad[0], (unsigned long long)t.channelBad[1], t.snrDb, t.cfoHz);
    printf("types:");
    for (int c = 0; c < 2; c++) {
        printf("  AIS %d:", c + 1);
        for (int k = 0; k < 32; k++) if (t.typeCount[c][k]) printf(" %d=%llu", k, (unsigned long long)t.typeCount[c][k]);
    }
    printf("\nlevel %.1f / %.1f dBFS, noise %.1f / %.1f dBFS\n", t.levelDbfs[0], t.levelDbfs[1], t.noiseDbfs[0], t.noiseDbfs[1]);
    printf("\n%-10s %-7s %-22s %-8s %-12s %9s %10s %5s %5s %4s %4s %5s\n", "MMSI", "class", "name", "call", "type", "lat", "lon", "sog", "cog", "hdg", "msgs", "age");
    for (const AisVessel& v : t.vessels) {
        printf("%-10u %-7s %-22s %-8s %-12.12s ", v.mmsi, aisClassText(v.cls), v.name.c_str(), v.callsign.c_str(), v.shipType >= 0 ? aisShipTypeText(v.shipType) : "");
        if (v.hasPos) printf("%9.4f %10.4f ", v.lat, v.lon); else printf("%9s %10s ", "-", "-");
        printf("%5.1f %5.1f %4d %4u %5.0f\n", v.sog, v.cog, v.heading, v.messages, v.ageSec);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "gen") return doGen(argc, argv);
    if (cmd == "rx") return doRx(argc, argv);
    return usage();
}
