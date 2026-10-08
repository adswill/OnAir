// Mesh (LoRa) round trip: the test signal (6 Meshtastic + 3 MeshCore nodes, packets built as the firmware builds them) through the
// receiver at 2 Msps with 8-bit samples. One whole cycle of the scene (60 s): every frame decodes, every node, name, position,
// battery and message arrives, nothing fails its CRC. Also measures the receiver's speed.
#include "dect2/gen_util.h"
#include "dect2/mesh_gen.h"
#include "dect2/mesh_rx.h"
#include "dect2/modes.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void quant8(cf32* x, size_t n) {
    for (size_t i = 0; i < n; i++)
        x[i] = cf32(std::round(std::max(-127.f, std::min(127.f, x[i].real() * 127.f))) / 127.f, std::round(std::max(-127.f, std::min(127.f, x[i].imag() * 127.f))) / 127.f);
}

int main() {
    const double rate = 2e6, secs = 62;
    SynthConfig cfg;
    cfg.mode = 22;
    cfg.snrDb = 25;
    cfg.modeVal[0] = 5;                    // weakest node at 5 dB
    auto syn = makeMeshSynth(cfg, rate);
    MeshReceiver rx;
    std::vector<std::string> logs;
    rx.setLogCallback([&](const std::string& s) { logs.push_back(s); });
    rx.configure(rate);
    rx.setSignalOffset(-meshTuning().tuneOffsetHz);
    std::vector<cf32> buf(65536);
    double rxSecs = 0, peak = 0;
    MeshTelemetry t;
    uint64_t last = 0;
    int reports = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) {
        syn->generate(buf.data(), buf.size());
        for (auto& v : buf) peak = std::max(peak, (double)std::max(std::fabs(v.real()), std::fabs(v.imag())));
        quant8(buf.data(), buf.size());
        const std::clock_t a = std::clock();
        rx.feed(buf.data(), buf.size());
        rxSecs += (double)(std::clock() - a) / CLOCKS_PER_SEC;
        if (rx.telemetry(t, last)) { CHECK(t.seq > last, "seq"); last = t.seq; reports++; }
    }
    rx.telemetry(t, 0);
    printf("%s\n", meshSummary(t).c_str());
    printf("receiver: %.2f s of CPU (process time) for %.0f s of signal at 2 Msps (%.0fx real time); peak sample %.2f; %d reports\n", rxSecs, secs, secs / rxSecs, peak, reports);
    for (const auto& d : t.decoders)
        printf("  %-12s %.3f MHz SF%d %5.1f kHz: %llu frames, %llu CRC errors, %llu bad headers, last SNR %.1f dB\n", d.preset.c_str(), d.freqHz / 1e6, d.sf,
               d.bwHz / 1e3, (unsigned long long)d.frames, (unsigned long long)d.crcBad, (unsigned long long)d.headerBad, d.lastSnrDb);
    for (const auto& n : t.nodes)
        printf("  node %d %-12s %-22s %-5s %-24s %-8s pos %d %.4f %.4f batt %.0f hops %d SNR %.1f packets %u\n", n.protocol, n.id.c_str(), n.longName.c_str(), n.shortName.c_str(), n.hwModel.c_str(),
               n.role.c_str(), n.hasPosition, n.lat, n.lon, n.batteryPct, n.hopsAway, n.lastSnrDb, n.packets);
    for (const auto& m : t.messages) printf("  msg %d [%s] %s (%s) -> %s: %s (hops %d)\n", m.protocol, m.channel.c_str(), m.from.c_str(), m.fromName.c_str(), m.to.c_str(), m.text.c_str(), m.hops);
    for (const auto& c : t.counts) printf("  count %d %s %u\n", c.protocol, c.type.c_str(), c.count);
    for (const auto& l : logs) printf("  log: %s\n", l.c_str());

    CHECK(peak < 0.9, "peak %.2f", peak);
    CHECK(reports >= secs * 4 - 2, "reports %d", reports);
    CHECK(t.state == 2 && t.dataValid, "state %d", t.state);
    CHECK(t.blocksBad == 0, "%llu CRC errors", (unsigned long long)t.blocksBad);
    // one cycle: 31 Meshtastic and 10 MeshCore frames (the first frames of the next cycle may be in too)
    uint64_t mt = 0, mc = 0;
    for (const auto& d : t.decoders) (d.protocol == 1 ? mt : mc) += d.frames;
    CHECK(mt >= 31 && mc >= 10, "frames: Meshtastic %llu, MeshCore %llu", (unsigned long long)mt, (unsigned long long)mc);
    int mtNodes = 0, mcNodes = 0, named = 0, placed = 0, batt = 0;
    for (const auto& n : t.nodes) {
        (n.protocol == 1 ? mtNodes : mcNodes)++;
        named += !n.longName.empty();
        placed += n.hasPosition && std::fabs(n.lat - 25.1) < 0.3 && std::fabs(n.lon - 55.2) < 0.3;
        batt += n.protocol == 1 && n.batteryPct > 0;
    }
    CHECK(mtNodes == 6 && mcNodes == 3, "nodes %d + %d", mtNodes, mcNodes);
    CHECK(named == 9 && placed == 9 && batt == 6, "named %d, placed %d, battery %d", named, placed, batt);
    std::set<std::string> texts;
    for (const auto& m : t.messages) texts.insert(m.text);
    CHECK(texts.count("Good morning mesh, Marina base is up") && texts.count("Coffee at the Marina walk at 9?"), "Meshtastic chat");
    CHECK(texts.count("Deira checking in") && texts.count("Repeater on the tower is working well"), "MeshCore chat");
    CHECK(t.messages.size() >= 12, "%zu messages", t.messages.size());
    bool trace = false, nodeinfo = false, advert = false, grp = false;
    for (const auto& c : t.counts) {
        trace |= c.type == "TRACEROUTE_APP" && c.count >= 2;
        nodeinfo |= c.type == "NODEINFO_APP" && c.count >= 6;
        advert |= c.type == "ADVERT" && c.count >= 4;
        grp |= c.type == "GRP_TXT" && c.count >= 6;
    }
    CHECK(trace && nodeinfo && advert && grp, "counts: traceroute %d nodeinfo %d advert %d group text %d", trace, nodeinfo, advert, grp);
    int decrypted = 0;
    for (const auto& p : t.packets) decrypted += p.decrypted;
    CHECK(decrypted >= 35, "%d packets decrypted", decrypted);
    // the packet times follow the scene: the first Meshtastic frame starts at 0.2 s
    CHECK(!t.packets.empty() && std::fabs(t.packets.front().timeSec - 0.2) < 0.01, "first packet at %.4f s", t.packets.empty() ? -1.0 : t.packets.front().timeSec);
    // a reset empties the tables; the report numbers go on
    rx.reset();
    syn->generate(buf.data(), buf.size());
    rx.feed(buf.data(), buf.size());
    for (int i = 0; i < 10; i++) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    MeshTelemetry t2;
    CHECK(rx.telemetry(t2, last) && t2.seq > last && t2.nodes.empty() && t2.blocksOk == 0, "after reset: seq %llu nodes %zu", (unsigned long long)t2.seq, t2.nodes.size());
    {   // speed on noise alone (the search runs all the time)
        MeshReceiver r2;
        r2.configure(rate);
        r2.setSignalOffset(-meshTuning().tuneOffsetHz);
        genutil::NoiseSource ns(5);
        double cpu = 0;
        for (int i = 0; i < 300; i++) {
            for (auto& v : buf) v = cf32(0, 0);
            ns.add(buf.data(), buf.size(), 0.05f);
            quant8(buf.data(), buf.size());
            const std::clock_t a = std::clock();
            r2.feed(buf.data(), buf.size());
            cpu += (double)(std::clock() - a) / CLOCKS_PER_SEC;
        }
        const double s = 300.0 * buf.size() / rate;
        MeshTelemetry t3;
        r2.telemetry(t3, 0);
        printf("noise only: %.2f s of CPU for %.1f s (%.0fx real time), %llu false preambles, %llu frames\n", cpu, s, s / cpu, (unsigned long long)t3.preambles, (unsigned long long)(t3.blocksOk + t3.blocksBad));
        CHECK(s / cpu > 3, "noise only: %.1fx real time", s / cpu);
        CHECK(t3.blocksOk == 0, "frames out of noise");
    }
    CHECK(secs / rxSecs > 3, "%.1fx real time", secs / rxSecs);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
