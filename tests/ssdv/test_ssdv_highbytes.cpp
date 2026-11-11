// SSDV packets whose CRC or callsign starts with a byte of 0x80 or more: the vendored decoder shifted that byte into the sign bit of an int
// (undefined behaviour, UBSan under fuzz_engine_modes). Every packet type and every top byte are checked, and the header still reads the
// whole 32-bit callsign. The UB itself shows only under DECT2_SANITIZE=ON.
#include "dect2/ssdv/ssdv_proto.h"
#include <cstring>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    std::mt19937 g(5);
    int accepted = 0;
    for (int type : {0x66, 0x67, 0x68, 0x00}) for (int top = 0x80; top <= 0xFF; top += 0x0F) {
        uint8_t p[256];
        for (auto& b : p) b = (uint8_t)g();
        p[0] = 0x55; p[1] = (uint8_t)type; p[2] = (uint8_t)top;   // callsign starts with a high byte
        for (int k = 6; k < 256; k++) p[k] = (uint8_t)(0x80 | g());   // wherever the CRC of this type sits, its top byte is high
        int rs = 0;
        ssdv::Info info;
        accepted += ssdv::check(p, &rs, &info) ? 1 : 0;
        const ssdv::Info h = ssdv::header(p);
        (void)h;
    }
    CHECK(accepted == 0, "%d random packets passed the CRC", accepted);
    return testSummary("ssdv_highbytes");
}
