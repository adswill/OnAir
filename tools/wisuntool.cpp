// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN tool: decode a recording, or write the test signal.
//   wisuntool rx <file> [--rate Hz] [--center Hz] [--region ANZ|NA|EU863|JP|...] [--secs s]
//       a SigMF recording (.sigmf-meta / .sigmf-data: rate and centre from the meta), or .cf32 / .cs16 / .cs8 with --rate and --center;
//       prints every frame, then the networks, nodes and links
//   wisuntool gen <out.cf32> [--rate Hz] [--secs s] [--snr dB]
//       the test schedule (wisun_gen.h) around 921.5 MHz
//   wisuntool live <log.txt> [--secs s] [--center Hz] [--rate Hz] [--gain dB] [--region ANZ|NA|EU863|JP|...]
//       the first radio found, for hours if need be: every frame (UTC time, MPDU hex) and a summary line a minute into the log
//       (appended), the tables at the end
#include "dect2/source/iq_file.h"
#include "dect2/source/source.h"
#include "dect2/wisun/wisun_gen.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_rx.h"
#include "dect2/wisun/wisun_tel.h"
#include <chrono>
#include <ctime>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "dect2/common/timecompat.h"
using namespace dect2;

static std::string utc(int64_t t) { return dect2::utcIsoText(t); }

// The frames decoded since the last call (the log is newest first: the new entries, oldest first), with the UTC time and the MPDU hex when asked
static void printNew(FILE* o, const WisunTelemetry& t, size_t& printed, bool wall = false, bool hex = getenv("WISUN_HEX") != nullptr) {
    size_t fresh = (size_t)(t.blocksOk - printed);
    if (fresh > t.log.size()) fresh = t.log.size();
    for (size_t i = fresh; i-- > 0;) {
        const WsFrame& f = t.log[i];
        if (wall) fprintf(o, "%s ", utc(f.wallTime).c_str());
        fprintf(o, "%9.4f %8.3f MHz ch %2d %3d kbit/s %6.1f dBFS %5.1f dB  %-23s > %-23s %4d  %s  [%s]\n", f.sec, f.mhz, f.channel, f.bitRate / 1000, f.rssi, f.snrDb,
                f.src.c_str(), f.dst.c_str(), f.len, f.info.c_str(), f.ies.c_str());
        if (hex) fprintf(o, "          %s\n", f.hex.c_str());
    }
    printed = (size_t)t.blocksOk;
}

static void printTables(FILE* o, const WisunTelemetry& t) {
    fprintf(o, "%llu mode switches:", (unsigned long long)t.modeSwitches);
    for (const auto& m : t.switchModes) fprintf(o, " 0x%02X x%u", m.first, m.second);
    fprintf(o, "\n");
    for (const WsPan& p : t.pans)
        fprintf(o, "PAN 0x%04X \"%s\" size %d, version %d, border router %s, %d nodes, %llu frames; %s\n", p.pan, p.netName.c_str(), p.panSize, p.panVersion,
                p.borderRouter.empty() ? "-" : p.borderRouter.c_str(), p.nodes, (unsigned long long)p.packets, p.unicast.c_str());
    for (const WsNode& n : t.nodes)
        fprintf(o, "node %s %-14s %-24s PAN %5d  %4llu frames  %6.1f dBFS  %6.3f MHz  %s\n", n.eui.c_str(), n.role.c_str(), n.vendor.c_str(), n.pan,
                (unsigned long long)n.packets, n.rssi, n.lastMhz, n.last.c_str());
    for (const WsLink& l : t.links)
        fprintf(o, "link %s > %s  %llu frames, %llu acks, RSL %d dBm\n", l.src.c_str(), l.dst.c_str(), (unsigned long long)l.frames, (unsigned long long)l.acks, l.rslDbm);
}

static void summaryLine(FILE* o, const WisunTelemetry& t) {
    fprintf(o, "SUMMARY %s %.0f s: %s; types", utc((int64_t)time(nullptr)).c_str(), t.timeSec, wisunSummary(t).c_str());
    for (int k = 0; k < kWsUttTypes; k++) if (t.perUtt[k]) fprintf(o, " %s %llu", wisun::uttShort(k), (unsigned long long)t.perUtt[k]);
    fprintf(o, ", plain %llu, corrected %llu, bad %llu, mode switches %llu, dropped %llu, level %.1f dBFS, load %.0f%%\n", (unsigned long long)t.plain,
            (unsigned long long)t.corrected, (unsigned long long)t.blocksBad, (unsigned long long)t.modeSwitches, (unsigned long long)t.dropped, t.levelDb, t.loadPct);
}

static int live(const std::string& path, double secs, double center, double rate, double gain, const std::string& region) {
    std::string err;
    const auto radios = listRadios(err);
    if (radios.empty()) { fprintf(stderr, "no radio found %s\n", err.c_str()); return 1; }
    const DeviceInfo& dev = radios[0];
    FILE* o = fopen(path.c_str(), "a");
    if (!o) { perror(path.c_str()); return 1; }
    TuneSettings ts;
    ts.centerHz = center; ts.sampleRate = rate; ts.gainDb = gain; ts.bandwidthMhz = rate / 1e6;
    auto src = makeSource(dev);
    IqRing ring(1u << 25);
    if (!src->start(ts, ring, err)) { fprintf(stderr, "start: %s\n", err.c_str()); fclose(o); return 1; }
    const double r = src->sampleRate();
    fprintf(o, "START %s %s, %.3f MHz, %.2f Msps, gain %.0f dB\n", utc((int64_t)time(nullptr)).c_str(), dev.name.c_str(), center / 1e6, r / 1e6, gain);
    fflush(o);
    WisunReceiver rx;
    rx.setCenterHz(center);
    if (!region.empty()) rx.setRegion(region);
    rx.configure(r);
    std::vector<cf32> buf(1 << 18);
    WisunTelemetry t;
    uint64_t seq = 0;
    size_t printed = 0;
    uint64_t total = 0;
    const uint64_t limit = secs > 0 ? (uint64_t)(secs * r) : UINT64_MAX;
    auto nextSummary = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (total < limit) {
        const size_t n = ring.read(buf.data(), (size_t)std::min<uint64_t>(buf.size(), limit - total));
        if (!n) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        rx.feed(buf.data(), n);
        total += n;
        if (rx.telemetry(t, seq)) {
            seq = t.seq;
            printNew(o, t, printed, true, true);
            if (std::chrono::steady_clock::now() >= nextSummary) {
                summaryLine(o, t);
                fprintf(stderr, "%s\n", wisunSummary(t).c_str());
                fflush(o);
                nextSummary += std::chrono::seconds(60);
            }
        }
    }
    src->stop();
    rx.flush();
    if (rx.telemetry(t, seq)) printNew(o, t, printed, true, true);
    summaryLine(o, t);
    fprintf(o, "radio dropped %llu samples\n", (unsigned long long)ring.dropped());
    printTables(o, t);
    fclose(o);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: wisuntool rx <file> [--rate Hz] [--center Hz] [--region ANZ|NA|EU863|JP|...] [--secs s]\n       wisuntool gen <out.cf32> [--rate Hz] [--secs s] [--snr dB]\n"
                        "       wisuntool live <log.txt> [--secs s] [--center Hz] [--rate Hz] [--gain dB] [--region ANZ|NA|EU863|JP|...]\n");
        return 2;
    }
    const std::string cmd = argv[1], path = argv[2];
    double rate = 0, center = 0, secs = 0, snr = 30, gain = 40;
    std::string region;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--rate") rate = atof(argv[i + 1]);
        else if (k == "--center") center = atof(argv[i + 1]);
        else if (k == "--region") region = argv[i + 1];
        else if (k == "--secs") secs = atof(argv[i + 1]);
        else if (k == "--snr") snr = atof(argv[i + 1]);
        else if (k == "--gain") gain = atof(argv[i + 1]);
    }
    if (cmd == "live") return live(path, secs, center > 0 ? center : wisunTuning().defMhz * 1e6, rate > 0 ? rate : wisunTuning().sampleRate, gain, region);
    if (cmd == "gen") {
        if (rate <= 0) rate = 20e6;
        if (secs <= 0) secs = 1;
        WisunGenConfig c;
        c.rate = rate; c.snrDb = snr; c.centerMhz = wisunTuning().defMhz;
        c.frames = wisunTestSchedule(c.centerMhz, std::min(6.0, 0.4 * rate / 1e6));
        WisunGenerator g(c);
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { perror(path.c_str()); return 1; }
        std::vector<cf32> buf(1 << 16);
        for (size_t left = (size_t)(secs * rate); left > 0;) {
            const size_t n = std::min(left, buf.size());
            g.generate(buf.data(), n);
            fwrite(buf.data(), sizeof(cf32), n, f);
            left -= n;
        }
        fclose(f);
        return 0;
    }
    if (cmd != "rx") { fprintf(stderr, "unknown command %s\n", cmd.c_str()); return 2; }
    std::string data = path;
    FileFormat fmt = FileFormat::CF32;
    uint64_t offset = 0;
    bool bigEndian = false;
    IqFileInfo info;
    std::string err;
    if (probeIqFile(path, info, err)) {
        data = info.dataPath; fmt = info.format; offset = info.offsetBytes; bigEndian = info.bigEndian;
        if (rate <= 0) rate = info.sampleRate;
        if (center <= 0) center = info.centerHz;
    } else if (!err.empty()) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    else {
        const std::string ext = path.substr(path.find_last_of('.') + 1);
        fmt = ext == "cs16" ? FileFormat::CI16 : ext == "cs8" ? FileFormat::CS8 : FileFormat::CF32;
    }
    if (rate <= 0 || center <= 0) { fprintf(stderr, "the sample rate and the centre frequency are needed (--rate, --center)\n"); return 2; }
    // the sample formats read here: 32-bit float, 16-bit and 8-bit signed, 8-bit unsigned pairs, little endian
    if ((fmt != FileFormat::CF32 && fmt != FileFormat::CI16 && fmt != FileFormat::CS8 && fmt != FileFormat::CU8) || bigEndian) {
        fprintf(stderr, "%s: sample format not supported (cf32, ci16, cs8 and cu8, little endian)\n", data.c_str());
        return 2;
    }
    FILE* f = fopen(data.c_str(), "rb");
    if (!f) { perror(data.c_str()); return 1; }
    fseek(f, (long)offset, SEEK_SET);
    WisunReceiver rx;
    rx.setCenterHz(center);
    if (!region.empty()) rx.setRegion(region);
    rx.configure(rate);
    rx.setOffline(true);
    const size_t chunk = 1 << 16;
    std::vector<cf32> buf(chunk);
    std::vector<int16_t> s16(fmt == FileFormat::CI16 ? 2 * chunk : 0);
    std::vector<int8_t> s8(fmt == FileFormat::CS8 ? 2 * chunk : 0);
    std::vector<uint8_t> u8(fmt == FileFormat::CU8 ? 2 * chunk : 0);
    WisunTelemetry t;
    uint64_t seq = 0;
    size_t printed = 0, total = 0;
    const size_t limit = secs > 0 ? (size_t)(secs * rate) : (size_t)-1;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const size_t want = std::min(chunk, limit - total);
        if (want == 0) break;
        // each format straight into a buffer of its own type: want pairs at most, which every buffer holds
        size_t n = 0;
        if (fmt == FileFormat::CF32) n = fread(buf.data(), sizeof(cf32), want, f);
        else if (fmt == FileFormat::CI16) {
            n = fread(s16.data(), 2 * sizeof(int16_t), want, f);
            for (size_t i = 0; i < n; i++) buf[i] = cf32(s16[2 * i] / 32768.f, s16[2 * i + 1] / 32768.f);
        } else if (fmt == FileFormat::CS8) {
            n = fread(s8.data(), 2 * sizeof(int8_t), want, f);
            for (size_t i = 0; i < n; i++) buf[i] = cf32(s8[2 * i] / 128.f, s8[2 * i + 1] / 128.f);
        } else {
            n = fread(u8.data(), 2 * sizeof(uint8_t), want, f);
            for (size_t i = 0; i < n; i++) buf[i] = cf32((u8[2 * i] - 127.5f) / 128.f, (u8[2 * i + 1] - 127.5f) / 128.f);
        }
        if (n == 0) break;
        rx.feed(buf.data(), n);
        total += n;
        if (rx.telemetry(t, seq)) { seq = t.seq; printNew(stdout, t, printed); }
    }
    fclose(f);
    const std::vector<cf32> tail((size_t)(0.3 * rate), cf32(0, 0));   // flush the last bursts and the report
    rx.feed(tail.data(), tail.size());
    rx.flush();
    if (rx.telemetry(t, seq)) printNew(stdout, t, printed);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("\n%.1f s of signal in %.1f s: %s; %llu corrected, %llu bad FCS, %llu FEC frames, %llu bursts, %llu dropped; region %s, %.1f - %.1f MHz, %d bins of %.1f kHz\n",
           total / rate, wall, wisunSummary(t).c_str(), (unsigned long long)t.corrected, (unsigned long long)t.blocksBad, (unsigned long long)t.fecFrames, (unsigned long long)t.bursts, (unsigned long long)t.dropped,
           t.region.c_str(), t.loMhz, t.hiMhz, t.bins, t.binKhz);
    printTables(stdout, t);
    return 0;
}
