// Home network health (homenet_health.h): a simulated Z-Wave network (controller 1, nodes 2-5) with lost frames, retries and a fading node.
#include "dect2/zwave/homenet_health.h"
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    for (double baseLoss : {0.0, 0.03}) {
        std::mt19937 r(2); std::uniform_real_distribution<double> u(0, 1); std::normal_distribution<double> g(0, 1);
        HomeNetHealth h; int seq = 0;
        for (double t = 0; t < 4 * 3600; t += 60) {   // each node reports every 5 min; node 4 is behind a wall: 40 % of its frames lost; node 5 fades after 2 h and dies at 3 h
            for (int n = 2; n <= 5; n++) {
                const double tt = t + n * 7;
                if (((int)(t / 60) + n) % 5) continue;
                if (n == 5 && t > 3 * 3600) continue;
                const double loss = n == 4 ? 0.4 : baseLoss;
                HomeNetFrame f; f.src = std::to_string(n); f.dst = "1"; f.seq = (seq++) & 0xFF; f.ackReq = true; f.rssi = (float)(-60 - (n == 5 && t > 2 * 3600 ? 12 : 0) + g(r));
                for (int tries = 0; tries < 3; tries++) {   // the node retries with the same sequence number until acknowledged
                    f.t = tt + tries * 0.5;
                    h.add(f);
                    if (u(r) < loss) continue;   // the frame (or its ack) is lost
                    HomeNetFrame a; a.t = f.t + 0.02; a.src = "1"; a.dst = f.src; a.seq = f.seq; a.isAck = true; a.rssi = -50; h.add(a);
                    break;
                }
            }
            h.tick(t + 59);
        }
        const auto& N = h.nodes();
        CHECK(N.count("2") && N.count("4") && N.count("5"), "nodes");
        const auto& n2 = N.at("2"); const auto& n4 = N.at("4"); const auto& n5 = N.at("5");
        CHECK(std::fabs(n2.intervalSec - 300) < 20, "interval %.0f", n2.intervalSec);
        CHECK(!n2.lossy && !n2.silent && !n2.weakening && n2.text.empty(), "loss %.2f: node 2 healthy '%s'", baseLoss, n2.text.c_str());
        CHECK(n4.lossy && n4.retryPct() > 20 && n4.ackMissing > 0, "node 4 lossy: %.0f %% retries, %llu missing", n4.retryPct(), (unsigned long long)n4.ackMissing);
        CHECK(n5.silent && n5.text.find("silent") != std::string::npos, "node 5 silent '%s'", n5.text.c_str());
        CHECK(n5.weakening, "node 5 weakening %.1f -> %.1f", n5.rssiFirst, n5.rssiNow);
        CHECK(N.at("1").frames > 0 && !N.at("1").silent, "controller acks counted, never silent from acks alone");
    }
    {   // Zigbee-like frames without sequence numbers: no retry or ack figures, the level and silence still work
        HomeNetHealth h;
        for (int i = 0; i < 30; i++) { HomeNetFrame f; f.t = i * 120; f.src = "0x1a2b"; f.dst = "0x0000"; f.rssi = -55; h.add(f); }
        h.tick(30 * 120 + 1200);
        const auto& n = h.nodes().at("0x1a2b");
        CHECK(n.retries == 0 && n.ackAsked == 0 && n.silent, "zigbee: silent only");
    }
    return testSummary("zwave_homenet");
}
