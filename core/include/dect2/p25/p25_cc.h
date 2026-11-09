// P25 Phase 1 control messages beyond the voice grants: Motorola and Harris patches, affiliation and registration, deny / queue / acknowledge responses,
// data channel grants, system services, time, and the multi-block (MBT) forms. Layouts: OP25 tk_p25.py and p25p1_fdma.cc, reason codes: SDRTrunk DenyReason /
// QueuedResponseReason (both GPL-3 projects; layouts only).
#pragma once
#include "dect2/p25/p25_proto.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace p25 {

enum CcKind { kCcNone = 0, kCcPatchAdd, kCcPatchDel, kCcPatchGrant, kCcAffiliation, kCcRegistration, kCcDeregistration, kCcDeny, kCcQueue, kCcAck, kCcDataGrant, kCcStatus, kCcOther };

struct CcItem {
    int kind = kCcNone;
    int supergroup = -1;              // patches
    std::vector<int> members;         // patch members (non-zero)
    int unit = -1, tg = -1, channel = -1, channel2 = -1;
    bool accepted = false;            // affiliation / registration result
    int reason = -1, service = -1;
    std::string text;                 // the event line
};

// One TSBK that is not a voice grant or a status broadcast already handled by the receiver. False when it holds none of these.
bool tsbkDescribe(int mfid, int opcode, uint64_t args, CcItem& out);

const char* denyReasonText(int r);
const char* queueReasonText(int r);
const char* serviceText(int s);       // the ISP / OSP a response refers to

// A rate 1/2 PDU in 12-byte blocks: 'blocks' = header + data blocks. Multi-block trunking (SAP 61, format 0x15 or 0x17) is checked with its CRC-32 and turned
// into the equivalent TSBKs where a short form exists, so the control channel logic treats it the same. False when the CRC or the layout is wrong.
bool mbtToTsbks(const std::vector<std::vector<uint8_t>>& blocks, std::vector<Tsbk>& out);

// A data PDU in 12-byte blocks: payload (padding and the CRC-32 removed, the CRC checked), its service access point and a readable summary
// (IPv4 addresses, UDP ports and any printable text). Only unconfirmed rate 1/2 PDUs are decoded here.
struct PduData { std::vector<uint8_t> payload; int sap = -1, llid = -1; bool outbound = false, crcOk = false; std::string summary; };
bool pduAssemble(const std::vector<std::vector<uint8_t>>& blocks, PduData& out);
uint32_t crc32Pdu(const uint8_t* p, size_t n);

} // namespace p25
} // namespace dect2
