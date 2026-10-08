// Inmarsat Aero receiver on the test signal: the band search finds both default channels (10500 bit/s and 1200 bit/s), the rate is found
// on its own, and every frame the receiver hands on is bit-exact with one the generator sent.
#include "dect2/aero_gen.h"
#include "dect2/aero_rx.h"
#include "dect2/aero_demod.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = 2e6;
    SynthConfig cfg;
    cfg.mode = 20;
    AeroSynth syn(cfg, rate);
    syn.record(true);
    AeroReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
    std::vector<std::vector<uint8_t>> got;
    rx.setFrameCallback([&](double, const AeroFrameEvent& e) { got.push_back(e.bytes); });
    int logLines = 0;
    rx.setLogCallback([&](const std::string& s) { if (logLines++ < 12) printf("  log: %s\n", s.c_str()); });
    std::vector<cf32> buf(65536);
    AeroTelemetry t;
    uint64_t last = 0;
    int reports = 0;
    const double secs = 24;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        syn.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, last)) { CHECK(t.seq > last, "seq"); last = t.seq; reports++; }
    }
    printf("%s\n", aeroSummary(t).c_str());
    for (const auto& c : t.channels)
        printf("  channel %+9.1f Hz  %5d bit/s  state %d  Eb/N0 %.1f dB  level %.1f dB  frames %llu  UW misses %llu  SUs %llu/%llu  BER %.4f\n", c.offsetHz, c.bitRate, c.state,
               c.ebn0Db, c.levelDb, (unsigned long long)c.frames, (unsigned long long)c.uwMisses, (unsigned long long)c.susOk, (unsigned long long)c.susBad, c.channelBer);
    CHECK(reports >= secs * 4 - 2, "reports %d", reports);
    CHECK(t.state == 2, "state %d", t.state);
    const auto sent = syn.takeFrames();
    CHECK(t.channels.size() == 2, "channels %zu", t.channels.size());
    for (const auto& sc : syn.channels()) {
        bool found = false;
        for (const auto& c : t.channels) {
            if (c.bitRate == sc.bitRate && std::fabs(c.offsetHz - sc.offsetHz) < 4000 && c.state == 3) {
                found = true;
                CHECK(c.susBad == 0, "rate %d bad SUs %llu", sc.bitRate, (unsigned long long)c.susBad);
                CHECK(c.ebn0Db > 10 && c.ebn0Db < 14, "rate %d Eb/N0 %.1f", sc.bitRate, c.ebn0Db);
                const double minFrames = (secs - 8) * sc.bitRate / aeroFrameFormat(sc.bitRate)->totalBits();
                CHECK(c.frames >= minFrames, "rate %d frames %llu, want %.0f", sc.bitRate, (unsigned long long)c.frames, minFrames);
            }
        }
        CHECK(found, "channel %d bit/s at %+.0f Hz not decoded", sc.bitRate, sc.offsetHz);
    }
    {
        std::set<std::vector<uint8_t>> sentSet;
        for (const auto& f : sent) sentSet.insert(f.bytes);
        size_t exact = 0;
        for (const auto& g : got) exact += sentSet.count(g);
        printf("frames handed on %zu, bit-exact with a sent frame %zu (sent %zu)\n", got.size(), exact, sent.size());
        CHECK(got.size() > 0 && exact == got.size(), "bit-exact frames %zu of %zu", exact, got.size());
    }
    {   // messages and logons: each decoded one was sent, and all sent early enough arrived
        const auto msgs = syn.takeMessages();
        const auto lgs = syn.takeLogons();
        size_t want = 0, match = 0;
        for (const auto& m : msgs) want += m.time < secs - 4;
        for (const auto& d : t.messages) {
            bool ok = false;
            for (const auto& m : msgs) ok |= m.msg.registration == d.registration && m.msg.label == d.label && m.msg.text == d.text && m.msg.aesId == d.aesId;
            match += ok;
            CHECK(ok, "decoded message %s %s '%s' was not sent", d.registration.c_str(), d.label.c_str(), d.text.c_str());
            CHECK(d.crcOk, "message check");
        }
        printf("messages: %zu sent (%zu early enough), %zu decoded, %zu match; logons sent %zu, decoded %llu; aircraft %zu\n", msgs.size(), want, t.messages.size(), match,
               lgs.size(), (unsigned long long)t.logonsTotal, t.aircraft.size());
        CHECK(t.messages.size() >= want && want >= 4, "messages %zu of %zu", t.messages.size(), want);
        CHECK(t.logonsTotal >= 6 && t.aircraft.size() == 6, "logons %llu aircraft %zu", (unsigned long long)t.logonsTotal, t.aircraft.size());
        CHECK(t.dataValid && t.suLayer, "data valid");
        size_t typed = 0;
        for (const auto& ty : t.suTypes) typed += ty.count;
        CHECK(typed == t.blocksOk + t.blocksBad && t.suTypes.size() >= 3, "SU type counts %zu over %zu types", typed, t.suTypes.size());
        for (const auto& ty : t.suTypes) printf("  SU type 0x%02X %-28s %llu\n", ty.type, ty.name.c_str(), (unsigned long long)ty.count);
    }
    CHECK(t.blocksOk > 0 && t.blocksBad == 0, "SUs %llu ok %llu bad", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
