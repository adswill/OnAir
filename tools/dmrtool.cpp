// DMR: write the test signal to a file (dmrtool gen ...) or run the receiver on a recording (dmrtool rx ...).
//
//   dmrtool gen out.cs8 [--secs 20] [--rate 2400000] [--snr 30] [--cfo 0] [--sro 0] [--cc 1] [--tg 3] [--direct|--mobile] [--traffic 0|1|2] [--seed 1]
//                       [--dev 1.0] [--dc 0] [--iq 0] [--no-text] [--no-alias] [--hang N] [--no-header-sync] [--no-block-sync] [--format cs8|cu8|cf32] [--truth]
//   dmrtool rx file.cs8 [--rate 2400000] [--format cs8|cu8|cf32] [--chunk 65536] [--quiet] [--voice out.bin]
//   dmrtool check [gen options] [--chunk N] [--float] [--truth]
//
// Nothing is transmitted and no sound is played. `rx` prints the events as they happen (colour code, calls, messages) and a summary at the end.
// `--voice` writes the vocoder bits of every voice burst, 29 bytes each: slot (1 or 2), position in the superframe (0 = A ... 5 = F), then the 216 bits
// VS(215)..VS(0) in 27 bytes. The AMBE+2 vocoder is not part of this receiver: this is what a codec would be fed with.
#include "dect2/dmr_eval.h"
#include "dect2/dmr_gen.h"
#include "dect2/dmr_proto.h"
#include "dect2/dmr_rx.h"
#include "dect2/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace dect2;

static const char* kindName(int k) {
    static const char* n[5] = {"group call", "private call", "all call", "data message", "control"};
    return n[k % 5];
}

static int usage() {
    fprintf(stderr,
            "usage:\n"
            "  dmrtool gen out.cs8|out.cf32 [--secs N] [--rate Hz] [--snr dB] [--cfo Hz] [--sro ppm] [--cc N] [--tg N] [--direct|--mobile] [--traffic 0|1|2]\n"
            "          [--seed N] [--dev x] [--dc x] [--iq dB] [--no-text] [--no-alias] [--hang N] [--no-header-sync] [--no-block-sync] [--format cs8|cu8|cf32] [--truth]\n"
            "  dmrtool rx file [--rate Hz] [--format cs8|cu8|cf32] [--chunk N] [--quiet] [--voice out.bin]\n"
            "  dmrtool check [gen options] [--chunk N] [--float] [--truth]     generate, decode and compare with what was sent\n");
    return 2;
}

struct GenOpts {
    DmrGenConfig c;
    double secs = 20;
    bool truth = false;
    std::string fmt;
    size_t chunk = 65536;
    bool quant = true;
};

static bool parseGen(int argc, char** argv, int from, GenOpts& o) {
    DmrGenConfig& c = o.c;
    for (int i = from; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--secs") o.secs = atof(next());
        else if (a == "--rate") c.rate = atof(next());
        else if (a == "--snr") c.snrDb = atof(next());
        else if (a == "--cfo") c.cfoHz = atof(next());
        else if (a == "--sro") c.sroPpm = atof(next());
        else if (a == "--cc") c.cc = atoi(next()) & 15;
        else if (a == "--tg") c.talkgroups = std::max(1, std::min(8, atoi(next())));
        else if (a == "--direct") c.direct = true;
        else if (a == "--mobile") { c.direct = true; c.mobile = true; }
        else if (a == "--traffic") c.traffic = atoi(next());
        else if (a == "--seed") c.seed = (uint32_t)atoi(next());
        else if (a == "--dev") c.devScale = atof(next());
        else if (a == "--dc") c.dcOffset = atof(next());
        else if (a == "--iq") c.iqImbalanceDb = atof(next());
        else if (a == "--no-text") c.textMessages = false;
        else if (a == "--no-alias") c.talkerAlias = false;
        else if (a == "--hang") c.hangBursts = std::max(0, atoi(next()));
        else if (a == "--no-header-sync") c.headerSync = false;
        else if (a == "--no-block-sync") c.blockSync = false;
        else if (a == "--format") o.fmt = next();
        else if (a == "--truth") o.truth = true;
        else if (a == "--chunk") o.chunk = (size_t)std::max(1, atoi(next()));
        else if (a == "--float") o.quant = false;
        else return false;
    }
    return true;
}

static int cmdGen(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string path = argv[2];
    GenOpts o;
    if (!parseGen(argc, argv, 3, o)) return usage();
    DmrGenConfig& c = o.c;
    const double secs = o.secs;
    const bool truth = o.truth;
    std::string fmt = o.fmt;
    if (fmt.empty()) fmt = guessFormat(path) == FileFormat::CF32 ? "cf32" : guessFormat(path) == FileFormat::CU8 ? "cu8" : "cs8";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return 1; }
    DmrSignal sig(c);
    const size_t total = (size_t)(secs * c.rate), block = 1 << 16;
    std::vector<cf32> buf(block);
    std::vector<uint8_t> raw;
    for (size_t done = 0; done < total; done += block) {
        const size_t n = std::min(block, total - done);
        sig.generate(buf.data(), n);
        if (fmt == "cf32") fwrite(buf.data(), sizeof(cf32), n, f);
        else {
            raw.resize(2 * n);
            for (size_t i = 0; i < n; i++) {
                const float re = std::max(-1.f, std::min(127.f / 128.f, buf[i].real())), im = std::max(-1.f, std::min(127.f / 128.f, buf[i].imag()));
                const int a = (int)std::lround(re * 128.f), b = (int)std::lround(im * 128.f);
                raw[2 * i] = fmt == "cu8" ? (uint8_t)(a + 128) : (uint8_t)(int8_t)a;
                raw[2 * i + 1] = fmt == "cu8" ? (uint8_t)(b + 128) : (uint8_t)(int8_t)b;
            }
            fwrite(raw.data(), 1, raw.size(), f);
        }
    }
    fclose(f);
    printf("wrote %.1f s of the DMR test signal (%s, %.0f sample/s, SNR %.0f dB in 12.5 kHz, %s) to %s\n", secs, fmt.c_str(), c.rate, c.snrDb,
           c.mobile ? "mobile (uplink)" : c.direct ? "direct mode" : "base station", path.c_str());
    if (truth) {
        for (const DmrTruth& t : sig.truth()) {
            if (t.startSec > secs) break;
            printf("  %7.2f s  slot %d  %-13s  src %u  dst %u  cc %d", t.startSec, t.slot, kindName(t.kind), t.src, t.dst, t.cc);
            if (t.kind <= 2) printf("  voice bursts %d%s%s", t.voiceBursts, t.alias.empty() ? "" : "  alias ", t.alias.c_str());
            if (t.kind == 3) printf("  \"%s\"", t.text.c_str());
            if (t.kind == 4) printf("  opcode %02X", t.csbkOpcode);
            printf("\n");
        }
    }
    return 0;
}

// generate and decode in one go (8 bit quantised like the radio) and compare with what was sent
static int cmdCheck(int argc, char** argv) {
    GenOpts o;
    if (!parseGen(argc, argv, 2, o)) return usage();
    DmrSignal sig(o.c);
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(o.c.rate);
    if (!rx.ready()) { fprintf(stderr, "sample rate too low\n"); return 1; }
    std::vector<cf32> buf(o.chunk);
    const size_t total = (size_t)(o.secs * o.c.rate);
    const auto t0 = std::chrono::steady_clock::now();
    double cpuRx = 0;
    for (size_t done = 0; done < total; done += o.chunk) {
        const size_t n = std::min(o.chunk, total - done);
        sig.generate(buf.data(), n);
        if (o.quant)
            for (size_t i = 0; i < n; i++) {
                auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; };
                buf[i] = cf32(q(buf[i].real()), q(buf[i].imag()));
            }
        const auto a = std::chrono::steady_clock::now();
        rx.feed(buf.data(), n);
        cpuRx += std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
    }
    (void)t0;
    DmrTelemetry t;
    rx.telemetry(t, 0);
    const DmrScore s = dmrScore(sig.truth(), t, o.secs, 3.0, 2.0, 0.05);
    printf("%s | voice %d/%d ids-wrong %d alias %d/%d msg %d/%d ctrl %d/%d false %d frames %.2f | SNR %.1f CFO %+.0f dev %.0f BER %.4f | %.0fx RT\n",
           dmrSummary(t).c_str(), s.voiceFound, s.voiceSent, s.idsWrong, s.aliasFound, s.aliasSent, s.messagesFound, s.messagesSent, s.controlFound, s.controlSent, s.falseCalls,
           s.frameRatio, t.snrDb, t.cfoHz, t.devHz, t.ber, o.secs / std::max(cpuRx, 1e-9));
    if (o.truth) printf("%s", s.report.c_str());
    return 0;
}

static int cmdRx(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string path = argv[2];
    double rate = guessSampleRate(path);
    FileFormat ff = guessFormat(path);
    size_t chunk = 65536;
    bool quiet = false;
    std::string voicePath;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
        if (a == "--rate") rate = atof(next());
        else if (a == "--format") { std::string s = next(); ff = s == "cf32" ? FileFormat::CF32 : s == "cu8" ? FileFormat::CU8 : FileFormat::CS8; }
        else if (a == "--chunk") chunk = (size_t)std::max(1, atoi(next()));
        else if (a == "--quiet") quiet = true;
        else if (a == "--voice") voicePath = next();
        else return usage();
    }
    if (rate <= 0) rate = 2.4e6;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror(path.c_str()); return 1; }
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    if (!rx.ready()) { fprintf(stderr, "the sample rate %.0f is too low for DMR (1 Msps and up)\n", rate); return 1; }
    FILE* vf = nullptr;
    uint64_t voiceBursts = 0;
    if (!voicePath.empty()) {
        vf = fopen(voicePath.c_str(), "wb");
        if (!vf) { perror(voicePath.c_str()); return 1; }
        rx.setVoiceCallback([&](int slot, int pos, const uint8_t* bits) {
            const uint8_t head[2] = {(uint8_t)slot, (uint8_t)pos};
            fwrite(head, 1, 2, vf);
            fwrite(bits, 1, 27, vf);
            voiceBursts++;
        });
    }
    if (!quiet) rx.setLogCallback([](const std::string& s) { printf("  %s\n", s.c_str()); });
    std::vector<cf32> samples;
    std::vector<uint8_t> raw;
    const size_t bytesPer = ff == FileFormat::CF32 ? 8 : 2;
    uint64_t total = 0, seq = 0;
    DmrTelemetry last;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        raw.resize(chunk * bytesPer);
        const size_t got = fread(raw.data(), bytesPer, chunk, f);
        if (!got) break;
        samples.resize(got);
        if (ff == FileFormat::CF32) memcpy(samples.data(), raw.data(), got * 8);
        else
            for (size_t i = 0; i < got; i++) {
                if (ff == FileFormat::CU8) samples[i] = cf32(((int)raw[2 * i] - 128) / 128.f, ((int)raw[2 * i + 1] - 128) / 128.f);
                else samples[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
            }
        rx.feed(samples.data(), got);
        total += got;
        DmrTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = std::move(t); }
    }
    fclose(f);
    if (vf) { fclose(vf); printf("%llu voice bursts written to %s\n", (unsigned long long)voiceBursts, voicePath.c_str()); }
    const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    DmrTelemetry t;
    if (rx.telemetry(t, 0)) last = std::move(t);
    const double sigSecs = (double)total / rate;
    printf("\n%s\n", dmrSummary(last).c_str());
    printf("signal %.1f s, CPU %.2f s (%.1fx real time)\n", sigSecs, cpu, sigSecs / std::max(cpu, 1e-9));
    printf("level %.1f dBFS, SNR %.1f dB, CNR %.1f dB, deviation %.0f Hz, symbol clock %+.1f ppm, CFO %+.0f Hz, BER %.4f\n", last.levelDbfs, last.snrDb, last.cnrDb, last.devHz,
           last.symbolPpm, last.cfoHz, last.ber);
    printf("syncs:");
    for (int i = 0; i < 9; i++) printf(" %llu", (unsigned long long)last.syncCount[i]);
    printf("   (BS v/d, MS v/d, RC, direct 1 v/d, 2 v/d)\n");
    printf("data bursts by type:");
    for (int i = 0; i < 12; i++) if (last.burstCount[i]) printf(" %s %llu", dmr::dataTypeShort(i), (unsigned long long)last.burstCount[i]);
    printf("\nvoice bursts %llu (embedded %llu), RC %llu, unknown %llu, idle verified %llu\n", (unsigned long long)last.voiceBursts, (unsigned long long)last.embeddedBursts,
           (unsigned long long)last.rcBursts, (unsigned long long)last.unknownBursts, (unsigned long long)last.idleOk);
    printf("FEC: BPTC ok %llu fixed %llu lost %llu | slot type fixed %llu lost %llu | RS ok %llu fixed %llu lost %llu | CRC ok %llu bad %llu | trellis ok %llu lost %llu | EMB %llu/%llu, embedded LC %llu/%llu\n",
           (unsigned long long)last.bptcOk, (unsigned long long)last.bptcFixed, (unsigned long long)last.bptcFail, (unsigned long long)last.golayFixed, (unsigned long long)last.golayFail,
           (unsigned long long)last.rsOk, (unsigned long long)last.rsFixed, (unsigned long long)last.rsFail, (unsigned long long)last.crcOk, (unsigned long long)last.crcBad,
           (unsigned long long)last.trellisOk, (unsigned long long)last.trellisFail, (unsigned long long)last.embOk, (unsigned long long)last.embFail, (unsigned long long)last.embLcOk,
           (unsigned long long)last.embLcFail);
    if (!last.cachInfo.empty()) printf("CACH: %s\n", last.cachInfo.c_str());
    printf("\ncall log (%zu):\n", last.callLog.size());
    for (const DmrCall& c : last.callLog) {
        printf("  %7.2f - %7.2f s  slot %d  CC %d  %-13s", c.startSec, c.endSec, c.slot, c.cc, kindName(c.kind));
        if (c.idsKnown) printf("  %u -> %u", c.src, c.dst); else printf("  ids unknown");
        if (c.kind <= 2) printf("  frames %d%s%s", c.voiceFrames, c.lateEntry ? " late" : "", c.terminated ? "" : " no-term");
        if (!c.alias.empty()) printf("  alias \"%s\"", c.alias.c_str());
        if (!c.note.empty()) printf("  %s", c.note.c_str());
        printf("\n");
    }
    printf("\nmessages (%zu):\n", last.messages.size());
    for (const DmrMessage& m : last.messages)
        printf("  %7.2f s  slot %d  %u -> %u  [%s]%s  \"%s\"\n", m.sec, m.slot, m.src, m.dst, m.format.c_str(), m.crcOk ? "" : " CRC wrong", m.text.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    const std::string cmd = argv[1];
    if (cmd == "gen") return cmdGen(argc, argv);
    if (cmd == "rx") return cmdRx(argc, argv);
    if (cmd == "check") return cmdCheck(argc, argv);
    return usage();
}
