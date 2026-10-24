// P25 trunking / Phase 2 file tool. Receive only.
//   p25p2tool cc <file.cf32|.sigmf-data> <rate Hz> [<centre MHz>]
//       the Phase 1 control channel: site, identifiers and every voice grant (Phase 2 grants with their carrier and slot)
//   p25p2tool p2 <file> <rate Hz> [--wacn X] [--sys X] [--nac X] [--slot 1|2] [--wav out.wav]
//       a Phase 2 voice channel: burst, ISCH, DUID and ESS counters, the calls, and the sound of one slot (WACN / system / NAC in hex)
//   p25p2tool engine <file.sigmf-meta> <control MHz> <Phase 2 MHz> <slot 1|2> <WACN> <system> <NAC> [hop]
//       the engine on the recording: the control channel, then after 2 s the Phase 2 channel as the second mode (or, with hop, the first after a retune)
#include "dect2/engine/engine.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/source/iq_file.h"
#include "dect2/voice/voice_monitor.h"
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace dect2;

namespace {

bool readCf32(const char* path, std::vector<cf32>& x) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::error_code ec;
    const long long n = (long long)(std::filesystem::file_size(path, ec) / 8);   // ftell is 32 bits on Windows
    if (ec) { fclose(f); return false; }
    x.resize((size_t)n);
    const size_t got = fread(x.data(), 8, (size_t)n, f);
    fclose(f);
    x.resize(got);
    return got > 0;
}

void writeWav(const char* path, const std::vector<float>& a, int rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint32_t data = (uint32_t)a.size() * 2;
    auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(1); u32((uint32_t)rate); u32((uint32_t)rate * 2); u16(2); u16(16);
    fwrite("data", 1, 4, f); u32(data);
    for (float s : a) { const int v = (int)std::lround(std::max(-1.f, std::min(1.f, s)) * 32767); u16((uint16_t)(int16_t)v); }
    fclose(f);
}

int runCc(const std::vector<cf32>& x, double rate, double centreMhz, int loops) {
    P25Receiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    trunk::Controller ctl;
    ctl.setCapture(centreMhz * 1e6, 0);
    std::set<uint64_t> seen;
    uint64_t lastSeq = 0;
    const size_t blk = (size_t)(rate / 20);
    double t = 0;
    uint64_t okAt = 0, badAt = 0;
    for (size_t j = 0; j < x.size() * (size_t)loops; j += blk) {
        const size_t i = j % x.size();
        const size_t n = std::min(blk, x.size() - i);
        rx.feed(x.data() + i, n);
        if (loops > 1 && i + n == x.size()) {   // the end of each pass: how this one went
            const trunk::ControllerStatus st = ctl.status(t);
            printf("  %5.0f s: %llu good, %llu bad this pass\n", t, (unsigned long long)(st.tsbkGood - okAt), (unsigned long long)(st.tsbkBad - badAt));
            okAt = st.tsbkGood; badAt = st.tsbkBad;
        }
        t += n / rate;
        P25Telemetry tel;
        if (rx.telemetry(tel, lastSeq)) {
            lastSeq = tel.seq;
            for (const auto& r : tel.tsbks) {
                if (!seen.insert(r.seq).second) continue;
                p25::Tsbk k;
                k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
                ctl.onTsbk(k, r.nac, t);
            }
        }
        ctl.tick(t);
    }
    for (const auto& m : ctl.messages) printf("  %s\n", m.c_str());
    const trunk::SiteInfo& s = ctl.sys.site();
    const trunk::ControllerStatus st = ctl.status(t);
    printf("TSBK %llu good, %llu bad (%.0f %%)\n", (unsigned long long)st.tsbkGood, (unsigned long long)st.tsbkBad, st.crcOkPct);
    if (s.valid) printf("site: WACN %05X system %03X RFSS %d site %d NAC %03X\n", s.wacn, s.sysId, s.rfss, s.site, s.nac);
    for (int i = 0; i < 16; i++) {
        const trunk::IdenInfo& d = ctl.sys.iden(i);
        if (d.valid) printf("iden %d: base %.5f MHz spacing %.2f kHz%s type %d slots %d\n", i, d.baseHz / 1e6, d.spacingHz / 1e3, d.tdma ? " TDMA" : "", d.chanType, d.slots);
    }
    for (const auto& r : ctl.log.records())
        printf("call: TG %d src %d channel %s %.5f MHz slot %d%s%s  %s\n", r.tg, r.src, p25::channelText(r.channel).c_str(), r.freqHz / 1e6, r.slot,
               r.encrypted ? " encrypted" : "", r.followed ? " followed" : "", r.note.c_str());
    return 0;
}

int runP2(const std::vector<cf32>& x, double rate, int wacn, int sys, int nac, int slot, const char* wav, bool monitor) {
    P25p2Receiver rx;
    rx.setSilent(!monitor);   // --monitor: through the P25 voice monitor (the sound device opens, muted)
    rx.setMuted(true);
    rx.configure(rate);
    if (wacn >= 0 || sys >= 0 || nac >= 0) rx.setSystem(std::max(0, wacn), std::max(0, sys), std::max(0, nac));
    rx.setListenSlot(slot);
    std::vector<float> audio;
    rx.setAudioTap([&](const float* a, size_t n) { audio.insert(audio.end(), a, a + n); });
    rx.setLogCallback([](const std::string& s) { printf("  %s\n", s.c_str()); });
    const size_t blk = (size_t)(rate / 20);
    for (size_t i = 0; i < x.size(); i += blk) rx.feed(x.data() + i, std::min(blk, x.size() - i));
    P25p2Telemetry t;
    rx.telemetry(t, 0);
    printf("syncs %llu, slots %llu (bad %llu), ISCH bad %llu, unlocks %llu, 4V %llu 2V %llu SACCH %llu FACCH %llu\n", (unsigned long long)t.syncs, (unsigned long long)t.slots,
           (unsigned long long)t.slotsBad, (unsigned long long)t.ischBad, (unsigned long long)t.unlocks, (unsigned long long)t.v4, (unsigned long long)t.v2,
           (unsigned long long)t.sacch, (unsigned long long)t.facch);
    printf("ISCH exact %llu corrected %llu, DUID bad %llu, ESS ok %llu bad %llu, voice frames %llu decoded %llu encrypted %llu, CFO %+.0f Hz SNR %.1f dB, codec %s\n",
           (unsigned long long)t.ischExact, (unsigned long long)t.ischCorrected, (unsigned long long)t.duidBad, (unsigned long long)t.essOk, (unsigned long long)t.essBad,
           (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded, (unsigned long long)t.encryptedSkipped, t.cfoHz, t.snrDb, t.codec ? t.codecPath.c_str() : "none");
    for (const auto& c : t.log)
        if (c.kind == 0) printf("call: slot %d %s -> %s%s, %.1f s, %u frames, ALGID %02X%s\n", c.slot, p25Id(c.src).c_str(), c.group ? "TG " : "", p25Id(c.dst).c_str(), c.durSec,
                                c.voiceFrames, c.algId, c.encrypted ? " encrypted" : "");
    if (monitor)
        for (const auto& e : voice::monitor("p25").history())
            printf("monitor: slot %d %s -> %s, %llu frames, %llu played, %s\n", e.info.slot, voice::sourceText(e.info).c_str(), voice::targetText(e.info).c_str(),
                   (unsigned long long)e.frames, (unsigned long long)e.played, voice::verdictText(e.verdict));
    if (wav && *wav) { writeWav(wav, audio, 8000); printf("wrote %s (%.1f s)\n", wav, audio.size() / 8000.0); }
    return 0;
}

// The engine with the recording as its radio, the way the trunking follower drives it: the Phase 1 control channel, then the Phase 2 channel as the
// second mode (inside the capture) or as the first mode after a retune (the radio hopped)
int runEngine(const char* meta, double ccMhz, double p2Mhz, int slot, int wacn, int sys, int nac, bool hop) {
    IqFileInfo info;
    std::string err;
    if (!probeIqFile(meta, info, err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    FileOptions file;
    file.path = info.dataPath; file.format = info.format; file.sampleRate = info.sampleRate; file.offsetBytes = info.offsetBytes; file.endBytes = info.endBytes;
    file.centreHz = info.centerHz; file.loop = false;
    DeviceInfo dev;
    dev.kind = DeviceInfo::File; dev.name = file.path;
    TuneSettings tune;
    tune.centerHz = ccMhz * 1e6; tune.sampleRate = info.sampleRate; tune.bandwidthMhz = 0.0125;
    Engine e;
    e.p25().setSilent(true);
    e.p25p2().setSilent(true);
    e.setStandard(52);
    if (!e.start(dev, tune, file)) { size_t n; for (auto& l : e.logSnapshot(n)) fprintf(stderr, "%s\n", l.c_str()); return 1; }
    const auto t0 = std::chrono::steady_clock::now();
    auto el = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    bool opened = false;
    RxTelemetry rx, rx2;
    uint64_t seq = 0, seq2 = 0, tsbks = 0, lastTsbk = 0;
    while (el() < info.numSamples / std::max(1.0, info.sampleRate) + 12 && e.running()) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            for (const auto& r : rx.p25.tsbks) if (r.seq > lastTsbk) { lastTsbk = r.seq; tsbks++; }
        }
        if (e.latestRx2(rx2, seq2)) seq2 = rx2.seq;
        if (!opened && el() > 2.0) {
            opened = true;
            e.p25p2().setSystem(wacn, sys, nac);
            e.p25p2().setListenSlot(slot);
            if (hop) { e.setStandard(182); tune.centerHz = p2Mhz * 1e6; e.retune(tune); }
            else if (!e.setSecondaryHz(182, p2Mhz * 1e6)) { fprintf(stderr, "second mode refused: %s\n", e.secondaryNote().c_str()); return 1; }
            printf("%.1f s: Phase 2 on %.5f MHz %s (%llu TSBKs before)\n", el(), p2Mhz, hop ? "as the first mode" : "as the second mode", (unsigned long long)tsbks);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (el() > 14) break;
    }
    const P25p2Telemetry& t = hop ? rx.p25p2 : rx2.p25p2;
    printf("control channel: %llu TSBKs, active standard %d\n", (unsigned long long)tsbks, e.activeStandard());
    printf("Phase 2: syncs %llu, ISCH exact %llu, ESS ok %llu bad %llu, voice frames %llu decoded %llu, seed %05X/%03X/%03X\n", (unsigned long long)t.syncs,
           (unsigned long long)t.ischExact, (unsigned long long)t.essOk, (unsigned long long)t.essBad, (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded,
           t.wacn, t.sysId, t.colorCode);
    e.stop();
    return t.essOk > 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 9 && !strcmp(argv[1], "engine"))
        return runEngine(argv[2], atof(argv[3]), atof(argv[4]), atoi(argv[5]), (int)strtol(argv[6], nullptr, 16), (int)strtol(argv[7], nullptr, 16),
                         (int)strtol(argv[8], nullptr, 16), argc > 9 && !strcmp(argv[9], "hop"));
    if (argc < 4) { fprintf(stderr, "usage: p25p2tool cc|p2 <file> <rate Hz> [options]\n"); return 2; }
    std::vector<cf32> x;
    if (!readCf32(argv[2], x)) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    const double rate = atof(argv[3]);
    fputs(argv[2], stdout);   // the path as it is (never a format)
    printf(": %zu samples, %.2f s\n", x.size(), x.size() / rate);
    if (!strcmp(argv[1], "cc")) {
        const int loops = argc > 5 ? std::max(1, atoi(argv[5])) : 1;   // the recording again and again: a long run in one receiver
        return runCc(x, rate, argc > 4 ? atof(argv[4]) : 0, loops);
    }
    int wacn = -1, sys = -1, nac = -1, slot = 0;
    const char* wav = nullptr;
    bool monitor = false;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--wacn") wacn = (int)strtol(argv[i + 1], nullptr, 16);
        else if (a == "--sys") sys = (int)strtol(argv[i + 1], nullptr, 16);
        else if (a == "--nac") nac = (int)strtol(argv[i + 1], nullptr, 16);
        else if (a == "--slot") slot = atoi(argv[i + 1]);
        else if (a == "--wav") wav = argv[i + 1];
        else if (a == "--monitor") monitor = atoi(argv[i + 1]) != 0;
    }
    return runP2(x, rate, wacn, sys, nac, slot, wav, monitor);
}
