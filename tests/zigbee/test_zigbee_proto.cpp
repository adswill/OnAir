// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 frame layers against values that do not come from our own encoder: the CRC-16 check value of the "Kermit" CRC the
// FCS uses, the 16 chip sequences written out bit by bit as the standard's table 24 prints them (against the table in the source),
// and frames written out byte by byte.
#include "dect2/zigbee/zigbee_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2::zigbee;
#include "../common/test_util.h"

int main() {
    // CRC-16 ITU-T, init 0, reflected (CRC-16/KERMIT): the catalogue's check value for "123456789" is 0x2189
    CHECK(crc16((const uint8_t*)"123456789", 9) == 0x2189, "crc check value 0x%04X", crc16((const uint8_t*)"123456789", 9));
    {   // the table in IEEE 802.15.4-2006 (table 24), chips c0 ... c31 as printed
        static const char* const t[16] = {
            "11011001110000110101001000101110", "11101101100111000011010100100010", "00101110110110011100001101010010", "00100010111011011001110000110101",
            "01010010001011101101100111000011", "00110101001000101110110110011100", "11000011010100100010111011011001", "10011100001101010010001011101101",
            "10001100100101100000011101111011", "10111000110010010110000001110111", "01111011100011001001011000000111", "01110111101110001100100101100000",
            "00000111011110111000110010010110", "01100000011101111011100011001001", "10010110000001110111101110001100", "11001001011000000111011110111000"};
        for (int s = 0; s < 16; s++) {
            std::string got;
            for (int c = 0; c < 32; c++) got += (kChipTable[s] >> (31 - c) & 1) ? '1' : '0';
            CHECK(got == t[s], "chip sequence %d: %s", s, got.c_str());
        }
        // the structure the standard describes: symbols 1 - 7 are the cyclic shifts of symbol 0 by 4 chips, 8 - 15 the same with the odd chips inverted
        for (int s = 1; s < 8; s++) CHECK(kChipTable[s] == ((kChipTable[0] >> (4 * s)) | (kChipTable[0] << (32 - 4 * s))), "shift %d", s);
        for (int s = 0; s < 8; s++) CHECK(kChipTable[s + 8] == (kChipTable[s] ^ 0x55555555u), "conjugate %d", s);
    }
    {   // an acknowledgement, byte by byte: frame control 0x0002 (type 2), sequence 0x5A, FCS
        std::vector<uint8_t> f = {0x02, 0x00, 0x5A};
        const uint16_t c = crc16(f.data(), f.size());
        f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
        CHECK(crc16(f.data(), f.size()) == 0, "remainder over frame and FCS");
        const Frame p = parse(f.data(), f.size());
        CHECK(p.ok && p.type == Ack && p.seq == 0x5A && p.destMode == 0 && p.srcMode == 0, "ack");
    }
    {   // a beacon request, as every scanner sends it: 03 08 <seq> FF FF FF FF 07
        const uint8_t f[] = {0x03, 0x08, 0x2A, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0, 0};
        const Frame p = parse(f, sizeof f);
        CHECK(p.ok && p.type == Command && p.command == 7 && p.destMode == 2 && p.destShort == 0xFFFF && p.destPan == 0xFFFF && p.srcMode == 0 && p.seq == 0x2A, "beacon request");
    }
    {   // a data frame with short addresses and PAN ID compression carrying a Zigbee NWK header: dest 0x0000, src 0x7A12, radius 30, security bit set
        const uint8_t f[] = {0x61, 0x88, 0x21, 0x62, 0x1A, 0x00, 0x00, 0x12, 0x7A,         // MAC: data, ack req, PAN compress, short / short, PAN 0x1A62
                             0x48, 0x02, 0x00, 0x00, 0x12, 0x7A, 0x1E, 0x07,               // NWK: frame control 0x0248 (data, protocol 2, security), dest, src, radius, seq
                             0x28, 0x01, 0x00, 0x00, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D, 0, 0};
        const Frame p = parse(f, sizeof f);
        CHECK(p.ok && p.type == Data && p.destPan == 0x1A62 && p.srcPan == 0x1A62 && p.destShort == 0 && p.srcShort == 0x7A12, "MAC addresses");
        CHECK(p.hasNwk && p.nwkSecured && p.encrypted && p.nwkSrc == 0x7A12 && p.nwkDest == 0 && p.nwkRadius == 30 && p.nwkProtocol == 2, "NWK header");
    }
    {   // a Zigbee beacon payload (15 bytes): protocol ID 0, stack profile 2, protocol version 2, router and end device capacity, extended PAN ID
        const uint8_t f[] = {0x00, 0x80, 0x07,                                               // beacon, short source address
                             0x62, 0x1A, 0x00, 0x00,                                         // PAN, address
                             0xFF, 0xCF, 0x00, 0x00,                                         // superframe: PAN coordinator, association permit; no GTS, no pending addresses
                             0x00, 0x22, 0x84, 0x18, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0x03, 0, 0};
        const Frame p = parse(f, sizeof f);
        CHECK(p.ok && p.type == Beacon && p.zbBeacon && p.stackProfile == 2 && p.nwkVersion == 2 && p.routerCapacity && p.endDeviceCapacity && p.depth == 0, "Zigbee beacon");
        CHECK(p.extPanId == 0x0102030405060718ull && addrText(p.extPanId) == "01:02:03:04:05:06:07:18", "ext PAN %s", addrText(p.extPanId).c_str());
        CHECK(p.panCoordinator && p.assocPermit && p.srcPan == 0x1A62 && p.nwkUpdateId == 3, "superframe");
    }
    {   // an association request: destination short, source extended; command 1, capability 0x8E
        const uint8_t f[] = {0x23, 0xC8, 0x05, 0x62, 0x1A, 0x00, 0x00, 0xFF, 0xFF, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x01, 0x8E, 0, 0};
        const Frame p = parse(f, sizeof f);
        CHECK(p.ok && p.type == Command && p.command == 1 && p.srcMode == 3 && p.srcExt == 0x0102030405060708ull && p.destShort == 0 && p.ackReq, "association request");
        CHECK(vendorOfExt(0x00124B0001020304ull)[0] == 'T' && std::string(vendorOfExt(0x00158D0000000001ull)).find("Lumi") == 0 && vendorOfExt(0x1234560000000001ull)[0] == 0, "vendors");
    }
    {   // a clear NWK data frame with an APS header: cluster 0x0006 (On/Off), profile 0x0104, endpoints 1 -> 1
        const uint8_t f[] = {0x61, 0x88, 0x30, 0x62, 0x1A, 0x34, 0x12, 0x00, 0x00,
                             0x08, 0x00, 0x34, 0x12, 0x00, 0x00, 0x1E, 0x05,
                             0x00, 0x01, 0x06, 0x00, 0x04, 0x01, 0x01, 0x44, 0x11, 0x22, 0, 0};
        const Frame p = parse(f, sizeof f);
        CHECK(p.hasNwk && !p.nwkSecured && p.hasAps && p.apsCluster == 0x0006 && p.apsProfile == 0x0104 && p.apsDestEp == 1 && p.apsSrcEp == 1 && p.apsCounter == 0x44, "APS: cluster 0x%04X", p.apsCluster);
        CHECK(std::string(clusterName(6, 0x0104)) == "On/Off" && std::string(profileName(0x0104)) == "Home Automation", "names");
    }
    printf(fails ? "zigbee proto: %d FAILED\n" : "zigbee proto: all passed\n", fails);
    return fails ? 1 : 0;
}
