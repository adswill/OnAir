// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Radio-control and mesh links, packet layer (no radio): Reticulum over RNode LoRa, ExpressLRS, MAVLink inside SiK radio packets, FrSky D8 / D16 and
// FlySky AFHDS / AFHDS 2A. Receive only: nothing here builds a command for a transmitter. Facts and sources are in docs/MESH-RC-3.8.md.
//
//   Reticulum: RNS/Packet.py (header), RNS/Identity.py (announce), RNode_Firmware (the one byte split header). Data stays encrypted: the destination, hops
//   and sizes are shown; an announce is checked with its own public key.
//   ExpressLRS: src/lib/OTA/OTA.h / OTA.cpp. Frames are bound to the binding phrase through the CRC seed and the hop sequence. Without a phrase the
//   receiver shows structure only (the SYNC frames carry the link id, rate and hop index in clear); RC channels are unpacked only with the phrase the user typed.
//   MAVLink: v1 / v2 framing and CRC_EXTRA (mavlink.io); SiK: Firmware/radio/radio_443x.c, packet.c, golay.c, tdm.c.
//   FrSky and FlySky: reverse-engineered by the DIY-Multiprotocol-TX-Module project (FrSkyD_cc2500.ino, FrSkyX_cc2500.ino, FlySky_a7105.ino, AFHDS2A_a7105.ino).
//   Futaba S-FHSS and Spektrum DSM2 / DSMX (3.9): the same project, Futaba_cc2500.ino (SFHSS_*), DSM_cyrf6936.ino and DSM.ino. See docs/RCLINK-3.9.md.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace rclink {

enum Proto { Reticulum = 1, Elrs = 2, Sik = 4, Frsky = 8, Flysky = 16, Sfhss = 32, Dsm = 64 };
const char* protoName(int p);

// one decoded frame, protocol independent, as the tables show it
struct Decoded {
    bool ok = false;           // a valid frame of this protocol (CRC, signature or strict format check passed)
    int proto = 0;
    std::string id;            // who sent it: destination hash, link id, net id, transmitter id
    std::string type;          // "ANNOUNCE", "SYNC", "HEARTBEAT", "D8 channels", ...
    std::string detail;        // one line about the content
    bool encrypted = false;    // the payload stays opaque (structure only)
    bool hasPos = false;
    double lat = 0, lon = 0, altM = 0;
};

std::string hexOf(const uint8_t* p, size_t n);

// ---- Reticulum ----
struct RnodeReassembly {       // the split packets of one RNode
    int seq = -1;
    std::vector<uint8_t> first;
};
// One LoRa payload of an RNode: strips the header byte and joins a two-part split. false: only the first half so far (nothing to parse yet).
bool rnodeUnframe(const uint8_t* p, size_t n, RnodeReassembly& st, std::vector<uint8_t>& packet);
Decoded reticulumParse(const uint8_t* p, size_t n);
// builders for the tests and known-answer checks
std::vector<uint8_t> reticulumAnnounce(const uint8_t seed[32], const uint8_t x25519Pub[32], const std::string& fullName, const uint8_t randomHash[10],
                                       const std::vector<uint8_t>& appData, int hops, std::array<uint8_t, 16>* destOut = nullptr);

// ---- ExpressLRS ----
inline constexpr int kElrsOtaVersion = 4;      // OTA_VERSION_ID of the 3.x / 4.x firmware
inline constexpr uint16_t kElrsCrc14Poly = 0x2E57, kElrsCrc16Poly = 0x3D65;
inline constexpr size_t kElrsOta4 = 8, kElrsOta8 = 13;
// "-DMY_BINDING_PHRASE=\"phrase\"" through MD5, first six bytes (python/build_flags.py)
std::array<uint8_t, 6> elrsUidFromPhrase(const std::string& phrase);
uint16_t elrsCrc(int bits, uint16_t poly, const uint8_t* p, size_t n, uint16_t init);
uint16_t elrsCrcInit(uint8_t uid4, uint8_t uid5);

struct ElrsLink {              // what is known about a link: from its SYNC frames, or from the phrase
    bool known = false, phrase = false;
    uint8_t uid4 = 0, uid5 = 0;
    std::array<uint8_t, 6> uid{};
};
// n is 8 or 13. A SYNC frame checks itself (the UID bytes it carries seed its CRC). Any other frame needs a known link and passes when one of the 256 nonces
// makes the CRC right. Channels are only unpacked when link.phrase is set.
Decoded elrsParse(const uint8_t* p, size_t n, ElrsLink& link);

// ---- MAVLink ----
// CRC_EXTRA of a message id; -1: not in the table
int mavCrcExtra(uint32_t msgId);
const char* mavName(uint32_t msgId);
uint16_t mavX25(const uint8_t* p, size_t n, uint16_t crc = 0xFFFF);
// Finds and checks the MAVLink v1 / v2 frames in a byte stream; `used` = bytes consumed (keep the rest for the next packet).
std::vector<Decoded> mavlinkScan(const uint8_t* p, size_t n, size_t* used = nullptr);
std::vector<uint8_t> mavlinkFrame(int version, uint32_t msgId, int seq, int sysId, int compId, const std::vector<uint8_t>& payload);

// ---- SiK ----
uint16_t sikCrc16(const uint8_t* p, size_t n);              // radio crc.c
void sikGolayEncode(const uint8_t* in, size_t n, std::vector<uint8_t>& out);      // n a multiple of 3
bool sikGolayDecode(const uint8_t* in, size_t n, std::vector<uint8_t>& out, int* corrected);    // n a multiple of 6
struct SikPacket {
    bool ok = false, golay = false;
    int netId = -1;
    std::vector<uint8_t> data;   // the payload without the two trailer bytes
    int window = 0;
    bool command = false, bonus = false, resend = false;
    int corrected = 0;
};
// body = the bytes after the two sync bytes. Non-ECC: header (2 net id bytes), length, data + trailer, CRC-16. ECC: Golay coded net id / length / CRC, then Golay coded data.
SikPacket sikParse(const uint8_t* body, size_t n);
std::vector<uint8_t> sikBuild(int netId, const std::vector<uint8_t>& data, int window, bool golay);

// ---- FrSky ----
uint16_t cc2500Crc(const uint8_t* p, size_t n);             // CRC-16 of the CC2500 packet handler
uint16_t frskyXCrc(const uint8_t* p, size_t n, uint16_t init = 0);
// after the sync word: length byte, packet
Decoded frskyD8Parse(const uint8_t* p, size_t n);
Decoded frskyXParse(const uint8_t* p, size_t n);
std::vector<uint8_t> frskyD8Build(uint16_t txId, uint8_t counter, const uint16_t ch[8]);
std::vector<uint8_t> frskyXBuild(uint16_t txId, uint8_t hopIdx, const std::vector<uint8_t>& body);

// ---- FlySky ----
Decoded flyskyParse(const uint8_t* p, size_t n);            // 21 bytes (AFHDS) or 38 bytes (AFHDS 2A); no CRC on the air: a strict format check
std::vector<uint8_t> flyskyBuild(uint32_t txId, const uint16_t ch[8]);
std::vector<uint8_t> afhds2aBuild(uint32_t txId, uint32_t rxId, const uint16_t ch[14]);

// ---- Futaba S-FHSS (CC2500 2-FSK 128 kbit/s, sync D3 91, 13 byte packets) ----
// p = the 13 packet bytes and the two CRC bytes. Multiprotocol sets the CC2500's CC2400_EN bit (PKTCTRL0 0x0C), so the CRC is the CC2400 one; both
// candidate CRCs are tried and the one that matched is reported (see the doc). The frame must also pass a strict format check.
Decoded sfhssParse(const uint8_t* p, size_t n);
// ch: four 12 bit values (1020 .. 2020, 1520 the middle), rfCh 0..29, fhssCode 0..27, command bits (bit 0 second packet, bit 2 failsafe, bit 3 CH5-8)
std::vector<uint8_t> sfhssBuild(uint16_t txId, int rfCh, int fhssCode, int command, const uint16_t ch[4]);
uint16_t sfhssCrc(const uint8_t* p, size_t n);   // the CRC the generator appends (CRC-16 CCITT, init 0xFFFF)
int sfhssNextChannel(int ch, int fhssCode);       // SFHSS_calc_next_chan

// ---- Spektrum DSM2 / DSMX (CYRF6936 64 chip 8DR DSSS at 1 Mchip/s; rc_dsm.cpp) ----
struct DsmCodes { uint64_t sop = 0, data0 = 0, data1 = 0; };      // bit j = chip j (sent first)
DsmCodes dsmCodes(int row, int sopCol);                           // pn row 0..4, SOP column 0..7
uint64_t dsmSymbol(const DsmCodes& k, uint8_t v);                 // the 64 chips of one byte
int dsmDespread(const DsmCodes& k, uint64_t chips, int* errors);  // the byte whose symbol is closest, and its chip errors
uint16_t dsmCrc(const uint8_t* p, size_t n, uint16_t seed);       // CRC-16 0x8005 (the generator's choice: the real one is unpublished, never checked)
int dsmSopColumn(const uint8_t id[4]);                            // (id0 + id1 + id2 + 2) & 7
int dsmPnRow(int rfChannel, bool dsmx);
std::vector<int> dsmxChannels(const uint8_t id[4]);               // the 23 hop channels of a DSMX transmitter (2400 + n MHz)
// the 16 byte data packet; idx[i] < 0: an unused word (FFFF); bits 11 or 10
std::vector<uint8_t> dsmDataPacket(const uint8_t id[4], bool dsmx, bool upper, int bits, const int idx[7], const uint16_t val[7]);
// the chips of a whole packet: preamble 33 33 04, SOP code and its complement, length, packet, CRC
std::vector<uint8_t> dsmChips(const DsmCodes& k, const std::vector<uint8_t>& packet, uint16_t crcSeed);
// p = the 16 despread data bytes; rfChannel and row (-1: unknown) tell DSM2 from DSMX
Decoded dsmParse(const uint8_t* p, size_t n, int rfChannel, int row);

} // namespace rclink
} // namespace dect2
