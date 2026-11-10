// cdma2000 / IS-95 base stations (spreading rate 1, 1.2288 Mchip/s): the forward pilot and the Sync Channel, from the 3GPP2 specifications
// C.S0002-0 (physical layer: pilot PN sequences 3.1.3.1.13, Walsh functions, the K = 9 rate 1/2 code 3.1.3.1.4.1.4, symbol repetition and the
// bit reversal interleaver 3.1.3.1.7), C.S0004-0 (the Sync Channel capsule: SOM bit, MSG_LENGTH, CRC-30, 2.1.1.5.1.2 and 3.1.2.4) and
// C.S0005-0 (the Sync Channel Message, 3.7.2.3.2.26).
//
// Receive only, and only what every base station broadcasts to all mobiles before they know anything about the system: the pilot (PN offset,
// level, Ec/Io) and the Sync Channel Message (system and network identity, pilot PN offset, protocol revisions, system time, leap seconds,
// local time offset, paging channel rate and the frequency of the paging channel). Nothing addressed to a mobile is decoded: the paging,
// access and traffic channels are out of scope, and the long code state of the message is only shown, never used.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::cdma {

constexpr double kChipRate = 1228800.0;
constexpr int kPnLen = 32768;            // chips per pilot PN period = one Sync Channel frame (26.666 ms)
constexpr int kOffsetUnit = 64;          // PILOT_PN is in units of 64 chips (512 offsets)
constexpr double kFs = 2 * kChipRate;    // the receiver's working rate: two samples per chip

using Bits = std::vector<uint8_t>;

// The zero-offset pilot PN sequences (period 2^15) as bits, index 0 = the first '1' after the run of 15 zeros (C.S0002 1.3 / 3.1.3.1.13.1)
const std::vector<uint8_t>& pnI();
const std::vector<uint8_t>& pnQ();
// Walsh function n of length len (Hadamard order, C.S0002 2.1.3.1.8.2), chip k, as a bit (0 -> +1)
inline int walshBit(int n, int k) { return __builtin_popcount((unsigned)(n & k)) & 1; }

// K = 9, rate 1/2 code (g0 = 753, g1 = 561 octal), c0 first; the state continues across calls (the Sync Channel encoder is never reset)
struct ConvEnc { uint32_t sr = 0; void push(int b, Bits& out); };
// Soft decision Viterbi over a whole stream: llr > 0 means code bit 0; 2 values per information bit; free start and free end
Bits viterbiK9(const std::vector<float>& llr);

uint32_t crc30(const Bits& b, size_t n);        // C.S0004 2.1.1.5.1.2: registers start at all ones, the output is inverted
int bro(int v, int m);                           // bit reversal of an m bit value
// The block interleaver of C.S0002 3.1.3.1.7.1.1: transmitted symbol i is input address A(i) = 2^m (i mod J) + BRO_m(i / J)
int interleaveAddr(int i, int m, int J);

struct SyncMsg {                // C.S0005 3.7.2.3.2.26
    int pRev = 6, minPRev = 2;
    int sid = 0, nid = 0;
    int pilotPn = 0;
    uint64_t lcState = 0;       // 42 bits
    uint64_t sysTime = 0;       // 36 bits, 80 ms units since the start of System Time (6 January 1980 00:00:00 UTC)
    int lpSec = 0;              // leap seconds since the start of System Time
    int ltmOff = 0;             // local time offset, two's complement, 30 minute units
    bool daylt = false;
    int prat = 0;               // paging channel rate: 0 9600 bps, 1 4800 bps
    int cdmaFreq = 0, extCdmaFreq = 0;
};
constexpr int kMsgTypeSync = 1;
Bits encodeSyncPdu(const SyncMsg& m);            // MSG_TYPE + the fields + PDU_PADDING (the PDU is 8k + 2 bits)
Bits encapsulate(const Bits& pdu);              // MSG_LENGTH + PDU + CRC-30
bool parseSyncPdu(const Bits& pdu, SyncMsg& m);  // false: not a Sync Channel Message
// Reassembles the capsules of a stream of Sync Channel frames (32 bits each: SOM + 31 bits). Returns the PDUs whose CRC is right.
struct SyncReassembler {
    Bits buf; bool active = false;
    int crcFails = 0;
    void frame(const uint8_t* bits32, std::vector<Bits>& pdus);
};
std::string sysTimeText(uint64_t sysTime80ms, int lpSec, int ltmOff, bool local);   // "2026-11-10 21:14:03.36 UTC"
double cdmaChannelHz(int bandClass, int chan);   // base station transmit frequency (band classes 0, 1, 4), 0 unknown
int cdmaBandClassAt(double hz);                  // -1: none of 0, 1, 4
const char* pratText(int prat);

// ---------------------------------------------------------------- the test transmitter (nothing is transmitted)
struct TxCell {
    SyncMsg msg;                // pilotPn decides the PN offset; sysTime is advanced every message
    double cfoHz = 0;
    double amp = 1;             // relative amplitude of the whole cell
    double pilotDb = -7, syncDb = -16, pagingDb = -12, otherDb = -2;   // channel powers relative to the cell's total
};
// Complex baseband at kFs (two samples per chip), root raised cosine chips. startChip: where in system time the capture begins (chips after
// an even second), so that a test can start in the middle of a frame.
std::vector<cf32> generate(const std::vector<TxCell>& cells, size_t samples, double snrDb, uint64_t startChip = 0, unsigned seed = 1);

// ---------------------------------------------------------------- the search
struct Found {
    double delaySamples = 0;    // where this pilot's PN period (and Sync Channel frame) starts in the block, samples at kFs
    double cfoHz = 0, clockPpm = 0;
    double ecIoDb = -99, pilotDbfs = -200;
    int frames = 0;             // Sync Channel frames decoded in the block
    int msgs = 0, crcFails = 0;
    bool hasMsg = false;
    SyncMsg msg;                // the last good message
    int relOffset = -1;         // PN offset relative to the first pilot of the block, 64 chip units, when no message gave it
};
struct SearchOpts { double maxCfoHz = 5000; int maxCells = 6; double thresh = 2.4; };
std::vector<Found> search(const cf32* x, size_t n, const SearchOpts& o = {}, double* ms = nullptr);

}  // namespace dect2::cdma
