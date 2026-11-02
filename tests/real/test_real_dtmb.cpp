// DTMB on the owner's own recording: 554 MHz, Lithuania, HackRF at 10 Msps, int8 IQ; the first 2 MB (0.1 s) of a 12 s capture. A transmitter with PN945
// headers, 16QAM, code rate 0.8, interleaver mode 2, multi-carrier. 0.1 s is enough for header lock and the system information (the whole capture
// decodes with 0 bad codewords; it takes about 0.4 s to the first transport stream packet, too much for a file in the repository).
#include "dect2/dtmb/dtmb_rx.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    const std::string path = std::string(DECT2_SOURCE_DIR) + "/tests/data/dtmb/owner_554mhz_10msps_2MB_iq.s8";
    std::ifstream f(path, std::ios::binary);
    CHECK(f.good(), "cannot open %s", path.c_str());
    if (!f.good()) return testSummary("real dtmb");
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(raw.size() == 2000000, "%zu bytes", raw.size());
    std::vector<cf32> x(raw.size() / 2);
    for (size_t i = 0; i < x.size(); i++) x[i] = cf32((int8_t)raw[2 * i] / 128.f, (int8_t)raw[2 * i + 1] / 128.f);
    DtmbReceiver rx;
    rx.configure(10e6);
    CHECK(rx.ready(), "not ready at 10 Msps");
    std::vector<std::string> log;
    rx.setLogCallback([&](const std::string& m) { log.push_back(m); });
    for (size_t at = 0; at < x.size(); at += 65536) rx.feed(x.data() + at, std::min<size_t>(65536, x.size() - at));
    rx.flush();
    // 0.1 s is shorter than the receiver's first telemetry report, so the check reads its log lines
    std::string pn, si;
    for (const auto& m : log) {
        printf("  [log] %s\n", m.c_str());
        if (m.find("frames found") != std::string::npos) pn = m;
        if (m.find("system information:") != std::string::npos) si = m;
    }
    CHECK(pn.find("PN945") != std::string::npos, "frame header: '%s' (want PN945)", pn.c_str());
    CHECK(si.find("16QAM, rate 0.8, interleaver mode 2") != std::string::npos && si.find("multi-carrier") != std::string::npos, "system information: '%s'", si.c_str());
    const size_t k = pn.find("offset ");
    const double cfo = k == std::string::npos ? 0 : atof(pn.c_str() + k + 7);
    CHECK(std::fabs(cfo + 557) < 150, "carrier offset %.0f Hz (the radio was about -500 Hz off)", cfo);
    return testSummary("real dtmb");
}
