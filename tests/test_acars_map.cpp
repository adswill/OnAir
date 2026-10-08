// ACARS positions end to end: the test signal's position reports (ADS-C as B6 and as H1 "#M1B/B6", a POS text report and a label 16 report)
// through modulation and the receiver into the aircraft table. Each aircraft ends up at a position it sent, the track holds the positions in
// order, and the ADS-C groups are read.
#include "dect2/acars_sim.h"
#include "dect2/aero_pos.h"
#include <cstdio>
#include <map>
#include <vector>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    AcarsSimCfg c;
    c.gen.positions = true;
    c.gen.rateFactor = 2;
    c.syn.snrDb = 35;
    c.secs = 50;
    const AcarsSimResult r = runAcarsSim(c);
    // what each aircraft's reports said, in the order sent (the 10 characters of message number and flight id come off the text)
    std::map<std::string, std::vector<AeroPosition>> want;
    int adsc = 0, text = 0, tooLong = 0;
    for (const auto& s : r.sentAll) {
        if (s.spec.blockId < '0' || s.spec.blockId > '9' || s.spec.text.size() < 10) continue;
        tooLong += s.spec.text.size() > 220;
        AeroPosition p;
        if (!aeroPositionFromMessage(std::string(s.spec.label, 2), s.spec.text.substr(10), false, p)) continue;
        want[s.spec.reg].push_back(p);
        (p.source == 1 ? adsc : text)++;
    }
    const AcarsTelemetry& t = r.last;
    printf("%s\nsent %d ADS-C and %d text position reports; %llu positions decoded\n", acarsSummary(t).c_str(), adsc, text, (unsigned long long)t.positionsTotal);
    CHECK(adsc >= 12 && text >= 4, "the generator sent %d ADS-C and %d text reports", adsc, text);
    CHECK(tooLong == 0, "%d blocks longer than 220 characters", tooLong);
    CHECK(r.wrong == 0 && r.decoded == r.sent, "decoded %d of %d blocks, %d wrong", r.decoded, r.sent, r.wrong);
    int withPos = 0, withTrack = 0, withRoute = 0, withIcao = 0, bySource[3] = {0, 0, 0};
    for (const auto& a : t.aircraft) {
        if (!a.hasPos) continue;
        withPos++;
        const auto it = want.find(a.reg);
        CHECK(it != want.end(), "%s has a position but sent none", a.reg.c_str());
        if (it == want.end()) continue;
        bool sentOne = false;
        for (const auto& p : it->second) sentOne |= p.lat == a.lat && p.lon == a.lon;
        CHECK(sentOne, "%s position %.4f %.4f was not sent", a.reg.c_str(), a.lat, a.lon);
        CHECK(!a.track.empty() && a.track.back().lat == a.lat && a.track.back().lon == a.lon && a.track.size() <= 24, "%s track", a.reg.c_str());
        // the track follows the order sent: every point is a sent position, later points come from later reports
        size_t from = 0;
        bool ordered = true;
        for (const auto& tp : a.track) {
            size_t k = from;
            while (k < it->second.size() && !(it->second[k].lat == tp.lat && it->second[k].lon == tp.lon)) k++;
            if (k == it->second.size()) { ordered = false; break; }
            from = k + 1;
        }
        CHECK(ordered, "%s track is not in the order sent", a.reg.c_str());
        withTrack += a.track.size() >= 2;
        withRoute += !a.route.empty();
        withIcao += a.icao != 0;
        CHECK(a.hasTrack, "%s has no track direction", a.reg.c_str());
        CHECK(a.posSource == 1 || a.posSource == 2, "%s source %d", a.reg.c_str(), a.posSource);
        bySource[a.posSource & 3]++;
        if (a.posSource == 1) CHECK(a.hasSpeed && a.hasAlt && a.reportSecPastHour >= 0 && !a.flight.empty(), "ADS-C fields of %s", a.reg.c_str());
        else CHECK(a.reportSecOfDay >= 0 || a.hasAlt, "text report fields of %s", a.reg.c_str());
        printf("  %-7s %8.4f %9.4f %5d ft  track %5.1f  %-22s %zu points\n", a.reg.c_str(), a.lat, a.lon, a.altFt, a.trackDeg, a.posKind.c_str(), a.track.size());
    }
    CHECK(withPos == 6, "%d of 6 aircraft with a position", withPos);
    CHECK(withTrack >= 5, "%d aircraft with a track of two or more points", withTrack);
    CHECK(bySource[1] == 4 && bySource[2] == 2, "ADS-C %d, text %d aircraft", bySource[1], bySource[2]);
    // ADS-C messages carry their decoded groups, text position messages a short reading; uplinks never place an aircraft
    int adscMsgs = 0, posMsgs = 0, upPos = 0, routeMsgs = 0;
    for (const auto& m : r.msgs) {
        posMsgs += m.hasPos;
        routeMsgs += m.adsc.find("predicted route") != std::string::npos;
        upPos += m.hasPos && !m.downlink;
        if (m.label == "B6" || m.sublabel == "M1") adscMsgs += m.adsc.find("Basic report") != std::string::npos && m.hasPos && !m.decoded.empty();
    }
    CHECK(routeMsgs >= 4 && withRoute >= 1, "%d messages with a predicted route, %d aircraft whose last report has one", routeMsgs, withRoute);
    CHECK(adscMsgs >= 8, "%d messages with decoded ADS-C groups", adscMsgs);
    CHECK(posMsgs >= (int)(adsc + text) * 9 / 10 && upPos == 0, "messages with a position %d of %d sent, %d on uplinks", posMsgs, adsc + text, upPos);
    CHECK(t.positionsTotal == (uint64_t)posMsgs, "positions total %llu, messages %d", (unsigned long long)t.positionsTotal, posMsgs);
    CHECK(t.blocksBad == 0, "blocks bad %llu", (unsigned long long)t.blocksBad);
    printf(fails ? "acars map: %d FAILED\n" : "acars map: all passed\n", fails);
    return fails ? 1 : 0;
}
