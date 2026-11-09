// TETRA Direct Mode Operation (ETSI EN 300 396-2 and -3): the DM bursts (470 modulation bits in the 510 bit slot), the coding of the DM logical channels (the same mother
// code, CRC and interleaving as TMO, dect2/tetra/tetra_phy.h), the DM colour code, and the layer 2 and layer 3 PDUs a listener reads. Air interface or end-to-end encrypted calls are
// reported and left alone; nothing is ever decrypted.
#pragma once
#include "dect2/tetra/tetra_phy.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::tetradmo {

using tetra::Bits;

constexpr int kBurstBits = 470;
constexpr int kBurstStartBit = 34;                                // the guard period before the burst, in bits of the 510 bit slot
extern const uint8_t kPreambleP1[12], kPreambleP2[12], kPreambleP3[12];   // of the DNB (one channel), DNB (two channels), DSB

// Burst bit fields (0-based in the 470 bits): Table 15 / 16
constexpr int kBkn1At = 14, kTrainAt = 230, kBkn2At = 252;        // DNB
constexpr int kFcAt = 14, kSbAt = 94, kYAt = 214;                  // DSB

Bits dnb(const Bits& bkn1, const Bits& bkn2, bool twoHalves);     // the 470 bits (phase adjustment bits set so that the training is at the right phase)
Bits dsb(const Bits& sb120, const Bits& bkn2216);

uint32_t dccInit(uint32_t mni24, uint32_t src24);                 // scrambling for the DNB channels (EN 300 396-3 clause 8.2.4.1)
inline uint32_t zeroInit() { return tetra::scramblerInit(0, 0, 0); }   // SCH/S and SCH/H of the DSB

// SCH/S (60 bits): Table 21
struct SyncS {
    int systemCode = 0, syncPduType = 0, commType = 0;   // commType 0 direct, 1 via DM-REP, 2 via DM-GATE, 3 via DM-REP / GATE
    bool msFlag = false;                                 // master/slave link (repeater)
    bool gatewayGenerated = false;
    int abUsage = 0, slot = 0, frame = 0;                // slot 1..4 (0 on the air = 1), frame 1..18
    int aiEncState = 0;                                  // 0 clear
    uint32_t tvp = 0;
    int ksg = 0, keyNumber = 0;
};
Bits encodeSyncS(const SyncS& s);
bool parseSyncS(const Bits& b60, SyncS& out);

enum class Dm { Reserved = 0, SdsOccupied = 1, TimingRequest = 2, TimingAck = 3, Setup = 8, SetupPres = 9, Connect = 10, Disconnect = 11, ConnectAck = 12, Occupied = 13, Release = 14, TxCeased = 15, TxRequest = 16, TxAccept = 17, Preempt = 18, PreAccept = 19, Reject = 20, Info = 21, SdsUdata = 22, SdsData = 23, SdsAck = 24, GatewaySpecific = 25, Proprietary = 30 };
const char* dmName(int messageType);

struct DmMessage {
    bool valid = false;
    int repeaterAddress = 0;             // 10 bits (comm type 1)
    int frameCountdown = 0, destType = 0, srcType = 0;
    uint32_t dest = 0, src = 0, mni = 0;
    int messageType = 0;
    bool fillBit = false, fragmented = false;
    int schFSlots = 0;
    // of DM-SETUP / DM-SETUP PRES
    bool setupKnown = false, e2eEncrypted = false, groupCall = true;
    int circuitMode = 0, priority = 0, powerClass = 0;
    bool timingFlag = false, lchFlag = false, preemption = false, twoFrequency = false;
};
// SCH/H of the DSB (124 bits), also DMAC-DATA (see below). commType: from the SCH/S.
Bits encodeSyncH(const DmMessage& m, int commType);
bool parseSyncH(const Bits& b124, int commType, DmMessage& out);

// DMAC-DATA on SCH/F (268 bits) or STCH (124 bits)
struct DmacData {
    bool valid = false, nullPdu = false, secondHalfStolen = false;
    int aiEncState = 0;
    DmMessage msg;
};
bool parseDmacData(const Bits& b, int commType, DmacData& out);
Bits encodeDmacData(const DmacData& d, int commType, int len);

std::string describeMessage(const DmMessage& m);

} // namespace dect2::tetradmo
