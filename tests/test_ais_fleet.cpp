// AIS: generator to receiver round trip with the simulated port (15 vessels, base station, aids, aircraft): every burst that was sent comes back
// bit for bit, on the right channel, nothing else comes out, and the station table matches what was sent.
#include "dect2/ais_testutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::ais;
using namespace dect2::aistest;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string key(const Bits& b, char ch) {
    std::string s(1, ch);
    for (uint8_t v : b) s.push_back((char)('0' + v));
    return s;
}

struct Result { size_t sent = 0, got = 0, missing = 0, extra = 0; AisTelemetry t; };

static Result run(const SynthConfig& cfg, double rate, double secs, size_t chunk) {
    auto synth = makeAisSynth(cfg, rate);
    aisSynthLog(*synth, true);
    AisReceiver rx;
    rx.configure(rate);
    Collector col;
    AisTelemetry t, last;
    uint64_t seq = 0;
    std::vector<cf32> buf(chunk);
    const size_t total = (size_t)(secs * rate);
    for (size_t done = 0; done < total; done += chunk) {
        const size_t n = std::min(chunk, total - done);
        synth->generate(buf.data(), n);
        rx.feed(buf.data(), n);
        while (rx.telemetry(t, seq)) { seq = t.seq; last = t; col.update(t); }
    }
    Result r;
    r.t = last;
    std::multiset<std::string> sent, got;
    for (const auto& b : aisSynthSent(*synth))
        if (b.startSec + 0.3 < secs - 0.6) sent.insert(key(b.payload, b.channel));    // sent early enough to have been reported
    for (const auto& g : col.got) got.insert(key(g.bits, g.channel));
    r.sent = sent.size();
    r.got = got.size();
    for (const auto& b : aisSynthSent(*synth))
        if (b.startSec + 0.3 < secs - 0.6 && got.count(key(b.payload, b.channel)) < sent.count(key(b.payload, b.channel))) {
            r.missing++;
            printf("  not returned: %.3f s, channel %c, type %u, mmsi %u, %zu bits\n", b.startSec, b.channel, getU(b.payload, 0, 6), getU(b.payload, 8, 30), b.payload.size());
        }
    // everything that was decoded must have been sent (the ones in the last 0.6 s are not in 'sent' but are allowed)
    std::multiset<std::string> allSent;
    for (const auto& b : aisSynthSent(*synth)) allSent.insert(key(b.payload, b.channel));
    for (const auto& g : got) if (!allSent.count(g)) r.extra++;
    return r;
}

int main() {
    SynthConfig cfg;
    cfg.snrDb = 34;
    {
        const Result r = run(cfg, 2e6, 60, 65536);
        printf("default port, 60 s at 2 MS/s: %zu bursts sent, %zu decoded, %zu not returned, %zu not sent, %zu stations, bad %llu\n", r.sent, r.got, r.missing, r.extra,
               (size_t)r.t.vesselCount, (unsigned long long)r.t.blocksBad);
        CHECK(r.sent > 100, "enough traffic (%zu)", r.sent);
        CHECK(r.missing == 0, "%zu sent bursts did not come back", r.missing);
        CHECK(r.extra == 0, "%zu decoded messages that were never sent", r.extra);
        CHECK(r.t.vesselCount == 15 + 1 + 2 + 1, "stations: %u", r.t.vesselCount);
        CHECK(r.t.blocksBad == 0, "no bad bursts at 30 dB (%llu)", (unsigned long long)r.t.blocksBad);
        CHECK(r.t.channelOk[0] > 20 && r.t.channelOk[1] > 20, "both channels used: %llu / %llu", (unsigned long long)r.t.channelOk[0], (unsigned long long)r.t.channelOk[1]);
        // the table: every class A vessel with its name and position, an aid, the base station
        int named = 0, positioned = 0;
        bool base = false, aton = false, sar = false, classB = false;
        for (const AisVessel& v : r.t.vessels) {
            if (!v.name.empty()) named++;
            if (v.hasPos && std::fabs(v.lat - 25.1) < 0.6 && std::fabs(v.lon - 55.1) < 0.6) positioned++;
            if (v.cls == AIS_CLASS_BASE) base = true;
            if (v.cls == AIS_CLASS_ATON && v.name.size()) aton = true;
            if (v.cls == AIS_CLASS_SAR) sar = true;
            if (v.cls == AIS_CLASS_B) classB = true;
        }
        CHECK(named >= 16, "names known for %d stations", named);
        CHECK(positioned >= 16, "positions known for %d stations", positioned);
        CHECK(base && aton && sar && classB, "base %d, aid %d, aircraft %d, class B %d", base, aton, sar, classB);
        // message types sent by the port
        const int want[] = {1, 3, 4, 5, 8, 9, 14, 18, 19, 21, 24};
        for (int k : want) {
            const uint64_t n = r.t.typeCount[0][k] + r.t.typeCount[1][k];
            CHECK(n > 0, "message type %d seen", k);
        }
        CHECK(r.t.nmea.size() == 50, "50 sentences kept (%zu)", r.t.nmea.size());
    }
    {   // another seed, other numbers of vessels, equal levels; a small chunk size
        cfg.modeOpt[0] = 40; cfg.modeOpt[1] = 5; cfg.modeOpt[2] = 1; cfg.snrDb = 25;
        const Result r = run(cfg, 2.4e6, 40, 4096);
        printf("40 vessels, 40 s at 2.4 MS/s: %zu sent, %zu decoded, %zu not returned, %zu not sent, %zu stations\n", r.sent, r.got, r.missing, r.extra, (size_t)r.t.vesselCount);
        CHECK(r.missing == 0 && r.extra == 0, "40 vessels: %zu missing, %zu extra", r.missing, r.extra);
        CHECK(r.t.vesselCount == 44, "stations: %u", r.t.vesselCount);
    }
    {   // one vessel, nothing else
        cfg.modeOpt[0] = 1; cfg.modeOpt[3] = 1; cfg.modeOpt[1] = 2;
        const Result r = run(cfg, 2e6, 30, 65536);
        printf("1 vessel, 30 s: %zu sent, %zu decoded\n", r.sent, r.got);
        CHECK(r.missing == 0 && r.extra == 0 && r.t.vesselCount == 1, "1 vessel: %zu missing, %zu extra, %u stations", r.missing, r.extra, r.t.vesselCount);
    }
    if (fails) { printf("%d checks failed\n", fails); return 1; }
    printf("ais_fleet: ok\n");
    return 0;
}
