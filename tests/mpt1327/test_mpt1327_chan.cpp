// MPT 1327 channel number to frequency: same channel, steps up and down, spacings, offsets, junk inputs.
#include "dect2/mpt1327/mpt1327_chan.h"
#include <cstdio>
using namespace dect2::mpt1327;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    CHECK(channelFreqHz(100, 100, 150.0e6, 12500) == 150.0e6);
    CHECK(channelFreqHz(104, 100, 150.0e6, 12500) == 150.05e6);
    CHECK(channelFreqHz(96, 100, 150.0e6, 12500) == 149.95e6);
    CHECK(channelFreqHz(101, 100, 150.0e6, 25000) == 150.025e6);
    CHECK(channelFreqHz(1023, 0, 400.0e6, 12500) == 400.0e6 + 1023 * 12500.0);   // 10-bit channel numbers
    CHECK(channelFreqHz(-1, 0, 150e6, 12500) == 0);
    CHECK(channelFreqHz(5, -1, 150e6, 12500) == 0);
    CHECK(channelFreqHz(5, 5, 150e6, 0) == 0);
    CHECK(channelFreqHz(5, 5, 0, 12500) == 0);
    CHECK(channelFreqHz(0, 1000, 1000.0, 12500) == 0);   // would go below 0 Hz
    printf(fails ? "FAILED\n" : "mpt1327 chan ok\n");
    return fails ? 1 : 0;
}
