// Headless receiver: prints sync / L1 telemetry once per second. Useful for bring-up and logs.
//   dect2cli --hackrf [--freq MHz] [--lna dB] [--vga dB] [--amp] [--bw MHz] [--secs N]
//   dect2cli --file path.cs8 --rate Msps [--format cs8|cu8|cf32] [--bw MHz] [--secs N]
//   dect2cli --record out.cs8 --secs N   (HackRF only; raw IQ capture at the 2x native rate)
#include "dect2/engine.h"
#include "dect2/modes.h"
#include "dect2/dvbt.h"
#include "dect2/channel.h"
#include "dect2/quality.h"
#include "dect2/nettuner.h"
#include "dect2/platform.h"
#include "dect2/timecompat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>

using namespace dect2;
static const auto gT0 = std::chrono::steady_clock::now();
static double secsSinceStart() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - gT0).count(); }

int main(int argc, char** argv) {
    if (!dect2::cpuSupportsBuild()) { fprintf(stderr, "this build needs a processor with AVX2 and FMA\n"); return 1; }
    Engine e;
    bool autoBw = false;
    DeviceInfo dev;
    TuneSettings tune;
    FileOptions file;
    double freq = 522, secs = 10;
    bool freqSet = false;
    int computeMode = 2, playSid = -1, standard = 0, servePort = 0; bool serveLan = false;
    uint64_t vseq = 0;
    bool continue_t2 = true;
    MultipathDetector mpd;
    QualityMeter qm;
    bool useFile = false, haveDev = false, synthetic = false, listen = false;
    std::string wavPath;
    std::vector<int16_t> wav;
    std::string record;
    OutputConfig out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)""; };
        if (a == "--hackrf") haveDev = true;
        else if (a == "--freq") { freq = atof(next()); freqSet = true; }
        else if (a == "--lna") tune.lnaDb = atoi(next());
        else if (a == "--vga") tune.vgaDb = atoi(next());
        else if (a == "--amp") tune.ampOn = true;
        else if (a == "--bw") tune.bandwidthMhz = atof(next());
        else if (a == "--autobw") autoBw = true;
        else if (a == "--secs") secs = atof(next());
        else if (a == "--play") playSid = atoi(next());
        else if (a == "--serve") servePort = atoi(next());
        else if (a == "--lan") serveLan = true;
        else if (a == "--synthetic") synthetic = true;
        else if (a == "--wav") wavPath = next();   // FM: write the sound to a 48 kHz stereo WAV file
        else if (a == "--listen") listen = true;   // FM: play the sound (the command line is silent otherwise)
        else if (a == "--demo") tune.synth.demoTv = true;   // synthetic DVB-T / ATSC carries the test-card programme
        else if (a == "--atsc") { tune.synth.atsc = true; tune.bandwidthMhz = 6; tune.sampleRate = 8e6; standard = 3; }   // ATSC 8-VSB (synthetic, or with --file at 8 Msps)
        else if (a == "--dvbt") { tune.synth.dvbt = true; synthetic = true; }                 // synthetic source generates DVB-T
        else if (a == "--dvbt-mode") tune.synth.dvbtMode = atoi(next());   // 0 = 2K, 1 = 8K
        else if (a == "--dvbt-gi") tune.synth.dvbtGuard = atoi(next());    // 0..3 = 1/32, 1/16, 1/8, 1/4
        else if (a == "--dvbt-mod") tune.synth.dvbtMod = atoi(next());     // 0 QPSK, 1 16-QAM, 2 64-QAM
        else if (a == "--dvbt-rate") tune.synth.dvbtRate = atoi(next());   // 0..4 = 1/2, 2/3, 3/4, 5/6, 7/8
        else if (a == "--snr") tune.synth.snrDb = atof(next());
        else if (a == "--sopt") { const int k = atoi(next()); const int v = atoi(next()); if (k >= 0 && k < 8) tune.synth.modeOpt[k] = v; }       // option k of the mode's test signal (see its <mode>_gen.h)
        else if (a == "--sval") { const int k = atoi(next()); const double v = atof(next()); if (k >= 0 && k < 4) tune.synth.modeVal[k] = v; }   // value k of the mode's test signal
        else if (a == "--standard") { std::string v = next(); standard = v == "t2" ? 1 : v == "t" || v == "dvbt" ? 2 : v == "atsc" ? 3 : v == "dab" ? 4 : v == "atsc3" ? 5 : v == "isdbt" ? 6 : v == "fm" ? 7 : 0; if (standard == 7) { tune.bandwidthMhz = 0.25; tune.sampleRate = 4e6; tune.basebandFilterHz = 2.5e6; }
            if (const ModeTuning* mt = modeTuningById(v)) {   // dvbs, dtmb, atv, dmr, drm, adsb: the tuning table of the mode says how to set the radio up
                standard = mt->stdMode; tune.bandwidthMhz = mt->bandwidthMhz; tune.sampleRate = mt->sampleRate; tune.basebandFilterHz = mt->basebandHz;
                if (!freqSet) freq = mt->defMhz;
            } }
        else if (a == "--compute") { std::string v = next(); computeMode = v == "cpu" ? 0 : v == "gpu" ? 1 : 2; }
        else if (a == "--file") { useFile = true; file.path = next(); file.format = guessFormat(file.path); }
        else if (a == "--rate") file.sampleRate = atof(next()) * 1e6;
        else if (a == "--format") { std::string f = next(); file.format = f == "cu8" ? FileFormat::CU8 : f == "cf32" ? FileFormat::CF32 : FileFormat::CS8; }
        else if (a == "--record") record = next();
        else if (a == "--ts") out.file = true, out.path = next();
        else if (a == "--udp") { std::string hp = next(); auto c = hp.find(':'); out.udp = true; out.host = hp.substr(0, c); if (c != std::string::npos) out.port = atoi(hp.c_str() + c + 1); }
        else if (a == "--gain") { tune.gainDb = atof(next()); }
        else if (a == "--rtp") out.rtp = true;
        else if (a == "--service") out.serviceId = atoi(next());
        else if (a == "--no-null") out.dropNull = true;
    }
    tune.centerHz = freq * 1e6;
    tune.synth.mode = standard >= 8 ? standard : 0;   // the synthetic source plays that mode's test signal
    if (standard != 7 && standard < 8) tune.sampleRate = tune.bandwidthMhz >= 7 ? 10e6 : 8e6; // HackRF Pro: exact tuning only at <= 10 Msps
    if (useFile) { dev.kind = DeviceInfo::File; dev.name = file.path; file.loop = false; }
    else if (synthetic) { dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic"; }
    else {
        std::string err;
        auto list = listRadios(err);
        if (list.empty()) { fprintf(stderr, "no radio found (HackRF or SoapySDR) %s\n", err.c_str()); return 1; }
        dev = list[0];
        printf("device: %s\n", dev.name.c_str());
        if (dev.isGeneric()) {
            if (dev.maxRateHz > 0) tune.sampleRate = std::min(tune.sampleRate, dev.maxRateHz);
            if (tune.gainDb == 30) tune.gainDb = std::max(dev.gainMinDb, dev.gainMaxDb * 0.6);   // a sensible start; --gain overrides
            printf("radio: up to %.2f Msps, gain %.0f..%.0f dB, using %.2f Msps / %.0f dB\n", dev.maxRateHz / 1e6, dev.gainMinDb, dev.gainMaxDb, tune.sampleRate / 1e6, tune.gainDb);
        }
    }
    (void)haveDev;
    if (getenv("DECT2_PLPLOG")) e.setPlpDump([&e](const PlpResult& r) {   // one line per decoded T2 frame: where do blocks get lost?
        RxTelemetry t;
        e.latestRx(t, 0);
        printf("[%6.2f] PLP frame %llu t2 %d: blocks %d ok %d bchFail %d hdrOk %d retry %d iters %.1f preBER %.2e MER %.1f dB %.0f ms | pilot SNR %.1f cp %.1f CFO %+.1f SRO %+.2f tim %+.2f%s\n", secsSinceStart(), (unsigned long long)r.frameNo, r.t2Frame, r.blocks, r.blocksOk, r.bchFailed, r.headerOk, r.retryRecovered, r.avgLdpcIters, r.preBer, r.merDb, r.decodeMs, t.dataSnrDb, t.cpSnrDb, t.cfoHz, t.sroPpm, t.timingErr, r.blocksOk < r.blocks ? "   <<< LOSS" : "");
    });
    if (getenv("DECT2_BBDUMP")) e.setPlpDump([](const PlpResult& r) {
        static int shown = 0;
        for (auto& f : r.frames) if (!f.bits.empty() && shown < 6) { shown++; printf("BBFRAME blk %d hdr:", f.blockIndex); for (int B = 0; B < 14; B++) { unsigned v = 0; for (int i = 0; i < 8; i++) v = (v << 1) | f.bits[B * 8 + i]; printf(" %02x", v); } printf("  crc %d upl %d dfl %d syncd %d\n", f.header.crcOk, f.header.upl, f.header.dfl, f.header.syncd); }
    });
    e.setOutputs(out);
    NetTuner net(e);
    if (servePort > 0) {
        NetTunerConfig nc; nc.port = servePort; nc.localOnly = !serveLan;
        if (!net.start(nc)) { fprintf(stderr, "network tuner: %s\n", net.stats().error.c_str()); return 1; }
        printf("network tuner on port %d (%s)\n", servePort, serveLan ? "whole network" : "this computer only");
    }
    e.setComputeMode(computeMode);
    e.fm().setSilent(!listen);
    if (!wavPath.empty()) e.fm().setAudioTap([&wav](const float* l, const float* r, size_t n) {
        for (size_t i = 0; i < n; i++) { wav.push_back((int16_t)std::lround(std::max(-1.f, std::min(1.f, l[i])) * 32767)); wav.push_back((int16_t)std::lround(std::max(-1.f, std::min(1.f, r[i])) * 32767)); }
    });
    e.setStandard(standard);
    e.setBandwidthAuto(autoBw);
    e.setSpectrumEnabled(autoBw);   // cli doesn't show the spectrum
    if (!e.start(dev, tune, file)) {
        size_t n; for (auto& l : e.logSnapshot(n)) fprintf(stderr, "%s\n", l.c_str());
        return 1;
    }
    size_t shown = 0;
    double lastPic = 0, freezeSecs = 0; int freezes = 0, pics = 0;
    RxTelemetry t; uint64_t seq = 0;
    SpectrumFrame sf; uint64_t sseq = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        for (int w = 0; w < 100; w++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (playSid >= 0) {
                const uint64_t before = vseq;
                auto shownFrame = e.player().videoFrame(vseq);
                if (vseq != before && shownFrame && getenv("DECT2_DUMPFRAMES")) {   // luma of every presented picture, quarter size, for contact sheets
                    static int dumped = 0;
                    char name[256];
                    snprintf(name, sizeof name, "%s/f%05d_%06.2f.pgm", getenv("DECT2_DUMPFRAMES"), dumped++, secsSinceStart());
                    if (FILE* df = fopen(name, "wb")) {
                        const int dw = shownFrame->w / 4, dh = shownFrame->h / 4;
                        fprintf(df, "P5 %d %d 255\n", dw, dh);
                        for (int yy = 0; yy < dh; yy++) for (int xx = 0; xx < dw; xx++) fputc(shownFrame->y.empty() ? 0 : shownFrame->y[(size_t)(yy * 4) * shownFrame->w + (size_t)(xx * 4)], df);
                        fclose(df);
                    }
                }
                if (vseq != before) {   // a new picture: record when it appeared (freeze statistics, DECT2_FREEZE=1)
                    const double now = secsSinceStart();
                    if (lastPic > 0 && now - lastPic > 0.15 && getenv("DECT2_FREEZE")) { freezes++; freezeSecs += now - lastPic; printf("[%6.2f] FREEZE: no new picture for %.2f s\n", now, now - lastPic); }
                    lastPic = now; pics++;
                }
            }
        }
        e.latestRx(t, 0);
        mpd.update(t);
        qm.update(t);
        e.latestSpectrum(sf, 0);
        if (playSid >= 0) {
            if (e.player().selected() < 0) { for (auto& sv : e.tsSnapshot().services) if (sv.id == playSid && sv.havePmt) { e.player().select(sv.id); e.player().setVolume(0.f); } }
            PlayerStats ps = e.player().stats();
            printf("  player: %s %dx%d %s decoded %llu shown %llu late %llu errors %llu queue %d audio buf %.0f ms A/V %+.0f ms underruns %d\n", ps.status.c_str(), ps.width, ps.height, ps.hardware ? "HW" : "SW", (unsigned long long)ps.decoded, (unsigned long long)ps.shown, (unsigned long long)ps.late, (unsigned long long)ps.errors, ps.videoQueue, ps.audioBufferMs, ps.avOffsetMs, ps.underruns);
        }
        size_t n;
        auto lines = e.logSnapshot(n);
        for (; shown < lines.size(); shown++) printf("  log: %s\n", lines[shown].c_str());
        if (out.udp) { OutputStats os = e.outputStats(); printf("  udp: sent %llu queue %.0f ms dropped %llu\n", (unsigned long long)os.udpDatagrams, os.udpQueueMs, (unsigned long long)os.udpDropped); }
        if (t.standard == 6) {
            const FmTelemetry& f = t.fm;
            printf("level %6.1f dBFS | FM %.3f MHz state %d carrier %d | SNR %4.1f dB | CFO %+7.0f Hz dev %2.0f kHz | %s pilot %.1f%% | RDS %s %3.0f%% groups %llu PI %04X '%s' [%s] %s | dropped %llu\n",
                   f.levelDbfs, freq, f.state, f.carrier, f.snrDb, f.cfoHz, f.devKhz, f.stereo ? "STEREO" : "mono  ", f.pilotPct, f.rdsSync ? "sync" : "----", f.rdsBlockOkPct,
                   (unsigned long long)f.rdsGroups, f.piCode, f.psName.c_str(), f.ptyText.c_str(), f.radioText.c_str(), (unsigned long long)e.droppedSamples());
            continue_t2 = false;
        } else if (t.standard >= 7) {
            printf("level %6.1f dBFS | %s | dropped %llu\n", sf.stats.rmsDbfs, modeSummary(t).c_str(), (unsigned long long)e.droppedSamples());
            continue_t2 = false;
        } else if (t.standard == 2) {
            const AtscTelemetry& a = t.atsc;
            printf("level %6.1f dBFS | ATSC pilot %d seg %d field %d ts %d | CFO %+7.0f Hz SRO %+6.1f ppm | SNR %.1f dB (data %.1f) sync %.2f | fields %llu RS clean %llu corrected %llu failed %llu | dropped %llu\n",
                   sf.stats.rmsDbfs, a.pilot, a.segSync, a.fieldSync, a.tsOk, a.cfoHz, a.sroPpm, a.snrDb, a.dataSnrDb, a.syncQuality, (unsigned long long)a.fields,
                   (unsigned long long)a.rsClean, (unsigned long long)a.rsCorrected, (unsigned long long)a.rsFailed, (unsigned long long)e.droppedSamples());
            continue_t2 = false;
        } else if (t.standard == 1) {
            printf("level %6.1f dBFS | DVB-T state %d %s GI %s | CFO %+8.1f Hz | TPS %d %s %s | data SNR %.1f dB | packets %llu RS clean %llu corrected %llu failed %llu | Viterbi margin %.2f phase %d | dropped %llu\n",
                   sf.stats.rmsDbfs, t.state, t.fftN == 8192 ? "8K" : t.fftN == 2048 ? "2K" : "?", t.giIdx >= 0 ? dvbt::guardName(t.giIdx) : "-", t.cfoHz, t.dvbt.tpsOk,
                   t.dvbt.tpsOk ? dvbt::modName(t.dvbt.mod) : "-", t.dvbt.tpsOk ? dvbt::rateName(t.dvbt.crHp) : "-", t.dataSnrDb, (unsigned long long)t.dvbt.packets,
                   (unsigned long long)t.dvbt.rsClean, (unsigned long long)t.dvbt.rsCorrected, (unsigned long long)t.dvbt.rsFailed, t.dvbt.viterbiMargin, t.dvbt.punctPhase, (unsigned long long)e.droppedSamples());
            continue_t2 = false;
        } else continue_t2 = true;
        if (continue_t2)
        printf("level %6.1f dBFS clip %.3f%% | state %d P1 %llu (s1 %d fft %d) GI %s | CFO %+8.1f Hz | CP %.2f | L1 pre %llu/%llu post %llu/%llu | data %d SNR %.1f dB | sym %llu sinceP1 %.2f | PLP %llu fr, blocks %llu ok/%llu bad, MER %.1f dB preBER %.1e it %.1f %.0f ms %s | plpDrop %llu | dropped %llu\n",
               sf.stats.rmsDbfs, sf.stats.clipFraction * 100, t.state, (unsigned long long)t.p1Count, t.p1.s1, t.fftN,
               t.giIdx >= 0 ? guardName(t.giIdx) : "-", t.cfoHz, t.cpCorr, (unsigned long long)t.l1preGood, (unsigned long long)(t.l1preGood + t.l1preBad),
               (unsigned long long)t.l1postGood, (unsigned long long)(t.l1postGood + t.l1postBad), t.dataValid, t.dataSnrDb, (unsigned long long)t.symbols, t.secSinceP1, (unsigned long long)t.plpFrames, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.plpMerDb, t.plpPreBer, t.plpIters, t.plpDecodeMs, t.plpOnGpu ? "GPU" : "CPU", (unsigned long long)t.plpFramesDropped, (unsigned long long)e.droppedSamples());
    }
    { const QualityReport& q = qm.report(); printf("quality: %.0f%% %s (SNR %.1f dB, needs %.1f, margin %+.1f dB, FEC %.1f%%)\n", q.percent, q.label.c_str(), q.snrDb, q.requiredDb, q.marginDb, q.fecOk * 100); }
    {
        auto epg = e.epg();
        TsSnapshot ts = e.tsSnapshot();
        const int64_t now = ts.utcNow;
        printf("programme guide (UTC now %s):\n", ts.utc.c_str());
        for (auto& sv : ts.services) {
            auto it = epg.find(sv.id);
            if (it == epg.end() || it->second.empty()) continue;
            printf("  %s: %zu events\n", sv.name.c_str(), it->second.size());
            int shown = 0;
            for (auto& ev : it->second) {
                if (now && ev.end() < now) continue;
                time_t a = (time_t)ev.start, b = (time_t)ev.end();
                char ta[16], tb[16];
                struct tm ma, mb; dect2::gmTime(a, &ma); dect2::gmTime(b, &mb);
                strftime(ta, sizeof ta, "%H:%M", &ma); strftime(tb, sizeof tb, "%H:%M", &mb);
                printf("    %s-%s %s%s%s\n", ta, tb, now && ev.start <= now && ev.end() > now ? "[now] " : "", ev.title.c_str(), ev.text.empty() ? "" : (" - " + ev.text.substr(0, 60)).c_str());
                if (++shown >= 4) break;
            }
        }
    }
    {
        const MultipathReport& mr = mpd.report();
        printf("multipath: %s (%s)\n", multipathName(mr.level), mr.headline.c_str());
        for (auto& x : mr.echoes) printf("   echo %.1f dB at %+.2f us%s\n", x.levelDb, x.delayUs, x.insideGuard ? "" : " (outside guard)");
        for (auto& r : mr.reasons) printf("   - %s\n", r.c_str());
    }
    if (t.l1preGood) {
        const L1Pre& p = t.l1pre;
        printf("\nL1-pre: type %d ext %d s1 %d s2 %d gi %s papr %d l1mod %d cod %d fec %d postSize %d postInfo %d PP%d tx %d cell %d net 0x%04X sys 0x%04X frames %d dataSyms %d regen %d ext %d rf %d/%d ver %d scr %d lite %d\n",
               p.type, p.bwtExt, p.s1, p.s2, guardName(p.guardInterval), p.papr, p.l1Mod, p.l1Cod, p.l1Fec, p.postSize, p.postInfoSize, p.pilotPattern + 1,
               p.txIdAvail, p.cellId, p.networkId, p.systemId, p.numFrames, p.numDataSyms, p.regen, p.postExtension, p.numRf, p.curRf, p.version, p.postScrambled, p.lite);
    }
    if (t.l1postGood) {
        const L1Post& q = t.l1post;
        printf("L1-post: plps %d aux %d rf %.3f MHz\n", q.numPlp, q.numAux, q.rf.empty() ? 0.0 : q.rf[0].freq / 1e6);
        for (auto& c : q.plps) printf("  PLP %d type %d payload %d cod %d mod %d rot %d fec %d blocksMax %d TI %d/%d group %d mode %d\n", c.id, c.type, c.payloadType, c.cod, c.mod, c.rotation, c.fecType, c.numBlocksMax, c.timeIlLength, c.timeIlType, c.groupId, c.plpMode);
    }
    {
        TsSnapshot ts = e.tsSnapshot();
        BbStats bb = e.bbStats();
        printf("\nBB frames %llu (lost %llu) packets %llu resyncs %llu  mode %s ISSYI %d NPD %d\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)bb.packets, (unsigned long long)bb.resyncs, bb.hem ? "HEM" : "normal", bb.issyi, bb.npd);
        printf("TS: network \"%s\" onid 0x%04X tsid 0x%04X  %s  mux %.0f kbit/s (null %.0f)  CC errors %llu\n", ts.networkName.c_str(), ts.onid, ts.tsid, ts.utc.c_str(), ts.muxKbps, ts.nullKbps, (unsigned long long)ts.ccErrors);
        for (auto& sv : ts.services) {
            printf("  [%4d] %-24s %-8s lcn %-3d pmt 0x%04X %s\n", sv.id, sv.name.c_str(), sv.typeName(), sv.lcn, sv.pmtPid, sv.caFlag ? "(scrambled)" : "");
            for (auto& st : sv.streams) printf("          pid 0x%04X %-10s %-14s %s %.0f kbit/s\n", st.pid, st.kind.c_str(), st.codec.c_str(), st.lang.c_str(), st.kbps);
            if (!sv.now.empty()) printf("          now: %s | next: %s\n", sv.now.c_str(), sv.next.c_str());
        }
        OutputStats os = e.outputStats();
        if (os.fileOpen) printf("file: %llu packets, %llu bytes\n", (unsigned long long)os.filePackets, (unsigned long long)os.fileBytes);
        if (os.udpOpen) printf("udp: %llu datagrams, dropped %llu\n", (unsigned long long)os.udpDatagrams, (unsigned long long)os.udpDropped);
    }
    e.stop();
    if (!wavPath.empty()) {
        if (FILE* f = fopen(wavPath.c_str(), "wb")) {
            const uint32_t bytes = (uint32_t)(wav.size() * 2), rate = 48000, br = rate * 4;
            const uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
            const uint32_t riff = 36 + bytes, fl = 16;
            fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
            fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
            fwrite(wav.data(), 2, wav.size(), f);
            fclose(f);
            printf("wrote %s (%.1f s)\n", wavPath.c_str(), wav.size() / 2 / 48000.0);
        }
    }
    { size_t n; auto lines = e.logSnapshot(n); for (; shown < lines.size(); shown++) printf("  log: %s\n", lines[shown].c_str()); }
    if (playSid >= 0 && getenv("DECT2_FREEZE")) printf("VIDEO: %d pictures, %d freezes > 150 ms, %.1f s frozen in total\n", pics, freezes, freezeSecs);
    return 0;
}
