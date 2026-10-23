// Cell towers: ASN.1 unaligned PER of the broadcast system information (3GPP TS 36.331 for LTE, 38.331 for NR, 44.018 for GSM is
// in cell_gsm.h). Only the cell's public broadcast is read: SIB1 (operator, area, cell identity, scheduling), SIB2 (common radio
// configuration), SIB3/4/5 (reselection and neighbours). Nothing subscriber-related exists in these messages.
#pragma once
#include "cell_fec.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::cell {

class BitReader {
public:
    explicit BitReader(const Bits& b, size_t start = 0) : b_(b), p_(start) {}
    uint32_t u(int n);                // n <= 32 bits; past the end reads as 0 and sets the error flag
    bool bit() { return u(1) != 0; }
    int range(int lo, int hi);        // constrained whole number
    void skip(size_t n) { p_ += n; if (p_ > b_.size()) err_ = true; }
    bool skipExtensions();            // a SEQUENCE's extension additions (after its root): skip the open types
    size_t pos() const { return p_; }
    bool ok() const { return !err_; }
    void fail() { err_ = true; }
private:
    const Bits& b_;
    size_t p_;
    bool err_ = false;
};

class BitWriter {
public:
    void u(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b_.push_back((v >> i) & 1); }
    void bit(bool v) { b_.push_back(v ? 1 : 0); }
    void range(int v, int lo, int hi);
    Bits& bits() { return b_; }
    void padOctet() { while (b_.size() % 8) b_.push_back(0); }
private:
    Bits b_;
};
int bitsFor(long nValues);            // bits of a constrained whole number with this many values

struct Plmn { int mcc = -1, mnc = -1, mncDigits = 2; bool reserved = false; };
std::string plmnText(const Plmn& p);  // "246-01"

struct LteSib1 {
    std::vector<Plmn> plmns;
    uint32_t tac = 0;                 // 16 bits
    uint32_t cellId = 0;              // 28 bits: eNB id (20) and cell (8)
    bool barred = false, intraFreqReselAllowed = true, csgIndication = false;
    int qRxLevMinDbm = -140, qRxLevMinOffset = 0;
    int pMax = -99;                   // dBm, -99 = absent
    int band = 0;                     // freqBandIndicator: 1..64, or 65..256 from freqBandIndicator-v9e0 (the root field then says 64)
    std::vector<int> multiBands;      // multiBandInfoList (v8h0) with the v9e0 bands above 64 filled in: the other bands this cell is also in (MFBI)
    bool imsEmergency = false;        // ims-EmergencySupport-r9 (v920)
    struct Sched { int periodRf = 8; std::vector<int> sibs; };   // sibs: 3 = SIB3 ...
    std::vector<Sched> sched;
    bool tdd = false; int sfAssign = 0, specialSf = 0;
    int siWindowMs = 0, valueTag = 0;
};

struct LteSib2 {
    bool acBarring = false, acEmergencyBarred = false, acSignallingBarred = false, acDataBarred = false;
    int numRaPreambles = 0, preambleTransMax = 0, raRespWindow = 0;
    int modPeriodCoeff = 0, pagingCycleRf = 0;
    int rootSeqIndex = 0, prachConfigIndex = 0, prachFreqOffset = 0; bool highSpeed = false;
    int refSignalPowerDbm = 0, pb = 0;
    bool enable64qam = false;
    int n1PucchAn = 0;
    int p0NominalPusch = 0, alpha = 0, p0NominalPucch = 0;
    int t300 = 0, t301 = 0, t310 = 0, n310 = 0, t311 = 0, n311 = 0;   // ms / counts
    int ulEarfcn = -1, ulBwRb = 0, addSpectrumEmission = 1;
    int mbsfnConfigs = 0;
    int timeAlignMs = 0;              // -1 infinity
    bool extended = false;            // later-release additions were present (skipped)
};

struct LteSib3 { int qHystDb = 0, threshServingLow = 0, priority = 0, qRxLevMinDbm = 0, sIntraSearch = -1, sNonIntraSearch = -1, tReselSec = 0; };
struct LteNeigh { int pci = 0, qOffsetDb = 0; };
struct LteSib4 { std::vector<LteNeigh> neigh; std::vector<std::pair<int, int>> black; };   // black: start, count
struct LteSib5Freq { int earfcn = 0, qRxLevMinDbm = 0, priority = -1, threshHigh = 0, threshLow = 0, bwRb = 0, qOffsetFreqDb = 0; std::vector<LteNeigh> neigh; };
struct LteSib5 { std::vector<LteSib5Freq> freqs; };

struct LteBcch {
    bool isSib1 = false;
    LteSib1 sib1;
    bool has2 = false, has3 = false, has4 = false, has5 = false;
    LteSib2 sib2; LteSib3 sib3; LteSib4 sib4; LteSib5 sib5;
    std::vector<int> sibsSeen;        // SIB numbers in an SI message (also those not read)
};

// BCCH-DL-SCH-Message: false when it does not parse
bool lteParseBcch(const Bits& msg, LteBcch& out);
// Encoders (the test signal): the complete BCCH-DL-SCH-Message, padded to whole octets
Bits lteEncodeSib1(const LteSib1& s);
Bits lteEncodeSi(const LteSib2* s2, const LteSib3* s3, const LteSib4* s4, const LteSib5* s5);

int qOffsetDb(int idx);               // Q-OffsetRange index -> dB
int qOffsetIdx(int db);

} // namespace dect2::cell
