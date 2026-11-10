// Mesh coverage map (mesh_coverage.h): simulated direct and relayed receptions with a log-distance path loss and fading.
#include "dect2/mesh/mesh_coverage.h"
#include <cmath>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    for (double fadeDb : {2.0, 6.0}) {
        std::mt19937 r(3); std::normal_distribution<double> g(0, 1); std::uniform_real_distribution<double> u(0, 1);
        MeshCoverage cov(1.0); const double rl = 54.90, ro = 23.90; cov.setReceiver(rl, ro);
        int taken = 0;
        for (int i = 0; i < 4000; i++) {
            const double dkm = 0.3 + 14 * u(r), az = 2 * M_PI * u(r);
            const double lat = rl + dkm * std::cos(az) / 111.32, lon = ro + dkm * std::sin(az) / (111.32 * std::cos(rl * M_PI / 180));
            const double snr = 18 - 30 * std::log10(dkm) + fadeDb * g(r);   // about -16 dB at 14 km
            if (snr < loraSnrFloorDb(11)) continue;   // not decoded
            const bool direct = u(r) < 0.7;
            taken += cov.add(lat, lon, snr, 11, direct, 1700000000 + i);
        }
        CHECK(taken > 1500, "taken %d", taken);
        auto cells = cov.cells();
        int near = 0, nearOk = 0, far = 0; double nearM = 0, farM = 0;
        for (const auto& c : cells) {
            CHECK(c.km >= 0 && c.km < 16, "cell distance %.1f", c.km);
            if (c.count < 2) continue;
            const double expect = 18 - 30 * std::log10(std::max(0.5, c.km));
            if (c.km < 4) { near++; nearM += c.marginDb; nearOk += std::fabs(c.medianSnr - expect) < 3 + fadeDb; }
            else if (c.km > 9) { far++; farM += c.marginDb; }
        }
        CHECK(near > 20 && nearOk >= near * 0.9, "fade %.0f: near cells %d/%d follow the path loss", fadeDb, nearOk, near);
        CHECK(far > 20 && farM / far < nearM / std::max(1, near) - 10, "far cells %d: margin %.1f dB against %.1f near", far, farM / std::max(1, far), nearM / std::max(1, near));
        CoverageCell fc; CHECK(cov.farthest(fc, 2) && fc.km > 11, "farthest %.1f km", fc.km);
    }
    {   // what is not taken: relayed, no position, 0/0, beyond 400 km; the same place twice is one cell
        MeshCoverage c; c.setReceiver(54.9, 23.9);
        CHECK(!c.add(54.91, 23.91, 5, 9, false, 1), "relayed");
        CHECK(!c.add(0, 0, 5, 9, true, 1), "0/0");
        CHECK(!c.add(NAN, 23, 5, 9, true, 1), "nan");
        CHECK(!c.add(60.0, 23.9, 5, 9, true, 1), "567 km away");
        CHECK(c.add(54.9101, 23.9101, 5, 9, true, 1) && c.add(54.9102, 23.9102, 7, 9, true, 2) && c.size() == 1, "one cell");
        auto v = c.cells(); CHECK(v.size() == 1 && v[0].count == 2 && v[0].bestSnr == 7 && std::fabs(v[0].marginDb - (7 - loraSnrFloorDb(9))) < 2.01, "cell values %.1f", v.empty() ? 0 : v[0].marginDb);
        CHECK(loraSnrFloorDb(7) == -7.5 && loraSnrFloorDb(12) == -20 && loraSnrFloorDb(10) == -15, "floors");
    }
    return testSummary("mesh_coverage");
}
