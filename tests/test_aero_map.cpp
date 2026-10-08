// Aero positions end to end: the test signal's position reports (ADS-C and a text POS report) through modulation, the receiver and
// the SU layer into the aircraft table, with tracks; each aircraft's position is the one in the last report it sent.
#include "dect2/aero_gen.h"
#include "dect2/aero_pos.h"
#include "dect2/aero_rx.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const double rate = 1e6;
    SynthConfig cfg;
    cfg.mode = 20;
    AeroSynth syn(cfg, rate);
    syn.record(true);
    AeroReceiver rx;
    rx.configure(rate);
    rx.setSignalOffset(-aeroTuning().tuneOffsetHz);
    std::vector<cf32> buf(65536);
    AeroTelemetry t;
    uint64_t last = 0;
    const double secs = 45;
    for (double done = 0; done < secs * rate; done += (double)buf.size()) {
        syn.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, last)) last = t.seq;
    }
    const auto sent = syn.takeMessages();
    // what each aircraft's reports said, in the order sent
    std::map<uint32_t, std::vector<AeroPosition>> want;
    int adsc = 0, text = 0;
    for (const auto& s : sent) {
        AeroPosition p;
        if (!aeroPositionFromMessage(s.msg.label, s.msg.text, s.msg.uplink, p)) continue;
        want[s.msg.aesId].push_back(p);
        (p.source == 1 ? adsc : text)++;
    }
    printf("%s\nsent %d ADS-C and %d text position reports; %llu positions decoded\n", aeroSummary(t).c_str(), adsc, text, (unsigned long long)t.positionsTotal);
    CHECK(adsc >= 8 && text >= 1, "the generator sent %d ADS-C and %d text reports", adsc, text);
    int withPos = 0, withTrack = 0, fromText = 0, msgsWithPos = 0;
    for (const auto& m : t.messages) msgsWithPos += m.hasPos;
    for (const auto& a : t.aircraft) {
        if (!a.hasPos) continue;
        withPos++;
        const auto it = want.find(a.aesId);
        CHECK(it != want.end(), "%06X has a position but sent none", a.aesId);
        if (it == want.end()) continue;
        // the decoded position is one that was sent, and the track holds the positions in order
        bool sentOne = false;
        for (const auto& p : it->second) sentOne |= p.lat == a.lat && p.lon == a.lon;
        CHECK(sentOne, "%s position %.4f %.4f was not sent", a.registration.c_str(), a.lat, a.lon);
        CHECK(!a.track.empty() && a.track.back().lat == a.lat && a.track.back().lon == a.lon && a.track.size() <= 24, "%s track", a.registration.c_str());
        withTrack += a.track.size() >= 2;
        CHECK(a.hasTrack, "%s has no track direction", a.registration.c_str());
        if (a.posSource == 2) {
            fromText++;
            CHECK(a.registration == "VT-ANA" && a.hasAlt && a.altFt == 33000 && a.reportSecOfDay >= 0, "text report fields %s %d", a.registration.c_str(), a.altFt);
        } else {
            CHECK(a.posSource == 1 && a.hasSpeed && a.hasAlt && a.reportSecPastHour >= 0 && !a.flight.empty(), "ADS-C fields of %s", a.registration.c_str());
        }
        printf("  %-7s %06X %8.4f %9.4f %5d ft  track %5.1f  %s  %zu points\n", a.registration.c_str(), a.aesId, a.lat, a.lon, a.altFt, a.trackDeg, a.posKind.c_str(), a.track.size());
    }
    CHECK(withPos == 6, "%d of 6 aircraft with a position", withPos);
    CHECK(withTrack >= 2, "%d aircraft with a track of two or more points", withTrack);
    CHECK(fromText == 1, "%d aircraft placed by a text report", fromText);
    CHECK(msgsWithPos > 0 && t.positionsTotal >= (uint64_t)withPos, "messages with a position %d, total %llu", msgsWithPos, (unsigned long long)t.positionsTotal);
    // the ADS-C reports show their decoded groups in the message list
    bool decoded = false;
    for (const auto& m : t.messages) decoded |= m.label == "B6" && m.decoded.find("Basic report") != std::string::npos;
    CHECK(decoded, "no B6 message with decoded ADS-C groups");
    CHECK(t.blocksBad == 0, "SUs bad %llu", (unsigned long long)t.blocksBad);
    printf(fails ? "aero map: %d FAILED\n" : "aero map: all passed\n", fails);
    return fails ? 1 : 0;
}
