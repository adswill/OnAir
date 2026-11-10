// Stage 2 of the Inmarsat all-carriers mode: ONE wide capture with the Aero test signal (P channels at 600, 1200 and 10500 bit/s), a second
// Aero group 2 MHz away, an Inmarsat-C channel with an extra channel, and an unrelated wide carrier, all decoded in parallel by the manager.
// Synthetic signals only (the same generators the single-channel tests use, moved to different places in the band and summed, with a
// carrier offset and a slow drift): needs a user test with a real capture.
#include "dect2/inmsat/inmsat_manager.h"
#include "dect2/aero/aero_gen.h"
#include "dect2/aero/aero_rx.h"
#include "dect2/inmc/inmc_testutil.h"
#include "inmsat_synth.h"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::inmsat;
#include "../common/cpu_report.h"
#include "../common/test_util.h"

namespace {
struct Shifter {
    double f, fs, ph = 0;
    void apply(cf32* x, size_t n) {
        const double w = 2 * M_PI * f / fs;
        for (size_t i = 0; i < n; i++) { x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += w; if (ph > 2 * M_PI) ph -= 2 * M_PI; }
    }
};
}

int main() {
    const double rate = 6e6;
    SynthConfig a1, a2;
    a1.mode = a2.mode = 20;
    a1.modeOpt[0] = 7;  a1.modeVal[0] = 17;                 // 600 + 1200 + 10500 bit/s
    a2.modeOpt[0] = 4;  a2.modeOpt[1] = 3; a2.modeVal[0] = 17;   // another 10500 bit/s group, other messages
    AeroSynth s1(a1, rate), s2(a2, rate);
    s1.record(true);
    s2.record(true);
    InmcGenConfig g;
    g.rate = rate; g.ebn0Db = 17; g.seed = 5; g.extraChannels = 1;
    auto inm = makeInmcGenerator(g);
    Shifter sh2{-2.0e6, rate}, shI{+1.0e6, rate};
    inmsat_test::WideSynth wide(rate, {{1.8e6, 3000, 16, 0, 0}}, 0, 11);   // an unrelated wide carrier: it must be listed, not decoded

    ManagerConfig mc;
    mc.rate = rate;
    mc.centerHz = 1545.0e6;
    Manager mgr;
    mgr.configure(mc);
    mgr.setLocation(true, 54.7, 25.3);          // Lithuania: 64.5E is above the horizon
    std::vector<std::string> logs;
    mgr.setLogCallback([&](const std::string& s) { logs.push_back(s); });
    const double secs = 80;
    const size_t chunk = 65536;
    std::vector<cf32> b1(chunk), b2(chunk), bi(chunk), bw(chunk), mix(chunk);
    Snapshot snap;
    uint64_t last = 0;
    double cpu = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += chunk) {
        s1.generate(b1.data(), chunk);
        s2.generate(b2.data(), chunk); sh2.apply(b2.data(), chunk);
        inm->generate(bi.data(), chunk); shI.apply(bi.data(), chunk);
        wide.generate(bw.data(), chunk);
        for (size_t i = 0; i < chunk; i++) mix[i] = b1[i] + b2[i] + bi[i] + bw[i] * 0.2f;
        const auto t0 = std::chrono::steady_clock::now();
        mgr.feed(mix.data(), chunk);
        cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (mgr.snapshot(snap, last)) last = snap.seq;
    }
    printf("manager: %.0f s of a %.0f Msps capture in %.1f s (%.2fx real time); windows %zu, cost %.1f of %.1f\n", secs, rate / 1e6, cpu, secs / cpu, snap.windows.size(), snap.cpuCost, snap.cpuBudget);
    for (const auto& w : snap.windows) printf("  window %+9.0f Hz  out %.0f sps  aero %d  inmc %d\n", w.centerHz, w.outRate, w.aero, w.inmc);
    for (const auto& c : snap.carriers)
        printf("  carrier %+10.0f Hz (%.4f MHz) width %6.0f snr %5.1f %-6s %-10s %5d  state %d  Eb/N0 %5.1f  frames %llu  %s%s\n", c.offsetHz, c.freqHz / 1e6, c.widthHz, c.snrDb,
               carrierClassName(c.cls), carrierTypeName(c.type), c.bitRate, c.state, c.ebn0Db, (unsigned long long)c.frames, c.region.c_str(), c.parked ? " PARKED" : "");
    printf("  messages %zu (total %llu), aircraft %zu, logons %zu, EGC %zu, SUs %llu ok %llu bad\n", snap.messages.size(), (unsigned long long)snap.messagesTotal, snap.aircraft.size(),
           snap.logons.size(), snap.egc.size(), (unsigned long long)snap.susOk, (unsigned long long)snap.susBad);
    for (const auto& l : logs) printf("  log: %s\n", l.c_str());

    // every Aero carrier the generators made is in the table and in sync with the right rate
    int aeroOk = 0;
    auto expect = [&](const AeroSynth& s, double shift) {
        for (const auto& ch : s.channels()) {
            const double want = ch.offsetHz - aeroTuning().tuneOffsetHz + shift;
            bool ok = false;
            for (const auto& c : snap.carriers) ok |= std::fabs(c.offsetHz - want) < 3000 && c.type == CarrierType::AeroP && c.bitRate == ch.bitRate && c.state >= 2;
            if (!ok) printf("  MISSING Aero %d bit/s at %+.0f Hz\n", ch.bitRate, want);
            aeroOk += ok;
        }
    };
    expect(s1, 0);
    expect(s2, -2.0e6);
    CHECK(aeroOk == (int)(s1.channels().size() + s2.channels().size()), "Aero carriers in sync %d of %zu", aeroOk, s1.channels().size() + s2.channels().size());
    int inmcOk = 0;
    for (const auto& c : snap.carriers) inmcOk += c.type == CarrierType::InmarsatC && c.state >= 2 && c.bitRate == 1200;
    CHECK(inmcOk >= 2, "Inmarsat-C carriers in sync: %d of 2", inmcOk);
    CHECK(snap.egc.size() >= 1, "Inmarsat-C messages: %zu", snap.egc.size());
    // the unrelated wide carrier is listed and nothing decodes on it
    bool wideListed = false;
    for (const auto& c : snap.carriers) if (std::fabs(c.offsetHz - 1.8e6) < 4000) { wideListed = true; CHECK(c.type == CarrierType::Unknown, "unrelated carrier decoded as %s", carrierTypeName(c.type)); CHECK(c.voiceCandidate, "the unrelated wide carrier is not flagged as a voice candidate"); }
    for (const auto& c : snap.carriers) if (c.type != CarrierType::Unknown) CHECK(!c.voiceCandidate, "a decoded carrier flagged as voice");
    CHECK(wideListed, "the unrelated carrier is not in the table");
    // messages from both Aero groups, and every message decoded was sent (checked against what the generators recorded)
    const auto sent1 = s1.takeMessages(), sent2 = s2.takeMessages();
    size_t known = 0;
    for (const auto& d : snap.messages) {
        bool ok = false;
        for (const auto* sv : {&sent1, &sent2}) for (const auto& m : *sv) ok |= m.msg.registration == d.registration && m.msg.label == d.label && m.msg.text == d.text;
        known += ok;
        CHECK(ok, "decoded message %s %s '%s' was not sent", d.registration.c_str(), d.label.c_str(), d.text.c_str());
    }
    CHECK(snap.messages.size() >= 6, "merged messages %zu", snap.messages.size());
    // the satellite: the Aero system table (64.5E) and the Inmarsat-C bulletin board (IOR) both say 64.5E, the geometry agrees
    printf("  satellite: %s\n", snap.sat.summary.c_str());
    CHECK(snap.sat.valid && std::fabs(snap.sat.lonDeg - 64.5) < 0.1 && snap.sat.confidence >= 0.5, "satellite '%s' conf %.2f", snap.sat.name.c_str(), snap.sat.confidence);
    if (!snap.sat.ranked.empty()) {
        for (const auto& e : snap.sat.ranked[0].evidence) printf("    %s: %s %.2f %s\n", e.kind.c_str(), e.available ? "on" : "off", e.score, e.text.c_str());
        CHECK(snap.sat.ranked[0].evidence[0].available && snap.sat.ranked[0].evidence[2].available, "content and geometry evidence present");
    }
    int named = 0;
    for (const auto& c : snap.carriers) if (c.type == CarrierType::AeroP && !c.satellite.empty()) named++;
    CHECK(named >= 1, "carriers with a satellite name: %d", named);
    CHECK(snap.susBad == 0, "bad SUs %llu", (unsigned long long)snap.susBad);
    cpuCal("inmsat_manager", "all carriers", secs / cpu, "x-realtime", '>', 1.0);
    CHECK(secs / cpu > 1.0, "slower than real time: %.2fx", secs / cpu);
    return testSummary("inmsat_manager");
}
