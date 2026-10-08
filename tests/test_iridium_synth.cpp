// Iridium test signal through the receiver and the frame layer: the satellites of the model, their ring alerts (positions as the
// model has them), the broadcast time, the pager messages assembled from their parts, the Doppler shift of every satellite;
// at 10 Msps (the default), 4, 8 and 20 Msps, and 2.4 Msps on the simplex channels only.
#include "dect2/iridium_frame.h"
#include "dect2/iridium_gen.h"
#include "dect2/iridium_phy.h"
#include "dect2/iridium_rx.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Seen {
    IridiumBurstBits b;
    IridiumFrame f;
};

static std::vector<Seen> run(double rate, double secs, double centerMhz, IridiumTelemetry& tel, uint32_t seed = 0, int nSats = 3) {
    SynthConfig c;
    c.snrDb = 25;
    c.mode = 21;
    c.modeOpt[0] = nSats;
    c.modeOpt[1] = (int)seed;
    c.modeVal[1] = centerMhz;
    auto s = makeIridiumSynth(c, rate);
    IridiumReceiver rx;
    rx.configure(rate);
    rx.setCenterMhz(centerMhz);
    rx.setOffline(true);
    std::mutex m;
    std::vector<Seen> seen;
    rx.setBurstCallback([&](const IridiumBurstBits& b, const IridiumFrame& f) { std::lock_guard<std::mutex> lk(m); seen.push_back(Seen{b, f}); });
    std::vector<cf32> buf(50000);
    float peak = 0;
    for (size_t done = 0; done < (size_t)(secs * rate); done += buf.size()) {
        s->generate(buf.data(), buf.size());
        for (const auto& v : buf) peak = std::max(peak, std::max(std::fabs(v.real()), std::fabs(v.imag())));
        rx.feed(buf.data(), buf.size());
    }
    rx.flush();
    rx.telemetry(tel, 0);
    CHECK(peak < 0.9f, "%.1f Msps: peak %.2f", rate / 1e6, peak);
    return seen;
}

int main() {
    const double t0 = iridiumSkyStart(0, 3);
    // the model's sky over Dubai: Doppler and Doppler rate within what an Iridium pass gives
    {
        double maxD = 0, maxR = 0;
        for (double t = 0; t < 6000; t += 10)
            for (const auto& s : iridiumSky(t, 0)) { maxD = std::max(maxD, std::fabs(s.dopplerHz)); maxR = std::max(maxR, std::fabs(s.dopplerRate)); CHECK(std::fabs(s.altKm - 780) < 1, "altitude %.1f", s.altKm); }
        printf("model over 6000 s: largest Doppler %.1f kHz, largest Doppler rate %.0f Hz/s\n", maxD / 1e3, maxR);
        CHECK(maxD > 34e3 && maxD < 37.6e3 && maxR > 250 && maxR < 400, "Doppler %.0f Hz, rate %.0f Hz/s", maxD, maxR);
        CHECK(iridiumSky(t0, 3).size() >= 3, "three satellites at the start");
    }
    // 10 Msps, 6 s
    {
        IridiumTelemetry tel;
        const double secs = 6;
        const std::vector<Seen> seen = run(10e6, secs, 1622, tel);
        std::map<int, int> nType;
        int bad = 0, posBad = 0, dopBad = 0, nPos = 0;
        double maxPosErr = 0, maxDopErr = 0;
        for (const auto& s : seen) {
            nType[(int)s.f.type]++;
            if (!s.f.ok && s.f.type != IridiumType::Voice) bad++;
            if (s.f.type == IridiumType::IRA && s.f.ok) {
                // the satellite's own position in the ring alert: the model's sub-point at that time (4 km units in the frame)
                const auto sky = iridiumSky(t0 + s.b.timeSec, -90);
                for (const auto& q : sky) {
                    if (q.id != s.f.satId) continue;
                    // Doppler of this burst: offset from its simplex access
                    const double off = s.b.freqHz - iridium::channelHz(iridium::nearestChannel(s.b.freqHz - q.dopplerHz));
                    maxDopErr = std::max(maxDopErr, std::fabs(off - q.dopplerHz));
                    if (std::fabs(off - q.dopplerHz) > 100) dopBad++;
                    if (s.f.altKm > 100) {
                        nPos++;
                        const double e = std::hypot(s.f.lat - q.lat, (s.f.lon - q.lon) * std::cos(q.lat * M_PI / 180));
                        maxPosErr = std::max(maxPosErr, e);
                        if (e > 0.1 || std::fabs(s.f.altKm - q.altKm) > 8) posBad++;
                    }
                }
            }
        }
        printf("10 Msps, %.0f s: %zu frames: IRA %d IBC %d ISY %d MSG %d voice %d, %d failed; satellite positions %d (largest error %.3f deg), Doppler error at most %.0f Hz\n",
               secs, seen.size(), nType[1], nType[2], nType[3], nType[5], nType[12], bad, nPos, maxPosErr, maxDopErr);
        const int frames = (int)(secs / 0.09);
        CHECK(bad == 0 && tel.blocksBad == 0, "%d frames failed", bad);
        CHECK(nType[1] >= 3 * frames - 15 && nType[2] >= 3 * frames / 2 - 3 && nType[3] >= 3 * frames / 2 - 3 && nType[12] > 100, "frame counts");
        CHECK(nPos > 80 && posBad == 0 && dopBad == 0, "positions %d (%d off), Doppler %d off", nPos, posBad, dopBad);
        // detections without a unique word: noise peaks over the threshold (a few a second at 10 Msps)
        CHECK(tel.bursts - tel.uwOk <= 0.04 * tel.bursts && tel.dropped == 0, "bursts %llu, unique words %llu, dropped %llu", (unsigned long long)tel.bursts, (unsigned long long)tel.uwOk,
              (unsigned long long)tel.dropped);
        // telemetry tables
        CHECK(tel.state == 2 && tel.dataValid && tel.sats.size() == 3, "state %d, %zu satellites", tel.state, tel.sats.size());
        for (const auto& s : tel.sats) CHECK(s.hasPos && s.beams.size() == 48 && s.frames >= 95, "satellite %d: position %d, %zu beams, %llu frames", s.id, s.hasPos, s.beams.size(), (unsigned long long)s.frames);
        CHECK(tel.ringAlerts.size() == 100 && tel.positions.size() > 150, "%zu ring alerts, %zu positions", tel.ringAlerts.size(), tel.positions.size());
        CHECK(tel.hasTime && std::fabs(tel.iridiumUtc - (kIridiumSynthEpoch + secs)) < 0.2, "Iridium time %.3f", tel.iridiumUtc - kIridiumSynthEpoch);
        std::set<std::string> texts;
        for (const auto& m : tel.messages) { CHECK(m.complete, "message %d incomplete", m.ric); texts.insert(m.text); }
        CHECK(tel.messages.size() >= 8 && texts.size() == 4, "%zu messages, %zu different texts", tel.messages.size(), texts.size());
        CHECK(texts.count("CALL OFFICE ASAP RE SHIPMENT 2231. CUSTOMS NEED THE INVOICE AND THE PACKING LIST BEFORE 1700 TODAY") == 1, "a two-part message");
        CHECK(tel.voiceFrames == (uint64_t)nType[12] && tel.typeCount[1] == (uint64_t)nType[1], "type counters");
        CHECK(tel.snrDb > 15 && tel.snrDb < 26, "snr %.1f", tel.snrDb);
        // Phase B: ACARS in short burst data, one message every 2.25 s
        CHECK(tel.acars.size() >= 2 && tel.acars[0].reg == ".A6-EDA" && tel.acars[0].label == "RA" &&
              tel.acars[0].text == "WX DXB 34C WIND 330/12KT VIS 10KM NOSIG QNH 1006", "%zu ACARS messages", tel.acars.size());
    }
    // other rates (the whole band each covers around 1622 MHz)
    for (double rate : {4e6, 8e6, 20e6}) {
        IridiumTelemetry tel;
        const std::vector<Seen> seen = run(rate, 2, 1622, tel, 3);
        int bad = 0;
        for (const auto& s : seen) if (!s.f.ok && s.f.type != IridiumType::Voice) bad++;
        printf("%.1f Msps: %zu frames, %llu bursts, %d failed, %llu IRA\n", rate / 1e6, seen.size(), (unsigned long long)tel.bursts, bad, (unsigned long long)tel.typeCount[1]);
        CHECK(bad == 0 && seen.size() > 60 && tel.bursts - tel.uwOk <= 0.04 * tel.bursts + 2, "%.1f Msps", rate / 1e6);
        if (rate >= 10e6) CHECK(tel.typeCount[1] > 50, "IRA at %.0f Msps", rate / 1e6);
    }
    // 2.4 Msps on 1626.25 MHz: the simplex channels (ring alerts and pager messages) and the top of the duplex band
    {
        IridiumTelemetry tel;
        const std::vector<Seen> seen = run(2.4e6, 4, 1626.25, tel, 5);
        int bad = 0;
        for (const auto& s : seen)
            if (!s.f.ok && s.f.type != IridiumType::Voice) { bad++; printf("  failed: %s at %.0f Hz, %.4f s, %zu bits\n", s.f.typeName.c_str(), s.b.freqHz, s.b.timeSec, s.b.bits.size()); }
        printf("2.4 Msps on 1626.25 MHz: %zu frames (%llu IRA, %llu MSG), %d failed, %zu messages\n", seen.size(), (unsigned long long)tel.typeCount[1],
               (unsigned long long)tel.typeCount[5], bad, tel.messages.size());
        CHECK(bad == 0 && tel.typeCount[1] > 100 && tel.messages.size() >= 4, "simplex band");
    }
    printf(fails ? "iridium synth: %d FAILED\n" : "iridium synth: all passed\n", fails);
    return fails ? 1 : 0;
}
