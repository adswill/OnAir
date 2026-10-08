// ACARS: write the test signal to a file, decode a recording, or run the generator straight into the receiver.
//   acarstool gen <secs> <file.cs8|.cu8|.cf32> [--rate 2e6] [--snr 30] [--aircraft 6] [--seed 1] [--depth 60] [--cfo 0] [--sro 0] [--mult 1] [--chan 0] [--weak 0]
//   acarstool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center 131.5] [--channels 131.525,131.725] [--thr 8] [--all]
//   acarstool sim [--secs 60] [--rate 2e6] [--snr 30] [--aircraft 6] [--seed 1] [--depth 60] [--cfo 0] [--sro 0] [--mult 1] [--chan 0] [--weak 0] [--chunk 65536]
//                  [--quant] [--dc 0.05] [--gap secs] [--reset secs] [--list]
// rx prints every good block as one line (time, channel, address, label, block id, text) and then the channel and aircraft tables.
// sim prints how many of the transmitted blocks came out right, and the real-time factor of the receiver.
#include "dect2/acars_gen.h"
#include "dect2/acars_rx.h"
#include "dect2/acars_sim.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace dect2;

static std::string formatOf(const std::string& path, std::string fmt) {
    if (!fmt.empty()) return fmt;
    const size_t d = path.rfind('.');
    const std::string ext = d == std::string::npos ? "" : path.substr(d + 1);
    return (ext == "cu8" || ext == "cf32") ? ext : "cs8";
}

static std::string show(const AcarsMessage& m) {
    char b[400];
    std::string t = m.text;
    for (auto& c : t) if (c == '\n') c = '|';
    if (t.size() > 100) t = t.substr(0, 100) + "...";
    snprintf(b, sizeof b, "%8.2f  %.3f  %-7s %c %c%c %c%c %-6s %s  %s%s%s", m.timeSec, m.freqHz / 1e6, m.reg.c_str(), m.downlink ? 'v' : '^', m.label[0], m.label[1], m.ack, m.blockId,
             m.flightId.c_str(), t.c_str(), m.decoded.empty() ? "" : "[", m.decoded.c_str(), m.decoded.empty() ? "" : "]");
    return b;
}

static AcarsGenOptions optsFrom(int chan, double weak, int aircraft, int seed, double depth, double mult) {
    AcarsGenOptions o;
    o.aircraft = aircraft; o.seed = (uint32_t)seed; o.depth = depth / 100.0; o.rateFactor = mult;
    if (chan == 1) o.channelsHz = {131.525e6};
    else if (chan == 2) o.channelsHz = {131.125e6, 131.550e6, 131.725e6};
    if (weak > 0) { o.levelDb.assign(o.channelsHz.size(), 0.0); o.levelDb.back() = -weak; }
    return o;
}

static int gen(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: acarstool gen <secs> <file> [options]\n"); return 2; }
    const double secs = atof(argv[2]);
    const std::string path = argv[3];
    double rate = 2e6, depth = 60, weak = 0, mult = 1;
    int aircraft = 6, seed = 1, chan = 0;
    std::string fmt;
    SynthConfig c;
    c.snrDb = 30;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--rate") rate = atof(nx()); else if (a == "--snr") c.snrDb = atof(nx()); else if (a == "--aircraft") aircraft = atoi(nx());
        else if (a == "--seed") seed = atoi(nx()); else if (a == "--depth") depth = atof(nx()); else if (a == "--cfo") c.cfoHz = atof(nx());
        else if (a == "--sro") c.sroPpm = atof(nx()); else if (a == "--mult") mult = atof(nx()); else if (a == "--chan") chan = atoi(nx());
        else if (a == "--weak") weak = atof(nx()); else if (a == "--format") fmt = nx();
    }
    fmt = formatOf(path, fmt);
    auto g = makeAcarsSynthEx(optsFrom(chan, weak, aircraft, seed, depth, mult), c, rate);
    if (!g) { fprintf(stderr, "bad rate\n"); return 1; }
    std::ofstream f(path, std::ios::binary);
    std::vector<cf32> buf(65536);
    size_t left = (size_t)(secs * rate);
    while (left) {
        const size_t n = std::min(left, buf.size());
        g->generate(buf.data(), n);
        if (fmt == "cf32") f.write((const char*)buf.data(), (std::streamsize)(n * sizeof(cf32)));
        else {
            std::vector<uint8_t> o(2 * n);
            for (size_t i = 0; i < n; i++) {
                const float re = buf[i].real(), im = buf[i].imag();
                if (fmt == "cu8") { o[2 * i] = (uint8_t)std::max(0.f, std::min(255.f, std::round(re * 127.f + 127.5f))); o[2 * i + 1] = (uint8_t)std::max(0.f, std::min(255.f, std::round(im * 127.f + 127.5f))); }
                else { o[2 * i] = (uint8_t)(int8_t)std::max(-127.f, std::min(127.f, std::round(re * 127.f))); o[2 * i + 1] = (uint8_t)(int8_t)std::max(-127.f, std::min(127.f, std::round(im * 127.f))); }
            }
            f.write((const char*)o.data(), (std::streamsize)o.size());
        }
        left -= n;
    }
    fprintf(stderr, "wrote %s (%.1f s at %.0f sps, %s)\n", path.c_str(), secs, rate, fmt.c_str());
    return 0;
}

static void tables(const AcarsTelemetry& t) {
    printf("\nchannels (centre %.3f MHz):\n", t.centerHz / 1e6);
    for (const auto& c : t.channels)
        printf("  %.3f MHz  ok %u  bad %u  last %.1f s  cfo %+.0f Hz\n", c.freqHz / 1e6, c.messages, c.bad, c.lastHeardSec, (double)c.cfoHz);
    printf("aircraft:\n");
    for (const auto& a : t.aircraft)
        printf("  %-7s %-6s  %u messages  last label %s  %.3f MHz\n", a.reg.c_str(), a.flight.c_str(), a.messages, a.lastLabel.c_str(), a.freqHz / 1e6);
    printf("blocks: %llu ok (%llu needed repair), %llu bad, %llu frames started\n", (unsigned long long)t.blocksOk, (unsigned long long)t.parityFixed, (unsigned long long)t.blocksBad,
           (unsigned long long)t.framesStarted);
}

static int rx(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: acarstool rx <file> --rate <sps> [--format cs8|cu8|cf32] [--center MHz] [--channels MHz,MHz] [--thr dB]\n"); return 2; }
    const std::string path = argv[2];
    double rate = 2e6, center = 131.5, thr = 8;
    std::string fmt;
    std::vector<double> chans;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--rate") rate = atof(nx()); else if (a == "--format") fmt = nx(); else if (a == "--center") center = atof(nx()); else if (a == "--thr") thr = atof(nx());
        else if (a == "--channels") { std::string s = nx(); size_t p = 0; while (p < s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); chans.push_back(atof(s.substr(p, q - p).c_str()) * 1e6); p = q + 1; } }
    }
    fmt = formatOf(path, fmt);
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    AcarsReceiver r;
    r.configure(rate);
    r.setCenterHz(center * 1e6);
    r.setChannels(chans);
    r.setThresholdDb(thr);
    r.setLogCallback([](const std::string& s) { fprintf(stderr, "%s\n", s.c_str()); });
    if (!r.ready()) { fprintf(stderr, "rate too low\n"); return 1; }
    const size_t bsz = fmt == "cf32" ? 8 : 2;
    std::vector<uint8_t> raw(65536 * bsz);
    std::vector<cf32> x(65536);
    uint64_t last = 0, lastSerial = 0;
    AcarsTelemetry t;
    std::vector<AcarsMessage> all;
    while (f) {
        f.read((char*)raw.data(), (std::streamsize)raw.size());
        const size_t n = (size_t)f.gcount() / bsz;
        if (!n) break;
        for (size_t i = 0; i < n; i++) {
            if (fmt == "cf32") memcpy(&x[i], &raw[8 * i], 8);
            else if (fmt == "cu8") x[i] = cf32(((int)raw[2 * i] - 127.5f) / 127.f, ((int)raw[2 * i + 1] - 127.5f) / 127.f);
            else x[i] = cf32((int8_t)raw[2 * i] / 127.f, (int8_t)raw[2 * i + 1] / 127.f);
        }
        r.feed(x.data(), n);
        while (r.telemetry(t, last)) {
            last = t.seq;
            for (auto it = t.messages.rbegin(); it != t.messages.rend(); ++it)
                if (it->serial > lastSerial) { printf("%s\n", show(*it).c_str()); lastSerial = it->serial; }
        }
    }
    tables(t);
    return 0;
}

static int sim(int argc, char** argv) {
    AcarsSimCfg c;
    c.secs = 60;
    c.syn.snrDb = 30;
    double depth = 60, weak = 0, mult = 1;
    int aircraft = 6, seed = 1, chan = 0;
    bool list = false;
    for (int i = 2; i < argc; i++) {
        const std::string a = argv[i];
        auto nx = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--secs") c.secs = atof(nx()); else if (a == "--rate") c.rate = atof(nx()); else if (a == "--snr") c.syn.snrDb = atof(nx()); else if (a == "--aircraft") aircraft = atoi(nx());
        else if (a == "--seed") seed = atoi(nx()); else if (a == "--depth") depth = atof(nx()); else if (a == "--cfo") c.syn.cfoHz = atof(nx());
        else if (a == "--sro") c.syn.sroPpm = atof(nx()); else if (a == "--mult") mult = atof(nx()); else if (a == "--chan") chan = atoi(nx());
        else if (a == "--weak") weak = atof(nx()); else if (a == "--chunk") c.chunk = (size_t)atol(nx()); else if (a == "--list") list = true;
        else if (a == "--quant") c.quant8 = true; else if (a == "--dc") { const float v = (float)atof(nx()); c.dc = cf32(v, v); }
        else if (a == "--gap") c.gapAt = atof(nx()); else if (a == "--reset") c.resetAt = atof(nx());
    }
    c.gen = optsFrom(chan, weak, aircraft, seed, depth, mult);
    const auto r = runAcarsSim(c);
    if (list) for (const auto& s : r.missing) printf("missing: %.2f s  %.3f MHz  %s %c%c %c  %s\n", s.startSec, s.freqHz / 1e6, s.spec.reg.c_str(), s.spec.label[0], s.spec.label[1], s.spec.blockId, s.spec.text.substr(0, 40).c_str());
    printf("sent %d, decoded %d (%.1f %%), wrong %d, blocks ok %llu bad %llu, receiver %.2f x real time\n", r.sent, r.decoded, r.sent ? 100.0 * r.decoded / r.sent : 0.0, r.wrong,
           (unsigned long long)r.last.blocksOk, (unsigned long long)r.last.blocksBad, r.rtf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "gen")) return gen(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "rx")) return rx(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "sim")) return sim(argc, argv);
    fprintf(stderr, "usage: acarstool gen <secs> <file> | rx <file> --rate <sps> | sim   (see the top of tools/acarstool.cpp)\n");
    return 2;
}
