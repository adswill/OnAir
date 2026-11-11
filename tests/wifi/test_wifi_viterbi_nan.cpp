// The 802.11 Viterbi decoder fed NaN, infinite and huge soft bits (an OFDM symbol equalised by a channel estimate of zero gives 0 / 0):
// every branch lost to the NaN, all metrics stayed at -1e30 and the int cast of the result was undefined (UBSan, fuzz_engine_modes on
// zeros into Wi-Fi and ITS-G5). Such bits count as erasures now: a clean block with some of them still decodes, and the metric is finite.
#include "dect2/wifi/wifi_ofdm.h"
#include <cmath>
#include <limits>
#include <random>
#include <vector>
#include "../common/test_util.h"
using namespace dect2::wifi;

int main() {
    const int n = 240;
    std::mt19937 g(11);
    std::vector<uint8_t> bits(n), out(n);
    for (int i = 0; i < n - 6; i++) bits[i] = (uint8_t)(g() & 1);   // the last 6 bits zero: the code ends in state 0
    const std::vector<uint8_t> c = ofdm::convEncode(bits);
    std::vector<float> soft(c.size());
    for (size_t i = 0; i < c.size(); i++) soft[i] = c[i] ? 1.f : -1.f;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    {   // all NaN: no decode, but a defined result
        std::vector<float> s(2 * n, nan);
        const int m = ofdm::viterbiDecode(s.data(), n, out.data());
        CHECK(m > -2000000001 && m < 2000000001, "metric %d", m);
    }
    {   // huge finite bits: the sums stay finite and the metric defined
        std::vector<float> s = soft;
        for (int k = 0; k < 12; k++) s[(size_t)(g() % s.size())] = k % 2 ? 3e38f : -3e38f;
        const int m = ofdm::viterbiDecode(s.data(), n, out.data());
        CHECK(m > -2000000001 && m < 2000000001, "metric %d with huge soft bits", m);
    }
    {   // a few NaN and infinite bits in a clean block: erasures, the block decodes
        std::vector<float> s = soft;
        for (int k = 0; k < 12; k++) s[(size_t)(g() % s.size())] = k % 2 ? nan : -inf;
        ofdm::viterbiDecode(s.data(), n, out.data());
        int err = 0;
        for (int i = 0; i < n; i++) err += out[i] != bits[i];
        CHECK(err == 0, "%d bit errors with 12 NaN / infinite soft bits", err);
    }
    return testSummary("wifi_viterbi_nan");
}
