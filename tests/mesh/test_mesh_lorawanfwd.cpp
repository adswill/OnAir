// LoRaWAN receive-only forwarder (round 4 #36): the rxpk JSON (time, freq, datr, codr, rssi, lsnr, size, base64 data), the PUSH_DATA and
// PULL_DATA headers, the EUI, which packets go out (uplinks only, CRC good), and a loopback network server: PUSH_DATA received and acked,
// the PULL_DATA keep-alive, a PULL_RESP downlink ignored.
#include "dect2/mesh/lorawan_fwd.h"
#include "../../core/src/net/tcp_client.h"
#include "../common/test_util.h"
#include <chrono>
#include <cstring>
#include <thread>
using namespace dect2;

int main() {
    MeshPacket p;
    p.protocol = 5; p.crcOk = true; p.mtype = 2; p.raw = "40F17DBE4900020001954378762B11FF0D";
    p.freqHz = 868.1e6 + 312; p.sf = 7; p.bwHz = 125000; p.cr = 5; p.levelDb = -35.4f; p.snrDb = 5.1f;
    const std::string j = semtechRxpk(p, 1'800'000'000'528002LL, 3512348611u, 2);
    CHECK(j == "{\"time\":\"2027-01-15T08:00:00.528002Z\",\"tmst\":3512348611,\"chan\":2,\"rfch\":0,\"freq\":868.100312,\"stat\":1,\"modu\":\"LORA\",\"datr\":\"SF7BW125\",\"codr\":\"4/5\",\"rssi\":-35,\"lsnr\":5.1,\"size\":17,\"data\":\"QPF9vkkAAgABlUN4disR/w0=\"}",
          "rxpk: %s", j.c_str());
    uint8_t eui[8];
    CHECK(semtechParseEui("AA:55:5A:00:00:00:01:01", eui) && eui[0] == 0xAA && eui[7] == 0x01 && !semtechParseEui("AA555A", eui) && !semtechParseEui("ZZ555A0000000101", eui), "EUI");
    semtechParseEui("AA555A0000000101", eui);
    const auto push = semtechPushData(0x1234, eui, "{}");
    CHECK(push.size() == 14 && push[0] == 2 && push[1] == 0x12 && push[2] == 0x34 && push[3] == 0 && push[4] == 0xAA && push[12] == '{', "PUSH_DATA header");
    const auto pull = semtechPullData(0xBEEF, eui);
    CHECK(pull.size() == 12 && pull[3] == 2 && pull[1] == 0xBE, "PULL_DATA");
    MeshPacket down = p; down.mtype = 3;
    MeshPacket bad = p; bad.crcOk = false;
    MeshPacket mesh = p; mesh.protocol = 1;
    MeshPacket join = p; join.mtype = 0;
    CHECK(semtechIsUplink(p) && semtechIsUplink(join) && !semtechIsUplink(down) && !semtechIsUplink(bad) && !semtechIsUplink(mesh), "uplinks only");

    // a network server on the loopback
    netInit();
    sock_t srv = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    CHECK(bind(srv, (sockaddr*)&a, sizeof a) == 0, "bind");
    socklen_t l = sizeof a; getsockname(srv, (sockaddr*)&a, &l);
    SemtechForwarder f;
    std::string err;
    CHECK(f.configure("127.0.0.1", ntohs(a.sin_port), "AA555A0000000101", err), "configure: %s", err.c_str());
    CHECK(!SemtechForwarder().configure("127.0.0.1", 1700, "12", err), "bad EUI refused");
    f.poll(0);            // the keep-alive
    f.forward(p);         // an uplink
    f.forward(down);      // a downlink seen on the air: not forwarded
    sockRecvTimeout(srv, 2000);
    int pushes = 0, pulls = 0;
    std::string body;
    for (int i = 0; i < 3; i++) {
        uint8_t b[2048]; sockaddr_in from{}; socklen_t fl = sizeof from;
        const long long n = recvfrom(srv, (char*)b, sizeof b, 0, (sockaddr*)&from, &fl);
        if (n < 12) continue;
        if (b[3] == 0) { pushes++; body.assign((const char*)b + 12, (size_t)n - 12); uint8_t ack[4] = {2, b[1], b[2], 1}; sendto(srv, (const char*)ack, 4, 0, (sockaddr*)&from, fl); }
        if (b[3] == 2) { pulls++; uint8_t ack[4] = {2, b[1], b[2], 4}; sendto(srv, (const char*)ack, 4, 0, (sockaddr*)&from, fl);
                         const char* resp = "\x02\x00\x00\x03{\"txpk\":{}}"; sendto(srv, resp, 15, 0, (sockaddr*)&from, fl); }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f.poll(1);
    const SemtechStats st = f.stats();
    CHECK(pushes == 1 && pulls == 1 && body.rfind("{\"rxpk\":[{\"time\":", 0) == 0 && body.find("\"data\":\"QPF9vkkAAgABlUN4disR/w0=\"") != std::string::npos, "server got one PUSH_DATA and one PULL_DATA (%d, %d)", pushes, pulls);
    CHECK(st.pushed == 1 && st.acked == 1 && st.pullAcks == 1 && st.pulls == 1, "acks counted; the downlink request ignored");
    sockClose(srv);
    return testSummary("mesh_lorawanfwd");
}
