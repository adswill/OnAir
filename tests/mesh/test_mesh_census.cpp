// LoRa / Meshtastic network census (mesh_census.h): the LoRa airtime formula against the Semtech calculator's figures, then two
// simulated days of a 20-node mesh (three routers, relayed copies with fewer hops left, CRC failures, an evening peak and a flood
// hour); checks nodes, roles, the fewest hops, unique packets against copies, utilisation, busiest hours and the two insights.
// Simulated only: no real mesh capture was used.
#include "dect2/mesh/mesh_census.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    // Semtech SX1276 calculator: SF7 / 125 kHz / 4/5 / 10 bytes / preamble 8 / CRC / explicit header = 41.216 ms; SF12 (LDRO) = 991.232 ms
    CHECK(std::fabs(loraAirtime(7, 125e3, 5, 10, 8) - 0.041216) < 1e-6, "SF7 %.6f", loraAirtime(7, 125e3, 5, 10, 8));
    CHECK(std::fabs(loraAirtime(12, 125e3, 5, 10, 8) - 0.991232) < 1e-6, "SF12 %.6f", loraAirtime(12, 125e3, 5, 10, 8));
    CHECK(std::fabs(loraAirtime(11, 250e3, 5, 50, 16) - 0.6410) < 0.001, "LongFast 50 B %.4f", loraAirtime(11, 250e3, 5, 50, 16));

    for (unsigned seed = 1; seed <= 3; seed++) {
        std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
        MeshCensus c;
        std::vector<std::pair<std::string, std::string>> ins;
        const double t0 = 1767225600;
        struct N { std::string id, role; int hopsAway; };
        std::vector<N> nodes;
        for (int i = 0; i < 20; i++) {
            char b[16]; snprintf(b, sizeof b, "!%08x", 0x1a2b0000 + i);
            nodes.push_back({b, i < 3 ? (i == 2 ? "ROUTER_LATE" : "ROUTER") : i < 6 ? "CLIENT_MUTE" : "CLIENT", i < 3 ? 0 : i % 4});
        }
        uint64_t sent = 0; double air = 0;
        uint32_t pid = 1;
        double floodAir = 0;
        for (double t = t0; t < t0 + 2 * 86400; t += 30) {
            const int h = (int)std::fmod((t - t0) / 3600, 24);
            const bool flood = t >= t0 + 86400 + 10 * 3600 && t < t0 + 86400 + 11 * 3600;
            const double p = flood ? 0.5 : (h >= 18 && h < 21 ? 0.06 : 0.015);
            for (const auto& n : nodes) {
                if (u(r) >= p) continue;
                sent++; pid++;
                const int nCopies = 1 + (int)(u(r) * 3);
                for (int k = 0; k < nCopies; k++) {
                    MeshCensusFrame f; f.t = t + k * 0.8; f.from = n.id; f.packetId = pid; f.type = u(r) < 0.5 ? "POSITION_APP" : "TEXT_MESSAGE_APP";
                    f.hopStart = 3; f.hopLimit = 3 - n.hopsAway - k; if (f.hopLimit < 0) f.hopLimit = 0;
                    f.role = u(r) < 0.1 ? n.role : "";
                    f.sf = 11; f.bwHz = 250e3; f.cr = 5; f.payloadBytes = flood ? 200 : 40; f.preamble = 16;
                    f.crcOk = u(r) > 0.05;
                    const double a = loraAirtime(11, 250e3, 5, f.payloadBytes, 16);
                    air += a;
                    if (flood) floodAir += a;
                    c.add(f, &ins);
                }
            }
        }
        CHECK(std::fabs(c.totalAirtime() - air) < 1e-6, "seed %u: airtime %.1f / %.1f", seed, c.totalAirtime(), air);
        CHECK(c.nodes().size() == 20 && c.activeNodes(t0 + 2 * 86400, 6 * 3600) == 20, "seed %u: %zu nodes", seed, c.nodes().size());
        // every packet with at least one good copy counted once
        CHECK(c.packets() <= sent && c.packets() > sent * 0.97, "seed %u: %llu packets of %llu sent", seed, (unsigned long long)c.packets(), (unsigned long long)sent);
        const auto roles = c.roles();
        CHECK(roles.count("ROUTER") && roles.at("ROUTER") == 2 && roles.count("ROUTER_LATE") && roles.count("CLIENT"), "seed %u: roles", seed);
        int hopsOk = 0;
        for (const auto& n : c.nodes()) for (const auto& x : nodes) if (x.id == n.id && n.minHops == x.hopsAway) hopsOk++;
        CHECK(hopsOk == 20, "seed %u: %d nodes with the right fewest hops", seed, hopsOk);
        const auto bh = c.busiestHours(3);
        CHECK(bh.size() == 3 && bh[0].first == 10, "seed %u: busiest %d", seed, bh.empty() ? -1 : bh[0].first);   // the flood hour
        int evening = 0; for (const auto& x : bh) if (x.first >= 18 && x.first < 21) evening++;
        CHECK(evening == 2, "seed %u: %d evening hours among the busiest", seed, evening);
        int busy = 0, routers = 0;
        for (const auto& [k, t] : ins) { if (k == "mesh_busy") busy++; if (k == "mesh_router") routers++; }
        CHECK(floodAir / 3600 * 100 > 25, "the flood is %.0f %%", floodAir / 3600 * 100);
        CHECK(busy == 1, "seed %u: %d busy insights", seed, busy);
        CHECK(routers == 3, "seed %u: %d router insights", seed, routers);
        CHECK(c.utilisation(t0 + 86400 + 11 * 3600) > 25 && c.utilisation(t0 + 86400 + 15 * 3600) < 5, "seed %u: utilisation %.1f / %.1f", seed,
              c.utilisation(t0 + 86400 + 11 * 3600), c.utilisation(t0 + 86400 + 15 * 3600));
    }
    return testSummary("mesh_census");
}
