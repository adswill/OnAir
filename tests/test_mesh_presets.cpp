// Mesh (LoRa), several Meshtastic presets at once (setPresetSearch): a text message on every EU_868 preset that has a slot, each on
// its own slot frequency and spreading factor, encrypted with the default key under the preset's channel name as the firmware does,
// some of them on the air at the same time; plus MeshCore. Every one decodes; the CPU cost of the wider search is printed.
#include "dect2/gen_util.h"
#include "dect2/mesh_lora.h"
#include "dect2/mesh_proto.h"
#include "dect2/mesh_rx.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = 2e6, tuned = 869.525e6, off = -meshTuning().tuneOffsetHz, noise = 0.03;
    const char* names[] = {"LongFast", "MediumFast", "MediumSlow", "ShortFast", "ShortSlow", "ShortTurbo", "LongModerate", "LongSlow", "LongTurbo"};
    std::vector<uint8_t> key;
    CHECK(meshtasticExpandPsk({1}, key), "default key");
    std::vector<lora::TxFrame> frames;
    std::set<std::string> want;
    double t = 0.1;
    uint32_t id = 0x5000;
    for (const char* n : names) {
        MeshLoraSettings s;
        const double f = meshtasticSlotHz("EU_868", n);
        if (!meshtasticPreset(n, s) || f <= 0) { printf("%-12s no slot in EU_868\n", n); continue; }
        const std::string text = std::string("hello on ") + n;
        const auto data = meshtasticData(1, std::vector<uint8_t>(text.begin(), text.end()));
        id++;
        const auto pkt = meshtasticRaw(0x2a000000u + id, 0xFFFFFFFFu, id, 3, 3, false, n, key, data);
        lora::TxFrame fr;
        fr.p.sf = s.sf; fr.p.bwHz = s.bwHz; fr.p.cr = s.cr; fr.p.preamble = s.preamble; fr.p.syncWord = s.syncWord; fr.p.ldro = s.ldro;
        fr.data = lora::encode(fr.p, pkt.data(), pkt.size());
        fr.startSec = t;
        fr.freqHz = f - tuned + off;
        fr.amp = (float)std::sqrt(std::pow(10.0, 10 / 10.0) * noise * noise * s.bwHz / rate);
        frames.push_back(fr);
        want.insert(text);
        printf("%-12s %.3f MHz SF%d %.0f kHz 4/%d%s, %.2f s on the air from %.2f s\n", n, f / 1e6, s.sf, s.bwHz / 1e3, s.cr, s.ldro ? " LDRO" : "", fr.endSec() - fr.startSec, t);
        t += 0.45;                                   // the next starts before this one ends (different settings)
    }
    double end = 0;
    for (const auto& f : frames) end = std::max(end, f.endSec());
    std::vector<cf32> x((size_t)((end + 0.3) * rate), cf32(0, 0));
    for (const auto& f : frames) f.render(x.data(), x.size(), 0, rate);
    genutil::NoiseSource ns(4);
    ns.add(x.data(), x.size(), (float)(noise / std::sqrt(2.0)));
    MeshReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(off);
    rx.setPresetSearch(true);
    const std::clock_t a = std::clock();
    for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    const double cpu = (double)(std::clock() - a) / CLOCKS_PER_SEC;
    MeshTelemetry tel;
    rx.telemetry(tel, 0);
    std::set<std::string> got;
    for (const auto& m : tel.messages) { got.insert(m.text); printf("  [%s] %s\n", m.channel.c_str(), m.text.c_str()); }
    int searched = 0;
    for (const auto& d : tel.decoders) searched += d.inBand;
    printf("%d settings searched, %zu of %zu messages, %llu CRC errors; %.2f s of CPU for %.1f s at 2 Msps (%.0fx real time)\n", searched, got.size(), want.size(),
           (unsigned long long)tel.blocksBad, cpu, x.size() / rate, x.size() / rate / cpu);
    CHECK(got == want, "messages");
    CHECK(tel.blocksBad == 0, "CRC errors");
    CHECK(x.size() / rate / cpu > 3, "speed");
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
