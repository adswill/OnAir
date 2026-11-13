// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// NFC / RFID protocol parsing (see nfc_proto.h). Clean-room from public specs; receive-only, no cryptography, no cloning.
#include "dect2/nfc/nfc_proto.h"
#include <cstdio>

namespace dect2::nfc {

// ---------------------------------------------------------------- CRCs
// Both CRCs use the reflected CCITT polynomial 0x8408; they differ only in the initial value and the final xor.
static uint16_t crc1021(const uint8_t* data, size_t len, uint16_t init, uint16_t xorout) {
    uint16_t crc = init;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return (uint16_t)(crc ^ xorout);
}
uint16_t crcA(const uint8_t* data, size_t len) { return crc1021(data, len, 0x6363, 0x0000); }
uint16_t crcB(const uint8_t* data, size_t len) { return crc1021(data, len, 0xFFFF, 0xFFFF); }

static bool crcOkWith(const std::vector<uint8_t>& f, uint16_t (*fn)(const uint8_t*, size_t)) {
    if (f.size() < 3) return false;
    const uint16_t c = fn(f.data(), f.size() - 2);
    return (uint8_t)(c & 0xFF) == f[f.size() - 2] && (uint8_t)(c >> 8) == f[f.size() - 1];
}
bool crcAOk(const std::vector<uint8_t>& frame) { return crcOkWith(frame, crcA); }
bool crcBOk(const std::vector<uint8_t>& frame) { return crcOkWith(frame, crcB); }

// ---------------------------------------------------------------- Type A
int atqaUidBytes(uint16_t atqa) {
    const int uidBits = (atqa >> 6) & 0x3;   // ATQA bits 7..6: 0 single, 1 double, 2 triple
    return uidBits == 0 ? 4 : uidBits == 1 ? 7 : 10;
}

ATypeA parseTypeA(const std::vector<uint8_t>& frame, int bits, bool fromReader) {
    ATypeA a;
    a.fromReader = fromReader;
    a.bits = bits ? bits : (int)frame.size() * 8;
    a.raw = frame;
    if (frame.empty()) return a;

    // short frames (7 bits): REQA / WUPA
    if (a.bits == 7 && fromReader) {
        if (frame[0] == 0x26) { a.kind = AKind::Reqa; a.note = "REQA"; return a; }
        if (frame[0] == 0x52) { a.kind = AKind::Wupa; a.note = "WUPA"; return a; }
        a.note = "7-bit short frame";
        return a;
    }

    if (fromReader) {
        const uint8_t c = frame[0];
        if (c == 0x50 && frame.size() >= 2 && frame[1] == 0x00) {
            a.kind = AKind::Hlta; a.crcPresent = true; a.crcOk = crcAOk(frame); a.note = "HLTA (halt)"; return a;
        }
        if (c == 0xE0) {   // RATS
            a.kind = AKind::Rats; a.crcPresent = frame.size() >= 4; a.crcOk = a.crcPresent && crcAOk(frame);
            a.note = "RATS"; return a;
        }
        if (c == 0x93 || c == 0x95 || c == 0x97) {   // SEL CL1 / CL2 / CL3
            a.cascadeLevel = c == 0x93 ? 1 : c == 0x95 ? 2 : 3;
            const uint8_t nvb = frame.size() >= 2 ? frame[1] : 0;
            if (nvb == 0x70 && frame.size() >= 7) {   // full 7-byte SELECT: SEL NVB UID0..3 BCC [CRC]
                a.kind = AKind::SelectReq;
                a.uidPart.assign(frame.begin() + 2, frame.begin() + 6);
                uint8_t bcc = 0; for (uint8_t v : a.uidPart) bcc ^= v;
                a.bccOk = frame[6] == bcc;
                a.cascadeTag = a.uidPart[0] == 0x88;
                a.crcPresent = frame.size() >= 9; a.crcOk = a.crcPresent && crcAOk(frame);
                char b[48]; snprintf(b, sizeof b, "SELECT CL%d", a.cascadeLevel); a.note = b;
            } else {
                a.kind = AKind::AnticollReq;
                char b[48]; snprintf(b, sizeof b, "Anticollision CL%d", a.cascadeLevel); a.note = b;
            }
            return a;
        }
        a.kind = AKind::Apdu; a.crcPresent = frame.size() >= 3; a.crcOk = a.crcPresent && crcAOk(frame);
        a.note = "APDU / block (reader)";
        return a;
    }

    // ---- tag answers ----
    if (a.bits == 16 || (frame.size() == 2 && !fromReader && (frame[0] & 0xF0) != 0x00)) {
        // ATQA is 16 bits with no CRC; heuristic: a two-byte tag answer right after REQA/WUPA
        a.kind = AKind::Atqa;
        a.atqa = (uint16_t)(frame[0] | (frame[1] << 8));
        a.uidSizeBits = (a.atqa >> 6) & 0x3;
        char b[64]; snprintf(b, sizeof b, "ATQA 0x%04X (%d-byte UID)", a.atqa, atqaUidBytes(a.atqa)); a.note = b;
        return a;
    }
    if (frame.size() == 1) {   // SAK (1 byte, CRC usually stripped by the caller when it checked it) or a short answer
        a.kind = AKind::Sak; a.sak = frame[0];
        a.uidComplete = (a.sak & 0x04) == 0;
        a.iso144434 = (a.sak & 0x20) != 0;
        char b[80]; snprintf(b, sizeof b, "SAK 0x%02X%s%s", a.sak, a.uidComplete ? ", UID complete" : ", cascade",
                             a.iso144434 ? ", ISO 14443-4" : ""); a.note = b;
        return a;
    }
    if (frame.size() == 3 && crcAOk(frame)) {   // SAK with CRC
        a.kind = AKind::Sak; a.sak = frame[0]; a.crcPresent = true; a.crcOk = true;
        a.uidComplete = (a.sak & 0x04) == 0; a.iso144434 = (a.sak & 0x20) != 0;
        char b[80]; snprintf(b, sizeof b, "SAK 0x%02X%s%s", a.sak, a.uidComplete ? ", UID complete" : ", cascade",
                             a.iso144434 ? ", ISO 14443-4" : ""); a.note = b;
        return a;
    }
    if (frame.size() == 5) {   // anticollision answer: UID0..3 + BCC
        a.kind = AKind::AnticollResp;
        a.uidPart.assign(frame.begin(), frame.begin() + 4);
        uint8_t bcc = 0; for (uint8_t v : a.uidPart) bcc ^= v;
        a.bccOk = frame[4] == bcc;
        a.cascadeTag = a.uidPart[0] == 0x88;
        a.note = a.cascadeTag ? "UID (cascade tag, more follows)" : "UID";
        return a;
    }
    if (!frame.empty() && (frame[0] & 0xFF) >= 2 && frame.size() >= 2 && frame[0] == frame.size()) {
        // ATS: first byte TL is the length of the whole ATS including itself
        a.kind = AKind::Ats;
        if (frame.size() > 1) a.historical.assign(frame.begin() + 1, frame.end());
        a.note = "ATS"; return a;
    }
    a.kind = AKind::Apdu; a.crcPresent = frame.size() >= 3; a.crcOk = a.crcPresent && crcAOk(frame);
    a.note = "APDU / block (tag)";
    return a;
}

// ---------------------------------------------------------------- Type B
ATypeB parseTypeB(const std::vector<uint8_t>& frame, bool fromReader) {
    ATypeB b;
    if (frame.empty()) return b;
    if (fromReader && frame[0] == 0x05) {   // APf 0x05 = REQB / WUPB
        b.valid = true; b.isReqb = true;
        if (frame.size() >= 3) {
            b.afi = frame[1];
            const uint8_t param = frame[2];
            b.wup = (param & 0x08) != 0;
            b.slots = 1 << (param & 0x07);
        }
        b.crcOk = crcBOk(frame);
        char s[64]; snprintf(s, sizeof s, "%s AFI 0x%02X, %d slot(s)", b.wup ? "WUPB" : "REQB", b.afi, b.slots); b.note = s;
        return b;
    }
    if (!fromReader && frame[0] == 0x50) {   // ATQB
        b.valid = true; b.isAtqb = true;
        if (frame.size() >= 5) b.pupi.assign(frame.begin() + 1, frame.begin() + 5);
        if (frame.size() >= 9) b.appData.assign(frame.begin() + 5, frame.begin() + 9);
        if (frame.size() >= 12) b.protoInfo.assign(frame.begin() + 9, frame.begin() + 12);
        b.crcOk = crcBOk(frame);
        b.note = "ATQB";
        return b;
    }
    return b;
}

// ---------------------------------------------------------------- ISO/IEC 15693
const char* vicinityManufacturer(uint8_t code) {
    switch (code) {
        case 0x01: return "Motorola";
        case 0x02: return "STMicroelectronics";
        case 0x04: return "NXP Semiconductors";
        case 0x05: return "Infineon";
        case 0x07: return "Texas Instruments";
        case 0x16: return "EM Microelectronic";
        case 0x2B: return "Maxim";
        default: return "";
    }
}

AVicinity parseVicinity(const std::vector<uint8_t>& frame, bool fromReader) {
    AVicinity v;
    v.fromReader = fromReader;
    if (frame.size() < 3) return v;   // flags + at least one byte + part of a CRC
    v.crcOk = crcBOk(frame);
    v.flags = frame[0];
    if (fromReader) {
        v.inventoryFlag = (v.flags & 0x04) != 0;
        v.highDataRate = (v.flags & 0x02) != 0;
        v.oneSubcarrier = (v.flags & 0x01) == 0;
        v.command = frame.size() >= 2 ? frame[1] : 0;
        if (v.command == 0x01) { v.kind = VKind::InventoryReq; v.note = "Inventory"; }
        else if (v.command == 0x2B) { v.kind = VKind::GetSysInfoReq; v.note = "Get System Information"; }
        else if (v.command == 0x20 || v.command == 0x23) { v.kind = VKind::ReadBlockReq; v.note = "Read block"; }
        else { v.note = "request"; }
        return v;
    }
    // response
    v.kind = VKind::Response;
    v.highDataRate = (v.flags & 0x02) != 0;
    v.error = (v.flags & 0x01) != 0;
    if (v.error) { v.errorCode = frame.size() >= 2 ? frame[1] : 0; v.note = "error response"; return v; }
    // An inventory response carries DSFID (1) + UID (8) before the CRC. A Get-System-Info response carries more.
    const size_t body = frame.size() >= 2 ? frame.size() - 2 : 0;   // without CRC
    if (body >= 9) {
        // inventory style: flags, DSFID, UID[8]
        size_t p = 1;
        v.dsfid = frame[p++]; v.haveDsfid = true;
        for (int i = 0; i < 8 && p < body; i++) v.uid.push_back(frame[p++]);
        // the UID is transmitted least-significant byte first; store most significant (0xE0) first
        std::vector<uint8_t> msb(v.uid.rbegin(), v.uid.rend());
        v.uid = msb;
        if (v.uid.size() >= 2) v.manufacturer = v.uid[1];
        v.note = "inventory response";
        // Get System Information response: info flags then optional UID/DSFID/AFI/block info/IC ref
        if (p < body) {
            // not an inventory frame after all: re-parse as system info
            v = AVicinity();
            v.fromReader = false; v.kind = VKind::Response; v.flags = frame[0]; v.crcOk = crcBOk(frame);
            size_t q = 1;
            const uint8_t info = frame[q++];
            std::vector<uint8_t> uidLsb;
            for (int i = 0; i < 8 && q < body; i++) uidLsb.push_back(frame[q++]);
            v.uid.assign(uidLsb.rbegin(), uidLsb.rend());
            if (v.uid.size() >= 2) v.manufacturer = v.uid[1];
            if ((info & 0x01) && q < body) { v.dsfid = frame[q++]; v.haveDsfid = true; }
            if ((info & 0x02) && q < body) { v.afi = frame[q++]; v.haveAfi = true; }
            if ((info & 0x04) && q + 1 < body) { v.blockCount = frame[q++] + 1; v.blockSize = (frame[q++] & 0x1F) + 1; v.haveBlocks = true; }
            if ((info & 0x08) && q < body) { v.icReference = frame[q++]; }
            v.note = "system information";
        }
    }
    return v;
}

// ---------------------------------------------------------------- NDEF
const char* ndefUriPrefix(uint8_t code) {
    static const char* t[] = {
        "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:",
        "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://", "nfs://",
        "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:", "pop:", "sip:",
        "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://", "tcpobex://", "irdaobex://",
        "file://", "urn:epc:id:", "urn:epc:tag:", "urn:epc:pat:", "urn:epc:raw:", "urn:epc:",
        "urn:nfc:"
    };
    const size_t n = sizeof(t) / sizeof(t[0]);
    return code < n ? t[code] : "";
}

static NdefRecord parseRecord(const std::vector<uint8_t>& m, size_t& p, size_t end) {
    NdefRecord r;
    if (p >= end) return r;
    const uint8_t hdr = m[p++];
    r.tnf = hdr & 0x07;
    const bool sr = (hdr & 0x10) != 0;   // short record
    const bool il = (hdr & 0x08) != 0;   // ID length present
    if (p >= end) return r;
    const uint8_t typeLen = m[p++];
    uint32_t payLen = 0;
    if (sr) { if (p >= end) return r; payLen = m[p++]; }
    else { if (p + 4 > end) return r; payLen = (uint32_t)((m[p] << 24) | (m[p+1] << 16) | (m[p+2] << 8) | m[p+3]); p += 4; }
    uint8_t idLen = 0;
    if (il) { if (p >= end) return r; idLen = m[p++]; }
    if (p + typeLen > end) return r;
    r.type.assign(m.begin() + p, m.begin() + p + typeLen); p += typeLen;
    p += idLen;   // skip the ID
    if (p + payLen > end) payLen = (uint32_t)(end - p);
    r.payload.assign(m.begin() + p, m.begin() + p + payLen); p += payLen;

    if (r.tnf == 0x01 && r.type == "T" && !r.payload.empty()) {   // well-known text
        const uint8_t status = r.payload[0];
        const int langLen = status & 0x3F;
        if ((int)r.payload.size() >= 1 + langLen) {
            r.lang.assign(r.payload.begin() + 1, r.payload.begin() + 1 + langLen);
            r.text.assign(r.payload.begin() + 1 + langLen, r.payload.end());
        }
    } else if (r.tnf == 0x01 && r.type == "U" && !r.payload.empty()) {   // well-known URI
        r.uri = ndefUriPrefix(r.payload[0]);
        r.uri.append(r.payload.begin() + 1, r.payload.end());
    }
    return r;
}

NdefMessage parseType2Ndef(const std::vector<uint8_t>& mem) {
    NdefMessage msg;
    // Walk the TLV stream. 0x00 NULL, 0x01 lock, 0x02 memory control, 0x03 NDEF, 0xFE terminator, others skipped by length.
    size_t p = 0;
    while (p < mem.size()) {
        const uint8_t tag = mem[p++];
        if (tag == 0x00) continue;
        if (tag == 0xFE) break;   // terminator
        if (p >= mem.size()) break;
        size_t len = mem[p++];
        if (len == 0xFF) { if (p + 1 >= mem.size()) break; len = (mem[p] << 8) | mem[p+1]; p += 2; }   // 3-byte length
        if (tag == 0x03) {        // NDEF message TLV
            const size_t end = p + len <= mem.size() ? p + len : mem.size();
            size_t q = p;
            while (q < end) {
                const size_t before = q;
                NdefRecord r = parseRecord(mem, q, end);
                if (q <= before) break;
                msg.records.push_back(r);
            }
            msg.valid = !msg.records.empty();
            if (!msg.valid) msg.note = "NDEF TLV present but no records decoded";
            return msg;
        }
        p += len;   // skip lock / memory-control / proprietary TLVs
    }
    msg.note = "no NDEF TLV found";
    return msg;
}

}  // namespace dect2::nfc
