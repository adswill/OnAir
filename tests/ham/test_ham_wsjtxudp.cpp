// WSJT-X UDP message encoding for GridTracker (#708): magic, schema, message type, id as a utf8 QString, field order and byte order.
#include "dect2/ham/ham_wsjtx_udp.h"
#include <cstdio>
using namespace dect2::ham::wsjtx;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint32_t rd32(const std::vector<uint8_t>& v, size_t at) { return ((uint32_t)v[at] << 24) | ((uint32_t)v[at + 1] << 16) | ((uint32_t)v[at + 2] << 8) | v[at + 3]; }

int main() {
    const std::vector<uint8_t> h = encodeHeartbeat("OnAir", "4.7", "a");
    CHECK(h.size() == 12 + 4 + 5 + 4 + 4 + 3 + 4 + 1);
    CHECK(rd32(h, 0) == 0xADBCCBDAu && rd32(h, 4) == 2 && rd32(h, 8) == 0);
    CHECK(rd32(h, 12) == 5 && h[16] == 'O' && h[20] == 'r');            // id: length then utf8 bytes
    CHECK(rd32(h, 21) == 2);                                            // maximum schema
    CHECK(rd32(h, 25) == 3 && h[29] == '4');                            // version string
    DecodeMsg m; m.msSinceMidnight = 45015000; m.snr = -12; m.dt = 0.5; m.df = 1234; m.text = "CQ DX K1ABC FN42";
    const std::vector<uint8_t> d = encodeDecode("OnAir", m);
    CHECK(rd32(d, 8) == 2);
    size_t at = 12 + 4 + 5;
    CHECK(d[at] == 1);                                                  // new
    CHECK(rd32(d, at + 1) == 45015000);                                 // time
    CHECK((int32_t)rd32(d, at + 5) == -12);                             // snr, signed
    CHECK(d[at + 9] == 0x3F && d[at + 10] == 0xE0 && d[at + 11] == 0);  // 0.5 as a big-endian IEEE double: 3FE0 0000 0000 0000
    CHECK(rd32(d, at + 17) == 1234);                                    // audio Hz
    CHECK(rd32(d, at + 21) == 1 && d[at + 25] == '~');                  // mode "~" (FT8)
    CHECK(rd32(d, at + 26) == 16 && d[at + 30] == 'C');                 // message
    CHECK(d.size() == at + 30 + 16 + 2);                                // low confidence, off air
    CHECK(modeChar("FT8") == "~" && modeChar("FT4") == "+" && modeChar("WSPR").empty());
    StatusMsg s; s.dialHz = 14074000; s.deCall = "LY1ABC";
    const std::vector<uint8_t> st = encodeStatus("OnAir", s);
    CHECK(rd32(st, 8) == 1);
    CHECK(rd32(st, 21) == 0 && rd32(st, 25) == 14074000);               // dial frequency, quint64
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("ok\n");
    return 0;
}
