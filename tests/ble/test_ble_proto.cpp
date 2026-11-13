// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising: CRC, whitening, PDU and advertising data parsing (ble_proto.h).
#include "dect2/ble/ble_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2;
using namespace dect2::ble;
#include "../common/test_util.h"

static bool has(const std::string& s, const char* x) { return s.find(x) != std::string::npos; }

int main() {
    CHECK(channelMhz(37) == 2402 && channelMhz(38) == 2426 && channelMhz(39) == 2480 && channelMhz(0) == 2404 && channelMhz(11) == 2428, "channels");
    CHECK(advChannelOf(2426.02) == 38 && advChannelOf(2440) == 0, "advChannelOf");

    // whitening is its own inverse and differs per channel
    uint8_t a[20], b[20];
    for (int i = 0; i < 20; i++) a[i] = b[i] = (uint8_t)(i * 37 + 5);
    whiten(b, 20, 37); CHECK(memcmp(a, b, 20) != 0, "whitening changed nothing");
    whiten(b, 20, 37); CHECK(memcmp(a, b, 20) == 0, "whitening twice");
    uint8_t c[20]; memcpy(c, a, 20); whiten(c, 20, 38); whiten(b, 20, 37);
    CHECK(memcmp(b, c, 20) != 0, "37 and 38 whiten alike");

    // ADV_IND with a name and manufacturer data
    const uint8_t addr[6] = {0x56, 0x34, 0x12, 0xEE, 0xFF, 0xC0};
    auto pdu = makePdu(AdvInd, true, advPayload(addr, AdBuilder().flags(0x06).name("Kitchen").txPower(-8).uuid16({0x180F}).mfr(0x0075, {1, 2, 3}).b));
    Adv o;
    CHECK(parsePdu(pdu.data(), pdu.size(), o), "parse ADV_IND");
    CHECK(o.type == AdvInd && o.txRandom && addrText(o.adv) == "C0:FF:EE:12:34:56", "header / address %s", addrText(o.adv).c_str());
    CHECK(addrKind(true, o.adv) == 1, "random static");
    CHECK(o.name == "Kitchen" && o.nameComplete && o.flags == 6 && o.txPower == -8, "name '%s' flags %d tx %d", o.name.c_str(), o.flags, o.txPower);
    CHECK(o.company == 0x0075 && o.vendor == "Samsung" && o.mfr.size() == 3, "vendor %s", o.vendor.c_str());
    CHECK(o.services.size() == 1 && has(o.services[0], "Battery"), "services");

    // CRC: good, and any one bit wrong fails
    const uint32_t crc = crc24(pdu.data(), pdu.size());
    CHECK(crc < (1u << 24), "24 bits");
    int caught = 0;
    for (size_t i = 0; i < pdu.size() * 8; i++) { auto q = pdu; q[i / 8] ^= (uint8_t)(1 << (i % 8)); if (crc24(q.data(), q.size()) != crc) caught++; }
    CHECK(caught == (int)pdu.size() * 8, "CRC caught %d of %zu flips", caught, pdu.size() * 8);
    CHECK(crc24(pdu.data(), pdu.size(), 0x123456) != crc, "init matters");

    // iBeacon
    const uint8_t uuid[16] = {0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0};
    pdu = makePdu(AdvNonconnInd, false, advPayload(addr, AdBuilder().flags(4).ibeacon(uuid, 100, 7, -59).b));
    CHECK(parsePdu(pdu.data(), pdu.size(), o), "parse iBeacon");
    CHECK(has(o.beacon, "iBeacon E2C56DB5-DFFB-48D2-B060-D0F5A71096E0 major 100 minor 7, -59 dBm"), "iBeacon: %s", o.beacon.c_str());
    CHECK(o.vendor == "Apple: iBeacon", "vendor %s", o.vendor.c_str());

    // Eddystone URL, UID, TLM
    pdu = makePdu(AdvNonconnInd, true, advPayload(addr, AdBuilder().flags(6).eddystoneUrl(-20, 3, "example\x07").b));
    CHECK(parsePdu(pdu.data(), pdu.size(), o), "parse Eddystone URL");
    CHECK(has(o.beacon, "Eddystone URL https://example.com,"), "URL: %s", o.beacon.c_str());
    const uint8_t ns[10] = {0xED, 0xD1, 0xEB, 0xEA, 0xC0, 0x4E, 0x5D, 0xEF, 0xA0, 0x17}, inst[6] = {0, 0, 0, 0, 0, 1};
    pdu = makePdu(AdvNonconnInd, true, advPayload(addr, AdBuilder().eddystoneUid(-20, ns, inst).b));
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && has(o.beacon, "Eddystone UID EDD1EBEAC04E5DEFA017 / 000000000001"), "UID: %s", o.beacon.c_str());
    pdu = makePdu(AdvNonconnInd, true, advPayload(addr, AdBuilder().eddystoneTlm(3000, 21.5, 1234, 56789).b));
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && has(o.beacon, "3000 mV, 21.5 C, 1234 adverts"), "TLM: %s", o.beacon.c_str());
    CHECK(eddystoneUrl((const uint8_t*)"\x00onair\x00", 7) == "http://www.onair.com/", "URL expansion");

    // exposure notification
    const uint8_t rpi[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, meta[4] = {0x40, 8, 0, 0};
    pdu = makePdu(AdvNonconnInd, true, advPayload(addr, AdBuilder().exposure(rpi, meta).b));
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && has(o.beacon, "Exposure Notification, RPI 0102"), "EN: %s", o.beacon.c_str());

    // SCAN_REQ, CONNECT_IND, ADV_EXT_IND
    const uint8_t sc[6] = {1, 2, 3, 4, 5, 0x66};
    std::vector<uint8_t> p(sc, sc + 6); p.insert(p.end(), addr, addr + 6);
    pdu = makePdu(ScanReq, true, p, true);
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && o.type == ScanReq && addrText(o.peer) == "66:05:04:03:02:01" && has(o.summary, "scan request"), "SCAN_REQ %s", o.summary.c_str());
    p.resize(34, 0);
    pdu = makePdu(ConnectInd, true, p, true);
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && has(o.summary, "connection request") && has(o.summary, "not followed"), "CONNECT_IND %s", o.summary.c_str());
    const uint16_t adi = (uint16_t)(2 << 12 | 0x123), aux = (uint16_t)(100 | 1 << 13);
    p = {6, 0x18, (uint8_t)adi, (uint8_t)(adi >> 8), 12, (uint8_t)aux, (uint8_t)(aux >> 8)};
    pdu = makePdu(AdvExtInd, false, p);
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && o.auxChannel == 12 && o.auxPhy == 1 && o.adiSid == 2 && o.adiDid == 0x123, "ADV_EXT_IND %s", o.summary.c_str());

    // malformed: a legacy length out of range, truncated AD
    pdu = makePdu(AdvInd, false, std::vector<uint8_t>(3, 0));
    CHECK(!parsePdu(pdu.data(), pdu.size(), o), "short ADV_IND accepted");
    pdu = makePdu(AdvInd, false, advPayload(addr, {9, 0x09, 'a'}));
    CHECK(parsePdu(pdu.data(), pdu.size(), o) && o.name.empty(), "truncated AD read");

    // the packet on the air: preamble, access address, PDU + CRC
    std::vector<uint8_t> bits;
    pdu = makePdu(AdvInd, true, advPayload(addr, {}));
    packetBits(pdu, 37, bits);
    CHECK(bits.size() == 8 + 32 + (pdu.size() + 3) * 8, "%zu bits", bits.size());
    uint32_t aa = 0; for (int i = 0; i < 32; i++) aa |= (uint32_t)bits[8 + i] << i;
    CHECK(aa == kAdvAccess, "access address %08X", aa);
    CHECK(companyName(0x004C) && companyName(0x0006) && companyName(0x00E0) && !companyName(0x7777), "company table");

    printf(fails ? "ble proto: %d FAILED\n" : "ble proto: all passed\n", fails);
    return fails ? 1 : 0;
}
