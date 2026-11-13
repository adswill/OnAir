// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time stations BPC, HLA and the coded BPM signal (BPMc): the frames and their parities, the generator through the receiver started part way
// into a minute, a sample clock that runs slow, noise alone gives nothing. With DECT2_TIME_REC_DIR set to a folder holding BPC.wav, HLA.wav and
// BPM.wav (mono recordings of the stations' audio, read only, never part of the repository) they are decoded too.
#include "dect2/dsp/gen_util.h"
#include "dect2/time/time_gen.h"
#include "dect2/time/time_proto.h"
#include "dect2/time/time_rx.h"
#include "dect2/time/time_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::timesig;
#include "../common/test_util.h"

static const double kLat = 54.687, kLon = 25.280;   // Vilnius

static void protoTests() {
    const int64_t M = daysFromCivil(2026, 10, 13) * 86400 + 12 * 3600 + 25 * 60;   // 12:25 UTC = 20:25 in China, 21:25 in Korea
    Fields f;
    // BPC: three frames, each its own 20 symbols
    const std::vector<uint8_t> b = encodeMinute(Bpc, M, {});
    CHECK(b.size() == 60 && b[0] == SNone && b[20] == SNone && b[40] == SNone, "BPC frame starts");
    for (int i = 0; i < 3; i++) {
        CHECK(decodeMinute(Bpc, &b[(size_t)(20 * i)], f, 2026), "BPC frame %d", i);
        CHECK(f.frameIndex == i && f.utcMinute == M + 20 * i && f.hour == 20 && f.minute == 25 && f.day == 13 && f.month == 10 && f.year == 2026 &&
              f.weekday == 2 && f.utcOffsetMin == 480, "BPC frame %d fields", i);
    }
    {   // the frame of the real recording: 20:25 PM, Tuesday 13 October 2026 (frame 2)
        const uint8_t pm0[20] = {SNone, 2, 0, 2, 0, 1, 2, 1, 0, 2, 2, 0, 3, 1, 2, 2, 1, 2, 2, 0};
        CHECK(decodeMinute(Bpc, pm0, f, 2026) && f.hour == 20 && f.minute == 25 && f.frameIndex == 2 && f.second == 40, "BPC recorded frame");
        uint8_t bad[20];
        memcpy(bad, pm0, 20);
        bad[5] ^= 1;
        CHECK(!decodeMinute(Bpc, bad, f, 2026), "BPC parity");
        memcpy(bad, pm0, 20);
        bad[1] = 3;
        CHECK(!decodeMinute(Bpc, bad, f, 2026), "BPC frame index 3");
    }
    {   // 12 AM / 12 PM: hour 0 and 12 are 0 with the AM / PM bit
        for (int h : {0, 11, 12, 23}) {
            const int64_t t = daysFromCivil(2026, 3, 1) * 86400 + h * 3600 - 8 * 3600 + 20 * 60 + 20;
            const std::vector<uint8_t> fr = encodeBpcFrame(t);
            CHECK(decodeMinute(Bpc, fr.data(), f, 2026) && f.hour == h && f.utcMinute == t && f.frameIndex == 1, "BPC hour %d", h);
        }
    }
    // HLA
    {
        const int64_t m = daysFromCivil(2026, 10, 13) * 86400 + 7 * 3600 + 22 * 60;   // 16:22 in Korea
        const std::vector<uint8_t> s = encodeMinute(Hla, m, {});
        CHECK(decodeMinute(Hla, s.data(), f, 2026) && f.doy == 286 && f.hour == 16 && f.minute == 22 && f.utcMinute == m && f.utcOffsetMin == 540, "HLA 16:22 doy 286");
        CHECK(fieldsText(f, Hla).find("KST") != std::string::npos, "HLA text");
        std::vector<uint8_t> bad = s;
        bad[3] = S1;   // a bit in an unused second
        CHECK(!decodeMinute(Hla, bad.data(), f, 2026), "HLA unused second");
        bad = s; bad[19] = S0;
        CHECK(!decodeMinute(Hla, bad.data(), f, 2026), "HLA marker");
        // doy 346 at 00:42 (the recording)
        const int64_t m2 = (daysFromCivil(2026, 1, 1) + 345) * 86400 - 9 * 3600 + 42 * 60;
        CHECK(decodeMinute(Hla, encodeMinute(Hla, m2, {}).data(), f, 2026) && f.doy == 346 && f.hour == 0 && f.minute == 42 && f.month == 12 && f.day == 12, "HLA doy 346");
    }
    // BPM coded: the worked example of the spec (07:38:01 UTC, DUT1 -35 ms, 0.1 ms digit 7, 37 leap seconds)
    {
        const int64_t u = daysFromCivil(2026, 10, 13) * 86400 + 7 * 3600 + 38 * 60 + 1;
        const std::vector<uint8_t> e = encodeBpmcSecond(u, -35.7);
        const char* ex = "100000 0 011001 0 11100 0 10110 0 0101 1 010110 0 1100010000 0 0";   // after S and the empty element
        std::string bits;
        for (size_t i = 2; i < e.size(); i++) bits += (char)('0' + e[i]);
        std::string want;
        for (const char* p = ex; *p; p++) if (*p != ' ') want += *p;
        CHECK(bits == want, "BPMc second frame: %s vs %s", bits.c_str(), want.c_str());
        CHECK(e[0] == 1 && e[1] == 0 && decodeBpmcSecond(e.data(), f, 2026) && f.utcMinute + f.second == u && f.dut1Ms == -35 && f.dut1Neg && f.second == 1, "BPMc second decode");
        std::vector<uint8_t> bad = e;
        bad[10] ^= 1;
        CHECK(!decodeBpmcSecond(bad.data(), f, 2026), "BPMc parity");
        int tenth, leap;
        bool ls;
        const std::vector<uint8_t> mf = encodeBpmcMinute(7, 37, false);
        std::string mb;
        for (size_t i = 32; i < mf.size(); i++) mb += (char)('0' + mf[i]);
        CHECK(mb == "11100" "1010010" "01", "BPMc minute frame bits %s", mb.c_str());
        CHECK(decodeBpmcMinute(mf.data(), tenth, leap, ls) && tenth == 7 && leap == 37 && !ls, "BPMc minute decode");
        std::vector<uint8_t> badm = mf;
        badm[37] ^= 1;
        CHECK(!decodeBpmcMinute(badm.data(), tenth, leap, ls), "BPMc minute parity");
    }
    CHECK(stationForKhz(68.5, Msf) == Bpc && stationIsHf(Hla) && stationIsHf(BpmC) && !stationIsHf(Bpc), "station tables");
    CHECK(std::string(stationName(Bpc)) == "BPC" && std::string(stationName(Hla)) == "HLA" && stationKhz(Bpc) == 68.5, "names");
}

struct Run { TimeTelemetry first, last; double firstAt = -1; };

static Run run(TimeGenConfig c, double secs, double scale = 1.0, bool noiseOnly = false) {
    c.dialOffsetHz = -timeTuning().tuneOffsetHz;
    double la, lo;
    stationSite(c.station, la, lo, false);
    c.delayMs = propagationMs(c.station, distanceKm(kLat, kLon, la, lo));
    TimeGenerator g(c);
    TimeReceiver r;
    r.configure(c.rate);
    r.setSignalOffset(-timeTuning().tuneOffsetHz);
    r.setStation(c.station);
    r.setClockMode(2, c.startUtc);
    r.setClockScale(scale);
    r.setReceiverLocation(kLat, kLon, true);
    genutil::NoiseSource ns(7);
    std::vector<cf32> buf(8192);
    Run out;
    const size_t total = (size_t)(secs * c.rate);
    uint64_t seq = 0;
    for (size_t done = 0; done < total; done += buf.size()) {
        if (noiseOnly) { for (auto& v : buf) v = 0; ns.add(buf.data(), buf.size(), 0.05f); } else g.generate(buf.data(), buf.size());
        r.feed(buf.data(), buf.size());
        TimeTelemetry t;
        if (r.telemetry(t, seq)) {
            seq = t.seq;
            if (t.timeValid && out.firstAt < 0) { out.firstAt = (double)(done + buf.size()) / c.rate; out.first = t; }
            out.last = std::move(t);
        }
    }
    return out;
}

static void station(int st, double rate, double snr, double cfo, double ppm, const char* what) {
    TimeGenConfig c;
    c.station = st; c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.clockPpm = ppm;
    c.startUtc = (double)(daysFromCivil(2026, 10, 12) * 86400 + 14 * 3600 + 2 * 60 + 37);   // 2026-10-12 14:02:37 UTC
    const double secs = st == Hla ? 175 : st == Bpc ? 100 : 40;
    const Run R = run(c, secs);
    const TimeTelemetry& t = R.last;
    printf("%-10s %-22s first time after %5.1f s, %s, ok %llu bad %llu, tick %.2f ms rms, offset avg %+.2f ms, SNR %.1f dB, depth %.2f, delay %.2f ms\n",
           stationName(st), what, R.firstAt, timeSummary(t).c_str(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.tickJitterMs,
           t.clockOffsetAvgMs, t.snrDb, t.depth, t.propDelayMs);
    CHECK(t.timeValid, "%s %s: no time", stationName(st), what);
    if (!t.timeValid) return;
    const double expect = c.startUtc + secs - c.delayMs * 1e-3;
    CHECK(std::fabs(t.utcNow - expect) < 1.5, "%s %s: time %.1f s off", stationName(st), what, t.utcNow - expect);
    CHECK(t.blocksBad == 0, "%s %s: %llu rejected", stationName(st), what, (unsigned long long)t.blocksBad);
    const double drift = ppm * 1e-6 * (secs - 30) * 1000;
    CHECK(std::fabs(t.clockOffsetAvgMs - drift) < (stationIsHf(st) ? 1.5 : 2.0), "%s %s: clock offset %.2f ms, drift %.2f", stationName(st), what, t.clockOffsetAvgMs, drift);
    const Fields& f = t.fields;
    switch (st) {
    case Bpc: CHECK(f.utcOffsetMin == 480 && f.day == 12 && f.month == 10 && f.hour == 22 && f.weekday == 1, "BPC fields"); break;
    case Hla: CHECK(f.utcOffsetMin == 540 && f.hour == 23 && f.doy == 285, "HLA fields"); break;
    case BpmC: CHECK(f.hasDut1Ms && std::fabs(f.dut1Ms - -35.7) < 0.01 && f.leapCount == 37 && f.doy == 285 && f.hour == 14, "BPMc fields: DUT1 %.1f leap %d", f.dut1Ms, f.leapCount); break;
    }
    CHECK(t.confirmed, "%s %s: not confirmed", stationName(st), what);
}

// ---- the real recordings
static bool loadWav(const std::string& path, std::vector<float>& x, double& rate) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char h[12];
    if (!f.read(h, 12) || memcmp(h, "RIFF", 4)) return false;
    int ch = 1, bits = 16;
    for (;;) {
        char id[4];
        uint32_t n;
        if (!f.read(id, 4) || !f.read((char*)&n, 4)) return false;
        if (!memcmp(id, "fmt ", 4)) {
            uint16_t v[8];
            f.read((char*)v, 16);
            ch = v[1]; bits = v[7];
            uint32_t sr; memcpy(&sr, (char*)v + 4, 4); rate = sr;
            f.seekg((long)n - 16, std::ios::cur);
        } else if (!memcmp(id, "data", 4)) {
            if (bits != 16 || ch != 1) return false;
            std::vector<int16_t> d(n / 2);
            f.read((char*)d.data(), (long)n);
            x.resize(d.size());
            for (size_t i = 0; i < d.size(); i++) x[i] = d[i] / 32768.f;
            return true;
        } else f.seekg((long)n + (n & 1), std::ios::cur);
    }
}

static void recording(const std::string& dir, const char* name, int st, double scale, double toneHz) {
    std::vector<float> x;
    double rate = 0;
    if (!loadWav(dir + "/" + name + ".wav", x, rate)) { printf("%s: not found, skipped\n", name); return; }
    TimeReceiver r;
    r.configure(rate);
    r.setStation(st);
    r.setAudioInput(true, toneHz);
    r.setClockScale(scale);
    r.setClockMode(0);
    std::vector<cf32> buf(4096);
    uint64_t seq = 0;
    TimeTelemetry last;
    for (size_t at = 0; at < x.size(); at += buf.size()) {
        const size_t n = std::min(buf.size(), x.size() - at);
        for (size_t i = 0; i < n; i++) buf[i] = cf32(x[at + i], 0);
        r.feed(buf.data(), n);
        TimeTelemetry t;
        if (r.telemetry(t, seq)) {
            seq = t.seq;
            last = std::move(t);
            if (getenv("DECT2_TIME_DEBUG") && (int)(last.timeSec * 4) % 20 == 0) printf("  t %.1f state %d phase %.1f depth %.2f snr %.1f valid %d\n", last.timeSec, last.state, last.secPhaseMs, last.depth, last.snrDb, last.timeValid);
        }
    }
    printf("%s (%s, scale %.4f, tone %.0f Hz): %s; state %d, ok %llu bad %llu, phase %.2f ms, depth %.2f, SNR %.1f dB\n", name, stationName(st), scale, toneHz,
           timeSummary(last).c_str(), last.state, (unsigned long long)last.blocksOk, (unsigned long long)last.blocksBad, last.secPhaseMs, last.depth, last.snrDb);
    if (last.timeValid) printf("  fields: %s, DUT1 %s%.1f ms, leap count %d\n", fieldsText(last.fields, st).c_str(), last.fields.hasDut1Ms ? "" : "(none) ", last.fields.dut1Ms, last.fields.leapCount);
    for (size_t k = last.log.size(); k-- > 0;) if (k + 8 > last.log.size() || !last.log[k].ok) printf("  %s %s  %s\n", last.log[k].ok ? "ok " : "bad", last.log[k].text.c_str(), last.log[k].bits.c_str());
    printf("  symbols now: %s\n", last.bitsNow.c_str());
}

int main() {
    protoTests();
    station(Bpc, 64000, 40, 0, 0, "clean");
    station(Bpc, 64000, 15, 3, 10, "15 dB, 3 Hz, 10 ppm");
    station(Hla, 64000, 40, 0, 0, "clean");
    station(Hla, 64000, 25, 15, 10, "25 dB, 15 Hz, 10 ppm");
    station(BpmC, 64000, 40, 0, 0, "clean");
    station(BpmC, 64000, 30, 10, 10, "30 dB, 10 Hz, 10 ppm");
    {   // a recorder 2.55 % slow: the signal is generated that way, the receiver told
        TimeGenConfig c;
        c.station = BpmC; c.rate = 64000; c.snrDb = 40; c.clockPpm = 25500; c.startUtc = (double)(daysFromCivil(2026, 10, 13) * 86400 + 7 * 3600 + 37 * 60 + 40);
        c.opts.dut1Ms = -35.7;
        const Run R = run(c, 40, 1.0255);
        printf("BPMc 2.55 %% slow: %s, ok %llu bad %llu\n", timeSummary(R.last).c_str(), (unsigned long long)R.last.blocksOk, (unsigned long long)R.last.blocksBad);
        CHECK(R.last.timeValid && R.last.fields.hour == 7 && R.last.fields.minute == 38 && std::fabs(R.last.fields.dut1Ms + 35.7) < 0.01, "BPMc slow clock");
    }
    for (int st : {Bpc, Hla, BpmC}) {
        TimeGenConfig c;
        c.station = st; c.rate = 64000; c.startUtc = 1.7e9;
        const Run R = run(c, 150, 1.0, true);
        printf("%-10s noise: %s, ok %llu bad %llu\n", stationName(st), timeSummary(R.last).c_str(), (unsigned long long)R.last.blocksOk, (unsigned long long)R.last.blocksBad);
        CHECK(R.last.blocksOk == 0 && !R.last.timeValid, "%s: a time from noise", stationName(st));
    }
    if (const char* d = getenv("DECT2_TIME_REC_DIR")) {
        recording(d, "BPC", Bpc, 1.0255, 500);
        recording(d, "HLA", Hla, 1.0, 0);
        recording(d, "BPM", BpmC, 1.0255, 0);
    }
    printf(fails ? "FAILED %d\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
