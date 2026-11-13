// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC / RFID at 13.56 MHz: the protocol layer (no DSP). Parses recovered byte frames of ISO/IEC 14443 Type A, Type B
// and ISO/IEC 15693 into named fields, checks the CRCs, and reads NDEF TLVs from Type 2 tag memory. Receive only; nothing here
// transmits and nothing is cloned. Clean-room from public summaries of ISO/IEC 14443-2/-3 and 15693 and the NFC Forum Type 2 / NDEF
// public descriptions; no proprietary or encrypted content is decoded and no cryptography is implemented.
//
// Public sources (parameters and frame shapes only):
//   - ISO/IEC 14443-3 public summaries: REQA 0x26 / WUPA 0x52 short frames, ATQA, anticollision SEL 0x93/0x95/0x97, NVB, BCC,
//     cascade tag 0x88, SAK bits, HLTA 0x50 0x00; CRC_A poly 0x1021 (reflected 0x8408), init 0x6363.
//   - ISO/IEC 14443-3 Type B: REQB/WUPB 0x05, ATQB 0x50, CRC_B init 0xFFFF xorout 0xFFFF.
//   - ISO/IEC 14443-4: RATS 0xE0, ATS structure (TL, T0, TA/TB/TC, historical bytes).
//   - NFC Forum Type 2 Tag and NDEF: READ 0x30, TLV 0x03 (NDEF), record header TNF/flags, type "T" (text) and "U" (URI),
//     the URI identifier abbreviation table.
//   - ISO/IEC 15693: flags, Inventory 0x01, Get System Information 0x2B, 64-bit UID starting 0xE0, CRC-16 (same polynomial as CRC_B).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::nfc {

// ---------------------------------------------------------------- CRCs
// CRC_A (ISO/IEC 14443-3 Type A): poly 0x1021 reflected (0x8408), initial 0x6363, no final xor, transmitted low byte first.
uint16_t crcA(const uint8_t* data, size_t len);
// CRC_B (ISO/IEC 14443-3 Type B and ISO/IEC 15693): poly 0x1021 reflected (0x8408), initial 0xFFFF, final xor 0xFFFF, low byte first.
uint16_t crcB(const uint8_t* data, size_t len);
// True when the last two bytes of the frame are a valid CRC over the bytes before them (frame must be at least 3 bytes).
bool crcAOk(const std::vector<uint8_t>& frame);
bool crcBOk(const std::vector<uint8_t>& frame);

// ---------------------------------------------------------------- Type A
enum class AKind {
    Unknown, Reqa, Wupa, Atqa, AnticollReq, AnticollResp, SelectReq, Sak, Hlta, Rats, Ats, Apdu
};

struct ATypeA {
    AKind kind = AKind::Unknown;
    int bits = 0;                 // frame length in bits (short frames are 7 bits: REQA / WUPA)
    bool fromReader = true;       // a reader (PCD) frame or a tag (PICC) answer
    bool crcPresent = false;      // the frame carries a CRC_A
    bool crcOk = false;
    int cascadeLevel = 0;         // 1..3 for SEL 0x93/0x95/0x97; 0 otherwise
    // ATQA
    uint16_t atqa = 0;
    int uidSizeBits = 0;          // 0 single (4 byte), 1 double (7 byte), 2 triple (10 byte) from ATQA bits 6..7
    // anticollision / select
    std::vector<uint8_t> uidPart; // the 4 UID bytes of this cascade level (the cascade tag 0x88 stripped)
    bool bccOk = false;           // BCC = XOR of the four UID bytes of this level
    bool cascadeTag = false;      // this level's first byte was the cascade tag 0x88 (more levels follow)
    // SAK
    uint8_t sak = 0;
    bool uidComplete = false;     // SAK bit 3 (0x04) clear: UID complete, no further cascade
    bool iso144434 = false;       // SAK bit 6 (0x20) set: the tag supports ISO/IEC 14443-4 (RATS)
    // ATS (answer to select)
    std::vector<uint8_t> historical;
    // generic
    std::vector<uint8_t> raw;     // the whole frame including any CRC
    std::string note;             // a short human description (never decoded content beyond what the clear fields say)
};

// Classify one recovered Type A byte frame (reader or tag). `bits` is the exact bit count (7 for the short frames, else 8*bytes).
ATypeA parseTypeA(const std::vector<uint8_t>& frame, int bits, bool fromReader);

// The number of UID bytes an ATQA announces (4, 7 or 10).
int atqaUidBytes(uint16_t atqa);

// ---------------------------------------------------------------- Type B
struct ATypeB {
    bool valid = false;
    bool isReqb = false;          // APf 0x05 (REQB / WUPB)
    bool isAtqb = false;          // the answer begins with 0x50
    bool wup = false;             // WUPB (PARAM bit 3) rather than REQB
    uint8_t afi = 0;
    int slots = 1;                // number of anticollision slots (REQB PARAM)
    std::vector<uint8_t> pupi;    // 4-byte PUPI from ATQB
    std::vector<uint8_t> appData; // 4 application-data bytes
    std::vector<uint8_t> protoInfo;
    bool crcOk = false;
    std::string note;
};
ATypeB parseTypeB(const std::vector<uint8_t>& frame, bool fromReader);

// ---------------------------------------------------------------- ISO/IEC 15693
enum class VKind { Unknown, InventoryReq, Response, GetSysInfoReq, ReadBlockReq };
struct AVicinity {
    VKind kind = VKind::Unknown;
    bool fromReader = true;
    uint8_t flags = 0;
    bool inventoryFlag = false;
    bool highDataRate = false;    // flags bit 1
    bool oneSubcarrier = true;    // flags bit 0 clear: one subcarrier
    uint8_t command = 0;
    bool error = false;           // response error flag (bit 0 of the response flags)
    uint8_t errorCode = 0;
    std::vector<uint8_t> uid;     // 8 bytes, least significant first on the air; stored here most significant first (0xE0 ...)
    uint8_t dsfid = 0, afi = 0;
    bool haveDsfid = false, haveAfi = false, haveBlocks = false;
    int blockCount = 0, blockSize = 0;
    uint8_t icReference = 0;
    uint8_t manufacturer = 0;     // UID byte after 0xE0
    bool crcOk = false;
    std::string note;
};
AVicinity parseVicinity(const std::vector<uint8_t>& frame, bool fromReader);
// Manufacturer name for an ISO/IEC 7816-6 / 15693 manufacturer code (a small public subset; "" when unknown).
const char* vicinityManufacturer(uint8_t code);

// ---------------------------------------------------------------- NDEF (Type 2 tag memory)
struct NdefRecord {
    uint8_t tnf = 0;
    std::string type;             // "T", "U", "Sp", ...
    std::string text;             // decoded text (TNF well-known "T")
    std::string uri;              // decoded URI (TNF well-known "U"), the abbreviation expanded
    std::string lang;             // language code of a text record
    std::vector<uint8_t> payload;
};
struct NdefMessage {
    bool valid = false;
    std::vector<NdefRecord> records;
    std::string note;
};
// Parse NFC Forum Type 2 tag memory (the bytes read back by READ 0x30, concatenated from block 4 up): find the NDEF TLV (0x03)
// and decode its NDEF message. Lock / memory-control / proprietary TLVs are skipped. Returns valid=false when no NDEF TLV is present.
NdefMessage parseType2Ndef(const std::vector<uint8_t>& mem);
// The NFC Forum URI identifier abbreviation for a prefix code (e.g. 0x01 -> "http://www."), "" for 0x00 and unknown codes.
const char* ndefUriPrefix(uint8_t code);

}  // namespace dect2::nfc
