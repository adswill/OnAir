// Which satellite the NIT says it is, and the plain-language echo advice. Built tables and values; simulated.
#include <cmath>
#include "dect2/tv/si_tables.h"
#include "dect2/channels/channel.h"
#include <cstdio>
#include "../common/test_util.h"
using namespace dect2;
int main() {
    SiSnapshot s; SiNetwork n;
    for (int i = 0; i < 20; i++) { SiTransponder t; t.kind = 1; t.orbitDeg = 19.2; t.freqMhz = 11000 + i * 40; n.transponders.push_back(t); }
    SiTransponder other; other.kind = 1; other.orbitDeg = 13.0; other.freqMhz = 12000; n.transponders.push_back(other);
    SiTransponder cable; cable.kind = 2; cable.freqMhz = 306; n.transponders.push_back(cable);
    s.networks.push_back(n);
    auto g = siGuessOrbit(s, {11001, 11040, 11082, 11120, 14000});
    CHECK(g.valid && std::fabs(g.orbitDeg - 19.2) < 0.01 && g.transponders == 20, "orbit %.1f", g.orbitDeg);
    CHECK(g.matched == 4 && g.carriers == 5, "matched %d of %d", g.matched, g.carriers);
    CHECK(!siGuessOrbit(SiSnapshot(), {}).valid, "no NIT");
    { EchoInfo e; e.delayUs = -6; e.levelDb = -3; e.insideGuard = true; const auto t = echoAdvice(e, 224); CHECK(t.find("pre-echo") != std::string::npos && t.find("1.8 km") != std::string::npos, "%s", t.c_str()); }
    { EchoInfo e; e.delayUs = 300; e.insideGuard = false; CHECK(echoAdvice(e, 224).find("beyond") != std::string::npos, "outside"); }
    { EchoInfo e; e.delayUs = 200; e.insideGuard = true; CHECK(echoAdvice(e, 224).find("close to the end") != std::string::npos, "near edge"); }
    { EchoInfo e; e.delayUs = 10; e.insideGuard = true; CHECK(echoAdvice(e, 224).find("equalises") != std::string::npos, "inside"); }
    return testSummary("dvbs_orbit");
}
