// Inmarsat Aero command line tool.
//   aerotool gen <secs> <file.cs8|.cf32> [--rate sps] [--ebn0 dB] [--channels mask] [--cfo Hz] [--drift Hz/s] [--ppm p] [--seed n] [--clean]
//       writes the test signal as the radio would see it (tuned tuneOffsetHz above the user's frequency)
//   aerotool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz] [--frames]
//       decodes a recording; --offset is where the user's frequency sits in the file (gen files: -50000), --frames prints every frame
#include "dect2/aero_gen.h"
#include "dect2/aero_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

static int usage() {
    printf("usage: aerotool gen <secs> <file.cs8|.cf32> [--rate sps] [--ebn0 dB] [--channels mask 1=600 2=1200 4=10500] [--cfo Hz] [--drift Hz/s] [--ppm p] [--seed n] [--clean]\n"
           "       aerotool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--offset Hz] [--frames]\n");
    return 2;
}

static bool endsWith(const std::string& s, const char* e) { const size_t n = std::strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }

static int gen(int argc, char** argv) {
    if (argc < 4) return usage();
    const double secs = std::atof(argv[2]);
    const std::string path = argv[3];
    double rate = aeroTuning().sampleRate;
    SynthConfig cfg;
    cfg.mode = 20;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "0";
        if (a == "--rate") { rate = std::atof(v); i++; }
        else if (a == "--ebn0") { cfg.modeVal[0] = std::atof(v); i++; }
        else if (a == "--channels") { cfg.modeOpt[0] = std::atoi(v); i++; }
        else if (a == "--cfo") { cfg.cfoHz = std::atof(v); i++; }
        else if (a == "--drift") { cfg.modeVal[1] = std::atof(v); i++; }
        else if (a == "--ppm") { cfg.sroPpm = std::atof(v); i++; }
        else if (a == "--seed") { cfg.modeOpt[1] = std::atoi(v); i++; }
        else if (a == "--clean") cfg.modeOpt[2] = 1;
        else return usage();
    }
    const bool asFloat = endsWith(path, ".cf32");
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { printf("cannot write %s\n", path.c_str()); return 1; }
    AeroSynth syn(cfg, rate);
    for (const auto& c : syn.channels()) printf("channel %5d bit/s at %+.0f Hz from the user's frequency (%+.0f Hz in the file)\n", c.bitRate, c.offsetHz, c.offsetHz - aeroTuning().tuneOffsetHz);
    std::vector<cf32> buf(65536);
    std::vector<int8_t> b8(2 * buf.size());
    const double total = secs * rate;
    for (double done = 0; done < total; done += (double)buf.size()) {
        const size_t n = (size_t)std::min<double>((double)buf.size(), total - done);
        syn.generate(buf.data(), n);
        if (asFloat) {
            std::fwrite(buf.data(), sizeof(cf32), n, f);
        } else {
            for (size_t i = 0; i < n; i++) {
                b8[2 * i] = (int8_t)std::lround(std::max(-1.f, std::min(1.f, buf[i].real())) * 127);
                b8[2 * i + 1] = (int8_t)std::lround(std::max(-1.f, std::min(1.f, buf[i].imag())) * 127);
            }
            std::fwrite(b8.data(), 1, 2 * n, f);
        }
    }
    std::fclose(f);
    printf("wrote %.1f s at %.0f sps to %s\n", secs, rate, path.c_str());
    return 0;
}

static int rx(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string path = argv[2];
    double rate = 0, offset = 0;
    std::string fmt = endsWith(path, ".cf32") ? "cf32" : endsWith(path, ".cu8") ? "cu8" : "cs8";
    bool frames = false;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--rate") { rate = std::atof(v); i++; }
        else if (a == "--format") { fmt = v; i++; }
        else if (a == "--offset") { offset = std::atof(v); i++; }
        else if (a == "--frames") frames = true;
        else return usage();
    }
    if (rate <= 0) return usage();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { printf("cannot read %s\n", path.c_str()); return 1; }
    AeroReceiver r;
    r.configure(rate);
    r.setSignalOffset(offset);
    if (!r.ready()) { printf("the rate is too low (at least %.0f sps)\n", aeroTuning().minSampleRate); return 1; }
    r.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    if (frames)
        r.setFrameCallback([](double off, const AeroFrameEvent& e) {
            printf("frame %+9.1f Hz %5d bit/s header %04X UW errors %d SUs %d/%d BER %.4f:", off, e.bitRate, e.header, e.uwErrors, e.susOk, e.susBad, e.channelBer);
            for (size_t i = 0; i < e.bytes.size() && i < 24; i++) printf(" %02X", e.bytes[i]);
            printf(e.bytes.size() > 24 ? " ...\n" : "\n");
        });
    const size_t n = 65536;
    std::vector<cf32> buf(n);
    std::vector<uint8_t> raw(n * 8);
    const size_t bps = fmt == "cf32" ? 8 : 2;
    for (;;) {
        const size_t got = std::fread(raw.data(), bps, n, f);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            if (fmt == "cf32") std::memcpy(&buf[i], &raw[8 * i], 8);
            else if (fmt == "cu8") buf[i] = cf32((raw[2 * i] - 127.5f) / 127.5f, (raw[2 * i + 1] - 127.5f) / 127.5f);
            else buf[i] = cf32((int8_t)raw[2 * i] / 127.f, (int8_t)raw[2 * i + 1] / 127.f);
        }
        r.feed(buf.data(), got);
    }
    std::fclose(f);
    AeroTelemetry t;
    r.telemetry(t, 0);
    printf("%s\n", aeroSummary(t).c_str());
    for (const auto& c : t.channels)
        printf("channel %+10.1f Hz %5d bit/s state %d Eb/N0 %5.1f dB level %5.1f dB frames %llu UW misses %llu SUs %llu good %llu bad\n", c.offsetHz, c.bitRate, c.state, c.ebn0Db,
               c.levelDb, (unsigned long long)c.frames, (unsigned long long)c.uwMisses, (unsigned long long)c.susOk, (unsigned long long)c.susBad);
    for (const auto& s : t.suTypes) printf("SU 0x%02X %-30s %llu\n", s.type, s.name.c_str(), (unsigned long long)s.count);
    for (const auto& a : t.aircraft)
        printf("aircraft %06X %-8s %-8s messages %llu last %s %s\n", a.aesId, a.registration.c_str(), a.flight.c_str(), (unsigned long long)a.messages, a.lastLabel.c_str(),
               a.loggedOn ? "logged on" : "");
    for (const auto& m : t.messages)
        printf("%8.1f s  %06X %-8s %-2s %-24s %s%s\n", m.time, m.aesId, m.registration.c_str(), m.label.c_str(), m.labelText.c_str(), m.text.c_str(), m.crcOk ? "" : "  (check failed)");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    if (!std::strcmp(argv[1], "gen")) return gen(argc, argv);
    if (!std::strcmp(argv[1], "rx")) return rx(argc, argv);
    return usage();
}
