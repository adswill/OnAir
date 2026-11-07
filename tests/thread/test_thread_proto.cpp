// Thread layers above 802.15.4, against frames written out byte by byte (RFC 4944 / RFC 6282 headers, the Thread beacon payload, MLE).
#include "dect2/thread/thread_proto.h"
#include "dect2/zigbee/zigbee_proto.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> dataFrame(const std::vector<uint8_t>& payload, bool secured = false) {
    zigbee::MacBuild b; b.type = zigbee::Data; b.destPan = 0xFFFF; b.destShort = 0xFFFF; b.srcShort = 0x1C00; b.secured = secured; b.payload = payload;
    return zigbee::buildMac(b);
}
static thread::Frame an(const std::vector<uint8_t>& psdu) {
    const zigbee::Frame z = zigbee::parse(psdu.data(), psdu.size());
    return thread::analyse(z, psdu.data(), psdu.size());
}

int main() {
    {   // a Thread beacon: protocol id 3, version 2 in the upper nibble, network name, extended PAN ID
        std::vector<uint8_t> pl = {0xFF, 0xCF, 0x00, 0x00, 0x03, 0x20};
        const char* nm = "OnAirNet";
        for (int i = 0; i < 16; i++) pl.push_back(i < 8 ? (uint8_t)nm[i] : 0);
        for (int i = 0; i < 8; i++) pl.push_back((uint8_t)(0xDE + i));
        zigbee::MacBuild b; b.type = zigbee::Beacon; b.destMode = 0; b.srcShort = 0x4400; b.srcPan = 0xAB12; b.payload = pl;
        const auto f = zigbee::buildMac(b);
        const thread::Frame t = an(f);
        CHECK(t.beacon && t.networkName == "OnAirNet" && t.version == 2 && t.extPan == 0xDEDFE0E1E2E3E4E5ULL, "beacon: %s", t.info.c_str());
        CHECK(t.info.find("Thread beacon") == 0 && t.info.find("joining open") != std::string::npos, "beacon info %s", t.info.c_str());
    }
    {   // link-layer encrypted data: AES-CCM level 5, key id mode 1 (aux header 0D, counter, key index), 12 bytes of ciphertext, 4 bytes of MIC
        std::vector<uint8_t> pl = {0x0D, 0x78, 0x56, 0x34, 0x12, 0x03};
        for (int i = 0; i < 16; i++) pl.push_back((uint8_t)(i * 7));
        const auto f = dataFrame(pl, true);
        const zigbee::Frame z = zigbee::parse(f.data(), f.size());
        CHECK(z.ok && z.secLevel == 5 && z.secKeyMode == 1 && z.secKeyIndex == 3 && z.frameCounter == 0x12345678u && z.payloadLen == 12, "mac aux: level %d mode %d key %d counter %08X len %zu", z.secLevel, z.secKeyMode, z.secKeyIndex, (unsigned)z.frameCounter, z.payloadLen);
        const thread::Frame t = an(f);
        CHECK(t.encrypted && !t.lowpan && t.info.find("encrypted") != std::string::npos && t.info.find("key index 3") != std::string::npos, "enc: %s", t.info.c_str());
    }
    {   // MLE Discovery Request, no security: IPHC 7F 3B, dst multicast 1 byte, UDP NHC with both ports inline, checksum inline, MLE FF 10 (command 16)
        const auto f = dataFrame({0x7F, 0x3B, 0x02, 0xF0, 0x4D, 0x4C, 0x4D, 0x4C, 0x12, 0x34, 0xFF, 0x10, 0x0B, 0x02, 0x20, 0x00});
        const thread::Frame t = an(f);
        CHECK(t.lowpan && t.iphc && t.udp && t.srcPort == 19788 && t.dstPort == 19788 && t.service == "MLE", "mle ports %d %d %s", t.srcPort, t.dstPort, t.service.c_str());
        CHECK(t.mle && !t.mleSecured && t.mleCommand == 16 && t.info.find("Discovery Request") != std::string::npos, "mle: %s", t.info.c_str());
    }
    {   // MLE secured with AES-CCM: security suite 0, the command is not readable
        const auto f = dataFrame({0x7F, 0x3B, 0x02, 0xF0, 0x4D, 0x4C, 0x4D, 0x4C, 0x12, 0x34, 0x00, 0x15, 0x00, 0x00, 0x00, 0x01, 0xAA, 0xBB});
        const thread::Frame t = an(f);
        CHECK(t.mle && t.mleSecured && t.mleCommand == -1 && t.info.find("secured with AES-CCM") != std::string::npos, "mle sec: %s", t.info.c_str());
    }
    {   // mesh header (V=1 F=1 hops 5), FRAG1 (size 300, tag 7), IPHC, UDP NHC with checksum elided and the 4-bit port form (0xF0B1, 0xF0B3)
        const auto f = dataFrame({0xB5, 0x44, 0x00, 0x10, 0x00, 0xC1, 0x2C, 0x00, 0x07, 0x7F, 0x3B, 0x02, 0xF7, 0x13, 0x99});
        const thread::Frame t = an(f);
        CHECK(t.mesh && t.meshHops == 5 && t.frag && t.fragSize == 300 && t.fragTag == 7 && t.fragOffset == 0, "mesh/frag: hops %d size %d tag %d", t.meshHops, t.fragSize, t.fragTag);
        CHECK(t.udp && t.srcPort == 0xF0B1 && t.dstPort == 0xF0B3, "nibble ports %X %X", t.srcPort, t.dstPort);
    }
    {   // a later fragment: FRAGN, offset 5 x 8 bytes
        const auto f = dataFrame({0xE1, 0x2C, 0x00, 0x07, 0x05, 1, 2, 3});
        const thread::Frame t = an(f);
        CHECK(t.frag && t.fragOffset == 40 && t.fragTag == 7 && !t.iphc, "fragn: %s", t.info.c_str());
    }
    {   // CoAP over UDP 5683 (ports inline): CON GET
        const auto f = dataFrame({0x7F, 0x3B, 0x02, 0xF0, 0x16, 0x33, 0x16, 0x33, 0x00, 0x00, 0x40, 0x01, 0x12, 0x34});
        const thread::Frame t = an(f);
        CHECK(t.coap && t.service == "CoAP" && t.info.find("CoAP CON 0.01 GET") != std::string::npos, "coap: %s", t.info.c_str());
    }
    {   // a source address carried inline (SAM=01, 8 bytes) and a 16-byte destination: shown in the line
        const auto g = dataFrame({0x7B, 0x10, 0x3A, 1, 2, 3, 4, 5, 6, 7, 8, 0xFD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
        const thread::Frame t = an(g);
        CHECK(t.iphc && t.nextHeader == 0x3A && t.info.find("src (inline 8 bytes 01:02:03:04:05:06:07:08)") != std::string::npos && t.info.find("dst FD:00") != std::string::npos, "addr: %s", t.info.c_str());
    }
    {   // not 6LoWPAN, and a truncated frame never reads past the end
        CHECK(!an(dataFrame({0x01, 0x02})).lowpan, "plain data");
        const auto f = dataFrame({0x7F});
        (void)an(f);
        const auto h = dataFrame({0xB5, 0x44});
        (void)an(h);
    }
    CHECK(std::string(thread::mleCommandName(10)) == "Parent Response" && std::string(thread::serviceOfPort(61631)).find("Thread Management") == 0 && thread::serviceOfPort(80)[0] == 0, "names");
    return testSummary("thread_proto");
}
