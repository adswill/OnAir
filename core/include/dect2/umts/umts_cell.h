// UMTS / WCDMA downlink cell search (3GPP TS 25.213): primary and secondary synchronisation channels and the CPICH, at 3.84 Mchip/s. For every cell heard: its primary
// scrambling code number, the code group, level and Ec/Io, the frame timing and the frequency offset. The P-CCPCH is decoded for the operator (MIB) and cell identity (SIB 3) (umts_bch.h).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace umts {
constexpr double kChipRate = 3.84e6;
constexpr int kChipsPerSlot = 2560, kSlotsPerFrame = 15, kChipsPerFrame = 38400;
}

struct UmtsCell {
    int scramblingCode = -1;       // primary scrambling code number, 0 .. 511
    int group = -1;                // code group 0 .. 63
    int frameOffsetChips = 0;      // where the frame starts in the block
    double cfoHz = 0;
    double ecIoDb = 0;             // CPICH power over total in-band power (dB)
    double rscpDbfs = 0;           // CPICH level, dB re full scale
    int slotPeak = 0;              // the slot timing of the sync peak (chips)
    int mcc = -1, mnc = -1, mncDigits = 0;   // from the master information block, -1 not in this block
    int64_t cellIdentity = -1;     // from SIB 3
    int bchBlocks = 0;             // transport blocks decoded (CRC right) in this block of samples
};

// Finds the cells in a block of samples at kChipRate * 2 (7.68 Msps). maxCfoHz: the carrier offset range to search.
std::vector<UmtsCell> umtsCellSearch(const cf32* x7p68, size_t n, double maxCfoHz, int maxCells, double* scanTimeMs = nullptr);

// A test transmitter: SCH + CPICH of several cells, plus a dummy code channel, as a complex baseband at 7.68 Msps. Nothing is transmitted.
struct UmtsTxCell { int code = 0; double cfoHz = 0; double amp = 1; int chipOffset = 0; double dataPower = 0; int mcc = -1, mnc = 0, mncDigits = 2; uint32_t cellIdentity = 0; };
std::vector<cf32> umtsGenerate(const std::vector<UmtsTxCell>& cells, size_t samples, double snrDb, unsigned seed = 1);

}  // namespace dect2
