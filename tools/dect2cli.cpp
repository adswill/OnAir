// Headless receiver: prints sync / L1 telemetry once per second. Useful for bring-up and logs.
//   dect2cli --hackrf [--freq MHz] [--lna dB] [--vga dB] [--amp] [--bw MHz] [--secs N]
//   dect2cli --list-devices   (index, driver, name, serial and antenna of every radio found, then exit)
//   dect2cli --device <index|serial|name part> [--antenna A|B|...] ...   (a radio other than the first: "--device 171007A210 --antenna A", "--device \"RSP2 antenna A\"", "--device 1"; --hackrf stays as it was)
//   dect2cli --file path.cs8 --rate Msps [--format cs8|cu8|cf32] [--bw MHz] [--secs N]
//   dect2cli ... --net-radio rtltcp,192.168.2.197,1234   (a radio on the network instead of the first one plugged in)
//   dect2cli ... --record out.cs8|out.cf32|folder|auto --secs N   (any radio or --synthetic, not --file: saves the radio's raw samples at its own
//     rate while receiving, for N seconds, and prints the size; a folder or "auto" (the current folder) gets a name like
//   dect2cli --standard iridium ... --iridium-bits output.bits   (Iridium: every burst as a gr-iridium "RAW:" line for iridium-toolkit;
//     then `iridium-parser.py output.bits > output.parsed`. --iridium-parsed is not written by OnAir: use the toolkit's parser)
//   dect2cli --standard isdbt --isdbt-bw 6|7|8|auto ...   (ISDB-T channel width: 6 MHz Japan / the Americas, 8 MHz the Maldives; auto tries each)
//   dect2cli --standard orbcomm --freq 137.25 --orbcomm-downlinks 2   (Orbcomm: up to two downlinks in the passband; satellites, Doppler and packets in the status line and the log)
//   dect2cli --standard tetra --freq 392.5   (TETRA network information: the cells found are listed at the end; receive only, nothing is decrypted)
//     onair_dvb_554.000MHz_10Msps_20261009-071530.cs8, which reopens with the right rate and format)
//   dect2cli --standard ble --ble-channel 37|38|39   (Bluetooth LE advertising, receive only: devices and packets in the status line and the log)
#include "dect2/wisun/wisun_proto.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/engine/engine.h"
#include "dect2/flarm/flarm_proto.h"
#include "dect2/source/iq_file.h"
#include "dect2/engine/crash_report.h"
#include "dect2/engine/modes.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/channels/channel.h"
#include "dect2/engine/quality.h"
#include "dect2/net/nettuner.h"
#include "dect2/net/netradio.h"
#include "dect2/source/device_pick.h"
#include "dect2/common/platform.h"
#include "dect2/common/timecompat.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>

using namespace dect2;
static const auto gT0 = std::chrono::steady_clock::now();
static double secsSinceStart() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - gT0).count(); }

int main(int argc, char** argv) {
    std::string msLog, sbLog, ipPcap;
    std::string dcsLog;
    dect2::crash::install("crash-cli");   // a crash leaves crash-cli.txt next to the log
    if (!dect2::cpuSupportsBuild()) { fprintf(stderr, "this build needs a processor with AVX2 and FMA\n"); return 1; }
    Engine e;
    bool autoBw = false;
    DeviceInfo dev;
    TuneSettings tune;
    FileOptions file;
    double freq = 522, secs = 10;
    bool freqSet = false, rateSet = false, fmtSet = false;
    int computeMode = 2, playSid = -1, standard = 0, servePort = 0; bool serveLan = false;
    uint64_t vseq = 0;
    bool continue_t2 = true;
    MultipathDetector mpd;
    QualityMeter qm;
    bool useFile = false, haveDev = false, synthetic = false, listen = false, atvFm = false;
    bool dpmrScan = false;
    double dvbcSr = 0; int dvbcQam = 0; bool dvbcAnnexC = false;
    std::string devSel, devAntenna; bool listDevices = false;   // --device, --antenna, --list-devices
    std::string netRadio;   // --net-radio: a radio on the network, as the settings keep it ("rtltcp,host,port"); used instead of the first one found
    int orbcommMax = 2;
    double refLat = 0, refLon = 0; bool haveRef = false;
    const SubMode* subMode = nullptr;   // --standard named one protocol of a shared receiver
    std::string wavPath;
    std::vector<int16_t> wav;
    std::string record, iridiumBits;
    OutputConfig out;
    T2ExportConfig t2x;
    int isdbtBw = 6, isdbtSynthBw = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)""; };
        if (a == "--version") { printf("dect2cli (OnAir)\nAIS frame layer: %s\n", dect2::ais::protoImplementation()); return 0; }   // always the Rust crate now
        else if (a == "--hackrf") haveDev = true;   // the first radio found (a HackRF when one is plugged in): kept as it was
        else if (a == "--list-devices") listDevices = true;
        else if (a == "--device") devSel = next();
        else if (a == "--antenna") devAntenna = next();
        else if (a == "--radio") { const std::string kv = next(); const auto eq = kv.find('='); if (eq != std::string::npos) tune.radio[kv.substr(0, eq)] = kv.substr(eq + 1); }   // a setting of the radio, e.g. --radio iqformat=cs8 (PlutoSDR with Tezuka), --radio ppm=-3
        else if (a == "--atv-fm") atvFm = true;
        else if (a == "--isdbt-bw") { const std::string v = next(); const int b = atoi(v.c_str()); isdbtBw = v == "auto" ? 0 : b == 7 || b == 8 ? b : 6; }   // with --standard isdbt: channel width 6, 7, 8 MHz (the Maldives) or auto
        else if (a == "--isdbt-synth-bw") { const int b = atoi(next()); isdbtSynthBw = b == 7 || b == 8 ? b : 6; }   // with --synthetic: the width of the ISDB-T test signal (for --isdbt-bw auto)
        else if (a == "--ble-channel") { freq = dect2::ble::channelMhz(atoi(next())); freqSet = true; }   // with --standard ble: advertising channel 37, 38 or 39
        else if (a == "--dvbc-sr") dvbcSr = atof(next()) * 1e6;   // with --standard dvbc: a manual symbol rate in Msym/s (default: measured)
        else if (a == "--dvbc-qam") dvbcQam = atoi(next());       // with --standard dvbc: 16, 32, 64, 128 or 256 (default: found from the stream)
        else if (a == "--net-radio") netRadio = next();         // e.g. rtltcp,192.168.2.197,1234 (any type the app's "Add network radio" takes)
        else if (a == "--dpmr-scan") dpmrScan = true;           // with --standard dpmr: scan the 16 PMR446 channels (tune to one of them)
        else if (a == "--orbcomm-downlinks") orbcommMax = atoi(next());   // with --standard orbcomm: 1 or 2 downlinks decoded at once (default 2)
        else if (a == "--dvbc-annexc") dvbcAnnexC = true;          // with --standard dvbc: J.83 annex C (roll-off 0.13)   // with --standard atv: FM video (analog FPV links), 20 Msps
        else if (a == "--freq") { freq = atof(next()); freqSet = true; }
        else if (a == "--ref" && i + 2 < argc) { refLat = atof(argv[++i]); refLon = atof(argv[++i]); haveRef = true; }
        else if (a == "--lna") tune.lnaDb = atoi(next());
        else if (a == "--vga") tune.vgaDb = atoi(next());
        else if (a == "--amp") tune.ampOn = true;
        else if (a == "--bw") tune.bandwidthMhz = atof(next());
        else if (a == "--autobw") autoBw = true;
        else if (a == "--secs") secs = atof(next());
        else if (a == "--dcs-log") dcsLog = next();   // with --standard goesdcs: append every report to this CSV file
        else if (a == "--ip-pcap") ipPcap = next();   // DVB-S2 with IP data (GSE): write every IP packet to this PCAP file (link type RAW)
        else if (a == "--sb-log") sbLog = next();   // with --standard satbeacon: append the carrier measurements and the frames to this CSV file
        else if (a == "--ms-log") msLog = next();   // with --standard mscatter: append every ping to this CSV file
        else if (a == "--play") playSid = atoi(next());
        else if (a == "--serve") servePort = atoi(next());
        else if (a == "--lan") serveLan = true;
        else if (a == "--synthetic") synthetic = true;
        else if (a == "--wav") wavPath = next();   // FM: write the sound to a 48 kHz stereo WAV file
        else if (a == "--listen") listen = true;   // FM, voice, CW and ham modes: play the sound (the command line is silent otherwise)
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
            const SubMode* sm = subModeById(v);   // rtty, ft8, meshtastic, pocsag, navtex ...: one protocol of a shared receiver
            subMode = sm;
            if (sm) tune.synth.mode = sm->stdMode, applySubModeSynth(tune.synth, *sm);
            if (const ModeTuning* mt = sm ? modeTuning(sm->stdMode) : modeTuningById(v)) {   // dvbs, dtmb, atv, dmr, drm, adsb: the tuning table of the mode says how to set the radio up
                standard = mt->stdMode; tune.bandwidthMhz = mt->bandwidthMhz; tune.sampleRate = mt->sampleRate; tune.basebandFilterHz = mt->basebandHz;
                if (!freqSet) freq = sm ? sm->defMhz : mt->defMhz;
            } }
        else if (a == "--compute") { std::string v = next(); computeMode = v == "cpu" ? 0 : v == "gpu" ? 1 : 2; }
        else if (a == "--file") { useFile = true; file.path = next(); file.format = guessFormat(file.path); }
        else if (a == "--rate") { file.sampleRate = atof(next()) * 1e6; rateSet = true; }
        else if (a == "--format") { fmtSet = true; std::string f = next(); file.format = f == "cu8" ? FileFormat::CU8 : f == "cf32" ? FileFormat::CF32 : FileFormat::CS8; }
        else if (a == "--record") record = next();
        else if (a == "--iridium-bits") iridiumBits = next();
        else if (a == "--iridium-parsed") { fprintf(stderr, "--iridium-parsed: OnAir writes .bits only; run iridium-toolkit's iridium-parser.py on an --iridium-bits file for output.parsed\n"); return 1; }
        else if (a == "--ts") out.file = true, out.path = next();
        else if (a == "--udp") { std::string hp = next(); auto c = hp.find(':'); out.udp = true; out.host = hp.substr(0, c); if (c != std::string::npos) out.port = atoi(hp.c_str() + c + 1); }
        else if (a == "--gain") { tune.gainDb = atof(next()); }
        else if (a == "--rtp") out.rtp = true;
        else if (a == "--service") out.serviceId = atoi(next());
        else if (a == "--no-null") out.dropNull = true;
        else if (a == "--t2-bbf") { t2x.format = T2ExportConfig::Bbf; t2x.file = next(); }                  // DVB-T2: raw BB frames of the PLP to a file
        else if (a == "--t2-mi") { t2x.format = T2ExportConfig::T2mi; t2x.file = next(); }                  // DVB-T2: the PLP as T2-MI in TS packets to a file
        else if (a == "--t2-udp") { std::string hp = next(); auto c = hp.find(':'); t2x.udpHost = hp.substr(0, c); if (c != std::string::npos) t2x.udpPort = atoi(hp.c_str() + c + 1); }   // ... and/or by UDP (T2-MI unless --t2-bbf)
        else if (a == "--t2-pid") t2x.pid = atoi(next());
    }
    if (useFile) {   // .sigmf-meta / .sigmf-data / two-channel .wav: the file says its rate, frequency and format (the flags still win)
        IqFileInfo info; std::string perr;
        if (probeIqFile(file.path, info, perr)) {
            file.path = info.dataPath; file.offsetBytes = info.offsetBytes; file.endBytes = info.endBytes; file.bigEndian = info.bigEndian;
            if (!fmtSet) file.format = info.format;
            if (!rateSet && info.sampleRate > 0) file.sampleRate = info.sampleRate;
            if (!freqSet && info.centerHz > 0) freq = info.centerHz / 1e6;
        } else if (!perr.empty()) { fprintf(stderr, "%s\n", perr.c_str()); return 1; }
    }
    if (atvFm) { tune.bandwidthMhz = 18; tune.sampleRate = 20e6; tune.basebandFilterHz = 18e6; }
    if (standard == 6) {   // ISDB-T: the channel width (the test signal follows it; on auto it is 6 MHz unless --isdbt-synth-bw says otherwise)
        tune.isdbtBwMhz = isdbtBw;
        tune.synth.isdbtBwMhz = isdbtSynthBw ? isdbtSynthBw : isdbtBw ? isdbtBw : 6;
        if (isdbtBw != 6) tune.bandwidthMhz = isdbtBw ? isdbtBw : 8;
    }
    tune.centerHz = freq * 1e6;
    tune.synth.mode = standard >= 4 ? standard : 0;   // the synthetic source plays that mode's test signal
    if (standard != 7 && standard < 8) tune.sampleRate = tune.bandwidthMhz >= 7 ? 10e6 : 8e6; // HackRF Pro: exact tuning only at <= 10 Msps
    if (useFile && !record.empty()) { fprintf(stderr, "--record does not work with --file: the samples are already in that file\n"); return 1; }
    if (!netRadio.empty() && (useFile || synthetic)) { fprintf(stderr, "--net-radio is a radio: it does not work with %s\n", useFile ? "--file" : "--synthetic"); return 1; }
    if (useFile) { dev.kind = DeviceInfo::File; dev.name = file.path; file.loop = false; }
    else if (synthetic) { dev.kind = DeviceInfo::Synthetic; dev.name = "synthetic"; }
    else {
        std::string err;
        std::string want;   // the list entry of the --net-radio radio
        if (!netRadio.empty()) {
            const auto nr = netRadiosFromString(netRadio);
            if (nr.size() != 1) { fprintf(stderr, "--net-radio: expected one radio as type,host,port (rtltcp,192.168.2.197,1234), got \"%s\"\n", netRadio.c_str()); return 1; }
            setNetworkRadios(nr);
            want = netRadioDeviceName(nr[0]);
        }
        auto list = listRadios(err);
        if (listDevices) { fputs(deviceListText(list).c_str(), stdout); if (list.empty()) fprintf(stderr, "no radio found %s\n", err.c_str()); return 0; }
        if (!devSel.empty() || !devAntenna.empty()) {   // --device / --antenna: that radio instead of the first one
            std::string perr;
            const int k = pickDevice(list, devSel, devAntenna, perr);
            if (k < 0) { fprintf(stderr, "--device: %s\n", perr.c_str()); return 1; }
            list = {list[(size_t)k]};
        }
        if (!want.empty()) {
            auto it = std::find_if(list.begin(), list.end(), [&](const DeviceInfo& d) { return d.name == want; });
            if (it == list.end()) { fprintf(stderr, "--net-radio: %s is not in the radio list %s\n", want.c_str(), err.c_str()); return 1; }
            list = {*it};
        }
        if (list.empty()) { fprintf(stderr, "no radio found (HackRF or SoapySDR) %s\n", err.c_str()); return 1; }
        dev = list[0];
        printf("device: %s\n", dev.name.c_str());
        if (dev.isGeneric()) {
            // DVB-T/T2 is natively 64/7 Msps per 8 MHz: a PlutoSDR can run exactly that, which saves the receiver its resampler; on its USB
            // cable above the 4 Msps the cable carries when the channel needs it (linkRateFor), with the note why
            if (dev.kind == DeviceInfo::Native && dev.board == "pluto" && standard <= 2 && tune.bandwidthMhz >= 5) tune.sampleRate = dvbNativeRate(tune.bandwidthMhz);
            tune.sampleRate = linkRateFor(dev, tune, tune.sampleRate, minSampleRateFor(standard, tune.bandwidthMhz));
            if (const std::string n = linkNote(dev, tune, tune.sampleRate); !n.empty()) printf("note: %s\n", n.c_str());
            if (tune.gainDb == 30) tune.gainDb = std::max(dev.gainMinDb, dev.gainMaxDb * 0.6);   // a sensible start; --gain overrides
            printf("radio: up to %.2f Msps, gain %.0f..%.0f dB, using %.2f Msps / %.0f dB\n", dev.maxRateHz / 1e6, dev.gainMinDb, dev.gainMaxDb, tune.sampleRate / 1e6, tune.gainDb);
        }
    }
    (void)haveDev;
    if (!t2x.file.empty() || !t2x.udpHost.empty()) { const std::string err = e.setT2Export(t2x); if (!err.empty()) { fprintf(stderr, "T2 export: %s\n", err.c_str()); return 1; } }
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
    e.vor().setSilent(!listen);   // VOR / ILS: the voice only with --listen
    e.m17().setSilent(!listen);   // M17 voice only with --listen as well
    e.cw().setSilent(!listen);    // CW sidetone only with --listen as well
    e.psk().setListen(listen);    // PSK31, JS8Call, HF digital (RTTY, SSTV, FT8 ...): the sideband audio only with --listen
    e.js8().setListen(listen);
    e.ale().setListen(listen);    // HF ALE as well
    e.hfdig().setListen(listen);
    e.specx().setSilent(true);  // spectrum explorer: the summary line only
    e.uplink().setSilent(true);   // Mode S uplink: no sound
    e.specan().setSilent(true);   // spectrum analyser: no sound
    e.astro().setSilent(true);    // radio astronomy: the summary line only
    e.iono().setSilent(true);     // ionospheric sounders: the summary line only
    e.qo100().setSilent(true);    // QO-100: Listen without the sound card
    e.general().setSilent(true);  // general receiver: no sound from the command line
    e.wifi().clearNetworks();     // Wi-Fi networks: no sound; the network table is printed with each line
    e.flarm().clearResults();
    if (!dcsLog.empty()) e.goesdcs().setLogFile(dcsLog);   // GOES DCS: the reports are appended to a CSV file
    if (!ipPcap.empty()) e.dvbs().setIpPcap(ipPcap);
    if (!sbLog.empty()) e.satbeacon().setLogFile(sbLog);   // satellite beacon: carrier measurements and frames as CSV
    if (!msLog.empty()) e.mscatter().setLogFile(msLog);   // meteor scatter: the pings are appended to a CSV file
    e.hfbeacon().setSilent(true);   // HF beacons: no sound in the command line
    e.dji().clearResults();       // DJI drones: the drones and signals are printed with each line
    e.wb().clearResults();
    e.datawell().clearResults();
    e.multitone().clearResults(); // Multitone paging: the transmissions are printed with each line
    e.codar().clearResults();     // CODAR radars: the table is printed with each line
    e.same().clearResults();      // SAME alerts: the newest are printed with each line
    e.ant().clearResults();       // ANT+ sensors: the sensors are printed with each line
    e.dect().clearResults();      // DECT: the base stations are printed with each line
    e.same().setSilent(true);     // weather radio: no sound from the command line
    e.halow().clearResults();     // Wi-Fi HaLow networks: the table is printed with each line
    e.wisun().clearResults();     // Wi-SUN FAN: the newest frames are printed with each line
    e.cubesat().clearFrames();    // CubeSat telemetry: the frames are printed with each line
    e.globalstar().clearResults();   // Globalstar: the events are printed with each line
    e.vdes().clearResults();      // VDES: the ASM frames and bursts are printed with each line
    e.rid().clearDrones();        // Drone Remote ID: the drone table is printed with each line
    e.dvbc().setSymbolRate(dvbcSr);   // DVB-C: no sound of its own; the transport stream goes to the player and outputs
    e.dvbc().setQam(dvbcQam == 16 ? 0 : dvbcQam == 32 ? 1 : dvbcQam == 64 ? 2 : dvbcQam == 128 ? 3 : dvbcQam == 256 ? 4 : -1);
    e.dvbc().setAnnexC(dvbcAnnexC);
    e.orbcomm().setMaxChannels(orbcommMax);   // Orbcomm: no sound; satellites and packets go to the log
    e.dstar().setSilent(!listen); // D-STAR voice (mbelib) too
    e.nxdn().setSilent(!listen);  // NXDN voice (mbelib) too
    e.dpmr().setSilent(!listen);  // dPMR voice (mbelib) too
    e.p25().setSilent(!listen);   // P25 voice (mbelib IMBE) too
    e.dpmr().setScan(dpmrScan);   // --dpmr-scan: step through the 16 PMR446 channels
    e.mac().setSilent(!listen);   // MAC television sound (--listen plays it)
    if (!wavPath.empty()) e.fm().setAudioTap([&wav](const float* l, const float* r, size_t n) {
        for (size_t i = 0; i < n; i++) { wav.push_back((int16_t)std::lround(std::max(-1.f, std::min(1.f, l[i])) * 32767)); wav.push_back((int16_t)std::lround(std::max(-1.f, std::min(1.f, r[i])) * 32767)); }
    });
    e.setStandard(standard);
    applySubMode(e, subMode);
    if (standard == 22) e.mesh().setTunedHz(freq * 1e6);   // mesh places its channels from the tuned frequency
    if (atvFm) e.atv().setModulation(1);
    e.setBandwidthAuto(autoBw);
    e.setSpectrumEnabled(autoBw);   // cli doesn't show the spectrum
    if (haveRef) e.adsb().setReference(refLat, refLon);
    if (!e.start(dev, tune, file)) {
        size_t n; for (auto& l : e.logSnapshot(n)) fprintf(stderr, "%s\n", l.c_str());
        return 1;
    }
    if (!record.empty()) {
        std::string rpath = record, rerr;
        FileFormat rfmt = guessFormat(record);
        const bool folder = record == "auto" || record.back() == '/' || record.back() == '\\' || std::filesystem::is_directory(std::filesystem::u8path(record));
        if (folder) { rfmt = FileFormat::CS8; rpath = (record == "auto" ? std::string(".") : record) + "/" + e.recordingName(rfmt); }
        if (!e.startRecording(rpath, rfmt, rerr)) { fprintf(stderr, "recording: %s\n", rerr.c_str()); e.stop(); return 1; }
        printf("recording the radio's samples to %s\n", rpath.c_str());
    }
    if (!iridiumBits.empty()) {
        std::string ierr;
        if (!e.iridium().startBitsFile(iridiumBits, &ierr)) { fprintf(stderr, "iridium bits: %s\n", ierr.c_str()); e.stop(); return 1; }
        printf("writing Iridium bursts for iridium-toolkit to %s\n", iridiumBits.c_str());
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
            if (t.standard == 55)   // Wi-Fi networks: the table, as a phone's Wi-Fi list
                for (const auto& ap : t.wifi.aps)
                    printf("  wifi: %s %-32s ch %2d %-12s %6.1f dBFS %s\n", ap.bssid.c_str(), ap.hidden && ap.ssid.empty() ? "(hidden)" : ap.ssid.c_str(), ap.channel,
                           wifi::securityName(ap.security, ap.enterprise), ap.rssi, ap.vendor.c_str());
            if (t.standard == 12) {   // ADS-B: aircraft and whether CPR produced a position
                for (const auto& a : t.adsb.aircraft) {
                    char pos[48] = "-";
                    if (a.hasPos) snprintf(pos, sizeof pos, "%.4f,%.4f", a.lat, a.lon);
                    printf("  ac: %06X %-8s alt=%d spd=%d pos=%s cpr=%c%c kind=%d msgs=%u%s\n",
                           a.icao & 0xFFFFFF, a.callsign.empty() ? "-" : a.callsign.c_str(),
                           a.hasAlt ? a.altFt : -1, a.hasSpeed ? (int)a.speedKt : -1, pos,
                           (a.cprHave & 1) ? 'E' : '-', (a.cprHave & 2) ? 'O' : '-', a.posKind, a.messages, a.hasPos ? " FIX" : "");
                }
                int nfr = 0;
                for (auto it = t.adsb.frames.rbegin(); it != t.adsb.frames.rend() && nfr < 12; ++it, ++nfr)
                    printf("  msg: DF%02d TC%2d %s\n", it->df, it->tc, it->what.c_str());
            }
            if (t.standard == 67)   // FLARM: the aircraft heard (flagged aircraft are only counted)
                for (size_t i = 0; i < t.flarm.aircraft.size() && i < 3; i++) {
                    const FlarmAircraft& a = t.flarm.aircraft[i];
                    printf("  flarm: %06X %-16s %5d m %5.0f km/h %4.0f deg %+5.1f m/s %s\n", a.id, dect2::flarm::aircraftTypeName(a.acType), a.altM, a.speedKmh, a.trackDeg, a.climbMs,
                           a.hasRange ? (std::to_string((int)(a.distKm * 10) / 10.0).substr(0, 5) + " km").c_str() : "-");
                }
            if (t.standard == 68) {   // HF beacons: the ones heard now (the built-in list)
                static const std::vector<hfb::Beacon> kList = hfb::defaultBeacons();
                for (size_t i = 0; i < t.hfb.beacons.size() && i < kList.size(); i++)
                    if (t.hfb.beacons[i].heard)
                        printf("  hfbeacon: %-34s %9.1f kHz %+6.1f dB conf %d %s\n", kList[i].name.c_str(), kList[i].khz, (double)t.hfb.beacons[i].snr, t.hfb.beacons[i].conf, t.hfb.beacons[i].note.c_str());
            }
            if (t.standard == 69) {   // DJI drones: the DroneID packets and what the shape detector sees
                for (const DjiDrone& d : t.dji.drones)
                    printf("  dji: DroneID %-16s %llu packets  %.5f,%.5f  home %.5f,%.5f  alt %.1f m  %s\n", d.serial.c_str(), (unsigned long long)d.packets, d.last.lat(), d.last.lon(), d.last.homeLat(), d.last.homeLon(), d.last.altitudeM(), d.last.crc16Ok ? "" : "(packet CRC-16 differs)");
                for (const DjiTrack& k : t.dji.tracks)
                    printf("  dji: %-24s %6.2f MHz wide at %+7.2f MHz  %d bursts  period %.2f ms  duty %.0f%%  OFDM %.2f  confidence %.0f%%\n", djiClassName(k.cls).c_str(), k.bwMhz, k.offsetHz / 1e6, k.bursts, k.periodMs, k.duty * 100, k.ofdm, k.conf * 100);
            }
            if (t.standard == 7 && t.dvbs.ip.active)   // DVB-S/S2 with IP data: the traffic
                printf("  ip: %s, %llu packets, %.1f MB, %zu flows, %zu labels%s\n", t.dvbs.ip.source.c_str(), (unsigned long long)t.dvbs.ip.packets, (double)t.dvbs.ip.bytes / 1e6, t.dvbs.ip.flowCount, t.dvbs.ip.labelCount, t.dvbs.ip.otherEncrypted ? ", non-IP PDUs encrypted" : "");
            if (t.standard == 73)   // CODAR / HF ocean radar: the radars heard
                for (const auto& s : t.codar.stations)
                    printf("  codar: #%d %9.4f MHz %6.1f kHz %s %7.1f kHz/s period %.2f s %5.1f dB %llu sweeps %s%s\n", s.id, s.centreMhz, s.bwKhz, s.dir > 0 ? "up  " : "down", s.rateKhzS, s.periodSec, s.snrDb,
                           (unsigned long long)s.sweeps, s.callsign.empty() ? "" : "ID ", s.callsign.empty() ? "" : (s.callsign + " (unverified)").c_str());
            if (t.standard == 75) {   // Datawell buoy HF link: the carrier, the bits and the frames found
                const auto& d = t.datawell;
                printf("  datawell: %s %.1f Hz wide at %+.1f Hz  %.2f baud (%+.0f ppm)  step %.1f Hz  symbol error %.2f  %.1f dB  %s\n", d.format.empty() ? "-" : d.format.c_str(), d.bwHz, d.offsetHz, d.baud, d.clockPpm, d.stepHz, d.symErr, d.snrDb, d.channel.c_str());
                if (d.hxvLocked) {
                    printf("  datawell: HXV %llu vectors (%llu clean, %llu repaired, %llu lost) %llu blocks%s\n", (unsigned long long)d.hxvVectors, (unsigned long long)d.hxvClean, (unsigned long long)d.hxvRepaired, (unsigned long long)d.hxvLost, (unsigned long long)d.hxvBlocks, d.hxvInverted ? " inverted" : "");
                    if (!d.disp.empty()) printf("  datawell: last vector heave %d north %d west %d cm%s\n", d.disp.back().h, d.disp.back().n, d.disp.back().w, d.disp.back().wrsg ? " (WR-SG)" : "");
                    if (d.sys.have[1]) printf("  datawell: Hs %.2f m  Tz %.1f s  water %.1f C  spectrum points %d\n", d.sys.hs, d.sys.tz, d.sys.tw, d.specCount);
                }
                if (d.periodBits) printf("  datawell: period %d bits (z %.0f) sync %s CRC attempt %s (%d / %d) unverified\n", d.periodBits, d.periodZ, d.syncHex.empty() ? "-" : d.syncHex.c_str(), d.crcName.empty() ? "none" : d.crcName.c_str(), d.crcPass, d.crcTotal);
                for (size_t i = 0; i < d.frames.size() && i < 3; i++) printf("  datawell: frame %s%s\n", d.frames[i].hex.c_str(), d.frames[i].crcOk ? " crc-attempt-ok" : "");
            }
            if (t.standard == 78)   // ANT+ sensors: one line a sensor
                for (const auto& d : t.ant.devices)
                    printf("  ant: %u %s%s trans %u  %s: %s  %.1f dBFS, %llu frames\n", d.deviceNumber, d.typeName.c_str(), d.marker == 0xA6C5 ? "" : " (other network)", d.transType,
                           d.lastPage.c_str(), d.lastText.c_str(), d.rssi, (unsigned long long)d.frames);
            if (t.standard == 79)   // DECT: one line a base station
                for (const auto& b : t.dect.bases)
                    printf("  dect: base %s emc %d  %.1f dBFS, %llu beacons, slots %06X\n", dect::hex40(b.rfpi).c_str(), b.emc, b.rssi, (unsigned long long)b.frames, b.slotsMask);
            if (t.standard == 77)   // NOAA weather radio SAME: the newest messages
                for (size_t i = 0; i < t.same.alerts.size() && i < 3; i++) {
                    const auto& al = t.same.alerts[i];
                    if (al.valid && !al.eom) {
                        printf("  same: %s (%s) from %s, valid %s, %zu location(s), %d of %d repeats agree%s\n", al.msg.eventName.empty() ? "unknown event" : al.msg.eventName.c_str(), al.msg.event.c_str(), al.msg.org.c_str(),
                               dect2::same::purgeText(al.msg.purgeMin).c_str(), al.msg.locs.size(), al.agreeing, al.bursts, al.toneSec > 0 ? ", 1050 Hz tone" : "");
                        for (const auto& l : al.msg.locs) printf("  same:   %s  %s\n", l.code.c_str(), l.text.c_str());
                    } else printf("  same: %s%s%s\n", al.eom ? "end of message" : "damaged: ", al.eom ? "" : al.raw.c_str(), al.eom ? "" : (" (" + al.err + ")").c_str());
                }
            if (t.standard == 76) {   // Multitone paging: what was measured in the last transmissions (never decoded text)
                for (const auto& x : t.multitone.tx) {
                    printf("  multitone: tx %llu %.1f s %.1f dB dev %.0f Hz  %s", (unsigned long long)x.id, x.durSec, x.snrDb, x.devHz, x.kind.c_str());
                    if (!x.tones.empty()) printf("  tones: %s", x.tones.c_str());
                    if (x.periodMs > 0) printf("  repeats %.0f ms (%.2f)", x.periodMs, x.periodStrength);
                    if (!x.lineHz.empty()) { printf("  lines:"); for (size_t i = 0; i < x.lineHz.size() && i < 4; i++) printf(" %.0f", x.lineHz[i]); printf(" Hz"); }
                    printf("\n");
                }
                for (const auto& b : t.multitone.bursts) {
                    printf("  multitone: fsk %.1f baud%s%s coh %.2f %d bits, BCH-valid words %d of %d (run %d, offset %d)%s\n    %s\n    bits %s\n", b.baud, b.nearestStd.empty() ? "" : " ~", b.nearestStd.c_str(), b.coherence,
                           b.nBits, b.validWords, b.totalWords, b.bestRun, b.align, b.syncFound ? " POCSAG-sync" : "", b.verdict.c_str(), b.bitsHex.c_str());
                    if (b.syncCount > 0) printf("    0x605A x%d, words after: %s\n", b.syncCount, b.afterSync.c_str());
                    for (size_t i = 0; i < b.words.size() && i < 16; i++) printf("    %08X %s\n", b.words[i], b.wordOk[i] ? "bch-ok" : "-");
                }
            }
            if (t.standard == 71)   // Wi-Fi HaLow networks: the table
                for (const auto& n : t.halow.nets)
                    printf("  halow: %s %-32s ch %3d %6.1f MHz %d MHz MCS %d x%d %-9s %-9s SNR %3.0f dB %6.1f dBFS %s\n", n.bssid.c_str(), n.hasSsid ? n.ssid.c_str() : n.hidden ? "(hidden)" : "(compressed SSID)", n.ccf, n.freqMhz,
                           n.seenWidthMhz, n.mcs, n.nss, n.security.c_str(), n.pre == 0 ? "S1G_SHORT" : n.pre == 1 ? "S1G_LONG" : "S1G_1M", n.snrDb, n.rssi, n.vendor.c_str());
            if (t.standard == 142)   // GOES DCS: the newest reports
                for (size_t i = 0; i < t.goesdcs.messages.size() && i < 3; i++) {
                    const auto& p = t.goesdcs.messages[i];
                    printf("  dcs: %08X ch %d %d bps %5.1f dB %.60s\n", p.address, p.channel, p.bps, (double)p.snrDb, p.text.c_str());
                }
            if (t.standard == 143)   // Mode A/C: the newest codes
                for (size_t i = 0; i < t.modeac.codes.size() && i < 4; i++) {
                    const auto& c = t.modeac.codes[i];
                    printf("  code: squawk %04d  alt %s  type %s  %u replies  %.0f dBFS%s%s\n", c.squawk, c.altValid ? std::to_string(c.altFt).c_str() : "-", c.kind == 1 ? "A" : c.kind == 2 ? "C" : "A/C", c.count, (double)c.levelDbfs, c.spi ? "  SPI" : "", c.icao ? "  matched" : "");
                }
            if (t.standard == 7 && t.dvbs.ip.active)   // DVB-S/S2 with IP data: the traffic
                printf("  ip: %s, %llu packets, %.1f MB, %zu flows, %zu labels%s\n", t.dvbs.ip.source.c_str(), (unsigned long long)t.dvbs.ip.packets, (double)t.dvbs.ip.bytes / 1e6, t.dvbs.ip.flowCount, t.dvbs.ip.labelCount, t.dvbs.ip.otherEncrypted ? ", non-IP PDUs encrypted" : "");
            if (t.standard == 145) {   // satellite beacon: the carrier, the telemetry, the newest frames
                const auto& b = t.satbeacon;
                printf("  beacon: carrier %+.1f Hz, %.1f dB-Hz, drift %+.1f Hz/min, %s; telemetry %d tone(s) at %.0f Hz, %.2f bit/s, frame %d bits, %llu good %llu bad\n", b.cfoHz, (double)b.snrDb, b.driftHzPerMin, b.carrierLocked ? "locked" : "searching", b.tones, b.subcarrierHz, b.baud, b.frameBits, (unsigned long long)b.blocksOk, (unsigned long long)b.blocksBad);
                for (size_t i = 0; i < b.frames.size() && i < 2; i++) { printf("  frame:"); for (uint8_t x : b.frames[i].bytes) printf(" %02X", x); printf(" (%d errors)\n", b.frames[i].fixedErrors); }
            }
            if (t.standard == 151)   // SBAS: the satellites with what they have sent
                for (const auto& sat : t.sbas.sats) {
                    int masked = 0, fast = 0;
                    for (const auto& g : t.sbas.geos) if (g.prn == sat.prn) { masked = g.nMasked; fast = dect2::sbas::Network::summarise(g).nFast; }
                    printf("  sbas: PRN %d %-6s %5.1f dB-Hz  %u messages  last type %d  mask %d satellites  %d fast corrections\n", sat.prn, dect2::sbasServiceOfPrn(sat.prn), (double)sat.cn0, sat.messages, sat.lastType, masked, fast);
                }
            if (t.standard == 105)   // meteor scatter: the newest pings
            if (t.standard == 105)   // meteor scatter: the newest pings
                for (size_t i = 0; i < t.mscatter.pings.size() && i < 3; i++) {
                    const auto& p = t.mscatter.pings[i];
                    printf("  ping: %.2f s %5.1f dB %+6.1f Hz %s\n", (double)p.durationSec, (double)p.peakSnrDb, (double)p.dopplerHz, p.kind ? "overdense" : "underdense");
                }
            if (t.standard == 85) {   // Wi-SUN FAN: frames by type, then the newest frames
                printf("  wisun types:");
                for (int k = 0; k < kWsUttTypes; k++) if (t.wisun.perUtt[k]) printf(" %s %llu", wisun::uttShort(k), (unsigned long long)t.wisun.perUtt[k]);
                printf(", plain %llu, bad %llu, corrected %llu, mode switches %llu, dropped %llu\n", (unsigned long long)t.wisun.plain, (unsigned long long)t.wisun.blocksBad,
                       (unsigned long long)t.wisun.corrected, (unsigned long long)t.wisun.modeSwitches, (unsigned long long)t.wisun.dropped);
            }
            if (t.standard == 85)
                for (size_t i = 0; i < t.wisun.log.size() && i < 4; i++) {
                    const WsFrame& f = t.wisun.log[i];
                    printf("  wisun: %8.3f s %7.1f MHz %3d kbit/s %6.1f dBFS %-23s > %-23s %s\n", f.sec, f.mhz, f.bitRate / 1000, f.rssi, f.src.c_str(), f.dst.c_str(), f.info.c_str());
                }
            if (t.standard == 72)   // LoRa (raw): the newest frames
                for (size_t i = 0; i < t.loraraw.log.size() && i < 4; i++) {
                    const LrFrame& f = t.loraraw.log[i];
                    printf("  lora: %8.2f s SF%-2d %3.0f kHz 4/%d %s%s %3d bytes %5.1f dB SNR %6.1f dBFS %+7.0f Hz | %s | %s\n", f.sec, f.sf, f.bwHz / 1e3, f.cr - 4, f.hasCrc ? (f.crcOk ? "crc ok " : "crc BAD") : "no crc ",
                           f.implicit ? " impl" : "", f.len, f.snrDb, f.levelDb, f.cfoHz, f.hex.substr(0, 48).c_str(), f.ascii.substr(0, 40).c_str());
                }
            if (t.standard == 62)   // CubeSat telemetry: the newest frames
                for (size_t i = 0; i < t.cube.frames.size() && i < 3; i++) {
                    const CubeFrame& f = t.cube.frames[i];
                    printf("  cubesat: %8.1f s %-9s %5d %-20s %-24s %3zu bytes rs %d %s\n", f.timeSec, cubesat::framingShort(f.framing), f.baud, f.sat.c_str(),
                           (f.from.empty() ? std::string("-") : f.from + ">" + f.to).c_str(), f.data.size(), f.rsErrors, cubesat::asciiOf(f.payload).substr(0, 60).c_str());
                }
            if (t.standard == 63) {   // Globalstar: the active channels
                for (const GsChannel& c : t.globalstar.chan)
                    if (c.active) printf("  globalstar: channel %2d %.2f MHz  SNR %5.1f dB  Doppler %+8.0f Hz  occupancy %.2f\n", c.index + 1, c.centreMhz, c.snrDb, c.dopplerHz, c.occupancy);
            }
            if (t.standard == 64) {   // VDES: the newest ASM frames and VDE-TER bursts
                for (size_t i = 0; i < t.vdes.asms.size() && i < 3; i++)
                    printf("  vdes asm %c: %8.1f s %3d bits %s\n", t.vdes.asms[i].channel, t.vdes.asms[i].timeSec, t.vdes.asms[i].bits, t.vdes.asms[i].hex.substr(0, 48).c_str());
                for (size_t i = 0; i < t.vdes.bursts.size() && i < 3; i++) {
                    const VdeBurst& b = t.vdes.bursts[i];
                    printf("  vde-ter: %8.1f s %.4f MHz %3d kHz %-9s %6.1f dBFS SNR %5.1f dB %5.1f ms\n", b.timeSec, b.centreMhz, b.bwKhz, vdeModName(b.mod), b.rssiDb, b.snrDb, b.durMs);
                }
            }
            if (t.standard == 57)   // Drone Remote ID: one line a drone
                for (const auto& d : t.rid.drones)
                    printf("  rid: %s %-3s %-20s %10.6f %11.6f h %6.1f m %5.1f m/s op %10.6f %11.6f %s\n", d.mac.c_str(), d.viaBle ? "BLE" : "WiFi",
                           !d.serial.empty() ? d.serial.c_str() : !d.sessionId.empty() ? d.sessionId.c_str() : d.caaId.c_str(), d.lat, d.lon, d.height, d.speed, d.opLat, d.opLon, d.operatorId.c_str());
            if (t.standard == 58)   // 5G NR cells: the cell table (broadcast information only)
                for (const auto& c : t.nr.cells)
                    printf("  nr: %.2f MHz GSCN %d n%d PCI %4d %2d kHz %s SSB %d/%zu SFN %4d kSSB %2d CORESET0 %2d SS0 %2d %s RSRP %6.1f RSRQ %5.1f SINR %5.1f\n", c.freqHz / 1e6, c.gscn, c.band, c.pci,
                           c.scsKhz, c.hasMib ? "MIB" : "sync", c.ssbIdx, c.beams.size(), c.sfn, c.kSsb, c.coreset0, c.ss0, c.barred ? "barred" : "-", c.rsrpDb, c.rsrqDb, c.sinrDb);
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
    if (!record.empty()) {
        const RecordingStats rs = e.recordingStats();
        e.stopRecording();
        printf("recorded %.1f s (%.1f MB) to %s%s\n", rs.seconds, rs.bytes / 1e6, rs.path.c_str(), rs.droppedSamples ? ": SOME SAMPLES WERE DROPPED, the disk was too slow" : "");
        if (!rs.error.empty()) printf("recording stopped early: %s\n", rs.error.c_str());
    }
    if (!iridiumBits.empty()) {
        e.iridium().stopBitsFile();
        printf("iridium bits: %llu lines in %s (iridium-parser.py %s > output.parsed)\n", (unsigned long long)e.iridium().bitsLines(), iridiumBits.c_str(), iridiumBits.c_str());
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
    if (t.standard == 47) {   // TETRA: the cells found, with what they broadcast to every terminal
        for (const auto& c : t.tetra.cells)
            printf("TETRA %.4f MHz  MCC %d MNC %d  colour code %d  LA %d  encryption %s  neighbours %zu  level %.0f dBFS  SNR %.0f dB\n", c.nominalHz / 1e6, c.mcc, c.mnc,
                   c.colourCode, c.la, c.encryption() ? "yes" : (c.hasSysinfo ? "no" : "?"), c.neighbours.size(), c.levelDb, c.snrDb);
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
        printf("\nBB frames %llu (lost %llu) packets %llu resyncs %llu unsupported %llu  mode %s ISSYI %d NPD %d SIS/MIS %d ISI %d\n", (unsigned long long)bb.frames, (unsigned long long)bb.framesLost, (unsigned long long)bb.packets, (unsigned long long)bb.resyncs, (unsigned long long)bb.unsupported, bb.hem ? "HEM" : "normal", bb.issyi, bb.npd, bb.sisMis, bb.isi);
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
    {   // where the CPU went (common/cpu_meter.h): seconds of CPU per account and the whole process
        std::string line = "cpu:";
        char b[96];
        for (const auto& u : e.cpuUsage()) { snprintf(b, sizeof b, " %s %.2f s", u.name.c_str(), u.seconds); line += b; }
        snprintf(b, sizeof b, " | process %.2f s", (double)cpu::processNs() * 1e-9);
        printf("%s\n", (line + b).c_str());
    }
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
