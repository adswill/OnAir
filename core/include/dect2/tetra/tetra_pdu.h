// TETRA broadcast PDUs (ETSI EN 300 392-2 clauses 18 and 21): what a base station sends in the clear to every terminal.
//   SYNC (BSCH) with D-MLE-SYNC: system code, colour code, timeslot / frame / multiframe numbers, sharing mode, MCC, MNC
//   SYSINFO (BNCH) with D-MLE-SYSINFO: main carrier, band, offset, duplex spacing, cell access parameters, location area, services
//   ACCESS-ASSIGN (AACH): what each timeslot carries (common control, assigned control, traffic, unallocated)
//   MAC-RESOURCE on the MCCH carrying the MLE D-NWRK-BROADCAST: neighbour cells, network time
// Signalling sent with air interface encryption (MAC-RESOURCE encryption mode not 0) is only counted. Nothing addressed to a terminal
// is kept: no subscriber identity, no call or short data content, no voice. The one exception is the call set-up PDUs (CallEvent): their call
// identifier and end-to-end encryption flag, read to decide whether a call may be decoded at all, and the group (GSSI) a group call is set up for.
#pragma once
#include "tetra_phy.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::tetra {

struct Sync {
    int systemCode = 0, colourCode = 0;
    int tn = 1, fn = 1, mn = 1;          // timeslot 1-4, frame 1-18, multiframe 1-60
    int sharingMode = 0;                 // 0 continuous transmission, 1 carrier sharing, 2 MCCH sharing, 3 traffic carrier sharing
    int tsReservedFrames = 0;
    bool uplaneDtx = false, frame18Ext = false;
    int mcc = 0, mnc = 0;
    int nbrBroadcast = 0;                // D-NWRK-BROADCAST: bit 1 broadcast supported, bit 0 enquiry supported
    int cellLoad = 0;                    // cell service level 0 unknown, 1 low, 2 medium, 3 high load
    bool lateEntry = false;
};
Bits encodeSync(const Sync& s);                 // 60 bits
bool parseSync(const Bits& b, Sync& s);

struct Sysinfo {
    int mainCarrier = 0, band = 0, offset = 0, duplex = 0;
    bool reverse = false;
    int numScch = 0, msTxPwrMax = 0, rxLevAccessMin = 0, accessParam = 0, dlTimeout = 0;
    bool cckFlag = false;                // the 16 bits are the common cipher key identifier (else the hyperframe number)
    int hyperframeOrCck = 0;
    int optFlag = 0;                     // 0 / 1 even / odd multiframe timeslot bitmap, 2 access code A, 3 extended services
    uint32_t optValue = 0;               // 20 bits
    int la = 0, subscriberClass = 0, bsService = 0;   // D-MLE-SYSINFO
};
Bits encodeSysinfo(const Sysinfo& s);           // 124 bits: a whole SCH/HD (BNCH)
bool parseSysinfo(const Bits& b, Sysinfo& s);   // b starts at the MAC PDU type (10 broadcast, 00 SYSINFO)

// BS service details (12 bits, 18.5.2), bit 11 first
enum : int { kSvcRegistration = 1 << 11, kSvcDeregistration = 1 << 10, kSvcPriorityCell = 1 << 9, kSvcNoMinimumMode = 1 << 8,
             kSvcMigration = 1 << 7, kSvcSystemWide = 1 << 6, kSvcVoice = 1 << 5, kSvcCircuitData = 1 << 4, kSvcSndcp = 1 << 2,
             kSvcAirEncryption = 1 << 1, kSvcAdvancedLink = 1 << 0 };
std::string serviceText(int bsService);

// Extended services broadcast (the SYSINFO optional field 3): security information
struct Security { bool valid = false, authRequired = false, class1 = false, class2 = false, class3 = false; };
Security parseSecurity(const Sysinfo& s);

double dlCarrierHz(int band, int carrier, int offset);          // band * 100 MHz + carrier * 25 kHz + offset (0, +6.25, -6.25, +12.5 kHz)
double duplexSpacingHz(int band, int duplex);                   // < 0: reserved
double ulCarrierHz(int band, int carrier, int offset, int duplex, bool reverse);   // 0 when unknown

struct Neighbour {
    int cellId = 0, reselTypes = 0, cellLoad = 0, carrier = 0;
    bool synced = false;
    bool hasExt = false; int band = 0, offset = 0, duplex = 0; bool reverse = false;
    int mcc = -1, mnc = -1, la = -1, maxTx = -1, minRx = -1, subscriberClass = -1, bsService = -1, timeshare = -1, frameOffset = -1;
};
struct NwrkBroadcast {
    int reselect = 0, cellLoad = 0;
    bool hasTime = false; uint64_t time48 = 0;
    std::vector<Neighbour> nbrs;
};
// TETRA network time (18.5.24): "YYYY-MM-DD HH:MM:SS UTC, local +HH:MM"; empty when not valid
std::string networkTimeText(uint64_t time48);

// The call set-up facts of one clear CMCE PDU (EN 300 392-2 clause 14.7): only the call identifier, the end-to-end encryption flag and the
// communication type of the basic service information are read, and the group address of a group call; nothing else of the call. kind: 1 D-SETUP, 2 D-CONNECT, 3 D-DISCONNECT or D-RELEASE.
// enc: 0 the PDU says clear, 1 it says encrypted, -1 not stated. slots: the "timeslot assigned" bit map (bit 3 = timeslot 1) and carrier: the
// carrier number of the channel allocation in the same MAC-RESOURCE, both -1 when it holds no channel allocation. gssi: the group a D-SETUP of a
// group call is sent to, -1 otherwise (a call to a single terminal keeps no address).
struct CallEvent { int kind = 0, callId = 0, enc = -1, slots = -1, carrier = -1, gssi = -1; };

// Everything one decoded signalling block (SCH/HD, SCH/F) of the downlink holds
struct MacBlock {
    bool sysinfo = false; Sysinfo si;
    bool accessDefine = false;
    int resources = 0;          // MAC-RESOURCE PDUs (null PDUs not counted)
    int encrypted = 0;          // of those, sent with air interface encryption: their content is not read
    int fragments = 0;          // fragmented PDUs and their continuations (not reassembled)
    int byPd[8] = {};           // clear MLE layer messages by protocol discriminator (1 MM, 2 CMCE, 4 SNDCP, 5 MLE, 6 management): counted, never read
    std::vector<NwrkBroadcast> nwrk;
    std::vector<CallEvent> calls;   // the call set-up PDUs sent in the clear (never from air interface encrypted PDUs)
};
void parseMacBlock(const Bits& type1, MacBlock& out);

// ACCESS-ASSIGN (21.4.7): the downlink use of the slot in frames 1 - 17: -1 not stated, 0 unallocated, 1 assigned control, 2 common control, 3 reserved, 4 and up traffic
int aachDlUsage(uint16_t aa, bool frame18);
const char* dlUsageText(int usage);

// The generator's PDUs
Bits macNullPdu(int type1Bits);                             // a block holding only a MAC-RESOURCE with the null address
// a MAC-RESOURCE to all terminals (SSI ffffff) carrying an LLC BL-UDATA with this MLE layer payload; encMode != 0 makes the
// payload look encrypted (random bits). The rest of the block (type1Bits long) is filled with null PDUs.
Bits macBroadcastResource(const Bits& tlSdu, int type1Bits, int encMode, uint32_t seed = 1);
Bits mleNwrkBroadcast(const NwrkBroadcast& n);             // protocol discriminator 5, D-NWRK-BROADCAST

} // namespace dect2::tetra
