// The UMTS mode end to end on the test signal: the receiver thread finds both cells and the mode is registered.
#include "dect2/umts/umts_rx.h"
#include "dect2/umts/umts_cell.h"
#include <chrono>
#include <cstdio>
#include <thread>
using namespace dect2;
int main() {
    auto s = umtsGenerate({{17, 800, 1, 500, 0.4}, {280, 3000, 0.7, 9000, 0.3}}, 38400 * 2 * 8, 12);
    UmtsReceiver rx; rx.configure(7.68e6);
    if (!rx.ready()) { printf("FAIL: not ready\n"); return 1; }
    for (int r = 0; r < 5; r++) for (size_t o = 0; o < s.size(); o += 8192) rx.feed(s.data() + o, std::min<size_t>(8192, s.size() - o));
    std::this_thread::sleep_for(std::chrono::seconds(4));
    UmtsTelemetry t; rx.telemetry(t, 0);
    printf("%zu cells, scans %llu\n", t.cells.size(), (unsigned long long)t.scans);
    bool a = false, b = false; for (auto& c : t.cells) { if (c.psc == 17) a = true; if (c.psc == 280) b = true; }
    if (!a || !b) { printf("FAIL: cells\n"); return 1; }
    ModeTuning m = umtsTuning();
    if (m.stdMode != 482 || m.minMhz >= m.defMhz) { printf("FAIL: tuning\n"); return 1; }
    printf("umts rx passed\n");
    return 0;
}
