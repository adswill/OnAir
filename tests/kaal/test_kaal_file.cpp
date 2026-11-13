// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL through the app path: a two-channel 16-bit WAV (made by the transmit generator and the IQ-file sink, or the file named by KAAL_FILE) is played by
// the engine's file source, the engine runs KAAL (standard 124) and must lock, count good frames and decode the beacon. Prints what the panel would show.
#include "dect2/engine/engine.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/kaal/kaal_live.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/source/iq_file.h"
#include "dect2/tx/tx_iqfile.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::string makeFile() {
    const std::string path = "/tmp/test_kaal_file.wav";
    uint64_t pulled = 0;
    TxRequest r;
    r.modeId = "kaal";
    r.callsign = "LY2ABC";
    r.grid = "KO26ab";
    r.lat = 54.687; r.lon = 25.279;
    r.text = "Hi, I am testing KAAL.";
    r.opt["kaal.powerDbm"] = 20;
    r.opt["kaal.beacon"] = 1;
    r.pullAudio = [&](float* st, int n) {
        for (int i = 0; i < n; i++, pulled++) {
            const double t = std::fmod((double)pulled / 48000.0, 6.0);   // a 6 s sentence, repeated
            const float v = (float)(0.09 * std::sin(2 * 3.141592653589793 * (150 + 60 * std::sin(2 * 3.141592653589793 * 1.3 * t)) * t) * (0.6 + 0.4 * std::sin(2 * 3.141592653589793 * 3 * t)));
            st[2 * i] = st[2 * i + 1] = v;
        }
        return n;
    };
    std::string err;
    auto gen = makeKaalTxGenerator(r, 48000, err);
    if (!gen) return std::string();
    std::vector<cf32> x(2327040);   // 48.48 s: whole 4-frame blocks (3840 samples) and station-ID sweep cycles (581760 samples), 1163520 together and sweep periods, so the file loops without a jump (as the sample files)
    for (size_t i = 0; i < x.size(); i += 4800) gen->generate(x.data() + i, std::min<size_t>(4800, x.size() - i));   // 2327040 is not a multiple of 4800: a full last chunk wrote 960 samples past the vector
    float pk = 0;
    for (const cf32& v : x) pk = std::max(pk, std::max(std::fabs(v.real()), std::fabs(v.imag())));
    for (cf32& v : x) v *= 0.8f / pk;   // as the sample files: peak at 0.8 of full scale
    IqFileTxConfig c;
    c.path = path;
    c.format = IqFileFormat::WavIq16;
    auto sink = makeIqFileTxSink(c);
    TxTune t;
    t.sampleRate = 48000;
    if (!sink->open(t, err)) return std::string();
    sink->write(x.data(), x.size(), err);
    sink->close();
    return path;
}

int main() {
    const char* env = getenv("KAAL_FILE");
    const std::string path = env ? env : makeFile();
    CHECK(!path.empty(), "no file");
    IqFileInfo info;
    std::string err;
    CHECK(probeIqFile(path, info, err), "probe: %s", err.c_str());
    printf("file %s: rate %.0f, format %d\n", path.c_str(), info.sampleRate, (int)info.format);
    Engine e;
    DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = "kaal file";
    TuneSettings tune; tune.sampleRate = getenv("KAAL_TUNE_RATE") ? atof(getenv("KAAL_TUNE_RATE")) : info.sampleRate; tune.centerHz = 433e6;
    const ModeTuning* mt = modeTuning(124);
    CHECK(mt != nullptr, "KAAL tuning");
    tune.bandwidthMhz = mt ? mt->bandwidthMhz : 0.012;
    FileOptions fo; fo.path = info.dataPath; fo.format = info.format; fo.sampleRate = info.sampleRate; fo.loop = true;
    fo.offsetBytes = info.offsetBytes; fo.endBytes = info.endBytes;
    e.setStandard(124);
    CHECK(e.start(dev, tune, fo), "engine start");
    RxTelemetry rx;
    uint64_t seq = 0;
    bool locked = false, beacon = false;
    double tLock = -1;
    const auto w0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - w0 < std::chrono::seconds(80)) {
        if (e.latestRx(rx, seq)) {
            seq = rx.seq;
            if (getenv("KAAL_TRACE")) printf("t %.2f state %d ok %llu bad %llu snr %.1f cfo %.0f\n", rx.kaal.timeSec, rx.kaal.state, (unsigned long long)rx.kaal.blocksOk, (unsigned long long)rx.kaal.blocksBad, (double)rx.kaal.snrDb, rx.kaal.cfoHz);
            if (rx.kaal.state == 2 && !locked) { locked = true; tLock = rx.kaal.timeSec; }
            if (rx.kaal.beaconValid) beacon = true;
            if (rx.kaal.timeSec > (getenv("KAAL_SECS") ? atof(getenv("KAAL_SECS")) : 26)) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    e.latestRx(rx, 0);
    const KaalTelemetry& k = rx.kaal;
    printf("standard %d, state %d, input rate %.0f, t %.1f s, locked first at %.2f s, frames ok %llu bad %llu, snr %.1f dB, sweep %.1f dB, cfo %.0f Hz\n", rx.standard, k.state,
           k.inputRate, k.timeSec, tLock, (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad, (double)k.snrDb, (double)k.sweepDb, k.cfoHz);
    printf("beacon: %s %s \"%s\" (%llu frames)\n", k.callsign.c_str(), k.grid.c_str(), k.text.c_str(), (unsigned long long)k.beacons);
    CHECK(rx.standard == 123, "report standard %d", rx.standard);
    CHECK(locked, "never locked");
    CHECK(k.blocksOk > 100 && k.blocksOk > 5 * k.blocksBad, "frames ok %llu bad %llu", (unsigned long long)k.blocksOk, (unsigned long long)k.blocksBad);
    CHECK(beacon, "no beacon decoded");
    CHECK(env || k.text == "Hi, I am testing KAAL.", "the bio is \"%s\" (a header-only frame must not wipe it)", k.text.c_str());
    e.stop();
    // the file played twice in a row, as a looping player does (fast, no engine): the wrap must not cost the lock
    {
        std::vector<cf32> all;
        {
            FILE* f = fopen(path.c_str(), "rb");
            CHECK(f != nullptr, "reopen");
            if (f) {
                fseek(f, (long)info.offsetBytes, SEEK_SET);
                std::vector<int16_t> raw((size_t)(info.endBytes ? info.endBytes - info.offsetBytes : 1 << 30) / 2);
                const size_t got = fread(raw.data(), 2, raw.size(), f);
                for (size_t i = 0; i + 1 < got; i += 2) all.emplace_back((float)raw[i] / 32768.f, (float)raw[i + 1] / 32768.f);
                fclose(f);
            }
        }
        KaalLiveReceiver lr;
        lr.setSilent(true);
        lr.configure(48000);
        KaalTelemetry t;
        uint64_t sq = 0;
        int lostAfterLock = 0, wasLocked = 0, states = 0;
        double wrapAt = (double)all.size() / 48000.0;
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < all.size(); i += 960) {
                lr.feed(all.data() + i, std::min<size_t>(960, all.size() - i));
                if (lr.telemetry(t, sq)) {
                    sq = t.seq; states++;
                    if (t.state == 2) wasLocked = 1;
                    else if (wasLocked && t.state != 2 && t.timeSec > 20) { lostAfterLock++; }
                }
            }
        printf("two passes of %.2f s: %d reports, lock lost %d times after the first lock (wrap at %.1f s), frames ok %llu bad %llu\n", wrapAt, states, lostAfterLock, wrapAt, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(wasLocked, "the direct feed never locked");
        CHECK(lostAfterLock == 0, "the lock was lost %d times (the file wrap)", lostAfterLock);
    }
    return testSummary("kaal_file");
}
